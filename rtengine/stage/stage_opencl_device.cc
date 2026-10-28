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

#include "stage_opencl_device.h"

#include "stage_buffer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef RT_HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif

#include <CL/cl.h>

#include <glibmm/error.h>
#include <glibmm/module.h>

#include "rt_math.h"
#include "sleef.h"
#include "stage/algorithms/lab_to_rgb_cl.h"
#include "stage/algorithms/rgb_proc_cl.h"
#include "stage/algorithms/transform_cl.h"
#include "transformmap.h"

/*
 * The ICD health probe asks a platform's questions in a throwaway process, so
 * a vendor that kills that process can be skipped; that needs fork().
 */
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define RT_STAGE_ICD_PROBE_FORK 1
#else
#define RT_STAGE_ICD_PROBE_FORK 0
#endif

#endif // RT_HAVE_OPENCL

namespace rtengine::stage
{

#ifdef RT_HAVE_OPENCL

namespace
{

/*
 * Budget for device-resident buffers. The default fits a pair of
 * full-resolution 45 MP three-plane F32 buffers with room for a resize
 * destination. RT_STAGE_DEVICE_BUDGET_MB overrides it; out-of-range values are
 * clamped to the range and a non-numeric value keeps the default, so a typo
 * can neither exhaust host memory nor silently disable the device path.
 */
constexpr std::size_t DEFAULT_DEVICE_BUDGET_MIB = 1024;
constexpr std::size_t MIN_DEVICE_BUDGET_MIB = 16;
constexpr std::size_t MAX_DEVICE_BUDGET_MIB = 65536;
constexpr std::size_t MIB = 1024 * 1024;

bool env_truthy(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    return std::strcmp(value, "0") != 0;
}

std::size_t device_budget_bytes()
{
    std::size_t mib = DEFAULT_DEVICE_BUDGET_MIB;
    if (const char* value = std::getenv("RT_STAGE_DEVICE_BUDGET_MB")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value) {
            mib = std::min<std::size_t>(
                std::max<std::size_t>(parsed, MIN_DEVICE_BUDGET_MIB),
                MAX_DEVICE_BUDGET_MIB);
        }
    }
    return mib * MIB;
}

/*
 * The ICD loader is resolved once and deliberately leaked: a context may
 * outlive static destructors, and unloading the loader first would leave
 * dangling entry points.
 */
struct Api {
    cl_int (CL_API_CALL *GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*) = nullptr;
    cl_int (CL_API_CALL *GetPlatformInfo)(cl_platform_id, cl_platform_info, size_t, void*, size_t*) = nullptr;
    cl_int (CL_API_CALL *GetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*) = nullptr;
    cl_int (CL_API_CALL *GetDeviceInfo)(cl_device_id, cl_device_info, size_t, void*, size_t*) = nullptr;
    cl_context (CL_API_CALL *CreateContext)(const cl_context_properties*, cl_uint,
                                            const cl_device_id*,
                                            void (CL_CALLBACK*)(const char*, const void*, size_t, void*),
                                            void*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseContext)(cl_context) = nullptr;
    cl_command_queue (CL_API_CALL *CreateCommandQueue)(cl_context, cl_device_id,
                                                       cl_command_queue_properties,
                                                       cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseCommandQueue)(cl_command_queue) = nullptr;
    cl_mem (CL_API_CALL *CreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseMemObject)(cl_mem) = nullptr;
    cl_int (CL_API_CALL *EnqueueWriteBufferRect)(cl_command_queue, cl_mem, cl_bool,
                                                 const size_t*, const size_t*, const size_t*,
                                                 size_t, size_t, size_t, size_t,
                                                 const void*, cl_uint, const cl_event*, cl_event*) = nullptr;
    cl_int (CL_API_CALL *EnqueueReadBufferRect)(cl_command_queue, cl_mem, cl_bool,
                                                const size_t*, const size_t*, const size_t*,
                                                size_t, size_t, size_t, size_t,
                                                void*, cl_uint, const cl_event*, cl_event*) = nullptr;
    cl_int (CL_API_CALL *Finish)(cl_command_queue) = nullptr;
    cl_program (CL_API_CALL *CreateProgramWithSource)(cl_context, cl_uint, const char**,
                                                      const size_t*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*,
                                       void (CL_CALLBACK*)(cl_program, void*), void*) = nullptr;
    cl_int (CL_API_CALL *GetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info,
                                              size_t, void*, size_t*) = nullptr;
    cl_kernel (CL_API_CALL *CreateKernel)(cl_program, const char*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseKernel)(cl_kernel) = nullptr;
    cl_int (CL_API_CALL *ReleaseProgram)(cl_program) = nullptr;
    cl_int (CL_API_CALL *SetKernelArg)(cl_kernel, cl_uint, size_t, const void*) = nullptr;
    cl_int (CL_API_CALL *EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint,
                                               const size_t*, const size_t*, const size_t*,
                                               cl_uint, const cl_event*, cl_event*) = nullptr;
    cl_int (CL_API_CALL *WaitForEvents)(cl_uint, const cl_event*) = nullptr;
    cl_int (CL_API_CALL *GetEventInfo)(cl_event, cl_event_info, size_t, void*, size_t*) = nullptr;
    cl_int (CL_API_CALL *ReleaseEvent)(cl_event) = nullptr;
};

template<typename T>
bool resolve(Glib::Module& module, const char* name, T& target)
{
    void* symbol = nullptr;
    if (!module.get_symbol(name, symbol) || symbol == nullptr) {
        return false;
    }
    target = reinterpret_cast<T>(symbol);
    return true;
}

struct Loader {
    Api api;
    bool loaded = false;
    std::string error;
};

Loader& loader()
{
    static Loader* instance = [] {
        auto* result = new Loader();
        const char* candidates[] = {"libOpenCL.so.1", "libOpenCL.so"};
        for (const char* candidate : candidates) {
            try {
                /*
                 * Deliberately leaked: OpenCL objects created through these
                 * entry points may outlive this loader object.
                 */
                auto* module = new Glib::Module(candidate);
                Api& api = result->api;
                const bool ok =
                    resolve(*module, "clGetPlatformIDs", api.GetPlatformIDs) &&
                    resolve(*module, "clGetPlatformInfo", api.GetPlatformInfo) &&
                    resolve(*module, "clGetDeviceIDs", api.GetDeviceIDs) &&
                    resolve(*module, "clGetDeviceInfo", api.GetDeviceInfo) &&
                    resolve(*module, "clCreateContext", api.CreateContext) &&
                    resolve(*module, "clReleaseContext", api.ReleaseContext) &&
                    resolve(*module, "clCreateCommandQueue", api.CreateCommandQueue) &&
                    resolve(*module, "clReleaseCommandQueue", api.ReleaseCommandQueue) &&
                    resolve(*module, "clCreateBuffer", api.CreateBuffer) &&
                    resolve(*module, "clReleaseMemObject", api.ReleaseMemObject) &&
                    resolve(*module, "clEnqueueWriteBufferRect", api.EnqueueWriteBufferRect) &&
                    resolve(*module, "clEnqueueReadBufferRect", api.EnqueueReadBufferRect) &&
                    resolve(*module, "clFinish", api.Finish) &&
                    resolve(*module, "clCreateProgramWithSource", api.CreateProgramWithSource) &&
                    resolve(*module, "clBuildProgram", api.BuildProgram) &&
                    resolve(*module, "clGetProgramBuildInfo", api.GetProgramBuildInfo) &&
                    resolve(*module, "clCreateKernel", api.CreateKernel) &&
                    resolve(*module, "clReleaseKernel", api.ReleaseKernel) &&
                    resolve(*module, "clReleaseProgram", api.ReleaseProgram) &&
                    resolve(*module, "clSetKernelArg", api.SetKernelArg) &&
                    resolve(*module, "clEnqueueNDRangeKernel", api.EnqueueNDRangeKernel) &&
                    resolve(*module, "clWaitForEvents", api.WaitForEvents) &&
                    resolve(*module, "clGetEventInfo", api.GetEventInfo) &&
                    resolve(*module, "clReleaseEvent", api.ReleaseEvent);
                if (ok) {
                    result->loaded = true;
                    return result;
                }
                result->error = "missing OpenCL entry points";
            } catch (const Glib::Error& error) {
                result->error = error.what();
            }
        }
        return result;
    }();
    return *instance;
}

struct SelectedDevice {
    cl_platform_id platform = nullptr;
    cl_device_id device = nullptr;
    bool cpu = false;
    std::string platform_name;
    std::string device_name;
    std::string driver;
    std::string version;
    unsigned long long global_memory = 0;
};

std::string device_string(const Api& api, cl_device_id device, cl_device_info param)
{
    std::size_t size = 0;
    if (api.GetDeviceInfo(device, param, 0, nullptr, &size) != CL_SUCCESS || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (api.GetDeviceInfo(device, param, size, &value[0], &size) != CL_SUCCESS) {
        return {};
    }
    value.resize(std::strlen(value.c_str()));
    return value;
}

bool enumerate_platforms(const Api& api, std::vector<cl_platform_id>& platforms)
{
    cl_uint platform_count = 0;
    if (api.GetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS ||
        platform_count == 0) {
        return false;
    }
    platforms.resize(platform_count);
    if (api.GetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
        platforms.clear();
        return false;
    }
    return true;
}

/* ------------------------------------------------------ ICD health probe */

/*
 * A broken ICD can take the whole process down inside the ICD loader. With the
 * distribution vendor set (pocl.icd next to rusticl.icd) the Mesa rusticl
 * platform publishes an uninitialised ICD dispatch chain, and the loader tail
 * jumps through a null entry while answering the first platform query (Debian
 * bug #1117930; `clinfo` crashes the same way). That happens before a CL error
 * code exists, so the platform cannot be validated in this address space.
 *
 * The probe therefore asks every platform the questions the selector will ask,
 * one child process per platform, so that a platform which kills or hangs its
 * child can be observed and skipped without hiding the platforms behind it.
 * The parent dispatches only to platforms that answered.
 */

enum class PlatformHealth {
    USABLE,    // answered every probe question
    CRASHED,   // the probe child died before answering
    TIMED_OUT, // the probe child hung and was killed by the alarm
    UNPROBED   // the probe itself could not run
};

/* Upper bound for one probe child, in seconds. A vendor that hangs the query
 * must not hang RawTherapee; the child is killed and its platform skipped. */
constexpr unsigned PROBE_TIMEOUT_SECONDS = 30;

struct PlatformProbe {
    std::vector<PlatformHealth> health;
    /* Set when the probe could not run at all. */
    std::string failure;
};

/*
 * The same calls find_device() makes for a platform. The CL status codes are
 * irrelevant: what is being established is that the call returned at all.
 */
void probe_platform(const Api& api, cl_platform_id platform)
{
    std::size_t size = 0;
    (void)api.GetPlatformInfo(platform, CL_PLATFORM_NAME, 0, nullptr, &size);

    cl_uint device_count = 0;
    if (api.GetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count) !=
            CL_SUCCESS ||
        device_count == 0) {
        return;
    }
    cl_device_id device = nullptr;
    if (api.GetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr) !=
            CL_SUCCESS ||
        device == nullptr) {
        return;
    }
    cl_device_type type = 0;
    (void)api.GetDeviceInfo(device, CL_DEVICE_NAME, 0, nullptr, &size);
    (void)api.GetDeviceInfo(device, CL_DEVICE_TYPE, sizeof(type), &type, nullptr);
    cl_ulong memory = 0;
    (void)api.GetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(memory),
                            &memory, nullptr);
}

