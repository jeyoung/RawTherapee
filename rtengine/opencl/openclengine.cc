/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 The RawTherapee Team
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "rtengine/opencl/openclengine.h"

#include "rtengine/opencl/openclcache.h"
#include "rtengine/opencl/openclloader.h"
#include "rtengine/imagefloat.h"
#include "rtengine/labimage.h"
#include "rtengine/settings.h"
#include "rt_math.h"
#include "sleef.h"

#include <glibmm/miscutils.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <glib/gstdio.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rtengine
{

namespace opencl
{

namespace
{

struct Backend {
    std::unique_ptr<Glib::Module> module;
    Api api;
    std::vector<DeviceInfo> infos;
    std::vector<cl_platform_id> platforms;
    std::vector<cl_device_id> deviceIds;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    std::unordered_map<std::string, cl_program> programs;
    std::mutex mutex;
    bool attempted = false;
    bool loaded = false;
    int selected = -1;
    std::string error = "OpenCL is unavailable";
};

Backend& backend()
{
    static Backend instance;
    return instance;
}

const char* probeKernelSource = R"(
__kernel void rt_probe(__global float* values)
{
    const size_t index = get_global_id(0);
    values[index] = values[index] + 1.0f;
}
)";

const char* resizeKernelSource = R"(
__kernel void rt_resize_vertical(__global const float* src,
                                 __global float* temp,
                                 __global const int* first,
                                 __global const int* last,
                                 __global const float* weights,
                                 const int srcWidth,
                                 const int support)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    const int start = first[y];
    const int end = last[y];
    float sum = 0.0f;

    for (int i = start; i < end; ++i) {
        sum += weights[y * support + (i - start)] * src[i * srcWidth + x];
    }

    temp[y * srcWidth + x] = sum;
}

__kernel void rt_resize_horizontal(__global const float* temp,
                                   __global float* dst,
                                   __global const int* first,
                                   __global const int* last,
                                   __global const float* weights,
                                   const int srcWidth,
                                   const int dstWidth,
                                   const int support)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    const int start = first[x];
    const int end = last[x];
    float sum = 0.0f;

    for (int j = start; j < end; ++j) {
        sum += weights[x * support + (j - start)] * temp[y * srcWidth + j];
    }

    dst[y * dstWidth + x] = sum;
}
)";

float lanczosWeight(float x, float a)
{
    if (x * x < 1e-6f) {
        return 1.0f;
    }

    if (x * x > a * a) {
        return 0.0f;
    }

    x = static_cast<float>(RT_PI) * x;
    return a * xsinf(x) * xsinf(x / a) / (x * x);
}

struct Cleanup {
    const Api* api = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    cl_mem buffer = nullptr;
    cl_program program = nullptr;
    cl_kernel kernel = nullptr;

    ~Cleanup()
    {
        if (kernel) {
            api->ReleaseKernel(kernel);
        }
        if (program) {
            api->ReleaseProgram(program);
        }
        if (buffer) {
            api->ReleaseMemObject(buffer);
        }
        if (queue) {
            api->ReleaseCommandQueue(queue);
        }
        if (context) {
            api->ReleaseContext(context);
        }
    }
};

std::string trim(std::string value)
{
    while (!value.empty()
            && (value.back() == '\0' || value.back() == ' '
                || value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }

    return value;
}

std::string deviceString(const Api& api, cl_device_id device, cl_device_info param)
{
    size_t size = 0;

    if (api.GetDeviceInfo(device, param, 0, nullptr, &size) != CL_SUCCESS
            || size == 0) {
        return {};
    }

    std::string value(size, '\0');

    if (api.GetDeviceInfo(device, param, size, &value[0], &size) != CL_SUCCESS) {
        return {};
    }

    return trim(std::move(value));
}

std::string programBuildLog(const Api& api, cl_program program, cl_device_id device)
{
    size_t size = 0;

    if (api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0,
                                nullptr, &size) != CL_SUCCESS
            || size == 0) {
        return {};
    }

    std::string log(size, '\0');

    if (api.GetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size,
                                &log[0], nullptr) != CL_SUCCESS) {
        return {};
    }

    return trim(std::move(log));
}

