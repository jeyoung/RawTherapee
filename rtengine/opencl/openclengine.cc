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

#include "rtengine/opencl/openclloader.h"
#include "rtengine/settings.h"

#include <glibmm/miscutils.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <glib/gstdio.h>
#include <memory>
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
    state.deviceIds.clear();
    state.platforms.clear();
    state.infos.clear();
    state.module.reset();
    state.api = Api();
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
    return available() && settings != nullptr && settings->useOpenCL;
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

    const char* source = probeKernelSource;
    cleanup.program = state.api.CreateProgramWithSource(cleanup.context, 1,
                                                        &source, nullptr,
                                                        &error);

    if (!cleanup.program || error != CL_SUCCESS) {
        report = "could not create the test program";
        return false;
    }

    const char* options = "-cl-std=CL1.2";
    error = state.api.BuildProgram(cleanup.program, 1, &device, options,
                                   nullptr, nullptr);

    if (error != CL_SUCCESS) {
        size_t logSize = 0;
        state.api.GetProgramBuildInfo(cleanup.program, device,
                                      CL_PROGRAM_BUILD_LOG, 0, nullptr,
                                      &logSize);
        std::string log(logSize, '\0');
        if (logSize > 0) {
            state.api.GetProgramBuildInfo(cleanup.program, device,
                                          CL_PROGRAM_BUILD_LOG, logSize,
                                          &log[0], nullptr);
        }
        report = "kernel build failed: " + trim(log);
        return false;
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

    report = "buffer round-trip and kernel compilation succeeded on "
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