/*
 * One child per platform, so a platform that kills its child cannot hide the
 * platforms behind it. The child answers with one byte and is reaped; its exit
 * status and the missing byte tell the parent what happened.
 */
PlatformHealth probe_platform_isolated(const Api& api, cl_platform_id platform)
{
#if RT_STAGE_ICD_PROBE_FORK
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return PlatformHealth::UNPROBED;
    }
    const pid_t child = fork();
    if (child < 0) {
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        return PlatformHealth::UNPROBED;
    }
    if (child == 0) {
        /* Probe child: no stdio, no atexit handlers, no core file, and an
         * alarm so a hung vendor cannot outlive the probe. */
        (void)close(pipe_fds[0]);
        const struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        (void)alarm(PROBE_TIMEOUT_SECONDS);
        probe_platform(api, platform);
        const unsigned char answered = 1;
        (void)write(pipe_fds[1], &answered, sizeof(answered));
        _exit(0);
    }

    (void)close(pipe_fds[1]);
    unsigned char answered = 0;
    ssize_t got = 0;
    do {
        got = read(pipe_fds[0], &answered, sizeof(answered));
    } while (got < 0 && errno == EINTR);
    (void)close(pipe_fds[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    if (got == static_cast<ssize_t>(sizeof(answered))) {
        return PlatformHealth::USABLE;
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
        return PlatformHealth::TIMED_OUT;
    }
    return PlatformHealth::CRASHED;
#else
    /* Without fork() a vendor that kills the process cannot be observed, so
     * query the platform directly. The defect this guards against is in the
     * ocl-icd loader, which these platforms do not use. */
    (void)api;
    (void)platform;
    return PlatformHealth::USABLE;
#endif
}

PlatformProbe probe_platforms(const Api& api,
                              const std::vector<cl_platform_id>& platforms)
{
    PlatformProbe probe;
    probe.health.reserve(platforms.size());
    for (cl_platform_id platform : platforms) {
        const PlatformHealth health = probe_platform_isolated(api, platform);
        if (health == PlatformHealth::UNPROBED && probe.failure.empty()) {
            probe.failure = "the ICD health probe could not start";
        }
        probe.health.push_back(health);
    }
    return probe;
}

std::string describe_skipped_platforms(const PlatformProbe& probe,
                                       std::size_t platform_count)
{
    if (!probe.failure.empty()) {
        return probe.failure + "; the OpenCL device path is disabled";
    }
    std::size_t skipped = 0;
    std::string detail;
    for (std::size_t index = 0; index < probe.health.size(); ++index) {
        std::string reason;
        switch (probe.health[index]) {
        case PlatformHealth::USABLE:
            continue;
        case PlatformHealth::CRASHED:
            reason = "crashed the ICD query";
            break;
        case PlatformHealth::TIMED_OUT:
            reason = "did not answer within " +
                     std::to_string(PROBE_TIMEOUT_SECONDS) + " s";
            break;
        case PlatformHealth::UNPROBED:
            reason = "was not probed";
            break;
        }
        ++skipped;
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += "platform " + std::to_string(index) + " " + reason;
    }
    if (skipped == 0) {
        return {};
    }
    return std::to_string(skipped) + " of " + std::to_string(platform_count) +
           " OpenCL platform(s) skipped: " + detail +
           "; check the installed ICDs (/etc/OpenCL/vendors, or OCL_ICD_VENDORS)";
}

/* Diagnostic about skipped platforms, set once beside the device choice. */
std::string& icd_note()
{
    static std::string note;
    return note;
}

bool find_device(const Api& api, const std::vector<cl_platform_id>& platforms,
                 const std::vector<PlatformHealth>& health, cl_device_type type,
                 bool cpu, SelectedDevice& result)
{
    assert(health.size() == platforms.size());

    for (std::size_t index = 0; index < platforms.size(); ++index) {
        /* A platform the probe could not validate is never dispatched to. */
        if (health[index] != PlatformHealth::USABLE) {
            continue;
        }
        const cl_platform_id platform = platforms[index];
        cl_uint device_count = 0;
        if (api.GetDeviceIDs(platform, type, 0, nullptr, &device_count) != CL_SUCCESS ||
            device_count == 0) {
            continue;
        }
        std::vector<cl_device_id> devices(device_count);
        if (api.GetDeviceIDs(platform, type, device_count, devices.data(), nullptr) !=
            CL_SUCCESS) {
            continue;
        }

        result.platform = platform;
        result.device = devices.front();
        result.cpu = cpu;
        std::size_t name_size = 0;
        if (api.GetPlatformInfo(platform, CL_PLATFORM_NAME, 0, nullptr, &name_size) ==
                CL_SUCCESS &&
            name_size > 0) {
            std::string name(name_size, '\0');
            if (api.GetPlatformInfo(platform, CL_PLATFORM_NAME, name_size, &name[0],
                                    &name_size) == CL_SUCCESS) {
                result.platform_name.assign(name.c_str());
            }
        }
        result.device_name = device_string(api, result.device, CL_DEVICE_NAME);
        result.driver = device_string(api, result.device, CL_DRIVER_VERSION);
        result.version = device_string(api, result.device, CL_DEVICE_VERSION);
        cl_ulong memory = 0;
        if (api.GetDeviceInfo(result.device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(memory),
                              &memory, nullptr) == CL_SUCCESS) {
            result.global_memory = memory;
        }
        return true;
    }
    return false;
}

const SelectedDevice* selected_device(std::string* reason)
{
    static std::once_flag once;
    static SelectedDevice choice;
    static bool available = false;
    static std::string failure;

    std::call_once(once, [] {
        Loader& backend = loader();
        if (!backend.loaded) {
            failure = "OpenCL loader unavailable: " + backend.error;
            return;
        }
        std::vector<cl_platform_id> platforms;
        if (!enumerate_platforms(backend.api, platforms)) {
            failure = "no OpenCL platform";
            return;
        }
        const PlatformProbe probe = probe_platforms(backend.api, platforms);
        icd_note() = describe_skipped_platforms(probe, platforms.size());

        /* A GPU or accelerator is used unconditionally. A CPU device is the
         * last resort and needs the explicit opt-in the classic backend also
         * requires, because it is not an accelerated path. */
        if (find_device(backend.api, platforms, probe.health, CL_DEVICE_TYPE_GPU,
                        false, choice) ||
            find_device(backend.api, platforms, probe.health,
                        CL_DEVICE_TYPE_ACCELERATOR, false, choice)) {
            available = true;
            return;
        }
        if (!env_truthy("RT_OPENCL_ALLOW_CPU")) {
            failure = "no GPU OpenCL device; set RT_OPENCL_ALLOW_CPU=1 to use a "
                      "CPU device";
            return;
        }
        if (find_device(backend.api, platforms, probe.health, CL_DEVICE_TYPE_CPU,
                        true, choice)) {
            available = true;
            return;
        }
        failure = "no OpenCL device";
    });

    if (!available) {
        if (reason != nullptr) {
            *reason = failure;
            if (!icd_note().empty()) {
                *reason += "; " + icd_note();
            }
        }
        return nullptr;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return &choice;
}

/* ------------------------------------------------------------------ kernels */

struct KernelDef {
    const char* name;
    const char* source;
    const char* entry;
};

/*
 * Three-plane Lanczos resize. The host precomputes the same weights the CPU
 * implementation uses, so the kernel only performs the separable dot products
 * in the same order and the two paths agree to within rounding.
 */
const char* const LANCZOS_SOURCE = R"CLC(
__kernel void lanczos3(__global const float* src,
                       __global float* dst,
                       const int dst_width,
                       const int dst_height,
                       const int src_pitch,
                       const int src_plane_pitch,
                       const int dst_pitch,
                       const int dst_plane_pitch,
                       __global const float* wwh,
                       __global const float* wwv,
                       __global const int* jj0,
                       __global const int* jj1,
                       __global const int* ii0,
                       __global const int* ii1,
                       const int support)
{
    const int gid = get_global_id(0);
    const int i = gid / dst_width;
    const int j = gid - i * dst_width;
    if (i >= dst_height) {
        return;
    }

    const int j0 = jj0[j];
    const int j1 = jj1[j];
    const int i0 = ii0[i];
    const int i1 = ii1[i];

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    for (int jj = j0; jj < j1; ++jj) {
        const float wh = wwh[j * support + (jj - j0)];
        float lr = 0.0f;
        float lg = 0.0f;
        float lb = 0.0f;
        for (int ii = i0; ii < i1; ++ii) {
            const float wv = wwv[i * support + (ii - i0)];
            const int o = ii * src_pitch + jj;
            lr += wv * src[o];
            lg += wv * src[src_plane_pitch + o];
            lb += wv * src[2 * src_plane_pitch + o];
        }
        r += wh * lr;
        g += wh * lg;
        b += wh * lb;
    }

    const int od = i * dst_pitch + j;
    dst[od] = r;
    dst[dst_plane_pitch + od] = g;
    dst[2 * dst_plane_pitch + od] = b;
}
)CLC";

const std::string& transform_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n") +
        RT_TRANSFORM_SAMPLING_CL +
        R"CLC(
/*
 * Three-plane rotation/scale with a host-built map: four floats per
 * destination pixel hold the source cell and the fractional offsets. The
 * sampling helpers are shared with the classic backend kernel, so both
 * interpolate identically; only the addressing differs.
 */
__kernel void rt_transform(__global const float* src,
                           __global float* dst,
                           __global const float* map,
                           const int srcWidth, const int srcHeight,
                           const int dstWidth, const int dstHeight,
                           const int srcPitch, const int srcPlanePitch,
                           const int dstPitch, const int dstPlanePitch,
                           const int highQuality, const int useLog)
{
    /* One work item per destination pixel, as the launch uses a 1-D range.
     * The runtime may pad the range to a work-group multiple, so the extra
     * items must return before they index the buffers. */
    const int gid = get_global_id(0);
    const int y = gid / dstWidth;
    if (y >= dstHeight) {
        return;
    }
    const int x = gid - y * dstWidth;

    const int index = y * dstPitch + x;
    const int mapIndex = (y * dstWidth + x) * 4;
    const int xc = (int)map[mapIndex];
    const int yc = (int)map[mapIndex + 1];
    const float dx = map[mapIndex + 2];
    const float dy = map[mapIndex + 3];

    __global const float* input[3] = {
        src, src + srcPlanePitch, src + 2 * srcPlanePitch
    };
    __global float* output[3] = {
        dst, dst + dstPlanePitch, dst + 2 * dstPlanePitch
    };

    if (yc < 0 || yc >= srcHeight || xc < 0 || xc >= srcWidth) {
        output[0][index] = 0.0f;
        output[1][index] = 0.0f;
        output[2][index] = 0.0f;
        return;
    }

    const int interior = yc > 0 && yc < srcHeight - 2
        && xc > 0 && xc < srcWidth - 2;

    for (int plane = 0; plane < 3; ++plane) {
        const float value = (interior && highQuality)
            ? rt_transform_cubic(input[plane], srcPitch, xc - 1, yc - 1, dx, dy)
            : rt_transform_bilinear(input[plane], srcPitch, srcWidth,
                                    srcHeight, xc, yc, dx, dy);
        output[plane][index] = rt_transform_finish(value, useLog);
    }
}
)CLC";
    return source;
}