bool programBinary(const Api& api, cl_program program,
                   std::vector<unsigned char>& binary)
{
    binary.clear();
    size_t size = 0;

    if (api.GetProgramInfo(program, CL_PROGRAM_BINARY_SIZES, sizeof(size),
                           &size, nullptr) != CL_SUCCESS
            || size == 0) {
        return false;
    }

    std::vector<unsigned char> data(size);
    unsigned char* pointer = data.data();

    if (api.GetProgramInfo(program, CL_PROGRAM_BINARIES, sizeof(pointer),
                           &pointer, nullptr) != CL_SUCCESS) {
        return false;
    }

    binary = std::move(data);
    return true;
}

bool ensureSession(Backend& state)
{
    if (state.context != nullptr && state.queue != nullptr) {
        return true;
    }

    if (!state.loaded || state.selected < 0) {
        return false;
    }

    const cl_device_id device = state.deviceIds[state.selected];
    cl_int error = CL_SUCCESS;
    state.context = state.api.CreateContext(nullptr, 1, &device, nullptr,
                                            nullptr, &error);

    if (state.context == nullptr || error != CL_SUCCESS) {
        state.context = nullptr;
        state.error = "could not create an OpenCL context";
        return false;
    }

    state.queue = state.api.CreateCommandQueue(state.context, device, 0, &error);

    if (state.queue == nullptr || error != CL_SUCCESS) {
        state.api.ReleaseContext(state.context);
        state.context = nullptr;
        state.error = "could not create an OpenCL command queue";
        return false;
    }

    return true;
}

cl_program programFor(Backend& state, const std::string& source,
                      const std::string& options, std::string& error)
{
    const DeviceInfo& info = state.infos[state.selected];
    const std::string key = programCacheKey(source, options, info);
    const auto found = state.programs.find(key);

    if (found != state.programs.end()) {
        return found->second;
    }

    const cl_device_id device = state.deviceIds[state.selected];
    cl_program program = nullptr;
    std::vector<unsigned char> binary;

    if (loadProgramBinary(key, binary)) {
        const unsigned char* pointer = binary.data();
        size_t size = binary.size();
        cl_int binaryStatus = CL_SUCCESS;
        cl_int status = CL_SUCCESS;
        program = state.api.CreateProgramWithBinary(
            state.context, 1, &device, &size, &pointer, &binaryStatus, &status);

        if (program == nullptr || status != CL_SUCCESS
                || binaryStatus != CL_SUCCESS
                || state.api.BuildProgram(program, 1, &device, options.c_str(),
                                          nullptr, nullptr) != CL_SUCCESS) {
            if (program != nullptr) {
                state.api.ReleaseProgram(program);
                program = nullptr;
            }
        }
    }

    if (program == nullptr) {
        const char* text = source.c_str();
        cl_int status = CL_SUCCESS;
        program = state.api.CreateProgramWithSource(state.context, 1, &text,
                                                    nullptr, &status);

        if (program == nullptr || status != CL_SUCCESS) {
            error = "could not create an OpenCL program";
            return nullptr;
        }

        if (state.api.BuildProgram(program, 1, &device, options.c_str(), nullptr,
                                   nullptr) != CL_SUCCESS) {
            error = "kernel build failed: "
                + programBuildLog(state.api, program, device);
            state.api.ReleaseProgram(program);
            return nullptr;
        }

        std::vector<unsigned char> compiled;

        if (programBinary(state.api, program, compiled)) {
            storeProgramBinary(key, compiled);
        }
    }

    state.programs.emplace(key, program);
    return program;
}

struct BufferSet {
    const Api* api = nullptr;
    std::vector<cl_mem> items;

    ~BufferSet()
    {
        for (const cl_mem memory : items) {
            if (memory != nullptr) {
                api->ReleaseMemObject(memory);
            }
        }
    }

    cl_mem add(cl_mem memory)
    {
        if (memory != nullptr) {
            items.push_back(memory);
        }
        return memory;
    }
};

struct KernelSet {
    const Api* api = nullptr;
    std::vector<cl_kernel> items;

    ~KernelSet()
    {
        for (const cl_kernel kernel : items) {
            if (kernel != nullptr) {
                api->ReleaseKernel(kernel);
            }
        }
    }

    cl_kernel add(cl_kernel kernel)
    {
        if (kernel != nullptr) {
            items.push_back(kernel);
        }
        return kernel;
    }
};

struct StageTimer {
    const char* name = nullptr;
    bool enabled = false;
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();

    StageTimer(const char* stageName, bool isEnabled)
        : name(stageName), enabled(isEnabled)
    {
    }

    ~StageTimer()
    {
        if (enabled) {
            const auto end = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count();
            std::printf("opencl %s: %lld ms\n", name,
                        static_cast<long long>(ms));
        }
    }
};

