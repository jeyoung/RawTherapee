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
#include "boxblur.h"
#include "rtengine/settings.h"
#include "rt_math.h"
#include "sleef.h"

#include <glibmm/miscutils.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
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

thread_local bool threadStageAllowed = false;

struct Backend {
    std::unique_ptr<Glib::Module> module;
    Api api;
    std::vector<DeviceInfo> infos;
    std::vector<cl_platform_id> platforms;
    std::vector<cl_device_id> deviceIds;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;
    std::unordered_map<std::string, cl_program> programs;
    std::recursive_mutex mutex;
    std::atomic<bool> stageEnabled{true};
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
#pragma OPENCL FP_CONTRACT OFF

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

const char* captureSharpeningKernelSource = R"(
#pragma OPENCL FP_CONTRACT OFF

#define RT_TILE 32
#define RT_FULL 48

inline float rt_conv(__local const float* src, const int x, const int y,
                     const int radius, __global const float* weights)
{
    const int size = 2 * radius + 1;
    float sum = 0.0f;

    for (int i = -radius; i <= radius; ++i) {
        for (int j = -radius; j <= radius; ++j) {
            sum += weights[(i + radius) * size + (j + radius)]
                * src[(y + i) * RT_FULL + (x + j)];
        }
    }

    return sum;
}

__kernel __attribute__((reqd_work_group_size(RT_TILE, RT_TILE, 1)))
void rt_capture(__global const float* oldLum,
                __global const float* blend,
                __global float* lum,
                __global const float* weights,
                const int width, const int height,
                const int border, const int full,
                const int iterations, const int checkStop,
                const int radius, const int phase)
{
    __local float tmpI[RT_FULL * RT_FULL];
    __local float tmp[RT_FULL * RT_FULL];
    __local float lumLocal[RT_FULL * RT_FULL];
    __local float iterCheck[RT_TILE * RT_TILE];
    __local float maxBlend[RT_TILE * RT_TILE];
    __local int stopFlag;

    const int wx = get_local_id(0);
    const int wy = get_local_id(1);
    const int tileX = border + get_group_id(0) * RT_TILE;
    const int tileY = border + get_group_id(1) * RT_TILE;
    const int endX = (tileX + RT_TILE + border) >= width;
    const int endY = (tileY + RT_TILE + border) >= height;
    const int groupPhase = endY ? (endX ? 3 : 2) : (endX ? 1 : 0);

    if (groupPhase != phase) {
        return;
    }

    const int startX = endX ? width - full : tileX - border;
    const int startY = endY ? height - full : tileY - border;

    for (int y = wy; y < RT_FULL; y += RT_TILE) {
        for (int x = wx; x < RT_FULL; x += RT_TILE) {
            tmp[y * RT_FULL + x] = 1.0f;
        }
    }

    for (int y = wy; y < full; y += RT_TILE) {
        for (int x = wx; x < full; x += RT_TILE) {
            const float value = oldLum[(startY + y) * width + startX + x];
            tmpI[y * RT_FULL + x] = value;
            lumLocal[y * RT_FULL + x] = value;
        }
    }

    const int outX = startX + border + wx;
    const int outY = startY + border + wy;
    const float blendValue = blend[outY * width + outX];
    iterCheck[wy * RT_TILE + wx] = oldLum[outY * width + outX]
        * blendValue * 0.5f;
    maxBlend[wy * RT_TILE + wx] = blendValue;
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int stride = (RT_TILE * RT_TILE) / 2; stride > 0; stride >>= 1) {
        if (wy * RT_TILE + wx < stride) {
            maxBlend[wy * RT_TILE + wx] = fmax(
                maxBlend[wy * RT_TILE + wx],
                maxBlend[wy * RT_TILE + wx + stride]);
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (maxBlend[0] < 0.01f) {
        return;
    }

    const int index = (border + wy) * RT_FULL + border + wx;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int y = radius + wy; y < full - radius; y += RT_TILE) {
            for (int x = radius + wx; x < full - radius; x += RT_TILE) {
                tmp[y * RT_FULL + x] = lumLocal[y * RT_FULL + x]
                    / fmax(rt_conv(tmpI, x, y, radius, weights), 0.00001f);
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        for (int y = radius + wy; y < full - radius; y += RT_TILE) {
            for (int x = radius + wx; x < full - radius; x += RT_TILE) {
                tmpI[y * RT_FULL + x] *= rt_conv(tmp, x, y, radius, weights);
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        if (checkStop) {
            if (wx == 0 && wy == 0) {
                stopFlag = 0;
            }
            barrier(CLK_LOCAL_MEM_FENCE);

            if (tmpI[index] < iterCheck[wy * RT_TILE + wx]) {
                atomic_or(&stopFlag, 1);
            }
            barrier(CLK_LOCAL_MEM_FENCE);

            if (stopFlag && iteration < iterations - 1) {
                break;
            }
        }
    }

    const float current = lum[outY * width + outX];
    lum[outY * width + outX] = blendValue * (tmpI[index] - current)
        + current;
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

const char* transformKernelSource = R"(
#pragma OPENCL FP_CONTRACT OFF

inline float rt_finish(const float value, const int useLog)
{
    return useLog ? exp(value) : value;
}

inline float rt_cubic1(__global const float* src, const int srcWidth,
                       const int xs, const int ys,
                       const float dx, const float dy)
{
    const float A = -0.85f;
    const float t1v = A * (dy - dy * dy);
    const float t2v = (3.0f - 2.0f * dy) * dy * dy;
    const float w0v = t1v - (t1v * dy);
    const float w1v = 1.0f - (t1v * dy) - t2v;
    const float w2v = t1v * dy - t1v + t2v;
    const float w3v = t1v * dy;
    float values[4];

    for (int i = 0; i < 4; ++i) {
        values[i] = (w0v * src[ys * srcWidth + xs + i]
            + w1v * src[(ys + 1) * srcWidth + xs + i])
            + (w2v * src[(ys + 2) * srcWidth + xs + i]
            + w3v * src[(ys + 3) * srcWidth + xs + i]);
    }

    const float t1h = A * (dx - dx * dx);
    const float t2h = (3.0f - 2.0f * dx) * dx * dx;
    const float w0h = t1h - (t1h * dx);
    const float w1h = 1.0f - (t1h * dx) - t2h;
    const float w2h = t1h * dx - t1h + t2h;
    const float w3h = t1h * dx;

    return (values[0] * w0h + values[2] * w2h)
        + (values[1] * w1h + values[3] * w3h);
}

inline float rt_bilinear1(__global const float* src, const int srcWidth,
                          const int srcHeight, const int xc, const int yc,
                          const float dx, const float dy)
{
    const int x1 = clamp(xc, 0, srcWidth - 1);
    const int x2 = clamp(xc + 1, 0, srcWidth - 1);
    const int y1 = clamp(yc, 0, srcHeight - 1);
    const int y2 = clamp(yc + 1, 0, srcHeight - 1);

    return src[y1 * srcWidth + x1] * (1.0f - dx) * (1.0f - dy)
        + src[y1 * srcWidth + x2] * dx * (1.0f - dy)
        + src[y2 * srcWidth + x1] * (1.0f - dx) * dy
        + src[y2 * srcWidth + x2] * dx * dy;
}

__kernel void rt_transform(__global const float* srcR,
                           __global const float* srcG,
                           __global const float* srcB,
                           __global float* dstR,
                           __global float* dstG,
                           __global float* dstB,
                           __global const float* map,
                           const int srcWidth, const int srcHeight,
                           const int dstWidth, const int dstHeight,
                           const int highQuality, const int useLog)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    const int index = y * dstWidth + x;
    const int mapIndex = index * 4;
    const int xc = (int)map[mapIndex];
    const int yc = (int)map[mapIndex + 1];
    const float dx = map[mapIndex + 2];
    const float dy = map[mapIndex + 3];

    if (yc < 0 || yc >= srcHeight || xc < 0 || xc >= srcWidth) {
        dstR[index] = 0.0f;
        dstG[index] = 0.0f;
        dstB[index] = 0.0f;
        return;
    }

    if (yc > 0 && yc < srcHeight - 2 && xc > 0 && xc < srcWidth - 2) {
        if (highQuality) {
            dstR[index] = rt_finish(rt_cubic1(srcR, srcWidth, xc - 1, yc - 1, dx, dy), useLog);
            dstG[index] = rt_finish(rt_cubic1(srcG, srcWidth, xc - 1, yc - 1, dx, dy), useLog);
            dstB[index] = rt_finish(rt_cubic1(srcB, srcWidth, xc - 1, yc - 1, dx, dy), useLog);
        } else {
            dstR[index] = rt_finish(rt_bilinear1(srcR, srcWidth, srcHeight, xc, yc, dx, dy), useLog);
            dstG[index] = rt_finish(rt_bilinear1(srcG, srcWidth, srcHeight, xc, yc, dx, dy), useLog);
            dstB[index] = rt_finish(rt_bilinear1(srcB, srcWidth, srcHeight, xc, yc, dx, dy), useLog);
        }
    } else {
        dstR[index] = rt_finish(rt_bilinear1(srcR, srcWidth, srcHeight, xc, yc, dx, dy), useLog);
        dstG[index] = rt_finish(rt_bilinear1(srcG, srcWidth, srcHeight, xc, yc, dx, dy), useLog);
        dstB[index] = rt_finish(rt_bilinear1(srcB, srcWidth, srcHeight, xc, yc, dx, dy), useLog);
    }
}
)";

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
            state.error = error;
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

std::string trimText(std::string value)
{
    while (!value.empty()
            && (value.back() == '\0' || value.back() == ' '
                || value.back() == '\n' || value.back() == '\r'
                || value.back() == '\t')) {
        value.pop_back();
    }

    return value;
}

bool icdLibraryExists(const std::string& library)
{
    const std::string name = trimText(library);

    if (name.empty()) {
        return false;
    }

    if (g_path_is_absolute(name.c_str())) {
        return g_file_test(name.c_str(), G_FILE_TEST_IS_REGULAR);
    }

    // Let the dynamic loader resolve the name, so multiarch and custom
    // library paths work on every architecture.
    Glib::Module module(name);
    return static_cast<bool>(module);
}

/*
 * Some systems have both a CPU ICD (POCL) and a GPU ICD installed. With the
 * ocl-icd loader this combination can crash in the vendor dispatch table.
 * POCL is only a fallback for tests, so hide it when a *usable* GPU ICD is
 * present. A GPU ICD whose library is missing is ignored, so it cannot hide
 * a working CPU device. An explicit OCL_ICD_VENDORS setting always wins, and
 * RT_OPENCL_VENDOR_FILTER=0 disables the workaround.
 */
void prepareVendors()
{
    if (g_getenv("OCL_ICD_VENDORS") != nullptr) {
        return;
    }

    const char* filter = g_getenv("RT_OPENCL_VENDOR_FILTER");

    if (filter != nullptr && std::string(filter) == "0") {
        return;
    }

    GDir* directory = g_dir_open("/etc/OpenCL/vendors", 0, nullptr);

    if (directory == nullptr) {
        return;
    }

    std::vector<std::string> gpuIcds;
    std::string material;
    bool hasPocl = false;
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
            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });

            material += name;
            material += ':';
            material += text;
            material += '\n';

            if (lower.find("pocl") != std::string::npos) {
                hasPocl = true;
            } else if (icdLibraryExists(text)) {
                gpuIcds.emplace_back(path);
            }
        }