/*
 * Lab to RGB. The matrix and the 16-bit gamma LUT come from the caller, which
 * builds them from the same ICC profile the classic backend uses; the
 * arithmetic is shared with the classic kernel. The float entry serves
 * output.convert, the packed 8-bit entry serves monitor.output,
 * viewport.output and thumbnail.output.
 */
const std::string& lab_to_float_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n") +
        RT_LAB_TO_RGB_CL +
        R"CLC(
__kernel void rt_lab_to_float(__global const float* src,
                              __global float* dst,
                              __global const float* lut,
                              __global const float* matrix,
                              const int width, const int height,
                              const int srcPitch, const int srcPlanePitch,
                              const int dstPitch, const int dstPlanePitch)
{
    const int gid = get_global_id(0);
    const int y = gid / width;
    if (y >= height) {
        return;
    }
    const int x = gid - y * width;
    const int offset = y * srcPitch + x;
    const float3 rgb = rt_lab_to_linear(src, src + srcPlanePitch,
                                        src + 2 * srcPlanePitch, matrix,
                                        offset);
    const int out = y * dstPitch + x;
    dst[out] = rt_gamma_lookup(lut, rgb.x);
    dst[out + dstPlanePitch] = rt_gamma_lookup(lut, rgb.y);
    dst[out + 2 * dstPlanePitch] = rt_gamma_lookup(lut, rgb.z);
}
)CLC";
    return source;
}

const std::string& lab_to_u8_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n") +
        RT_LAB_TO_RGB_CL +
        R"CLC(
__kernel void rt_lab_to_u8(__global const float* src,
                           __global unsigned char* dst,
                           __global const float* lut,
                           __global const float* matrix,
                           const int width, const int height,
                           const int srcPitch, const int srcPlanePitch,
                           const int dstRowBytes)
{
    const int gid = get_global_id(0);
    const int y = gid / width;
    if (y >= height) {
        return;
    }
    const int x = gid - y * width;
    const int offset = y * srcPitch + x;
    const float3 rgb = rt_lab_to_linear(src, src + srcPlanePitch,
                                        src + 2 * srcPlanePitch, matrix,
                                        offset);
    const int out = y * dstRowBytes + x * 3;
    dst[out] = rt_to_u8(rt_gamma_lookup(lut, rgb.x));
    dst[out + 1] = rt_to_u8(rt_gamma_lookup(lut, rgb.y));
    dst[out + 2] = rt_to_u8(rt_gamma_lookup(lut, rgb.z));
}
)CLC";
    return source;
}

const std::string& rgb_proc_source()
{
    static const std::string source = RT_RGB_PROC_CL;
    return source;
}

/*
 * Thumbnail white balance: a per-pixel multiply with the highlight-average
 * branch. The multipliers and the highlight flag are resolved on the host, so
 * this kernel only reproduces the loop. 65535 is rt_math.h's MAXVALF.
 */
const std::string& thumbnail_wb_source()
{
    static const std::string source = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

__kernel void rt_thumbnail_wb(__global const float* src, __global float* dst,
                              const float rmi, const float gmi,
                              const float bmi, const int hrenabled,
                              const int width, const int height,
                              const int srcPitch, const int srcPlanePitch,
                              const int dstPitch, const int dstPlanePitch)
{
    const float MAX_VALUE = 65535.0f;
    const int gid = get_global_id(0);
    const int y = gid / width;
    if (y >= height) {
        return;
    }
    const int x = gid - y * width;
    const int in = y * srcPitch + x;
    const int out = y * dstPitch + x;

    const float red = src[in] * rmi;
    const float green = src[srcPlanePitch + in] * gmi;
    const float blue = src[2 * srcPlanePitch + in] * bmi;

    if (hrenabled && red > MAX_VALUE && blue > MAX_VALUE) {
        const float value = clamp((red + green + blue) / 3.0f, 0.0f, MAX_VALUE);
        dst[out] = value;
        dst[dstPlanePitch + out] = value;
        dst[2 * dstPlanePitch + out] = value;
    } else {
        dst[out] = clamp(red, 0.0f, MAX_VALUE);
        dst[dstPlanePitch + out] = clamp(green, 0.0f, MAX_VALUE);
        dst[2 * dstPlanePitch + out] = clamp(blue, 0.0f, MAX_VALUE);
    }
}
)CLC";
    return source;
}

const std::vector<KernelDef>& kernels()
{
    static const std::vector<KernelDef> table = {
        {"lanczos3", LANCZOS_SOURCE, "lanczos3"},
        {"transform", transform_source().c_str(), "rt_transform"},
        {"output.convert", lab_to_float_source().c_str(), "rt_lab_to_float"},
        {"monitor.output", lab_to_u8_source().c_str(), "rt_lab_to_u8"},
        {"viewport.output", lab_to_u8_source().c_str(), "rt_lab_to_u8"},
        {"thumbnail.output", lab_to_u8_source().c_str(), "rt_lab_to_u8"},
        {"rgb.proc", rgb_proc_source().c_str(), "rt_rgb_proc"},
        {"thumbnail.whitebalance", thumbnail_wb_source().c_str(), "rt_thumbnail_wb"}
    };
    return table;
}

const KernelDef* find_kernel(const std::string& name)
{
    for (const KernelDef& def : kernels()) {
        if (name == def.name) {
            return &def;
        }
    }
    return nullptr;
}

/* --------------------------------------------------------- host weights */

inline float lanc(float x, float a)
{
    if (x * x < 1e-6f) {
        return 1.0f;
    }
    if (x * x > a * a) {
        return 0.0f;
    }
    x = static_cast<float>(rtengine::RT_PI) * x;
    return a * xsinf(x) * xsinf(x / a) / (x * x);
}

struct LanczosWeights {
    int support = 0;
    std::vector<float> wwh;
    std::vector<float> wwv;
    std::vector<int> jj0;
    std::vector<int> jj1;
    std::vector<int> ii0;
    std::vector<int> ii1;
};

/* Mirrors algorithms::lanczos3() exactly; see stage/algorithms/lanczos.cc. */
bool lanczos_weights(int src_width, int src_height, int dst_width, int dst_height,
                     float scale, LanczosWeights& weights)
{
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0 ||
        !(scale > 0.0f)) {
        return false;
    }

    const float delta = 1.0f / scale;
    constexpr float a = 3.0f;
    const float sc = std::min(scale, 1.0f);
    const int support = static_cast<int>(2.0f * a / sc) + 1;

    weights.support = support;
    weights.wwh.assign(static_cast<std::size_t>(support) * dst_width, 0.0f);
    weights.wwv.assign(static_cast<std::size_t>(support) * dst_height, 0.0f);
    weights.jj0.resize(dst_width);
    weights.jj1.resize(dst_width);
    weights.ii0.resize(dst_height);
    weights.ii1.resize(dst_height);

    for (int j = 0; j < dst_width; ++j) {
        const float x0 = (static_cast<float>(j) + 0.5f) * delta - 0.5f;
        float* const w = weights.wwh.data() + static_cast<std::size_t>(j) * support;
        float ws = 0.0f;
        weights.jj0[j] =
            std::max(0, static_cast<int>(floorf(x0 - a / sc)) + 1);
        weights.jj1[j] =
            std::min(src_width, static_cast<int>(floorf(x0 + a / sc)) + 1);
        for (int jj = weights.jj0[j]; jj < weights.jj1[j]; ++jj) {
            const int k = jj - weights.jj0[j];
            const float z = sc * (x0 - static_cast<float>(jj));
            w[k] = lanc(z, a);
            ws += w[k];
        }
        for (int k = 0; k < support; ++k) {
            w[k] /= ws;
        }
    }

    for (int i = 0; i < dst_height; ++i) {
        const float y0 = (static_cast<float>(i) + 0.5f) * delta - 0.5f;
        float* const w = weights.wwv.data() + static_cast<std::size_t>(i) * support;
        float ws = 0.0f;
        weights.ii0[i] =
            std::max(0, static_cast<int>(floorf(y0 - a / sc)) + 1);
        weights.ii1[i] =
            std::min(src_height, static_cast<int>(floorf(y0 + a / sc)) + 1);
        for (int ii = weights.ii0[i]; ii < weights.ii1[i]; ++ii) {
            const int k = ii - weights.ii0[i];
            const float z = sc * (y0 - static_cast<float>(ii));
            w[k] = lanc(z, a);
            ws += w[k];
        }
        for (int k = 0; k < support; ++k) {
            w[k] /= ws;
        }
    }

    return true;
}

/* --------------------------------------------------------------- OpenCL */

struct OpenCLEnv {
    const Api* api = nullptr;
    cl_device_id device = nullptr;
    cl_context context = nullptr;
    cl_command_queue queue = nullptr;

    /*
     * One in-order queue is shared by the context; serialise host calls.
     * Recursive because wait() holds it while the residency performs the
     * blocking readback on the same queue.
     */
    std::recursive_mutex queue_mutex;
    /* Guards the compiled kernel cache. */
    std::mutex program_mutex;
    std::map<std::string, cl_kernel> kernels;

    ~OpenCLEnv()
    {
        for (auto& entry : kernels) {
            api->ReleaseKernel(entry.second);
        }
        kernels.clear();
        if (queue != nullptr) {
            api->Finish(queue);
            api->ReleaseCommandQueue(queue);
        }
        if (context != nullptr) {
            api->ReleaseContext(context);
        }
    }
};

std::shared_ptr<OpenCLEnv> make_env()
{
    std::string reason;
    const SelectedDevice* selected = selected_device(&reason);
    if (selected == nullptr) {
        return nullptr;
    }

    const Loader& backend = loader();
    cl_int error = CL_SUCCESS;
    auto env = std::make_shared<OpenCLEnv>();
    env->api = &backend.api;
    env->device = selected->device;
    env->context = env->api->CreateContext(nullptr, 1, &env->device, nullptr,
                                           nullptr, &error);
    if (env->context == nullptr) {
        return nullptr;
    }
    env->queue = env->api->CreateCommandQueue(env->context, env->device,
                                              CL_QUEUE_PROFILING_ENABLE, &error);
    if (env->queue == nullptr) {
        env->api->ReleaseContext(env->context);
        env->context = nullptr;
        return nullptr;
    }
    return env;
}