#ifdef __linux__

/*
 * Some systems have both a CPU ICD (POCL) and a GPU ICD installed. With the
 * ocl-icd loader this combination can crash in the vendor dispatch table.
 * POCL is only a fallback for tests, so hide it when a GPU ICD is present.
 * An explicit OCL_ICD_VENDORS setting always wins.
 */
void prepareVendors()
{
    if (g_getenv("OCL_ICD_VENDORS") != nullptr) {
        return;
    }

    GDir* directory = g_dir_open("/etc/OpenCL/vendors", 0, nullptr);

    if (directory == nullptr) {
        return;
    }

    std::vector<std::string> gpuIcds;
    bool hasPocl = false;
    bool hasGpu = false;
    const char* name = nullptr;

    while ((name = g_dir_read_name(directory)) != nullptr) {
        if (!g_str_has_suffix(name, ".icd")) {
            continue;
        }

        gchar* path = g_build_filename("/etc/OpenCL/vendors", name, nullptr);
        gchar* contents = nullptr;
        gsize length = 0;

        if (g_file_get_contents(path, &contents, &length, nullptr)) {
            std::string text(contents, length);
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });

            if (text.find("pocl") != std::string::npos) {
                hasPocl = true;
            } else {
                hasGpu = true;
                gpuIcds.emplace_back(path);
            }
        }

        g_free(contents);
        g_free(path);
    }

    g_dir_close(directory);

    if (!hasPocl || !hasGpu) {
        return;
    }

    const Glib::ustring base = settings != nullptr && !settings->cacheDir.empty()
        ? settings->cacheDir
        : Glib::ustring(g_get_tmp_dir());
    const Glib::ustring target = Glib::build_filename(base, "opencl-vendors");

    if (g_mkdir_with_parents(target.c_str(), 0700) != 0) {
        return;
    }

    for (const std::string& path : gpuIcds) {
        gchar* contents = nullptr;
        gsize length = 0;

        if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)) {
            continue;
        }

        gchar* baseName = g_path_get_basename(path.c_str());
        gchar* destination = g_build_filename(target.c_str(), baseName, nullptr);
        g_file_set_contents(destination, contents, length, nullptr);
        g_free(destination);
        g_free(baseName);
        g_free(contents);
    }

    g_setenv("OCL_ICD_VENDORS", target.c_str(), TRUE);
}

#endif // __linux__

bool openLoader(Backend& state)
{
#ifdef __linux__
    prepareVendors();
#endif

#ifdef _WIN32
    const char* candidates[] = {"OpenCL.dll"};
#elif defined(__APPLE__)
    const char* candidates[] = {
        "/System/Library/Frameworks/OpenCL.framework/OpenCL"
    };
#else
    const char* candidates[] = {"libOpenCL.so.1", "libOpenCL.so"};
#endif

    std::string lastError;

    for (const char* candidate : candidates) {
        std::unique_ptr<Glib::Module> module(new Glib::Module(candidate));

        if (!*module) {
            lastError = Glib::Module::get_last_error();
            continue;
        }

        std::string error;

        if (!loadApi(*module, state.api, error)) {
            lastError = error;
            continue;
        }

        state.module = std::move(module);
        return true;
    }

    state.error = "OpenCL loader not found";
    if (!lastError.empty()) {
        state.error += ": " + lastError;
    }

    return false;
}

int chooseDevice(const std::vector<DeviceInfo>& infos);