        g_free(contents);
        g_free(path);
    }

    g_dir_close(directory);

    if (!hasPocl || gpuIcds.empty()) {
        return;
    }

    Glib::ustring base;

    if (settings != nullptr && !settings->cacheDir.empty()
            && Glib::path_is_absolute(settings->cacheDir)) {
        base = settings->cacheDir;
    } else {
        base = Glib::build_filename(g_get_user_cache_dir(), "rawtherapee");
    }

    gchar* checksum = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, material.c_str(), static_cast<gssize>(material.size()));
    const std::string suffix = checksum != nullptr
        ? std::string(checksum).substr(0, 12) : std::string("default");
    g_free(checksum);

    const Glib::ustring target = Glib::build_filename(
        base, "opencl-vendors-" + suffix);

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
{
    state.platforms.clear();
    state.deviceIds.clear();
    state.infos.clear();

    cl_uint platformCount = 0;

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

            cl_ulong localMemory = 0;

            if (state.api.GetDeviceInfo(device, CL_DEVICE_LOCAL_MEM_SIZE,
                                        sizeof(localMemory), &localMemory,
                                        nullptr) == CL_SUCCESS) {
                info.localMemory = localMemory;
            }

            size_t maxWorkGroup = 0;

            if (state.api.GetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE,
                                        sizeof(maxWorkGroup), &maxWorkGroup,
                                        nullptr) == CL_SUCCESS) {
                info.maxWorkGroup = maxWorkGroup;
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
    state.stageEnabled = settings == nullptr || settings->useOpenCL;
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

static bool initLocked()
{
    Backend& state = backend();

    if (state.loaded) {
        return true;
    }

    state.attempted = true;
    state.error = "OpenCL is unavailable";
    setProgramCacheDirectory(settings != nullptr ? settings->cacheDir.raw() : std::string());

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

bool init()
{
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    return initLocked();
}

bool ensureInit()
{
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

    if (!state.attempted) {
        initLocked();
    }

    return state.loaded;
}

void shutdown()
{
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

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
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    shutdown();
    initLocked();
}

bool available()
{
    ensureInit();
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    return state.loaded && !state.infos.empty();
}

bool enabled()
{
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

    if (!state.attempted) {
        initLocked();
    }

    return state.loaded && state.stageEnabled && !state.infos.empty();
}

std::vector<DeviceInfo> devices()
{
    ensureInit();
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    return state.infos;
}

int selectedDevice()
{
    ensureInit();
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    return state.selected;
}

static bool resizeLanczosPlanes(const float* const sourcePointers[3],
                                size_t sourceRowPitch, int srcWidth, int srcHeight,
                                float* const destPointers[3], size_t destRowPitch,
                                int dstWidth, int dstHeight, float scale);

bool resizeLanczos(const Imagefloat* src, Imagefloat* dst, float scale)
{
    if (src == nullptr || dst == nullptr
            || src->getWidth() <= 0 || src->getHeight() <= 0
            || dst->getWidth() <= 0 || dst->getHeight() <= 0
            || src->getRowStride() == 0 || dst->getRowStride() == 0) {
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
    if (src == nullptr || dst == nullptr
            || src->W <= 0 || src->H <= 0 || dst->W <= 0 || dst->H <= 0
            || src->L == nullptr || dst->L == nullptr) {
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

static bool resizeLanczosPlanes(const float* const sourcePointers[3],
                                size_t sourceRowPitch, int srcWidth, int srcHeight,
                                float* const destPointers[3], size_t destRowPitch,
                                int dstWidth, int dstHeight, float scale)
{
    if (!(scale > 0.0f) || srcWidth <= 0 || srcHeight <= 0
            || dstWidth <= 0 || dstHeight <= 0) {
        return false;
    }

    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

    if (!ensureInit() || state.infos.empty() || state.selected < 0
            || !state.stageEnabled) {
        return false;
    }

    if (!ensureSession(state)) {
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

    const size_t totalBytes = 3 * (sourceBytes + tempBytes + destBytes);
    if (info.globalMemory > 0 && totalBytes > info.globalMemory * 8 / 10) {
        if (settings != nullptr && settings->verbose) {
            std::fprintf(stderr,
                         "OpenCL: resize needs %zu bytes, device has %llu\n",
                         totalBytes, info.globalMemory);
        }
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
        if (settings != nullptr && settings->verbose && !error.empty()) {
            std::fprintf(stderr, "OpenCL: %s\n", error.c_str());
        }
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

bool transformRotateScale(const Imagefloat* src, Imagefloat* dst,
                          int cx, int cy, int sx, int sy, int oW, int oH,
                          double hscale, double vscale,
                          double cost, double sint, bool highQuality,
                          bool useLog)
{
    if (src == nullptr || dst == nullptr
            || src->getWidth() <= 0 || src->getHeight() <= 0
            || dst->getWidth() <= 0 || dst->getHeight() <= 0
            || src->getRowStride() == 0 || dst->getRowStride() == 0) {
        return false;
    }

    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

    if (!ensureInit() || state.infos.empty() || state.selected < 0
            || !state.stageEnabled) {
        return false;
    }

    if (!ensureSession(state)) {
        return false;
    }

    StageTimer timer("transform", g_getenv("RT_OPENCL_PROFILE") != nullptr);

    const int srcWidth = src->getWidth();
    const int srcHeight = src->getHeight();
    const int dstWidth = dst->getWidth();
    const int dstHeight = dst->getHeight();
    const size_t sourceBytes = static_cast<size_t>(srcWidth) * srcHeight * sizeof(float);
    const size_t destBytes = static_cast<size_t>(dstWidth) * dstHeight * sizeof(float);
    const size_t mapBytes = static_cast<size_t>(dstWidth) * dstHeight * 4 * sizeof(float);
    const DeviceInfo& info = state.infos[state.selected];

    if ((info.maxAllocation > 0 && (sourceBytes > info.maxAllocation
                                    || destBytes > info.maxAllocation
                                    || mapBytes > info.maxAllocation))
            || (info.globalMemory > 0
                && 3 * (sourceBytes + destBytes) + mapBytes
                    > info.globalMemory * 8 / 10)) {
        return false;
    }

    const double w2 = static_cast<double>(oW) / 2.0 - 0.5;
    const double h2 = static_cast<double>(oH) / 2.0 - 0.5;
    const double centerX = cx - w2;
    const double centerY = cy - h2;
    std::vector<float> map(static_cast<size_t>(dstWidth) * dstHeight * 4);

    for (int y = 0; y < dstHeight; ++y) {
        for (int x = 0; x < dstWidth; ++x) {
            const size_t index = (static_cast<size_t>(y) * dstWidth + x) * 4;
            const double xd = hscale * (static_cast<double>(x) + centerX);
            const double yd = vscale * (static_cast<double>(y) + centerY);
            const double dxr = xd * cost - yd * sint;
            const double dyr = xd * sint + yd * cost;
            double dx = dxr + w2;
            double dy = dyr + h2;
            int xc = static_cast<int>(dx);
            dx -= xc;
            xc -= sx;
            int yc = static_cast<int>(dy);
            dy -= yc;
            yc -= sy;
            map[index] = static_cast<float>(xc);
            map[index + 1] = static_cast<float>(yc);
            map[index + 2] = static_cast<float>(dx);
            map[index + 3] = static_cast<float>(dy);
        }
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
    cl_mem destBuffers[3] = {};
    const float* sourcePointers[3] = {src->r(0), src->g(0), src->b(0)};
    float* destPointers[3] = {dst->r(0), dst->g(0), dst->b(0)};
    const size_t origin[3] = {0, 0, 0};
    const size_t sourceRegion[3] = {
        static_cast<size_t>(srcWidth) * sizeof(float),
        static_cast<size_t>(srcHeight), 1};
    const size_t destRegion[3] = {
        static_cast<size_t>(dstWidth) * sizeof(float),
        static_cast<size_t>(dstHeight), 1};

    for (int plane = 0; plane < 3; ++plane) {
        sourceBuffers[plane] = createBuffer(CL_MEM_READ_ONLY, sourceBytes, nullptr);
        destBuffers[plane] = createBuffer(CL_MEM_WRITE_ONLY, destBytes, nullptr);

        if (sourceBuffers[plane] == nullptr || destBuffers[plane] == nullptr) {
            return false;
        }

        if (state.api.EnqueueWriteBufferRect(
                state.queue, sourceBuffers[plane], CL_TRUE, origin, origin,
                sourceRegion, 0, 0, src->getRowStride(), 0,
                sourcePointers[plane], 0, nullptr, nullptr) != CL_SUCCESS) {
            return false;
        }
    }

    cl_mem mapBuffer = createBuffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    mapBytes, map.data());

    if (mapBuffer == nullptr) {
        return false;
    }

    std::string error;
    cl_program program = programFor(state, transformKernelSource, "-cl-std=CL1.2",
                                    error);

    if (program == nullptr) {
        if (settings != nullptr && settings->verbose && !error.empty()) {
            std::fprintf(stderr, "OpenCL: %s\n", error.c_str());
        }
        return false;
    }

    cl_int status = CL_SUCCESS;
    cl_kernel kernel = kernels.add(
        state.api.CreateKernel(program, "rt_transform", &status));

    if (kernel == nullptr || status != CL_SUCCESS) {
        return false;
    }

    const cl_int srcWidthArg = srcWidth;
    const cl_int srcHeightArg = srcHeight;
    const cl_int dstWidthArg = dstWidth;
    const cl_int dstHeightArg = dstHeight;
    const cl_int highQualityArg = highQuality ? 1 : 0;
    const cl_int useLogArg = useLog ? 1 : 0;

    status |= state.api.SetKernelArg(kernel, 0, sizeof(cl_mem), &sourceBuffers[0]);
    status |= state.api.SetKernelArg(kernel, 1, sizeof(cl_mem), &sourceBuffers[1]);
    status |= state.api.SetKernelArg(kernel, 2, sizeof(cl_mem), &sourceBuffers[2]);
    status |= state.api.SetKernelArg(kernel, 3, sizeof(cl_mem), &destBuffers[0]);
    status |= state.api.SetKernelArg(kernel, 4, sizeof(cl_mem), &destBuffers[1]);
    status |= state.api.SetKernelArg(kernel, 5, sizeof(cl_mem), &destBuffers[2]);
    status |= state.api.SetKernelArg(kernel, 6, sizeof(cl_mem), &mapBuffer);
    status |= state.api.SetKernelArg(kernel, 7, sizeof(cl_int), &srcWidthArg);
    status |= state.api.SetKernelArg(kernel, 8, sizeof(cl_int), &srcHeightArg);
    status |= state.api.SetKernelArg(kernel, 9, sizeof(cl_int), &dstWidthArg);
    status |= state.api.SetKernelArg(kernel, 10, sizeof(cl_int), &dstHeightArg);
    status |= state.api.SetKernelArg(kernel, 11, sizeof(cl_int), &highQualityArg);
    status |= state.api.SetKernelArg(kernel, 12, sizeof(cl_int), &useLogArg);

    if (status != CL_SUCCESS) {
        return false;
    }

    const size_t global[2] = {
        static_cast<size_t>(dstWidth), static_cast<size_t>(dstHeight)};

    if (state.api.EnqueueNDRangeKernel(state.queue, kernel, 2, nullptr, global,
                                       nullptr, 0, nullptr, nullptr) != CL_SUCCESS) {
        return false;
    }

    if (state.api.Finish(state.queue) != CL_SUCCESS) {
        return false;
    }

    for (int plane = 0; plane < 3; ++plane) {
        if (state.api.EnqueueReadBufferRect(
                state.queue, destBuffers[plane], CL_TRUE, origin, origin,
                destRegion, 0, 0, dst->getRowStride(), 0,
                destPointers[plane], 0, nullptr, nullptr) != CL_SUCCESS) {
            return false;
        }
    }

    return true;
}

void setStageAllowed(bool allowed)
{
    threadStageAllowed = allowed;
}

bool stageAllowed()
{
    return threadStageAllowed;
}

bool captureSharpening(float** luminance, const float* const* oldLuminance,
                       const float* const* blend, int width, int height,
                       float sigma, float sigmaCornerOffset, int iterations,
                       bool checkIterStop)
{
    if (luminance == nullptr || oldLuminance == nullptr || blend == nullptr
            || width <= 0 || height <= 0 || sigmaCornerOffset != 0.0f
            || iterations <= 0) {
        return false;
    }

    int radius = 0;

    if (sigma < 0.6f) {
        radius = 1;
    } else if (sigma <= 0.84f) {
        radius = 2;
    } else if (sigma <= 1.15f) {
        radius = 3;
    } else if (sigma <= 1.5f) {
        radius = 4;
    } else {
        radius = 6;
    }

    const int border = radius <= 3 ? (iterations <= 30 ? 5 : 7) : 8;
    constexpr int tile = 32;
    const int full = tile + 2 * border;

    if (width < full || height < full) {
        return false;
    }

    const int kernelSize = 2 * radius + 1;
    std::vector<float> flatKernel(static_cast<size_t>(kernelSize) * kernelSize);

    if (radius == 1) {
        float kernel[3][3];
        compute3x3kernel2(sigma, kernel);
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                flatKernel[y * 3 + x] = kernel[y][x];
            }
        }
    } else if (radius == 2) {
        float kernel[5][5];
        compute5x5kernel2(sigma, kernel);
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 5; ++x) {
                flatKernel[y * 5 + x] = kernel[y][x];
            }
        }
    } else if (radius == 3) {
        float kernel[7][7];
        compute7x7kernel2(sigma, kernel);
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                flatKernel[y * 7 + x] = kernel[y][x];
            }
        }
    } else if (radius == 4) {
        float kernel[9][9];
        compute9x9kernel2(sigma, kernel);
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 9; ++x) {
                flatKernel[y * 9 + x] = kernel[y][x];
            }
        }
    } else {
        float kernel[13][13];
        compute13x13kernel2(sigma, kernel);
        for (int y = 0; y < 13; ++y) {
            for (int x = 0; x < 13; ++x) {
                flatKernel[y * 13 + x] = kernel[y][x];
            }
        }
    }

    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

    if (!threadStageAllowed || !ensureInit() || state.infos.empty()
            || state.selected < 0 || !state.stageEnabled
            || !ensureSession(state)) {
        return false;
    }

    const size_t planeBytes = static_cast<size_t>(width) * height * sizeof(float);
    const DeviceInfo& info = state.infos[state.selected];

    // The tiled kernel needs 1024 work-items and about 35 KiB of local memory.
    // Large images also exceed the GPU watchdog on older AMD hardware.
    constexpr unsigned long long maxPixels = 12000000ULL;

    if (static_cast<unsigned long long>(width) * height > maxPixels
            || (info.localMemory > 0 && info.localMemory < 36864ULL)
            || (info.maxWorkGroup > 0 && info.maxWorkGroup < 1024ULL)) {
        return false;
    }

    if ((info.maxAllocation > 0 && planeBytes > info.maxAllocation)
            || (info.globalMemory > 0 && 3 * planeBytes > info.globalMemory * 8 / 10)) {
        return false;
    }

    std::string error;
    cl_program program = programFor(state, captureSharpeningKernelSource,
                                    "-cl-std=CL1.2", error);

    if (program == nullptr) {
        if (settings != nullptr && settings->verbose && !error.empty()) {
            std::fprintf(stderr, "OpenCL: %s\n", error.c_str());
        }
        return false;
    }

    StageTimer timer("capture sharpening",
                     g_getenv("RT_OPENCL_PROFILE") != nullptr);

    std::vector<float> oldPlane(static_cast<size_t>(width) * height);
    std::vector<float> blendPlane(static_cast<size_t>(width) * height);
    std::vector<float> lumPlane(static_cast<size_t>(width) * height);

    for (int y = 0; y < height; ++y) {
        const size_t offset = static_cast<size_t>(y) * width;
        std::memcpy(&oldPlane[offset], oldLuminance[y], width * sizeof(float));
        std::memcpy(&blendPlane[offset], blend[y], width * sizeof(float));
        std::memcpy(&lumPlane[offset], luminance[y], width * sizeof(float));
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

    cl_mem oldBuffer = createBuffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    planeBytes, oldPlane.data());
    cl_mem blendBuffer = createBuffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      planeBytes, blendPlane.data());
    cl_mem lumBuffer = createBuffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                    planeBytes, lumPlane.data());
    cl_mem kernelBuffer = createBuffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       flatKernel.size() * sizeof(float),
                                       flatKernel.data());

    if (oldBuffer == nullptr || blendBuffer == nullptr || lumBuffer == nullptr
            || kernelBuffer == nullptr) {
        return false;
    }

    cl_int status = CL_SUCCESS;
    cl_kernel kernelObject = kernels.add(
        state.api.CreateKernel(program, "rt_capture", &status));

    if (kernelObject == nullptr || status != CL_SUCCESS) {
        return false;
    }

    const cl_int widthArg = width;
    const cl_int heightArg = height;
    const cl_int borderArg = border;
    const cl_int fullArg = full;
    const cl_int iterationsArg = iterations;
    const cl_int checkStopArg = checkIterStop ? 1 : 0;
    const cl_int radiusArg = radius;

    status |= state.api.SetKernelArg(kernelObject, 0, sizeof(cl_mem), &oldBuffer);
    status |= state.api.SetKernelArg(kernelObject, 1, sizeof(cl_mem), &blendBuffer);
    status |= state.api.SetKernelArg(kernelObject, 2, sizeof(cl_mem), &lumBuffer);
    status |= state.api.SetKernelArg(kernelObject, 3, sizeof(cl_mem), &kernelBuffer);
    status |= state.api.SetKernelArg(kernelObject, 4, sizeof(cl_int), &widthArg);
    status |= state.api.SetKernelArg(kernelObject, 5, sizeof(cl_int), &heightArg);
    status |= state.api.SetKernelArg(kernelObject, 6, sizeof(cl_int), &borderArg);
    status |= state.api.SetKernelArg(kernelObject, 7, sizeof(cl_int), &fullArg);
    status |= state.api.SetKernelArg(kernelObject, 8, sizeof(cl_int), &iterationsArg);
    status |= state.api.SetKernelArg(kernelObject, 9, sizeof(cl_int), &checkStopArg);
    status |= state.api.SetKernelArg(kernelObject, 10, sizeof(cl_int), &radiusArg);

    if (status != CL_SUCCESS) {
        return false;
    }

    const int tilesX = (width - 2 * border + tile - 1) / tile;
    const int tilesY = (height - 2 * border + tile - 1) / tile;
    const size_t global[2] = {
        static_cast<size_t>(tilesX) * tile,
        static_cast<size_t>(tilesY) * tile};
    const size_t local[2] = {tile, tile};

    // CPU tiles overlap at the right and bottom edges; run the non-overlapping
    // tiles first, then the overlapping edge phases in the same order.
    for (cl_int phase = 0; phase < 4; ++phase) {
        if (state.api.SetKernelArg(kernelObject, 11, sizeof(cl_int),
                                   &phase) != CL_SUCCESS
                || state.api.EnqueueNDRangeKernel(state.queue, kernelObject,
                                                  2, nullptr, global, local,
                                                  0, nullptr,
                                                  nullptr) != CL_SUCCESS) {
            return false;
        }
    }

    if (state.api.Finish(state.queue) != CL_SUCCESS) {
        return false;
    }

    if (state.api.EnqueueReadBuffer(state.queue, lumBuffer, CL_TRUE, 0,
                                    planeBytes, lumPlane.data(), 0, nullptr,
                                    nullptr) != CL_SUCCESS) {
        return false;
    }

    for (int y = 0; y < height; ++y) {
        std::memcpy(luminance[y], &lumPlane[static_cast<size_t>(y) * width],
                    width * sizeof(float));
    }

    return true;
}

bool selfTest(std::string& report)
{
    report.clear();

    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

    if (!ensureInit()) {
        report = statusText();
        return false;
    }

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
    Backend& state = backend();
    std::lock_guard<std::recursive_mutex> lock(state.mutex);

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