class OpenCLDeviceBuffer final : public StageDeviceBuffer
{
public:
    OpenCLDeviceBuffer(std::shared_ptr<OpenCLEnv> env, cl_mem memory,
                       const StageBufferShape& shape, std::size_t bytes,
                       std::size_t row_pitch, std::size_t plane_pitch)
        : env_(std::move(env)), memory_(memory), shape_(shape), bytes_(bytes),
          row_pitch_(row_pitch), plane_pitch_(plane_pitch)
    {
    }

    ~OpenCLDeviceBuffer() override
    {
        if (memory_ != nullptr) {
            env_->api->ReleaseMemObject(memory_);
        }
    }

    StageDeviceType type() const override
    {
        return StageDeviceType::OPENCL;
    }

    StageBufferShape shape() const override
    {
        return shape_;
    }

    std::size_t bytes() const override
    {
        return bytes_;
    }

    std::size_t row_pitch() const override
    {
        return row_pitch_;
    }

    std::size_t plane_pitch() const override
    {
        return plane_pitch_;
    }

    bool valid() const override
    {
        return memory_ != nullptr && bytes_ > 0;
    }

    cl_mem memory() const
    {
        return memory_;
    }

private:
    std::shared_ptr<OpenCLEnv> env_;
    cl_mem memory_ = nullptr;
    StageBufferShape shape_{};
    std::size_t bytes_ = 0;
    std::size_t row_pitch_ = 0;
    std::size_t plane_pitch_ = 0;
};

bool same_shape(const StageBufferShape& a, const StageBufferShape& b)
{
    return a.format == b.format && a.width == b.width && a.height == b.height &&
           a.planes == b.planes;
}

std::size_t host_row_bytes(const RtStageBuffer& host)
{
    std::size_t row = static_cast<std::size_t>(host.width) *
                      bytes_per_pixel(host.format);
    if (host.format == RT_STAGE_U8 && host.planes == 1) {
        row *= 3;
    }
    return row;
}

class OpenCLResidency final : public StageDeviceResidency
{
public:
    OpenCLResidency(std::shared_ptr<OpenCLEnv> env, std::size_t budget)
        : env_(std::move(env))
    {
        stats_.budget_bytes = budget;
    }

    StageDeviceType type() const override
    {
        return StageDeviceType::OPENCL;
    }

    StageResidencyStats stats() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    bool set_budget(std::size_t bytes) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.budget_bytes = bytes;
        evict_locked(0, nullptr);
        return stats_.used_bytes <= stats_.budget_bytes;
    }

    StageDeviceBufferPtr acquire(const void* key, const StageBufferShape& shape,
                                 std::string* error) override
    {
        const auto fail = [error](const std::string& message) {
            if (error != nullptr) {
                *error = message;
            }
            return StageDeviceBufferPtr{};
        };

        if (key == nullptr) {
            return fail("a device buffer needs a host buffer identity");
        }

        std::size_t row_pitch = 0;
        std::size_t plane_pitch = 0;
        std::size_t bytes = 0;
        std::string layout_error;
        if (!stage_buffer_layout(shape, row_pitch, plane_pitch, bytes,
                                 &layout_error)) {
            return fail(layout_error);
        }

        std::lock_guard<std::mutex> lock(mutex_);

        const auto found = entries_.find(key);
        if (found != entries_.end() && same_shape(found->second.shape, shape)) {
            found->second.last_use = ++clock_;
            ++stats_.reused;
            return found->second.buffer;
        }

        if (bytes > stats_.budget_bytes) {
            ++stats_.refusals;
            return fail("device buffer of " + std::to_string(bytes) +
                        " bytes exceeds the stage budget of " +
                        std::to_string(stats_.budget_bytes) + " bytes");
        }

        /* A binding for the same address with a different shape is stale. */
        if (found != entries_.end()) {
            stats_.used_bytes -= found->second.buffer->bytes();
            entries_.erase(found);
        }

        if (!evict_locked(bytes, key)) {
            ++stats_.refusals;
            return fail("stage device budget exhausted: need " +
                        std::to_string(bytes) + " bytes, " +
                        std::to_string(stats_.budget_bytes - stats_.used_bytes) +
                        " free");
        }

        cl_int error_code = CL_SUCCESS;
        cl_mem memory = env_->api->CreateBuffer(env_->context, CL_MEM_READ_WRITE,
                                                bytes, nullptr, &error_code);
        if (memory == nullptr) {
            ++stats_.refusals;
            return fail("clCreateBuffer failed with error " +
                        std::to_string(error_code));
        }

        auto buffer = std::make_shared<OpenCLDeviceBuffer>(
            env_, memory, shape, bytes, row_pitch, plane_pitch);
        stats_.used_bytes += bytes;
        stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.used_bytes);
        ++stats_.allocations;
        entries_[key] = Entry{shape, buffer, ++clock_};
        return buffer;
    }

    RtStageStatus upload(const StageDeviceBufferPtr& buffer,
                         const RtStageBuffer& host) override
    {
        return transfer(buffer, host, true);
    }

    RtStageStatus readback(const StageDeviceBufferPtr& buffer,
                           const RtStageBuffer& host) override
    {
        return transfer(buffer, host, false);
    }

    StageDeviceBufferPtr find(const void* key) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        return found == entries_.end() ? StageDeviceBufferPtr{}
                                       : found->second.buffer;
    }

    bool release(const void* key) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return false;
        }
        stats_.used_bytes -= found->second.buffer->bytes();
        entries_.erase(found);
        return true;
    }

    void clear() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        stats_.used_bytes = 0;
    }

private:
    struct Entry {
        StageBufferShape shape;
        StageDeviceBufferPtr buffer;
        std::uint64_t last_use = 0;
    };

    /* Evict least-recently-used entries that nobody else holds. */
    bool evict_locked(std::size_t needed, const void* keep)
    {
        while (stats_.used_bytes + needed > stats_.budget_bytes) {
            auto victim = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->first == keep || it->second.buffer.use_count() != 1) {
                    continue;
                }
                if (victim == entries_.end() ||
                    it->second.last_use < victim->second.last_use) {
                    victim = it;
                }
            }
            if (victim == entries_.end()) {
                return false;
            }
            stats_.used_bytes -= victim->second.buffer->bytes();
            ++stats_.evictions;
            entries_.erase(victim);
        }
        return true;
    }

    RtStageStatus transfer(const StageDeviceBufferPtr& buffer,
                           const RtStageBuffer& host, bool to_device)
    {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        if (!device || !device->valid() || host.data == nullptr) {
            return RT_STAGE_INVALID;
        }
        const StageBufferShape shape = device->shape();
        if (!same_shape(shape, StageBufferShape{host.format, host.width,
                                                host.height, host.planes})) {
            return RT_STAGE_INVALID;
        }

        const std::size_t row_bytes = host_row_bytes(host);
        if (row_bytes == 0 || host.row_pitch < row_bytes) {
            return RT_STAGE_INVALID;
        }

        std::lock_guard<std::recursive_mutex> lock(env_->queue_mutex);
        for (std::uint32_t plane = 0; plane < host.planes; ++plane) {
            /*
             * Rect origins are in rows, not bytes: a plane starts at row
             * `plane * height` of the device buffer, while the host plane is
             * addressed by advancing the pointer by its plane pitch. This
             * keeps the copy valid for host views whose pitch differs from the
             * device layout.
             */
            const std::size_t buffer_origin[3] = {
                0, static_cast<std::size_t>(plane) * host.height, 0};
            const std::size_t host_origin[3] = {0, 0, 0};
            const std::size_t region[3] = {
                row_bytes, static_cast<std::size_t>(host.height), 1};
            void* host_plane = static_cast<unsigned char*>(host.data) +
                               static_cast<std::size_t>(plane) * host.plane_pitch;

            const cl_int error = to_device
                ? env_->api->EnqueueWriteBufferRect(
                      env_->queue, device->memory(), CL_TRUE, buffer_origin,
                      host_origin, region, device->row_pitch(), 0,
                      host.row_pitch, 0, host_plane, 0, nullptr, nullptr)
                : env_->api->EnqueueReadBufferRect(
                      env_->queue, device->memory(), CL_TRUE, buffer_origin,
                      host_origin, region, device->row_pitch(), 0,
                      host.row_pitch, 0, host_plane, 0, nullptr, nullptr);
            if (error != CL_SUCCESS) {
                return RT_STAGE_ERROR;
            }
        }
        return RT_STAGE_OK;
    }

    std::shared_ptr<OpenCLEnv> env_;
    mutable std::mutex mutex_;
    std::map<const void*, Entry> entries_;
    StageResidencyStats stats_;
    std::uint64_t clock_ = 0;
};

class OpenCLSubmission final : public StageSubmission
{
public:
    OpenCLSubmission(std::shared_ptr<OpenCLEnv> env, cl_event done,
                     std::vector<cl_mem> scratch,
                     std::shared_ptr<OpenCLResidency> residency,
                     StageDeviceBufferPtr output, RtStageBuffer host_output)
        : env_(std::move(env)), event_(done), scratch_(std::move(scratch)),
          residency_(std::move(residency)), output_(std::move(output)),
          host_output_(host_output),
          begin_(std::chrono::steady_clock::now())
    {
    }

    ~OpenCLSubmission() override
    {
        /*
         * Releasing an event or a buffer does not cancel queued work: the
         * driver keeps both alive until the command completes. An event that
         * was never waited therefore drops its result, which is why callers
         * must wait() before consuming the output.
         */
        for (cl_mem memory : scratch_) {
            env_->api->ReleaseMemObject(memory);
        }
        if (event_ != nullptr) {
            env_->api->ReleaseEvent(event_);
            event_ = nullptr;
        }
    }

    RtStageStatus wait() override
    {
        std::lock_guard<std::recursive_mutex> lock(env_->queue_mutex);
        if (final_) {
            return result_;
        }
        if (event_ != nullptr) {
            const cl_int error = env_->api->WaitForEvents(1, &event_);
            env_->api->ReleaseEvent(event_);
            event_ = nullptr;
            if (error != CL_SUCCESS) {
                return finish(RT_STAGE_ERROR);
            }
        }
        if (output_ && host_output_.data != nullptr) {
            const RtStageStatus status =
                residency_->readback(output_, host_output_);
            if (status != RT_STAGE_OK) {
                return finish(status);
            }
        }
        return finish(RT_STAGE_OK);
    }

    StageEventStatus poll() override
    {
        std::lock_guard<std::recursive_mutex> lock(env_->queue_mutex);
        if (final_) {
            return status_;
        }
        if (event_ != nullptr) {
            cl_int status = CL_QUEUED;
            const cl_int error = env_->api->GetEventInfo(
                event_, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(status),
                &status, nullptr);
            if (error == CL_SUCCESS && status < 0) {
                /* The driver failed the command; the output is not valid. */
                finish(RT_STAGE_ERROR);
                return status_;
            }
            /* A completed kernel still needs the readback that wait()
             * performs, so the event is not complete yet. */
        }
        return status_;
    }