bool enumerate(Backend& state)
{    cl_uint platformCount = 0;

    if (state.api.GetPlatformIDs(0, nullptr, &platformCount) != CL_SUCCESS
            || platformCount == 0) {
        state.error = "No OpenCL platform found";
        return false;
    }

    state.platforms.resize(platformCount);

    if (state.api.GetPlatformIDs(platformCount, state.platforms.data(),
                                 nullptr) != CL_SUCCESS) {
        state.error = "Could not enumerate OpenCL platforms";
        state.platforms.clear();
        return false;
    }

    for (const cl_platform_id platform : state.platforms) {
        cl_uint deviceCount = 0;

        if (state.api.GetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr,
                                   &deviceCount) != CL_SUCCESS
                || deviceCount == 0) {
            continue;
        }

        std::vector<cl_device_id> found(deviceCount);

        if (state.api.GetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, deviceCount,
                                   found.data(), nullptr) != CL_SUCCESS) {
            continue;
        }

        for (const cl_device_id device : found) {
            DeviceInfo info;
            info.name = deviceString(state.api, device, CL_DEVICE_NAME);
            info.vendor = deviceString(state.api, device, CL_DEVICE_VENDOR);
            info.version = deviceString(state.api, device, CL_DEVICE_VERSION);
            info.driver = deviceString(state.api, device, CL_DRIVER_VERSION);

            cl_device_type type = 0;
            cl_ulong memory = 0;

            if (state.api.GetDeviceInfo(device, CL_DEVICE_TYPE, sizeof(type),
                                        &type, nullptr) == CL_SUCCESS) {
                info.cpu = (type & CL_DEVICE_TYPE_CPU) != 0;
            }

            if (state.api.GetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE,
                                        sizeof(memory), &memory,
                                        nullptr) == CL_SUCCESS) {
                info.globalMemory = memory;
            }

            cl_ulong maxAllocation = 0;

            if (state.api.GetDeviceInfo(device, CL_DEVICE_MAX_MEM_ALLOC_SIZE,
                                        sizeof(maxAllocation), &maxAllocation,
                                        nullptr) == CL_SUCCESS) {
                info.maxAllocation = maxAllocation;
            }

            if (info.name.empty()) {
                info.name = "Unknown OpenCL device";
            }

            state.deviceIds.push_back(device);
            state.infos.push_back(std::move(info));
        }
    }

    if (state.infos.empty()) {
        state.error = "No OpenCL device found";
        return false;
    }

    state.selected = chooseDevice(state.infos);
    state.loaded = true;
    return true;
}

int chooseDevice(const std::vector<DeviceInfo>& infos)
{
    if (infos.empty()) {
        return -1;
    }

    if (settings != nullptr && !settings->openCLDevice.empty()) {
        for (size_t i = 0; i < infos.size(); ++i) {
            const std::string haystack =
                infos[i].name + " " + infos[i].vendor + " " + infos[i].version;

            if (haystack.find(settings->openCLDevice) != std::string::npos) {
                return static_cast<int>(i);
            }
        }
    }

    int best = -1;
    unsigned long long bestMemory = 0;

    for (size_t i = 0; i < infos.size(); ++i) {
        if (!infos[i].cpu && infos[i].globalMemory >= bestMemory) {
            best = static_cast<int>(i);
            bestMemory = infos[i].globalMemory;
        }
    }

    return best >= 0 ? best : 0;
}

} // namespace

bool init()
{
    Backend& state = backend();

    if (state.loaded) {
        return true;
    }

    state.attempted = true;
    state.error = "OpenCL is unavailable";

    if (!openLoader(state)) {
        return false;
    }

    if (!enumerate(state)) {
        state.module.reset();
        return false;
    }

    if (settings && settings->verbose) {
        std::printf("OpenCL: %zu device(s) found\n", state.infos.size());

        for (const DeviceInfo& info : state.infos) {
            std::printf("  %s (%s, %llu MB)%s\n", info.name.c_str(),
                        info.version.c_str(), info.globalMemory / (1024 * 1024),
                        info.cpu ? " [CPU]" : "");
        }
    }

    return true;
}

bool ensureInit()
{
    Backend& state = backend();

    if (!state.attempted) {
        init();
    }

    return state.loaded;
}

void shutdown()
{
    Backend& state = backend();

    for (auto& entry : state.programs) {
        if (entry.second != nullptr) {
            state.api.ReleaseProgram(entry.second);
        }
    }
    state.programs.clear();

    if (state.queue != nullptr) {
        state.api.ReleaseCommandQueue(state.queue);
    }
    if (state.context != nullptr) {
        state.api.ReleaseContext(state.context);
    }

    state.deviceIds.clear();
    state.platforms.clear();
    state.infos.clear();
    state.module.reset();
    state.api = Api();
    state.context = nullptr;
    state.queue = nullptr;
    state.attempted = false;
    state.loaded = false;
    state.selected = -1;
    state.error = "OpenCL is unavailable";
}

void reconfigure()
{
    shutdown();
    init();
}

bool available()
{
    ensureInit();
    const Backend& state = backend();
    return state.loaded && !state.infos.empty();
}

bool enabled()
{
    if (settings != nullptr && !settings->useOpenCL) {
        return false;
    }

    return available();
}

const std::vector<DeviceInfo>& devices()
{
    ensureInit();
    return backend().infos;
}

int selectedDevice()
{
    ensureInit();
    return backend().selected;
}