    std::uint64_t elapsed_ns() const override
    {
        return elapsed_ns_.load(std::memory_order_acquire);
    }

private:
    RtStageStatus finish(RtStageStatus result)
    {
        if (final_) {
            return result_;
        }
        final_ = true;
        result_ = result;
        status_ = result == RT_STAGE_OK ? StageEventStatus::Complete
                                        : StageEventStatus::Failed;
        const auto end = std::chrono::steady_clock::now();
        elapsed_ns_.store(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin_)
                .count()));
        return result_;
    }

    std::shared_ptr<OpenCLEnv> env_;
    cl_event event_ = nullptr;
    std::vector<cl_mem> scratch_;
    std::shared_ptr<OpenCLResidency> residency_;
    StageDeviceBufferPtr output_;
    RtStageBuffer host_output_{};
    std::chrono::steady_clock::time_point begin_;
    std::atomic<std::uint64_t> elapsed_ns_{0};
    bool final_ = false;
    StageEventStatus status_ = StageEventStatus::Running;
    RtStageStatus result_ = RT_STAGE_OK;
};

/*
 * Upload the inputs, compute the weights, enqueue the kernel and read the
 * result back in wait(). Returns null with a reason when the node cannot run
 * on the device, so the caller can fall back to the host program.
 */
/*
 * Compile a kernel once per queue and keep it for the context lifetime. The
 * program cache is per environment, so a rebuild of the same entry point does
 * not pay the build again.
 */
cl_kernel compiled_kernel(const std::shared_ptr<OpenCLEnv>& env,
                          const KernelDef* def, std::string& error)
{
    std::lock_guard<std::mutex> lock(env->program_mutex);
    const auto found = env->kernels.find(def->name);
    if (found != env->kernels.end()) {
        return found->second;
    }

    cl_int build_error = CL_SUCCESS;
    const char* source = def->source;
    cl_program program = env->api->CreateProgramWithSource(
        env->context, 1, &source, nullptr, &build_error);
    if (program == nullptr) {
        error = "clCreateProgramWithSource failed";
        return nullptr;
    }

    build_error = env->api->BuildProgram(program, 1, &env->device, nullptr,
                                         nullptr, nullptr);
    if (build_error != CL_SUCCESS) {
        std::size_t log_size = 0;
        env->api->GetProgramBuildInfo(program, env->device, CL_PROGRAM_BUILD_LOG,
                                      0, nullptr, &log_size);
        std::string log(log_size, '\0');
        if (log_size > 0) {
            env->api->GetProgramBuildInfo(program, env->device,
                                          CL_PROGRAM_BUILD_LOG, log_size,
                                          &log[0], nullptr);
        }
        env->api->ReleaseProgram(program);
        error = "OpenCL kernel build failed: " + log;
        return nullptr;
    }

    cl_kernel kernel = env->api->CreateKernel(program, def->entry, &build_error);
    env->api->ReleaseProgram(program);
    if (kernel == nullptr) {
        error = "clCreateKernel failed";
        return nullptr;
    }

    env->kernels[def->name] = kernel;
    return kernel;
}

std::shared_ptr<OpenCLSubmission> launch_lanczos(
    const std::shared_ptr<OpenCLEnv>& env,
    const std::shared_ptr<OpenCLResidency>& residency,
    const std::vector<RtStageArg>& args, std::string& error)
{
    if (args.size() < 4 || args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_BUFFER ||
        args[2].kind != RT_STAGE_ARG_DOUBLE || args[3].kind != RT_STAGE_ARG_INT) {
        error = "lanczos3 expects two buffers, a double scale and an int";
        return nullptr;
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (!valid(src) || !valid(dst) || src.format != RT_STAGE_F32 ||
        dst.format != RT_STAGE_F32 || src.planes != 3 || dst.planes != 3) {
        error = "lanczos3 needs two three-plane F32 buffers";
        return nullptr;
    }
    if (src.data == dst.data) {
        error = "lanczos3 needs distinct source and destination buffers";
        return nullptr;
    }

    const float scale = static_cast<float>(args[2].u.f);
    LanczosWeights weights;
    if (!lanczos_weights(src.width, src.height, dst.width, dst.height, scale,
                         weights)) {
        error = "invalid Lanczos geometry";
        return nullptr;
    }

    const KernelDef* def = find_kernel("lanczos3");
    assert(def != nullptr);

    const StageBufferShape src_shape{RT_STAGE_F32, src.width, src.height, 3};
    const StageBufferShape dst_shape{RT_STAGE_F32, dst.width, dst.height, 3};

    StageDeviceBufferPtr src_device = residency->acquire(src.data, src_shape, &error);
    if (!src_device) {
        return nullptr;
    }
    StageDeviceBufferPtr dst_device = residency->acquire(dst.data, dst_shape, &error);
    if (!dst_device) {
        return nullptr;
    }
    if (residency->upload(src_device, src) != RT_STAGE_OK) {
        error = "device upload failed";
        return nullptr;
    }

    auto device_src = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(src_device);
    auto device_dst = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(dst_device);
    if (!device_src || !device_dst) {
        error = "device buffer type mismatch";
        return nullptr;
    }

    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return nullptr;
    }

    /* Scratch buffers hold the weights and tap bounds for this submission. */
    const auto create_scratch = [&](std::size_t bytes, const void* data,
                                    cl_mem& out) -> bool {
        cl_int scratch_error = CL_SUCCESS;
        out = env->api->CreateBuffer(env->context,
                                     CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     bytes, const_cast<void*>(data),
                                     &scratch_error);
        return out != nullptr;
    };

    std::vector<cl_mem> scratch(6, nullptr);
    const bool scratch_ok =
        create_scratch(weights.wwh.size() * sizeof(float), weights.wwh.data(),
                       scratch[0]) &&
        create_scratch(weights.wwv.size() * sizeof(float), weights.wwv.data(),
                       scratch[1]) &&
        create_scratch(weights.jj0.size() * sizeof(int), weights.jj0.data(),
                       scratch[2]) &&
        create_scratch(weights.jj1.size() * sizeof(int), weights.jj1.data(),
                       scratch[3]) &&
        create_scratch(weights.ii0.size() * sizeof(int), weights.ii0.data(),
                       scratch[4]) &&
        create_scratch(weights.ii1.size() * sizeof(int), weights.ii1.data(),
                       scratch[5]);
    if (!scratch_ok) {
        for (cl_mem memory : scratch) {
            if (memory != nullptr) {
                env->api->ReleaseMemObject(memory);
            }
        }
        error = "device scratch allocation failed";
        return nullptr;
    }

    const cl_int src_pitch =
        static_cast<cl_int>(device_src->row_pitch() / sizeof(float));
    const cl_int src_plane_pitch =
        static_cast<cl_int>(device_src->plane_pitch() / sizeof(float));
    const cl_int dst_pitch =
        static_cast<cl_int>(device_dst->row_pitch() / sizeof(float));
    const cl_int dst_plane_pitch =
        static_cast<cl_int>(device_dst->plane_pitch() / sizeof(float));
    const cl_int dst_width = dst.width;
    const cl_int dst_height = dst.height;
    const cl_int support = weights.support;

    const auto set_arg = [&](cl_uint index, std::size_t bytes, const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    cl_mem src_memory = device_src->memory();
    cl_mem dst_memory = device_dst->memory();
    const bool args_ok =
        set_arg(0, sizeof(cl_mem), &src_memory) &&
        set_arg(1, sizeof(cl_mem), &dst_memory) &&
        set_arg(2, sizeof(cl_int), &dst_width) &&
        set_arg(3, sizeof(cl_int), &dst_height) &&
        set_arg(4, sizeof(cl_int), &src_pitch) &&
        set_arg(5, sizeof(cl_int), &src_plane_pitch) &&
        set_arg(6, sizeof(cl_int), &dst_pitch) &&
        set_arg(7, sizeof(cl_int), &dst_plane_pitch) &&
        set_arg(8, sizeof(cl_mem), &scratch[0]) &&
        set_arg(9, sizeof(cl_mem), &scratch[1]) &&
        set_arg(10, sizeof(cl_mem), &scratch[2]) &&
        set_arg(11, sizeof(cl_mem), &scratch[3]) &&
        set_arg(12, sizeof(cl_mem), &scratch[4]) &&
        set_arg(13, sizeof(cl_mem), &scratch[5]) &&
        set_arg(14, sizeof(cl_int), &support);
    if (!args_ok) {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        error = "clSetKernelArg failed";
        return nullptr;
    }

    const std::size_t global =
        static_cast<std::size_t>(dst.width) * static_cast<std::size_t>(dst.height);
    cl_event done = nullptr;
    const cl_int enqueue_error = env->api->EnqueueNDRangeKernel(
        env->queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr, &done);
    if (enqueue_error != CL_SUCCESS || done == nullptr) {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        error = "clEnqueueNDRangeKernel failed with error " +
                std::to_string(enqueue_error);
        return nullptr;
    }

    return std::make_shared<OpenCLSubmission>(env, done, std::move(scratch),
                                              residency, dst_device, dst);
}


/*
 * Rotation and scale. Argument order:
 *   [0] source buffer, [1] destination buffer,
 *   [2..9] ints cx, cy, sx, sy, oW, oH, highQuality, useLog,
 *   [10..13] doubles hscale, vscale, cost, sint.
 *
 * The map is built on the host in double precision with the same builder the
 * classic backend uses, and travels as a scratch buffer for this submission.
 */
std::shared_ptr<OpenCLSubmission> launch_transform(
    const std::shared_ptr<OpenCLEnv>& env,
    const std::shared_ptr<OpenCLResidency>& residency,
    const std::vector<RtStageArg>& args, std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 2;
    constexpr std::size_t INT_ARGS = 8;
    constexpr std::size_t DOUBLE_ARGS = 4;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + INT_ARGS + DOUBLE_ARGS;

    if (args.size() != ARG_COUNT || args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_BUFFER) {
        error = "transform expects two buffers, eight ints and four doubles";
        return nullptr;
    }
    for (std::size_t i = BUFFER_ARGS; i < BUFFER_ARGS + INT_ARGS; ++i) {
        if (args[i].kind != RT_STAGE_ARG_INT) {
            error = "transform argument " + std::to_string(i) + " is not an int";
            return nullptr;
        }
    }
    for (std::size_t i = BUFFER_ARGS + INT_ARGS; i < ARG_COUNT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_DOUBLE) {
            error = "transform argument " + std::to_string(i) +
                    " is not a double";
            return nullptr;
        }
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (!valid(src) || !valid(dst) || src.format != RT_STAGE_F32 ||
        dst.format != RT_STAGE_F32 || src.planes != 3 || dst.planes != 3) {
        error = "transform needs two three-plane F32 buffers";
        return nullptr;
    }
    if (src.data == dst.data) {
        error = "transform needs distinct source and destination buffers";
        return nullptr;
    }

    TransformGeometry geometry;
    geometry.cx = args[2].u.i;
    geometry.cy = args[3].u.i;
    geometry.sx = args[4].u.i;
    geometry.sy = args[5].u.i;
    geometry.oW = args[6].u.i;
    geometry.oH = args[7].u.i;
    geometry.hscale = args[10].u.f;
    geometry.vscale = args[11].u.f;
    geometry.cost = args[12].u.f;
    geometry.sint = args[13].u.f;
    const bool high_quality = args[8].u.i != 0;
    const bool use_log = args[9].u.i != 0;

    std::vector<float> map;
    buildTransformMap(geometry, dst.width, dst.height, map);

    const KernelDef* def = find_kernel("transform");
    assert(def != nullptr);

    const StageBufferShape src_shape{RT_STAGE_F32, src.width, src.height, 3};
    const StageBufferShape dst_shape{RT_STAGE_F32, dst.width, dst.height, 3};

    StageDeviceBufferPtr src_device =
        residency->acquire(src.data, src_shape, &error);
    if (!src_device) {
        return nullptr;
    }
    StageDeviceBufferPtr dst_device =
        residency->acquire(dst.data, dst_shape, &error);
    if (!dst_device) {
        return nullptr;
    }
    if (residency->upload(src_device, src) != RT_STAGE_OK) {
        error = "device upload failed";
        return nullptr;
    }

    auto device_src = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(src_device);
    auto device_dst = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(dst_device);
    if (!device_src || !device_dst) {
        error = "device buffer type mismatch";
        return nullptr;
    }

    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return nullptr;
    }

    cl_int map_error = CL_SUCCESS;
    cl_mem map_memory = env->api->CreateBuffer(
        env->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        map.size() * sizeof(float), map.data(), &map_error);
    if (map_memory == nullptr || map_error != CL_SUCCESS) {
        if (map_memory != nullptr) {
            env->api->ReleaseMemObject(map_memory);
        }
        error = "device map allocation failed";
        return nullptr;
    }

    std::vector<cl_mem> scratch{map_memory};

    const cl_int src_width = src.width;
    const cl_int src_height = src.height;
    const cl_int dst_width = dst.width;
    const cl_int dst_height = dst.height;
    const cl_int src_pitch =
        static_cast<cl_int>(device_src->row_pitch() / sizeof(float));
    const cl_int src_plane_pitch =
        static_cast<cl_int>(device_src->plane_pitch() / sizeof(float));
    const cl_int dst_pitch =
        static_cast<cl_int>(device_dst->row_pitch() / sizeof(float));
    const cl_int dst_plane_pitch =
        static_cast<cl_int>(device_dst->plane_pitch() / sizeof(float));
    const cl_int high_quality_arg = high_quality ? 1 : 0;
    const cl_int use_log_arg = use_log ? 1 : 0;

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    cl_mem src_memory = device_src->memory();
    cl_mem dst_memory = device_dst->memory();
    const bool args_ok =
        set_arg(0, sizeof(cl_mem), &src_memory) &&
        set_arg(1, sizeof(cl_mem), &dst_memory) &&
        set_arg(2, sizeof(cl_mem), &map_memory) &&
        set_arg(3, sizeof(cl_int), &src_width) &&
        set_arg(4, sizeof(cl_int), &src_height) &&
        set_arg(5, sizeof(cl_int), &dst_width) &&
        set_arg(6, sizeof(cl_int), &dst_height) &&
        set_arg(7, sizeof(cl_int), &src_pitch) &&
        set_arg(8, sizeof(cl_int), &src_plane_pitch) &&
        set_arg(9, sizeof(cl_int), &dst_pitch) &&
        set_arg(10, sizeof(cl_int), &dst_plane_pitch) &&
        set_arg(11, sizeof(cl_int), &high_quality_arg) &&
        set_arg(12, sizeof(cl_int), &use_log_arg);
    if (!args_ok) {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        error = "clSetKernelArg failed";
        return nullptr;
    }

    const std::size_t global =
        static_cast<std::size_t>(dst.width) * static_cast<std::size_t>(dst.height);
    cl_event done = nullptr;
    const cl_int enqueue_error = env->api->EnqueueNDRangeKernel(
        env->queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr, &done);
    if (enqueue_error != CL_SUCCESS || done == nullptr) {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        error = "clEnqueueNDRangeKernel failed with error " +
                std::to_string(enqueue_error);
        return nullptr;
    }

    return std::make_shared<OpenCLSubmission>(env, done, std::move(scratch),
                                              residency, dst_device, dst);
}