bool resizeLanczosPlanes(const float* const sourcePointers[3],
                         size_t sourceRowPitch, int srcWidth, int srcHeight,
                         float* const destPointers[3], size_t destRowPitch,
                         int dstWidth, int dstHeight, float scale);

bool resizeLanczos(const Imagefloat* src, Imagefloat* dst, float scale)
{
    if (src == nullptr || dst == nullptr) {
        return false;
    }

    const float* sourcePointers[3] = {src->r(0), src->g(0), src->b(0)};
    float* destPointers[3] = {dst->r(0), dst->g(0), dst->b(0)};

    return resizeLanczosPlanes(sourcePointers, src->getRowStride(),
                               src->getWidth(), src->getHeight(),
                               destPointers, dst->getRowStride(),
                               dst->getWidth(), dst->getHeight(), scale);
}

bool resizeLanczos(const LabImage* src, LabImage* dst, float scale)
{
    if (src == nullptr || dst == nullptr) {
        return false;
    }

    const float* sourcePointers[3] = {src->L[0], src->a[0], src->b[0]};
    float* destPointers[3] = {dst->L[0], dst->a[0], dst->b[0]};

    return resizeLanczosPlanes(sourcePointers,
                               static_cast<size_t>(src->W) * sizeof(float),
                               src->W, src->H,
                               destPointers,
                               static_cast<size_t>(dst->W) * sizeof(float),
                               dst->W, dst->H, scale);
}

bool resizeLanczosPlanes(const float* const sourcePointers[3],
                         size_t sourceRowPitch, int srcWidth, int srcHeight,
                         float* const destPointers[3], size_t destRowPitch,
                         int dstWidth, int dstHeight, float scale)
{
    if (!(scale > 0.0f) || srcWidth <= 0 || srcHeight <= 0
            || dstWidth <= 0 || dstHeight <= 0) {
        return false;
    }

    if (settings != nullptr && !settings->useOpenCL) {
        return false;
    }

    if (!available()) {
        return false;
    }

    Backend& state = backend();
    std::lock_guard<std::mutex> lock(state.mutex);

    if (!ensureSession(state) || state.selected < 0) {
        return false;
    }

    StageTimer timer("resize", g_getenv("RT_OPENCL_PROFILE") != nullptr);

    constexpr float a = 3.0f;
    const float delta = 1.0f / scale;
    const float sc = std::min(scale, 1.0f);
    const int support = static_cast<int>(2.0f * a / sc) + 1;

    if (support <= 0) {
        return false;
    }

    std::vector<float> horizontalWeights(static_cast<size_t>(support) * dstWidth);
    std::vector<cl_int> horizontalFirst(dstWidth);
    std::vector<cl_int> horizontalLast(dstWidth);

    for (int x = 0; x < dstWidth; ++x) {
        const float x0 = (static_cast<float>(x) + 0.5f) * delta - 0.5f;
        float* weights = &horizontalWeights[static_cast<size_t>(x) * support];
        float sum = 0.0f;

        horizontalFirst[x] = std::max(0, static_cast<int>(std::floor(x0 - a / sc)) + 1);
        horizontalLast[x] = std::min(srcWidth, static_cast<int>(std::floor(x0 + a / sc)) + 1);

        for (int j = horizontalFirst[x]; j < horizontalLast[x]; ++j) {
            const int k = j - horizontalFirst[x];
            const float z = sc * (x0 - static_cast<float>(j));
            weights[k] = lanczosWeight(z, a);
            sum += weights[k];
        }

        for (int k = 0; k < support; ++k) {
            weights[k] /= sum;
        }
    }

    std::vector<float> verticalWeights(static_cast<size_t>(support) * dstHeight);
    std::vector<cl_int> verticalFirst(dstHeight);
    std::vector<cl_int> verticalLast(dstHeight);

    for (int y = 0; y < dstHeight; ++y) {
        const float y0 = (static_cast<float>(y) + 0.5f) * delta - 0.5f;
        float* weights = &verticalWeights[static_cast<size_t>(y) * support];
        float sum = 0.0f;

        verticalFirst[y] = std::max(0, static_cast<int>(std::floor(y0 - a / sc)) + 1);
        verticalLast[y] = std::min(srcHeight, static_cast<int>(std::floor(y0 + a / sc)) + 1);

        for (int i = verticalFirst[y]; i < verticalLast[y]; ++i) {
            const int k = i - verticalFirst[y];
            const float z = sc * (y0 - static_cast<float>(i));
            weights[k] = lanczosWeight(z, a);
            sum += weights[k];
        }

        for (int k = 0; k < support; ++k) {
            weights[k] /= sum;
        }
    }

    const size_t sourceBytes = static_cast<size_t>(srcWidth) * srcHeight * sizeof(float);
    const size_t tempBytes = static_cast<size_t>(srcWidth) * dstHeight * sizeof(float);
    const size_t destBytes = static_cast<size_t>(dstWidth) * dstHeight * sizeof(float);
    const DeviceInfo& info = state.infos[state.selected];
    const auto fits = [&info](size_t bytes) {
        return info.maxAllocation == 0 || bytes <= info.maxAllocation;
    };

    if (!fits(sourceBytes) || !fits(tempBytes) || !fits(destBytes)) {
        return false;
    }

    BufferSet buffers;
    buffers.api = &state.api;
    KernelSet kernels;
    kernels.api = &state.api;

    const auto createBuffer = [&](cl_mem_flags flags, size_t bytes,
                                  const void* host) -> cl_mem {
        cl_int error = CL_SUCCESS;
        cl_mem memory = state.api.CreateBuffer(state.context, flags, bytes,
                                               const_cast<void*>(host), &error);

        if (memory == nullptr || error != CL_SUCCESS) {
            if (memory != nullptr) {
                state.api.ReleaseMemObject(memory);
            }
            return nullptr;
        }

        return buffers.add(memory);
    };

    cl_mem sourceBuffers[3] = {};
    cl_mem tempBuffers[3] = {};
    cl_mem destBuffers[3] = {};

    const size_t sourceOrigin[3] = {0, 0, 0};
    const size_t sourceRegion[3] = {
        static_cast<size_t>(srcWidth) * sizeof(float),
        static_cast<size_t>(srcHeight), 1};
    const size_t destRegion[3] = {
        static_cast<size_t>(dstWidth) * sizeof(float),
        static_cast<size_t>(dstHeight), 1};
    for (int plane = 0; plane < 3; ++plane) {
        sourceBuffers[plane] = createBuffer(CL_MEM_READ_ONLY, sourceBytes, nullptr);
        tempBuffers[plane] = createBuffer(CL_MEM_READ_WRITE, tempBytes, nullptr);
        destBuffers[plane] = createBuffer(CL_MEM_WRITE_ONLY, destBytes, nullptr);

        if (sourceBuffers[plane] == nullptr || tempBuffers[plane] == nullptr
                || destBuffers[plane] == nullptr) {
            return false;
        }

        if (state.api.EnqueueWriteBufferRect(
                state.queue, sourceBuffers[plane], CL_TRUE, sourceOrigin,
                sourceOrigin, sourceRegion, 0, 0, sourceRowPitch, 0,
                sourcePointers[plane], 0, nullptr, nullptr) != CL_SUCCESS) {
            return false;
        }
    }

    cl_mem horizontalWeightBuffer = createBuffer(
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        horizontalWeights.size() * sizeof(float), horizontalWeights.data());
    cl_mem horizontalFirstBuffer = createBuffer(
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        horizontalFirst.size() * sizeof(cl_int), horizontalFirst.data());
    cl_mem horizontalLastBuffer = createBuffer(
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        horizontalLast.size() * sizeof(cl_int), horizontalLast.data());
    cl_mem verticalWeightBuffer = createBuffer(
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        verticalWeights.size() * sizeof(float), verticalWeights.data());
    cl_mem verticalFirstBuffer = createBuffer(
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        verticalFirst.size() * sizeof(cl_int), verticalFirst.data());
    cl_mem verticalLastBuffer = createBuffer(
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        verticalLast.size() * sizeof(cl_int), verticalLast.data());

    if (horizontalWeightBuffer == nullptr || horizontalFirstBuffer == nullptr
            || horizontalLastBuffer == nullptr || verticalWeightBuffer == nullptr
            || verticalFirstBuffer == nullptr || verticalLastBuffer == nullptr) {
        return false;
    }

    std::string error;
    cl_program program = programFor(state, resizeKernelSource, "-cl-std=CL1.2",
                                    error);

    if (program == nullptr) {
        return false;
    }

    cl_int status = CL_SUCCESS;
    cl_kernel vertical = kernels.add(
        state.api.CreateKernel(program, "rt_resize_vertical", &status));

    if (vertical == nullptr || status != CL_SUCCESS) {
        return false;
    }

    cl_kernel horizontal = kernels.add(
        state.api.CreateKernel(program, "rt_resize_horizontal", &status));

    if (horizontal == nullptr || status != CL_SUCCESS) {
        return false;
    }

    const cl_int sourceWidthArg = srcWidth;
    const cl_int destWidthArg = dstWidth;
    const cl_int supportArg = support;

    for (int plane = 0; plane < 3; ++plane) {
        status = CL_SUCCESS;
        status |= state.api.SetKernelArg(vertical, 0, sizeof(cl_mem), &sourceBuffers[plane]);
        status |= state.api.SetKernelArg(vertical, 1, sizeof(cl_mem), &tempBuffers[plane]);
        status |= state.api.SetKernelArg(vertical, 2, sizeof(cl_mem), &verticalFirstBuffer);
        status |= state.api.SetKernelArg(vertical, 3, sizeof(cl_mem), &verticalLastBuffer);
        status |= state.api.SetKernelArg(vertical, 4, sizeof(cl_mem), &verticalWeightBuffer);
        status |= state.api.SetKernelArg(vertical, 5, sizeof(cl_int), &sourceWidthArg);
        status |= state.api.SetKernelArg(vertical, 6, sizeof(cl_int), &supportArg);

        if (status != CL_SUCCESS) {
            return false;
        }

        const size_t verticalGlobal[2] = {
            static_cast<size_t>(srcWidth), static_cast<size_t>(dstHeight)};

        status = state.api.EnqueueNDRangeKernel(state.queue, vertical, 2, nullptr,
                                                verticalGlobal, nullptr, 0,
                                                nullptr, nullptr);

        if (status != CL_SUCCESS) {
            return false;
        }

        status = CL_SUCCESS;
        status |= state.api.SetKernelArg(horizontal, 0, sizeof(cl_mem), &tempBuffers[plane]);
        status |= state.api.SetKernelArg(horizontal, 1, sizeof(cl_mem), &destBuffers[plane]);
        status |= state.api.SetKernelArg(horizontal, 2, sizeof(cl_mem), &horizontalFirstBuffer);
        status |= state.api.SetKernelArg(horizontal, 3, sizeof(cl_mem), &horizontalLastBuffer);
        status |= state.api.SetKernelArg(horizontal, 4, sizeof(cl_mem), &horizontalWeightBuffer);
        status |= state.api.SetKernelArg(horizontal, 5, sizeof(cl_int), &sourceWidthArg);
        status |= state.api.SetKernelArg(horizontal, 6, sizeof(cl_int), &destWidthArg);
        status |= state.api.SetKernelArg(horizontal, 7, sizeof(cl_int), &supportArg);

        if (status != CL_SUCCESS) {
            return false;
        }

        const size_t horizontalGlobal[2] = {
            static_cast<size_t>(dstWidth), static_cast<size_t>(dstHeight)};

        status = state.api.EnqueueNDRangeKernel(state.queue, horizontal, 2,
                                                nullptr, horizontalGlobal,
                                                nullptr, 0, nullptr, nullptr);

        if (status != CL_SUCCESS) {
            return false;
        }
    }

    if (state.api.Finish(state.queue) != CL_SUCCESS) {
        return false;
    }

    for (int plane = 0; plane < 3; ++plane) {
        if (state.api.EnqueueReadBufferRect(
                state.queue, destBuffers[plane], CL_TRUE, sourceOrigin,
                sourceOrigin, destRegion, 0, 0, destRowPitch, 0,
                destPointers[plane], 0, nullptr, nullptr) != CL_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool selfTest(std::string& report)
{
    report.clear();

    if (!ensureInit()) {
        report = statusText();
        return false;
    }

    Backend& state = backend();

    const int index = state.selected >= 0 ? state.selected : 0;
    const cl_device_id device = state.deviceIds[index];
    Cleanup cleanup;
    cleanup.api = &state.api;

    cl_int error = CL_SUCCESS;
    cleanup.context = state.api.CreateContext(nullptr, 1, &device, nullptr,
                                              nullptr, &error);

    if (!cleanup.context || error != CL_SUCCESS) {
        report = "could not create an OpenCL context";
        return false;
    }

    cleanup.queue = state.api.CreateCommandQueue(cleanup.context, device, 0, &error);

    if (!cleanup.queue || error != CL_SUCCESS) {
        report = "could not create an OpenCL command queue";
        return false;
    }

    constexpr size_t count = 1024;
    const size_t bytes = count * sizeof(float);
    cleanup.buffer = state.api.CreateBuffer(cleanup.context, CL_MEM_READ_WRITE,
                                            bytes, nullptr, &error);

    if (!cleanup.buffer || error != CL_SUCCESS) {
        report = "could not allocate a device buffer";
        return false;
    }

    std::vector<float> host(count, 1.0f);

    if (state.api.EnqueueWriteBuffer(cleanup.queue, cleanup.buffer, CL_TRUE, 0,
                                     bytes, host.data(), 0, nullptr,
                                     nullptr) != CL_SUCCESS) {
        report = "could not upload the test buffer";
        return false;
    }

    const char* options = "-cl-std=CL1.2";
    const DeviceInfo& deviceInfo = state.infos[index];
    const std::string cacheKey = programCacheKey(probeKernelSource, options,
                                                 deviceInfo);
    std::vector<unsigned char> cachedBinary;
    bool fromCache = loadProgramBinary(cacheKey, cachedBinary);

    if (fromCache) {
        const unsigned char* pointer = cachedBinary.data();
        size_t size = cachedBinary.size();
        cl_int binaryStatus = CL_SUCCESS;
        cleanup.program = state.api.CreateProgramWithBinary(
            cleanup.context, 1, &device, &size, &pointer, &binaryStatus,
            &error);

        if (!cleanup.program || error != CL_SUCCESS
                || binaryStatus != CL_SUCCESS) {
            if (cleanup.program) {
                state.api.ReleaseProgram(cleanup.program);
                cleanup.program = nullptr;
            }
            fromCache = false;
        } else if (state.api.BuildProgram(cleanup.program, 1, &device, options,
                                          nullptr, nullptr) != CL_SUCCESS) {
            state.api.ReleaseProgram(cleanup.program);
            cleanup.program = nullptr;
            fromCache = false;
        }
    }

    if (!fromCache) {
        const char* source = probeKernelSource;
        cleanup.program = state.api.CreateProgramWithSource(
            cleanup.context, 1, &source, nullptr, &error);

        if (!cleanup.program || error != CL_SUCCESS) {
            report = "could not create the test program";
            return false;
        }

        error = state.api.BuildProgram(cleanup.program, 1, &device, options,
                                       nullptr, nullptr);

        if (error != CL_SUCCESS) {
            report = "kernel build failed: "
                + programBuildLog(state.api, cleanup.program, device);
            return false;
        }

        std::vector<unsigned char> binary;

        if (programBinary(state.api, cleanup.program, binary)) {
            storeProgramBinary(cacheKey, binary);
        }
    }

    cleanup.kernel = state.api.CreateKernel(cleanup.program, "rt_probe", &error);

    if (!cleanup.kernel || error != CL_SUCCESS) {
        report = "could not create the test kernel";
        return false;
    }

    if (state.api.SetKernelArg(cleanup.kernel, 0, sizeof(cl_mem),
                               &cleanup.buffer) != CL_SUCCESS) {
        report = "could not set the kernel argument";
        return false;
    }

    const size_t global = count;

    if (state.api.EnqueueNDRangeKernel(cleanup.queue, cleanup.kernel, 1,
                                       nullptr, &global, nullptr, 0, nullptr,
                                       nullptr) != CL_SUCCESS) {
        report = "could not run the test kernel";
        return false;
    }

    if (state.api.EnqueueReadBuffer(cleanup.queue, cleanup.buffer, CL_TRUE, 0,
                                    bytes, host.data(), 0, nullptr,
                                    nullptr) != CL_SUCCESS) {
        report = "could not read back the test buffer";
        return false;
    }

    state.api.Finish(cleanup.queue);

    for (const float value : host) {
        if (value != 2.0f) {
            report = "kernel produced an unexpected value";
            return false;
        }
    }

    report = std::string("buffer round-trip and kernel compilation succeeded ")
        + (fromCache ? "(cached)" : "(compiled)") + " on "
        + state.infos[index].name;
    return true;
}

std::string statusText()
{
    ensureInit();
    const Backend& state = backend();

    if (!state.loaded || state.infos.empty()) {
        return state.error;
    }

    const int index = state.selected >= 0 ? state.selected : 0;
    const DeviceInfo& info = state.infos[index];
    char memory[32];
    std::snprintf(memory, sizeof(memory), "%llu MB",
                  info.globalMemory / (1024 * 1024));

    return info.name + " (" + info.version + ", " + memory + ")";
}

} // namespace opencl

} // namespace rtengine