/*
 * Lab to RGB for one node. Argument order:
 *   [0] source Lab buffer, [1] destination buffer,
 *   [2] gamma LUT buffer (65536 F32 samples),
 *   [3] matrix buffer (9 F32 entries).
 *
 * output.convert takes a three-plane F32 destination; the monitor, viewport
 * and thumbnail nodes take a packed U8 destination. The source may be a
 * sub-rectangle view of a larger Lab image: its pitches describe the parent,
 * so the kernel addresses it exactly as the caller laid it out.
 */
std::shared_ptr<OpenCLSubmission> launch_lab_to_rgb(
    const std::shared_ptr<OpenCLEnv>& env,
    const std::shared_ptr<OpenCLResidency>& residency,
    const std::vector<RtStageArg>& args, const char* node, bool eight_bit,
    std::string& error)
{
    constexpr std::size_t ARG_COUNT = 4;
    constexpr int GAMMA_LUT_ENTRIES = 65536;
    constexpr int MATRIX_ENTRIES = 9;

    if (args.size() != ARG_COUNT) {
        error = "Lab to RGB expects four buffers";
        return nullptr;
    }
    for (std::size_t i = 0; i < ARG_COUNT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_BUFFER) {
            error = "Lab to RGB expects four buffers";
            return nullptr;
        }
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    const RtStageBuffer& lut = args[2].u.buffer;
    const RtStageBuffer& matrix = args[3].u.buffer;

    if (!valid(src) || !valid(dst) || !valid(lut) || !valid(matrix) ||
        src.format != RT_STAGE_F32 || src.planes != 3 ||
        lut.format != RT_STAGE_F32 || lut.planes != 1 ||
        matrix.format != RT_STAGE_F32 || matrix.planes != 1) {
        error = "Lab to RGB needs a three-plane F32 source and F32 tables";
        return nullptr;
    }
    if (lut.width * lut.height != GAMMA_LUT_ENTRIES ||
        matrix.width * matrix.height != MATRIX_ENTRIES) {
        error = "Lab to RGB table sizes are wrong";
        return nullptr;
    }
    if (eight_bit) {
        if (dst.format != RT_STAGE_U8 || dst.planes != 1) {
            error = "the 8-bit output node needs a packed U8 destination";
            return nullptr;
        }
    } else if (dst.format != RT_STAGE_F32 || dst.planes != 3) {
        error = "output.convert needs a three-plane F32 destination";
        return nullptr;
    }
    if (src.data == dst.data) {
        error = "Lab to RGB needs distinct source and destination buffers";
        return nullptr;
    }
    if (dst.width != src.width || dst.height != src.height) {
        error = "Lab to RGB source and destination sizes differ";
        return nullptr;
    }

    const KernelDef* def = find_kernel(node);
    assert(def != nullptr);

    const StageBufferShape src_shape{RT_STAGE_F32, src.width, src.height, 3};
    const StageBufferShape dst_shape = eight_bit
        ? StageBufferShape{RT_STAGE_U8, dst.width, dst.height, 1}
        : StageBufferShape{RT_STAGE_F32, dst.width, dst.height, 3};
    const StageBufferShape lut_shape{RT_STAGE_F32, lut.width, lut.height, 1};
    const StageBufferShape matrix_shape{RT_STAGE_F32, matrix.width,
                                        matrix.height, 1};

    StageDeviceBufferPtr src_device =
        residency->acquire(src.data, src_shape, &error);
    StageDeviceBufferPtr dst_device =
        residency->acquire(dst.data, dst_shape, &error);
    StageDeviceBufferPtr lut_device =
        residency->acquire(lut.data, lut_shape, &error);
    StageDeviceBufferPtr matrix_device =
        residency->acquire(matrix.data, matrix_shape, &error);
    if (!src_device || !dst_device || !lut_device || !matrix_device) {
        return nullptr;
    }
    if (residency->upload(src_device, src) != RT_STAGE_OK ||
        residency->upload(lut_device, lut) != RT_STAGE_OK ||
        residency->upload(matrix_device, matrix) != RT_STAGE_OK) {
        error = "device upload failed";
        return nullptr;
    }

    auto device_src = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(src_device);
    auto device_dst = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(dst_device);
    auto device_lut = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(lut_device);
    auto device_matrix =
        std::dynamic_pointer_cast<OpenCLDeviceBuffer>(matrix_device);
    if (!device_src || !device_dst || !device_lut || !device_matrix) {
        error = "device buffer type mismatch";
        return nullptr;
    }

    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return nullptr;
    }

    const cl_int width = src.width;
    const cl_int height = src.height;
    const cl_int src_pitch =
        static_cast<cl_int>(device_src->row_pitch() / sizeof(float));
    const cl_int src_plane_pitch =
        static_cast<cl_int>(device_src->plane_pitch() / sizeof(float));
    const cl_int dst_pitch =
        static_cast<cl_int>(device_dst->row_pitch() / sizeof(float));
    const cl_int dst_plane_pitch =
        static_cast<cl_int>(device_dst->plane_pitch() / sizeof(float));
    const cl_int dst_row_bytes = static_cast<cl_int>(device_dst->row_pitch());

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    cl_mem src_memory = device_src->memory();
    cl_mem dst_memory = device_dst->memory();
    cl_mem lut_memory = device_lut->memory();
    cl_mem matrix_memory = device_matrix->memory();

    bool args_ok =
        set_arg(0, sizeof(cl_mem), &src_memory) &&
        set_arg(1, sizeof(cl_mem), &dst_memory) &&
        set_arg(2, sizeof(cl_mem), &lut_memory) &&
        set_arg(3, sizeof(cl_mem), &matrix_memory) &&
        set_arg(4, sizeof(cl_int), &width) &&
        set_arg(5, sizeof(cl_int), &height) &&
        set_arg(6, sizeof(cl_int), &src_pitch) &&
        set_arg(7, sizeof(cl_int), &src_plane_pitch);
    if (args_ok) {
        if (eight_bit) {
            args_ok = set_arg(8, sizeof(cl_int), &dst_row_bytes);
        } else {
            args_ok = set_arg(8, sizeof(cl_int), &dst_pitch) &&
                      set_arg(9, sizeof(cl_int), &dst_plane_pitch);
        }
    }
    if (!args_ok) {
        error = "clSetKernelArg failed";
        return nullptr;
    }

    const std::size_t global =
        static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.height);
    cl_event done = nullptr;
    const cl_int enqueue_error = env->api->EnqueueNDRangeKernel(
        env->queue, kernel, 1, nullptr, &global, nullptr, 0, nullptr, &done);
    if (enqueue_error != CL_SUCCESS || done == nullptr) {
        error = "clEnqueueNDRangeKernel failed with error " +
                std::to_string(enqueue_error);
        return nullptr;
    }

    return std::make_shared<OpenCLSubmission>(env, done, std::vector<cl_mem>{},
                                              residency, dst_device, dst);
}


/* A single-plane view of one plane inside a pitched image buffer. */
RtStageBuffer plane_view(const RtStageBuffer& image, std::uint32_t plane)
{
    RtStageBuffer view = image;
    view.data = static_cast<unsigned char*>(image.data) +
                static_cast<std::size_t>(plane) * image.plane_pitch;
    view.planes = 1;
    return view;
}

/*
 * RGB tone and Lab conversion. Argument order:
 *   [0] source RGB buffer, [1] destination Lab buffer,
 *   [2] highlight LUT, [3] shadow LUT, [4] tone LUT,
 *   [5] red curve, [6] green curve, [7] blue curve,
 *   [8] cachef, [9] cachefy, [10] channel mixer matrix, [11] XYZ matrix,
 *   [12..20] ints width, height, toneCurveBlack, clampOog, mixChannels,
 *            rgbCurveMask and doubles expScale, comp, hlRange.
 *
 * A LUT is 65536 samples; an absent RGB curve may be a one-sample dummy
 * because rgbCurveMask stops the kernel reading it. The matrix tables hold
 * nine floats. The kernel indexes compact planes, so each plane is uploaded
 * into its own device allocation and the three destinations are read back
 * here; the submission is therefore complete when it is returned.
 */
RtStageStatus run_rgb_proc(const std::shared_ptr<OpenCLEnv>& env,
                           const std::shared_ptr<OpenCLResidency>& residency,
                           const std::vector<RtStageArg>& args,
                           std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 12;
    constexpr std::size_t INT_ARGS = 6;
    constexpr std::size_t DOUBLE_ARGS = 3;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + INT_ARGS + DOUBLE_ARGS;
    constexpr int LUT_ENTRIES = 65536;
    constexpr int MATRIX_ENTRIES = 9;

    if (args.size() != ARG_COUNT) {
        error = "rgb.proc expects twelve buffers, six ints and three doubles";
        return RT_STAGE_INVALID;
    }
    for (std::size_t i = 0; i < BUFFER_ARGS; ++i) {
        if (args[i].kind != RT_STAGE_ARG_BUFFER) {
            error = "rgb.proc expects twelve buffers";
            return RT_STAGE_INVALID;
        }
    }
    for (std::size_t i = BUFFER_ARGS; i < BUFFER_ARGS + INT_ARGS; ++i) {
        if (args[i].kind != RT_STAGE_ARG_INT) {
            error = "rgb.proc has a non-int scalar";
            return RT_STAGE_INVALID;
        }
    }
    for (std::size_t i = BUFFER_ARGS + INT_ARGS; i < ARG_COUNT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_DOUBLE) {
            error = "rgb.proc has a non-double scalar";
            return RT_STAGE_INVALID;
        }
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (!valid(src) || !valid(dst) || src.format != RT_STAGE_F32 ||
        dst.format != RT_STAGE_F32 || src.planes != 3 || dst.planes != 3) {
        error = "rgb.proc needs two three-plane F32 buffers";
        return RT_STAGE_INVALID;
    }
    if (src.data == dst.data || src.width != dst.width ||
        src.height != dst.height) {
        error = "rgb.proc source and destination do not match";
        return RT_STAGE_INVALID;
    }

    for (std::size_t i = 2; i < BUFFER_ARGS; ++i) {
        const RtStageBuffer& table = args[i].u.buffer;
        if (!valid(table) || table.format != RT_STAGE_F32 ||
            table.planes != 1) {
            error = "rgb.proc needs F32 table buffers";
            return RT_STAGE_INVALID;
        }
        const int entries = table.width * table.height;
        const bool lut = i <= 9;
        if (lut && entries != LUT_ENTRIES && entries != 1) {
            error = "rgb.proc LUT sizes are wrong";
            return RT_STAGE_INVALID;
        }
        if (!lut && entries != MATRIX_ENTRIES) {
            error = "rgb.proc matrix sizes are wrong";
            return RT_STAGE_INVALID;
        }
    }

    const KernelDef* def = find_kernel("rgb.proc");
    assert(def != nullptr);
    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const StageBufferShape plane_shape{RT_STAGE_F32, src.width, src.height, 1};
    StageDeviceBufferPtr src_devices[3];
    StageDeviceBufferPtr dst_devices[3];
    for (int plane = 0; plane < 3; ++plane) {
        const RtStageBuffer src_plane = plane_view(src, plane);
        const RtStageBuffer dst_plane = plane_view(dst, plane);
        src_devices[plane] =
            residency->acquire(src_plane.data, plane_shape, &error);
        dst_devices[plane] =
            residency->acquire(dst_plane.data, plane_shape, &error);
        if (!src_devices[plane] || !dst_devices[plane]) {
            return RT_STAGE_FALLBACK;
        }
        if (residency->upload(src_devices[plane], src_plane) != RT_STAGE_OK) {
            error = "rgb.proc source upload failed";
            return RT_STAGE_FALLBACK;
        }
    }

    StageDeviceBufferPtr tables[BUFFER_ARGS - 2];
    for (std::size_t i = 2; i < BUFFER_ARGS; ++i) {
        const RtStageBuffer& table = args[i].u.buffer;
        const StageBufferShape shape{RT_STAGE_F32, table.width, table.height,
                                     1};
        tables[i - 2] = residency->acquire(table.data, shape, &error);
        if (!tables[i - 2] ||
            residency->upload(tables[i - 2], table) != RT_STAGE_OK) {
            error = "rgb.proc table upload failed";
            return RT_STAGE_FALLBACK;
        }
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const cl_int width = src.width;
    const cl_int height = src.height;
    const cl_int tone_curve_black = static_cast<cl_int>(args[14].u.i);
    const cl_int clamp_oog = static_cast<cl_int>(args[15].u.i);
    const cl_int mix_channels = static_cast<cl_int>(args[16].u.i);
    const cl_int rgb_curve_mask = static_cast<cl_int>(args[17].u.i);
    const cl_float exp_scale = static_cast<cl_float>(args[18].u.f);
    const cl_float comp = static_cast<cl_float>(args[19].u.f);
    const cl_float hl_range = static_cast<cl_float>(args[20].u.f);

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    cl_uint arg = 0;
    bool ok = true;
    for (int plane = 0; plane < 3 && ok; ++plane) {
        cl_mem memory = memory_of(src_devices[plane]);
        ok = set_arg(arg++, sizeof(cl_mem), &memory);
    }
    for (int plane = 0; plane < 3 && ok; ++plane) {
        cl_mem memory = memory_of(dst_devices[plane]);
        ok = set_arg(arg++, sizeof(cl_mem), &memory);
    }
    for (std::size_t i = 0; i < BUFFER_ARGS - 2 && ok; ++i) {
        cl_mem memory = memory_of(tables[i]);
        ok = set_arg(arg++, sizeof(cl_mem), &memory);
    }
    ok = ok &&
         set_arg(arg++, sizeof(cl_int), &width) &&
         set_arg(arg++, sizeof(cl_int), &height) &&
         set_arg(arg++, sizeof(cl_float), &exp_scale) &&
         set_arg(arg++, sizeof(cl_float), &comp) &&
         set_arg(arg++, sizeof(cl_float), &hl_range) &&
         set_arg(arg++, sizeof(cl_int), &tone_curve_black) &&
         set_arg(arg++, sizeof(cl_int), &clamp_oog) &&
         set_arg(arg++, sizeof(cl_int), &mix_channels) &&
         set_arg(arg++, sizeof(cl_int), &rgb_curve_mask);
    if (!ok) {
        error = "clSetKernelArg failed";
        return RT_STAGE_FALLBACK;
    }

    /* The shared kernel derives x and y from a two-dimensional range. */
    const std::size_t global[2] = {static_cast<std::size_t>(width),
                                   static_cast<std::size_t>(height)};
    cl_event done = nullptr;
    if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr, global,
                                       nullptr, 0, nullptr,
                                       &done) != CL_SUCCESS ||
        done == nullptr) {
        error = "rgb.proc launch failed";
        return RT_STAGE_ERROR;
    }

    const cl_int wait_status = env->api->WaitForEvents(1, &done);
    env->api->ReleaseEvent(done);
    if (wait_status != CL_SUCCESS) {
        error = "rgb.proc did not complete";
        return RT_STAGE_ERROR;
    }

    for (int plane = 0; plane < 3; ++plane) {
        if (residency->readback(dst_devices[plane], plane_view(dst, plane)) !=
            RT_STAGE_OK) {
            error = "rgb.proc readback failed";
            return RT_STAGE_ERROR;
        }
    }

    return RT_STAGE_OK;
}


/*
 * Thumbnail white balance. Arguments:
 *   [0] image buffer, [1] doubles rmi, gmi, bmi, [2] ints hrenabled, width,
 *   height. The image may be in place: source and destination share a key.
 * The three planes are read back after a wait, so the node reports a
 * completed device event.
 */
RtStageStatus run_thumbnail_wb(const std::shared_ptr<OpenCLEnv>& env,
                               const std::shared_ptr<OpenCLResidency>& residency,
                               const std::vector<RtStageArg>& args,
                               std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 1;
    constexpr std::size_t DOUBLE_ARGS = 3;
    constexpr std::size_t INT_ARGS = 3;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + DOUBLE_ARGS + INT_ARGS;

    if (args.size() != ARG_COUNT || args[0].kind != RT_STAGE_ARG_BUFFER) {
        error = "thumbnail.whitebalance expects a buffer, three doubles and "
                "three ints";
        return RT_STAGE_INVALID;
    }
    for (std::size_t i = 1; i <= DOUBLE_ARGS; ++i) {
        if (args[i].kind != RT_STAGE_ARG_DOUBLE) {
            error = "thumbnail.whitebalance has a non-double multiplier";
            return RT_STAGE_INVALID;
        }
    }
    for (std::size_t i = 1 + DOUBLE_ARGS; i < ARG_COUNT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_INT) {
            error = "thumbnail.whitebalance has a non-int scalar";
            return RT_STAGE_INVALID;
        }
    }

    const RtStageBuffer& image = args[0].u.buffer;
    if (!valid(image) || image.format != RT_STAGE_F32 || image.planes != 3) {
        error = "thumbnail.whitebalance needs a three-plane F32 buffer";
        return RT_STAGE_INVALID;
    }

    const KernelDef* def = find_kernel("thumbnail.whitebalance");
    assert(def != nullptr);
    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const StageBufferShape shape{RT_STAGE_F32, image.width, image.height, 1};
    StageDeviceBufferPtr planes[3];
    for (int plane = 0; plane < 3; ++plane) {
        const RtStageBuffer view = plane_view(image, plane);
        planes[plane] = residency->acquire(view.data, shape, &error);
        if (!planes[plane]) {
            return RT_STAGE_FALLBACK;
        }
        if (residency->upload(planes[plane], view) != RT_STAGE_OK) {
            error = "thumbnail.whitebalance upload failed";
            return RT_STAGE_FALLBACK;
        }
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(planes[0]);
    if (!device) {
        error = "thumbnail.whitebalance device buffer mismatch";
        return RT_STAGE_FALLBACK;
    }
    const cl_int width = image.width;
    const cl_int height = image.height;
    const cl_int pitch = static_cast<cl_int>(device->row_pitch() / sizeof(float));
    const cl_int plane_pitch =
        static_cast<cl_int>(device->plane_pitch() / sizeof(float));
    const cl_float rmi = static_cast<cl_float>(args[1].u.f);
    const cl_float gmi = static_cast<cl_float>(args[2].u.f);
    const cl_float bmi = static_cast<cl_float>(args[3].u.f);
    const cl_int hrenabled = static_cast<cl_int>(args[4].u.i);

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    cl_mem memory = memory_of(planes[0]);
    const bool args_ok =
        set_arg(0, sizeof(cl_mem), &memory) &&
        set_arg(1, sizeof(cl_mem), &memory) &&
        set_arg(2, sizeof(cl_float), &rmi) &&
        set_arg(3, sizeof(cl_float), &gmi) &&
        set_arg(4, sizeof(cl_float), &bmi) &&
        set_arg(5, sizeof(cl_int), &hrenabled) &&
        set_arg(6, sizeof(cl_int), &width) &&
        set_arg(7, sizeof(cl_int), &height) &&
        set_arg(8, sizeof(cl_int), &pitch) &&
        set_arg(9, sizeof(cl_int), &plane_pitch) &&
        set_arg(10, sizeof(cl_int), &pitch) &&
        set_arg(11, sizeof(cl_int), &plane_pitch);
    if (!args_ok) {
        error = "clSetKernelArg failed";
        return RT_STAGE_FALLBACK;
    }

    const std::size_t global =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    cl_event done = nullptr;
    if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 1, nullptr, &global,
                                       nullptr, 0, nullptr,
                                       &done) != CL_SUCCESS ||
        done == nullptr) {
        error = "thumbnail.whitebalance launch failed";
        return RT_STAGE_ERROR;
    }
    const cl_int wait_status = env->api->WaitForEvents(1, &done);
    env->api->ReleaseEvent(done);
    if (wait_status != CL_SUCCESS) {
        error = "thumbnail.whitebalance did not complete";
        return RT_STAGE_ERROR;
    }

    for (int plane = 0; plane < 3; ++plane) {
        if (residency->readback(planes[plane], plane_view(image, plane)) !=
            RT_STAGE_OK) {
            error = "thumbnail.whitebalance readback failed";
            return RT_STAGE_ERROR;
        }
    }

    return RT_STAGE_OK;
}

class OpenCLStageQueue final : public StageQueue
{
public:
    explicit OpenCLStageQueue(std::shared_ptr<OpenCLEnv> env)
        : env_(std::move(env))
    {
        residency_ = std::make_shared<OpenCLResidency>(env_, device_budget_bytes());
    }

    StageDeviceType type() const override
    {
        return StageDeviceType::OPENCL;
    }

    StageEvent submit(const StageProgram& program,
                      const std::vector<RtStageArg>& args,
                      RtStageProfiling* profiling) override
    {
        (void)program;
        (void)args;
        if (profiling != nullptr) {
            *profiling = RtStageProfiling{};
        }
        StageEvent event;
        event.status = StageEventStatus::Failed;
        event.result = RT_STAGE_FALLBACK;
        event.device = StageDeviceType::NONE;
        event.asynchronous = false;
        event.note = "a compiled host program cannot run on the OpenCL queue";
        return event;
    }

    StageEvent submit(const StageRequest& request,
                      RtStageProfiling* profiling) override
    {
        if (find_kernel(request.name) == nullptr || !request.impl.empty()) {
            StageEvent event = StageQueue::submit(request, profiling);
            event.note = request.impl.empty()
                             ? "no OpenCL implementation for stage '" +
                                   request.name + "'"
                             : "an explicit implementation is a host program";
            return event;
        }

        StageEvent event;
        event.device = StageDeviceType::OPENCL;

        std::string error;
        std::shared_ptr<OpenCLSubmission> submission;
        if (request.name == "lanczos3") {
            submission = launch_lanczos(env_, residency_, request.args, error);
        } else if (request.name == "transform") {
            submission = launch_transform(env_, residency_, request.args, error);
        } else if (request.name == "output.convert") {
            submission = launch_lab_to_rgb(env_, residency_, request.args,
                                           request.name.c_str(), false, error);
        } else if (request.name == "thumbnail.whitebalance") {
            const RtStageStatus status =
                run_thumbnail_wb(env_, residency_, request.args, error);
            if (status == RT_STAGE_OK) {
                StageEvent done;
                done.status = StageEventStatus::Complete;
                done.result = RT_STAGE_OK;
                done.device = StageDeviceType::OPENCL;
                done.asynchronous = false;
                return done;
            }
            StageEvent fallback = StageQueue::submit(request, profiling);
            fallback.note = error;
            return fallback;
        } else if (request.name == "rgb.proc") {
            const RtStageStatus status =
                run_rgb_proc(env_, residency_, request.args, error);
            if (status == RT_STAGE_OK) {
                StageEvent done;
                done.status = StageEventStatus::Complete;
                done.result = RT_STAGE_OK;
                done.device = StageDeviceType::OPENCL;
                done.asynchronous = false;
                return done;
            }
            StageEvent fallback = StageQueue::submit(request, profiling);
            fallback.note = error;
            return fallback;
        } else if (request.name == "monitor.output" ||
                   request.name == "viewport.output" ||
                   request.name == "thumbnail.output") {
            submission = launch_lab_to_rgb(env_, residency_, request.args,
                                           request.name.c_str(), true, error);
        } else {
            error = "no OpenCL launcher for stage '" + request.name + "'";
        }
        if (!submission) {
            StageEvent fallback = StageQueue::submit(request, profiling);
            fallback.note = error;
            return fallback;
        }

        event.status = StageEventStatus::Running;
        event.asynchronous = true;
        event.submission = std::move(submission);
        return event;
    }

    RtStageStatus wait(StageEvent& event) const override
    {
        if (!event.submission) {
            return event.result;
        }
        event.result = event.submission->wait();
        event.status = event.submission->poll();
        event.elapsed_ns = event.submission->elapsed_ns();
        return event.result;
    }

    StageDeviceResidency* residency() override
    {
        return residency_.get();
    }

private:
    std::shared_ptr<OpenCLEnv> env_;
    std::shared_ptr<OpenCLResidency> residency_;
};

/* Keeps a context usable when the device disappears between the availability
 * check and queue creation: every submission reports a fallback instead of
 * dereferencing a missing environment. */
class UnavailableStageQueue final : public StageQueue
{
public:
    StageDeviceType type() const override
    {
        return StageDeviceType::OPENCL;
    }

    StageEvent submit(const StageProgram&, const std::vector<RtStageArg>&,
                      RtStageProfiling*) override
    {
        return failed_event();
    }

    StageEvent submit(const StageRequest&, RtStageProfiling*) override
    {
        return failed_event();
    }

    RtStageStatus wait(StageEvent& event) const override
    {
        return event.result;
    }

private:
    static StageEvent failed_event()
    {
        StageEvent event;
        event.status = StageEventStatus::Failed;
        event.result = RT_STAGE_FALLBACK;
        event.device = StageDeviceType::NONE;
        event.note = "OpenCL device is unavailable";
        return event;
    }
};

std::unique_ptr<StageQueue> make_opencl_queue()
{
    auto env = make_env();
    if (!env) {
        return std::make_unique<UnavailableStageQueue>();
    }
    return std::make_unique<OpenCLStageQueue>(std::move(env));
}

} // namespace

bool opencl_stage_available(std::string* reason)
{
    return selected_device(reason) != nullptr;
}

bool opencl_stage_device_info(StageOpenCLDeviceInfo* info)
{
    const SelectedDevice* selected = selected_device(nullptr);
    if (selected == nullptr) {
        return false;
    }
    if (info != nullptr) {
        info->platform = selected->platform_name;
        info->device = selected->device_name;
        info->driver = selected->driver;
        info->version = selected->version;
        info->cpu = selected->cpu;
        info->global_memory = selected->global_memory;
        info->note = icd_note();
    }
    return true;
}

std::vector<std::string> opencl_stage_nodes()
{
    std::vector<std::string> names;
    for (const KernelDef& def : kernels()) {
        names.emplace_back(def.name);
    }
    return names;
}

bool has_opencl_stage_node(const std::string& name)
{
    return find_kernel(name) != nullptr;
}

StageDeviceType OpenCLStageDevice::type() const
{
    return StageDeviceType::OPENCL;
}

const char* OpenCLStageDevice::name() const
{
    return "opencl";
}

bool OpenCLStageDevice::available() const
{
    return opencl_stage_available(nullptr);
}

StageCapabilities OpenCLStageDevice::capabilities() const
{
    StageCapabilities capabilities;
    capabilities.deterministic = false;
    capabilities.full_precision = true;
    capabilities.async = true;
    capabilities.device_buffers = true;
    return capabilities;
}

std::shared_ptr<StageContext> OpenCLStageDevice::create_context() const
{
    return std::make_shared<StageContext>(make_opencl_stage_device());
}

std::unique_ptr<StageQueue> OpenCLStageDevice::create_queue() const
{
    return make_opencl_queue();
}

std::shared_ptr<StageDevice> make_opencl_stage_device()
{
    return std::make_shared<OpenCLStageDevice>();
}

#else // RT_HAVE_OPENCL

namespace
{

class UnavailableStageQueue final : public StageQueue
{
public:
    StageDeviceType type() const override
    {
        return StageDeviceType::OPENCL;
    }

    StageEvent submit(const StageProgram&, const std::vector<RtStageArg>&,
                      RtStageProfiling*) override
    {
        StageEvent event;
        event.status = StageEventStatus::Failed;
        event.result = RT_STAGE_FALLBACK;
        event.device = StageDeviceType::NONE;
        event.note = "built without OpenCL";
        return event;
    }

    RtStageStatus wait(StageEvent& event) const override
    {
        return event.result;
    }
};

} // namespace

bool opencl_stage_available(std::string* reason)
{
    if (reason != nullptr) {
        *reason = "built without OpenCL";
    }
    return false;
}

bool opencl_stage_device_info(StageOpenCLDeviceInfo*)
{
    return false;
}

std::vector<std::string> opencl_stage_nodes()
{
    return {};
}

bool has_opencl_stage_node(const std::string&)
{
    return false;
}

StageDeviceType OpenCLStageDevice::type() const
{
    return StageDeviceType::OPENCL;
}

const char* OpenCLStageDevice::name() const
{
    return "opencl";
}

bool OpenCLStageDevice::available() const
{
    return false;
}

StageCapabilities OpenCLStageDevice::capabilities() const
{
    return StageCapabilities{};
}

std::shared_ptr<StageContext> OpenCLStageDevice::create_context() const
{
    return std::make_shared<StageContext>(make_opencl_stage_device());
}

std::unique_ptr<StageQueue> OpenCLStageDevice::create_queue() const
{
    return std::make_unique<UnavailableStageQueue>();
}

std::shared_ptr<StageDevice> make_opencl_stage_device()
{
    return std::make_shared<OpenCLStageDevice>();
}

#endif // RT_HAVE_OPENCL

} // namespace rtengine::stage
