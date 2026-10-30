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
#include <limits>
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

#include "color.h"
#include "rt_math.h"
#include "sleef.h"
#include "stage/algorithms/gamut_cl.h"
#include "stage/algorithms/lab_to_rgb_cl.h"
#include "stage/algorithms/rgb_proc_cl.h"
#include "stage/algorithms/sharpen_cl.h"
#include "stage/algorithms/transform_cl.h"
#include "stage/algorithms/lab_tools_cl.h"
#include "stage/algorithms/local_contrast_cl.h"
#include "stage/algorithms/lab_curve_cl.h"
#include "stage/algorithms/lab_regions_cl.h"
#include "stage/algorithms/lab_wavelet_cl.h"
#include "stage/algorithms/local_adjustments_cl.h"
#include "stage/algorithms/munsell_lch_cl.h"
#include "stage/algorithms/vibrance_cl.h"
#include "stage/algorithms/working_trc_cl.h"
#include "stage/lab_tools_context.h"
#include "stage/lab_wavelet_context.h"
#include "stage/local_adjustments_context.h"
#include "stage/working_trc_context.h"

#include "cplx_wavelet_filter_coeffs.h"

#include "ipshadowshighlights.h"
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

/*
 * The working.trc program: the second-pass, apsatur and RGB-to-Lab entries
 * share one source. The host prefix (lab2rgb and the fixed sRGB inverse) runs
 * before the node, so no device first pass exists. RT_LAB_TO_RGB_CL supplies
 * rt_gamma_lookup(); the node-specific arithmetic is in working_trc_cl.h.
 */
const std::string& working_trc_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n") +
        RT_LAB_TO_RGB_CL + RT_WORKING_TRC_CL;
    return source;
}

/*
 * The lab.tools program: the LabGrid pass, the shadows/highlights mask,
 * guided-filter and blend passes, and vibrance. lab_tools_cl.h carries the
 * block's own arithmetic and its Lab-cache lookups; the Munsell and vibrance
 * fragments follow because vibrance calls the Munsell correction. The
 * Lab-curve kernel is a separate program (lab_curve_source()) because it
 * needs fp64.
 */
const std::string& lab_tools_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n") +
        RT_LAB_TOOLS_CL + RT_MUNSELL_LCH_CL + RT_VIBRANCE_CL;
    return source;
}

/*
 * The lab.wavelet program: the directional pyramid decomposition and the
 * reconstruction equaliser. The range tables and hue/chroma masks arrive as
 * host-built buffers; lab_wavelet_cl.h carries the block's arithmetic.
 */
const std::string& local_adjustments_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n"
                    "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n") +
        local_adjustments_program();
    return source;
}

const std::string& lab_wavelet_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n"
                    "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n") +
        RT_LAB_WAVELET_CL;
    return source;
}

/*
 * The Lab-curve program. It repeats chromiLuminanceCurve()'s double-precision
 * square root and divide, which `sqrt(float) / 327.68f` performs on the host,
 * so the Munsell chroma index and the block's chroma values round exactly as
 * the host does. fp64 is required, so the program is only built when the
 * device advertises CL_DEVICE_DOUBLE_FP_CONFIG; the launcher refuses the
 * Lab-curve pass otherwise and the block stays on the host.
 */
const std::string& lab_curve_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n"
                    "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n") +
        RT_LAB_TOOLS_CL + RT_MUNSELL_LCH_CL + RT_VIBRANCE_CL +
        RT_LAB_CURVE_CL;
    return source;
}

/*
 * The Lab-region program. The mask product, the hue mapping, the working
 * matrix products and the rgbLuminance() overload the block calls are double
 * expressions, so the program needs fp64 and is built only on a device that
 * advertises it. It reuses lab_tools_cl.h's Lab helpers but none of its
 * kernels.
 */
const std::string& lab_regions_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n"
                    "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n") +
        RT_LAB_TOOLS_CL + RT_LAB_REGIONS_CL;
    return source;
}

/*
 * The local-contrast program. gaussianBlur() above sigma 25 recurs in double
 * and the SSE path's tail lines evaluate in double too, so the program needs
 * fp64 and is built only on a device that advertises it. The launcher refuses
 * the pass otherwise and the block stays on the host.
 */
const std::string& local_contrast_source()
{
    static const std::string source =
        std::string("#pragma OPENCL FP_CONTRACT OFF\n"
                    "#pragma STDC FP_CONTRACT OFF\n"
                    "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n") +
        RT_LOCAL_CONTRAST_CL;
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
 * Frame drawing. Every destination pixel is written exactly once: inside the
 * source rectangle the source sample is copied, and elsewhere the constant
 * border colour is written. That is the same result as filling the frame and
 * then copying the source over it, in one pass. Row pitches stay explicit
 * because the stage device pads rows to the stage alignment.
 */
const std::string& framing_source()
{
    static const std::string source = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

__kernel void rt_framing(__global const float* srcR,
                         __global const float* srcG,
                         __global const float* srcB,
                         __global float* dstR,
                         __global float* dstG,
                         __global float* dstB,
                         const float borderR, const float borderG,
                         const float borderB,
                         const int srcWidth, const int srcHeight,
                         const int dstWidth, const int dstHeight,
                         const int srcPitch, const int dstPitch,
                         const int rowOffset, const int colOffset)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    const int sx = x - colOffset;
    const int sy = y - rowOffset;
    const int out = y * dstPitch + x;
    if (sx >= 0 && sx < srcWidth && sy >= 0 && sy < srcHeight) {
        const int in = sy * srcPitch + sx;
        dstR[out] = srcR[in];
        dstG[out] = srcG[in];
        dstB[out] = srcB[in];
    } else {
        dstR[out] = borderR;
        dstG[out] = borderG;
        dstB[out] = borderB;
    }
}
)CLC";
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

/*
 * Thumbnail coarse transform: the rotate-then-flip composition of
 * `IImage::rotate`, `hflip` and `vflip`, expressed as one destination-to-source
 * mapping. The rotation is applied first, then the flips, so the inverse walks
 * back through them.
 */
const std::string& thumbnail_permute_source()
{
    static const std::string source = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

__kernel void rt_thumbnail_permute(__global const float* src,
                                   __global float* dst,
                                   const int srcW, const int srcH,
                                   const int dstW, const int dstH,
                                   const int deg, const int hflip,
                                   const int vflip,
                                   const int srcPitch, const int srcPlanePitch,
                                   const int dstPitch, const int dstPlanePitch)
{
    const int gid = get_global_id(0);
    const int y = gid / dstW;
    if (y >= dstH) {
        return;
    }
    const int x = gid - y * dstW;

    const int x1 = hflip ? dstW - 1 - x : x;
    const int y1 = vflip ? dstH - 1 - y : y;

    int sx = x1;
    int sy = y1;
    if (deg == 90) {
        sx = y1;
        sy = srcH - 1 - x1;
    } else if (deg == 180) {
        sx = srcW - 1 - x1;
        sy = srcH - 1 - y1;
    } else if (deg == 270) {
        sx = srcW - 1 - y1;
        sy = x1;
    }

    const int in = sy * srcPitch + sx;
    const int out = y * dstPitch + x;
    dst[out] = src[in];
    dst[dstPlanePitch + out] = src[srcPlanePitch + in];
    dst[2 * dstPlanePitch + out] = src[2 * srcPlanePitch + in];
}

__kernel void rt_thumbnail_permute_u8(__global const uchar* src,
                                      __global uchar* dst,
                                      const int srcW, const int srcH,
                                      const int dstW, const int dstH,
                                      const int deg, const int hflip,
                                      const int vflip,
                                      const int srcPitch, const int dstPitch)
{
    const int gid = get_global_id(0);
    const int y = gid / dstW;
    if (y >= dstH) {
        return;
    }
    const int x = gid - y * dstW;

    const int x1 = hflip ? dstW - 1 - x : x;
    const int y1 = vflip ? dstH - 1 - y : y;

    int sx = x1;
    int sy = y1;
    if (deg == 90) {
        sx = y1;
        sy = srcH - 1 - x1;
    } else if (deg == 180) {
        sx = srcW - 1 - x1;
        sy = srcH - 1 - y1;
    } else if (deg == 270) {
        sx = srcW - 1 - y1;
        sy = x1;
    }

    const int in = sy * srcPitch + sx * 3;
    const int out = y * dstPitch + x * 3;
    dst[out] = src[in];
    dst[out + 1] = src[in + 1];
    dst[out + 2] = src[in + 2];
}
)CLC";
    return source;
}

/*
 * Thumbnail scale: the resampling loop of `IImage::resizeImgTo`.
 *
 * The source coordinate map is built on the host with the CPU's own float
 * arithmetic, because the two reference variants differ in how they derive it:
 * PlanarRGBData (Imagefloat and Image16) accumulates a float position row by
 * row, while ChunkyRGBData (Image8) multiplies and divides per row and column.
 * Recomputing either inside the kernel would round differently.
 *
 * The packed 8-bit variant is not a float resize: the reference stores the
 * bilinear blend in the source type before converting, so the value is
 * truncated toward zero, and an 8-bit source is converted to float by *257.
 *
 * One work item owns one destination row, so the per-row map is loaded once
 * and the row stays cache friendly. The extra work items a runtime may append
 * to reach a work-group multiple return before indexing anything.
 */
const std::string& thumbnail_scale_source()
{
    static const std::string source = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

/* Mirrors the host selectors; the ABI passes TI_Nearest and TI_Bilinear. */
#define RT_SCALE_NEAREST 0
#define RT_SCALE_BILINEAR 1
#define RT_SCALE_COPY 2

#define RT_SCALE_SRC_F32 0
#define RT_SCALE_SRC_U16 1
#define RT_SCALE_SRC_U8 2

/* ImageDatas::convertTo(unsigned char, float&) multiplies by 257. */
#define RT_SCALE_U8_TO_F32 257.0f

/* One destination axis entry, built on the host. `next` equals `index` on the
 * last source row or column, as the reference clamps it. */
typedef struct {
    int index;
    int next;
    float fraction;
} RtScaleAxis;

/* ImageDatas::convertTo(float, float&): a NaN becomes zero. */
inline float rt_scale_finish(float value)
{
    return isnan(value) ? 0.0f : value;
}

/* The term order is the reference's: four products, summed left to right. */
inline float rt_scale_planar_f32(__global const float* src, const int pitch,
                                 const int plane_pitch, const int plane,
                                 const RtScaleAxis row, const RtScaleAxis col,
                                 const int mode)
{
    const int base = plane * plane_pitch;
    if (mode == RT_SCALE_NEAREST || mode == RT_SCALE_COPY) {
        return src[base + row.index * pitch + col.index];
    }
    const float dx = col.fraction;
    const float dy = row.fraction;
    const int sy = row.index * pitch;
    const int ny = row.next * pitch;
    return src[base + sy + col.index] * (1.0f - dx) * (1.0f - dy) +
           src[base + sy + col.next] * dx * (1.0f - dy) +
           src[base + ny + col.index] * (1.0f - dx) * dy +
           src[base + ny + col.next] * dx * dy;
}

inline float rt_scale_planar_u16(__global const ushort* src, const int pitch,
                                 const int plane_pitch, const int plane,
                                 const RtScaleAxis row, const RtScaleAxis col,
                                 const int mode)
{
    const int base = plane * plane_pitch;
    if (mode == RT_SCALE_NEAREST || mode == RT_SCALE_COPY) {
        return (float)src[base + row.index * pitch + col.index];
    }
    const float dx = col.fraction;
    const float dy = row.fraction;
    const int sy = row.index * pitch;
    const int ny = row.next * pitch;
    const float a = (float)src[base + sy + col.index];
    const float b = (float)src[base + sy + col.next];
    const float c = (float)src[base + ny + col.index];
    const float d = (float)src[base + ny + col.next];
    return a * (1.0f - dx) * (1.0f - dy) +
           b * dx * (1.0f - dy) +
           c * (1.0f - dx) * dy +
           d * dx * dy;
}

/*
 * Packed 8-bit source: the reference assigns the float blend to an unsigned
 * char before converting it, so the truncation toward zero is part of the
 * result. Read as one plane of packed RGB.
 */
inline uchar rt_scale_packed_u8(__global const uchar* src, const int pitch,
                                const int plane, const RtScaleAxis row,
                                const RtScaleAxis col, const int mode)
{
    const int sy = row.index * pitch;
    if (mode == RT_SCALE_NEAREST || mode == RT_SCALE_COPY) {
        return src[sy + col.index * 3 + plane];
    }
    const int ny = row.next * pitch;
    const int sx = col.index * 3 + plane;
    const int nx = col.next * 3 + plane;
    const float dx = col.fraction;
    const float dy = row.fraction;
    return (uchar)(src[sy + sx] * (1.0f - dx) * (1.0f - dy) +
                   src[sy + nx] * dx * (1.0f - dy) +
                   src[ny + sx] * (1.0f - dx) * dy +
                   src[ny + nx] * dx * dy);
}

/*
 * The source pointer is typed as bytes because the three source formats have
 * different element types; each cast keeps the global address space, and the
 * device allocation starts on a 64-byte boundary. Pitches are in elements of
 * the format that reads them.
 */
__kernel void rt_thumbnail_scale_f32(__global const uchar* src,
                                     __global float* dst,
                                     __global const RtScaleAxis* rows,
                                     __global const RtScaleAxis* cols,
                                     const int src_format, const int mode,
                                     const int dst_width,
                                     const int dst_height,
                                     const int src_pitch,
                                     const int src_plane_pitch,
                                     const int dst_pitch,
                                     const int dst_plane_pitch)
{
    const int y = get_global_id(0);
    if (y >= dst_height) {
        return;
    }
    const RtScaleAxis row = rows[y];
    __global const float* plane_src = (__global const float*)src;
    __global const ushort* plane_u16 = (__global const ushort*)src;

    for (int x = 0; x < dst_width; ++x) {
        const RtScaleAxis col = cols[x];
        const int out = y * dst_pitch + x;
        for (int plane = 0; plane < 3; ++plane) {
            float value;
            if (src_format == RT_SCALE_SRC_U8) {
                value = (float)rt_scale_packed_u8(src, src_pitch, plane, row,
                                                  col, mode) *
                        RT_SCALE_U8_TO_F32;
            } else if (src_format == RT_SCALE_SRC_U16) {
                value = rt_scale_planar_u16(plane_u16, src_pitch,
                                            src_plane_pitch, plane, row, col,
                                            mode);
            } else {
                /* RT_SCALE_SRC_F32 */
                value = rt_scale_planar_f32(plane_src, src_pitch,
                                            src_plane_pitch, plane, row, col,
                                            mode);
            }
            dst[plane * dst_plane_pitch + out] = rt_scale_finish(value);
        }
    }
}

/* The packed 8-bit destination: an unsigned char source converts to an
 * unsigned char destination unchanged. */
__kernel void rt_thumbnail_scale_u8(__global const uchar* src,
                                    __global uchar* dst,
                                    __global const RtScaleAxis* rows,
                                    __global const RtScaleAxis* cols,
                                    const int mode,
                                    const int dst_width,
                                    const int dst_height,
                                    const int src_pitch,
                                    const int dst_pitch)
{
    const int y = get_global_id(0);
    if (y >= dst_height) {
        return;
    }
    const RtScaleAxis row = rows[y];

    for (int x = 0; x < dst_width; ++x) {
        const RtScaleAxis col = cols[x];
        const int out = y * dst_pitch + x * 3;
        for (int plane = 0; plane < 3; ++plane) {
            dst[out + plane] = rt_scale_packed_u8(src, src_pitch, plane, row,
                                                  col, mode);
        }
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
        {"gamut", RT_GAMUT_CL, "rt_gamut"},
        {"working.trc", working_trc_source().c_str(), "rt_working_trc_second"},
        {"lab.tools", lab_tools_source().c_str(), "rt_lab_tools_mask"},
        {"lab.tools.labcurve", lab_curve_source().c_str(), "rt_lab_tools_lab_curve"},
        {"lab.tools.localcontrast", local_contrast_source().c_str(), "rt_lab_tools_lc_line"},
        {"lab.tools.regions", lab_regions_source().c_str(), "rt_lab_tools_region_mask"},
        {"lab.wavelet", lab_wavelet_source().c_str(), "rt_lab_wavelet_decompose"},
        {"local.adjustments", local_adjustments_source().c_str(), "rt_local_adjustments_apply"},
        {"output.sharpen", RT_SHARPEN_CL, "rt_sharpen_mask"},
        {"framing", framing_source().c_str(), "rt_framing"},
        {"thumbnail.whitebalance", thumbnail_wb_source().c_str(), "rt_thumbnail_wb"},
        {"thumbnail.transform", thumbnail_permute_source().c_str(), "rt_thumbnail_permute"},
        {"thumbnail.transform.u8", thumbnail_permute_source().c_str(), "rt_thumbnail_permute_u8"},
        {"thumbnail.scale", thumbnail_scale_source().c_str(), "rt_thumbnail_scale_f32"},
        {"thumbnail.scale.u8", thumbnail_scale_source().c_str(), "rt_thumbnail_scale_u8"}
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
cl_kernel compiled_kernel_entry(const std::shared_ptr<OpenCLEnv>& env,
                                const KernelDef* def, const char* entry,
                                std::string& error)
{
    std::lock_guard<std::mutex> lock(env->program_mutex);
    const std::string key = std::string(def->name) + ':' + entry;
    const auto found = env->kernels.find(key);
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

    /* apsatur and the analytic TRC divide, and the host uses correctly
     * rounded IEEE division. Without this option OpenCL may lower float
     * division to a reciprocal approximation. */
    const char* build_options =
        std::strcmp(def->name, "working.trc") == 0 ||
                std::strcmp(def->name, "lab.tools") == 0 ||
                std::strcmp(def->name, "lab.tools.labcurve") == 0 ||
                std::strcmp(def->name, "lab.tools.localcontrast") == 0 ||
                std::strcmp(def->name, "local.adjustments") == 0 ||
                std::strcmp(def->name, "lab.wavelet") == 0
            ? "-cl-fp32-correctly-rounded-divide-sqrt"
            : nullptr;
    build_error = env->api->BuildProgram(program, 1, &env->device,
                                         build_options, nullptr, nullptr);
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

    cl_kernel kernel = env->api->CreateKernel(program, entry, &build_error);
    env->api->ReleaseProgram(program);
    if (kernel == nullptr) {
        error = "clCreateKernel failed";
        return nullptr;
    }

    env->kernels[key] = kernel;
    return kernel;
}

cl_kernel compiled_kernel(const std::shared_ptr<OpenCLEnv>& env,
                          const KernelDef* def, std::string& error)
{
    return compiled_kernel_entry(env, def, def->entry, error);
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
 * The stage ABI does not carry the row pitch: the launcher inserts the two
 * device pitches after height when it sets the kernel arguments, so the node
 * argument order above is unchanged.
 *
 * A LUT is 65536 samples; an absent RGB curve may be a one-sample dummy
 * because rgbCurveMask stops the kernel reading it. The matrix tables hold
 * nine floats. Each plane is uploaded into its own device allocation, the
 * kernel addresses it by the allocation's row pitch, and the three
 * destinations are read back here; the submission is therefore complete when
 * it is returned.
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

    /*
     * The kernel addresses rows by pitch, and the residency pads rows to the
     * stage alignment. Every plane of one side must therefore agree, and the
     * pitch must be at least the image width.
     */
    const std::size_t src_pitch_bytes = src_devices[0]->row_pitch();
    const std::size_t dst_pitch_bytes = dst_devices[0]->row_pitch();
    for (int plane = 1; plane < 3; ++plane) {
        if (src_devices[plane]->row_pitch() != src_pitch_bytes ||
            dst_devices[plane]->row_pitch() != dst_pitch_bytes) {
            error = "rgb.proc planes disagree on the row pitch";
            return RT_STAGE_FALLBACK;
        }
    }
    if (src_pitch_bytes % sizeof(float) != 0 ||
        dst_pitch_bytes % sizeof(float) != 0 ||
        src_pitch_bytes / sizeof(float) < static_cast<std::size_t>(src.width) ||
        dst_pitch_bytes / sizeof(float) < static_cast<std::size_t>(dst.width)) {
        error = "rgb.proc row pitch does not cover the image width";
        return RT_STAGE_FALLBACK;
    }

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const cl_int width = src.width;
    const cl_int height = src.height;
    const cl_int src_pitch =
        static_cast<cl_int>(src_pitch_bytes / sizeof(float));
    const cl_int dst_pitch =
        static_cast<cl_int>(dst_pitch_bytes / sizeof(float));
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
         set_arg(arg++, sizeof(cl_int), &src_pitch) &&
         set_arg(arg++, sizeof(cl_int), &dst_pitch) &&
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
 * End-of-process gamut. Argument order:
 *   [0] Lab three-plane F32 buffer, read and written in place,
 *   [1] wip, [2] wp, [3] to_out, [4] from_out: four 9x1 F32 matrices,
 *   [5] double gain (2^EV), [6] double power, [7] int compress.
 *
 * The matrices are derived on the host in double from the same helpers and in
 * the same order as ImProcFunctions::gamutcompr() for beginend == 1; see
 * gamut_cl.h. The image argument is both source and destination on purpose:
 * every pixel is read and written exactly once, so the in-place pass is the
 * same as the host's separate provcomp buffer. The single Lab allocation is
 * addressed through its row and plane pitches, and the three planes are read
 * back after a wait, so the node reports a completed device event.
 *
 * Everything the kernel consumes is checked here: the buffer shape and format,
 * four 9x1 tables, a positive finite gain and power, and a compress flag
 * that is exactly 0 or 1. A suspicious value is refused, never guessed.
 */
RtStageStatus run_gamut(const std::shared_ptr<OpenCLEnv>& env,
                        const std::shared_ptr<OpenCLResidency>& residency,
                        const std::vector<RtStageArg>& args,
                        std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 5;
    constexpr std::size_t DOUBLE_ARGS = 2;
    constexpr std::size_t INT_ARGS = 1;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + DOUBLE_ARGS + INT_ARGS;
    constexpr std::size_t FIRST_DOUBLE = BUFFER_ARGS;
    constexpr std::size_t FIRST_INT = BUFFER_ARGS + DOUBLE_ARGS;
    /* The ABI contract is a 9x1 table. The kernel reads nine flat entries, so
     * a taller table would be read across the device's padded row pitch. */
    constexpr int MATRIX_ROWS = 1;
    constexpr int MATRIX_COLUMNS = 9;
    constexpr int MATRIX_ENTRIES = MATRIX_ROWS * MATRIX_COLUMNS;
    constexpr int TABLE_ARGS = 4;

    if (args.size() != ARG_COUNT) {
        error = "gamut expects five buffers, two doubles and one int";
        return RT_STAGE_INVALID;
    }
    for (std::size_t i = 0; i < BUFFER_ARGS; ++i) {
        if (args[i].kind != RT_STAGE_ARG_BUFFER) {
            error = "gamut expects five buffers";
            return RT_STAGE_INVALID;
        }
    }
    for (std::size_t i = FIRST_DOUBLE; i < FIRST_INT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_DOUBLE) {
            error = "gamut has a non-double scalar";
            return RT_STAGE_INVALID;
        }
    }
    if (args[FIRST_INT].kind != RT_STAGE_ARG_INT) {
        error = "gamut has a non-int compress flag";
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        error = "gamut needs a three-plane F32 Lab buffer";
        return RT_STAGE_INVALID;
    }

    for (std::size_t i = 1; i < BUFFER_ARGS; ++i) {
        const RtStageBuffer& table = args[i].u.buffer;
        if (!valid(table) || table.format != RT_STAGE_F32 ||
            table.planes != 1 || table.width != MATRIX_COLUMNS ||
            table.height != MATRIX_ROWS) {
            error = "gamut needs four 9x1 F32 matrices";
            return RT_STAGE_INVALID;
        }
        const float* entries = static_cast<const float*>(table.data);
        for (int entry = 0; entry < MATRIX_ENTRIES; ++entry) {
            if (!std::isfinite(entries[entry])) {
                error = "gamut matrix has a non-finite entry";
                return RT_STAGE_INVALID;
            }
        }
    }

    const double gainev = args[FIRST_DOUBLE].u.f;
    const double pwr = args[FIRST_DOUBLE + 1].u.f;
    if (!std::isfinite(gainev) || gainev <= 0.0) {
        error = "gamut gain is not a positive finite number";
        return RT_STAGE_INVALID;
    }
    if (!std::isfinite(pwr) || pwr <= 0.0) {
        error = "gamut power is not a positive finite number";
        return RT_STAGE_INVALID;
    }
    if (!std::isfinite(static_cast<float>(gainev)) ||
        static_cast<float>(gainev) <= 0.0f) {
        error = "gamut gain does not fit the kernel's float multiplier";
        return RT_STAGE_INVALID;
    }

    const std::int64_t compress = args[FIRST_INT].u.i;
    if (compress != 0 && compress != 1) {
        error = "gamut compress flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }

    const KernelDef* def = find_kernel("gamut");
    assert(def != nullptr);
    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const StageBufferShape image_shape{RT_STAGE_F32, lab.width, lab.height, 3};
    StageDeviceBufferPtr image =
        residency->acquire(lab.data, image_shape, &error);
    if (!image) {
        return RT_STAGE_FALLBACK;
    }
    if (residency->upload(image, lab) != RT_STAGE_OK) {
        error = "gamut image upload failed";
        return RT_STAGE_FALLBACK;
    }

    StageDeviceBufferPtr tables[TABLE_ARGS];
    for (std::size_t i = 0; i < TABLE_ARGS; ++i) {
        const RtStageBuffer& table = args[i + 1].u.buffer;
        const StageBufferShape shape{RT_STAGE_F32, table.width, table.height,
                                     1};
        tables[i] = residency->acquire(table.data, shape, &error);
        if (!tables[i] ||
            residency->upload(tables[i], table) != RT_STAGE_OK) {
            error = "gamut matrix upload failed";
            return RT_STAGE_FALLBACK;
        }
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };

    const std::size_t row_pitch_bytes = image->row_pitch();
    const std::size_t plane_pitch_bytes = image->plane_pitch();
    if (row_pitch_bytes % sizeof(float) != 0 ||
        plane_pitch_bytes % sizeof(float) != 0 ||
        row_pitch_bytes / sizeof(float) < static_cast<std::size_t>(lab.width) ||
        plane_pitch_bytes / sizeof(float) <
            static_cast<std::size_t>(lab.height) *
                (row_pitch_bytes / sizeof(float))) {
        error = "gamut row or plane pitch does not cover the image";
        return RT_STAGE_FALLBACK;
    }
    const std::int64_t plane_pitch = static_cast<std::int64_t>(
        plane_pitch_bytes / sizeof(float));
    /* The kernel addresses the third plane as 2 * plane_pitch + offset. */
    if (plane_pitch > std::numeric_limits<cl_int>::max() / 3) {
        error = "gamut plane pitch does not fit the kernel's int arithmetic";
        return RT_STAGE_FALLBACK;
    }

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const cl_float gain = static_cast<cl_float>(gainev);
    const cl_float power = static_cast<cl_float>(pwr);
    const cl_int comp = static_cast<cl_int>(compress);
    const cl_int width = lab.width;
    const cl_int height = lab.height;
    const cl_int pitch = static_cast<cl_int>(row_pitch_bytes / sizeof(float));
    const cl_int plane = static_cast<cl_int>(plane_pitch);

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    cl_uint arg = 0;
    bool ok = true;
    cl_mem image_memory = memory_of(image);
    ok = set_arg(arg++, sizeof(cl_mem), &image_memory);
    for (std::size_t i = 0; i < TABLE_ARGS && ok; ++i) {
        cl_mem memory = memory_of(tables[i]);
        ok = set_arg(arg++, sizeof(cl_mem), &memory);
    }
    ok = ok &&
         set_arg(arg++, sizeof(cl_float), &gain) &&
         set_arg(arg++, sizeof(cl_float), &power) &&
         set_arg(arg++, sizeof(cl_int), &comp) &&
         set_arg(arg++, sizeof(cl_int), &width) &&
         set_arg(arg++, sizeof(cl_int), &height) &&
         set_arg(arg++, sizeof(cl_int), &pitch) &&
         set_arg(arg++, sizeof(cl_int), &plane);
    if (!ok) {
        error = "clSetKernelArg failed";
        return RT_STAGE_FALLBACK;
    }

    const std::size_t global[2] = {static_cast<std::size_t>(width),
                                   static_cast<std::size_t>(height)};
    cl_event done = nullptr;
    if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr, global,
                                       nullptr, 0, nullptr,
                                       &done) != CL_SUCCESS ||
        done == nullptr) {
        error = "gamut launch failed";
        return RT_STAGE_ERROR;
    }

    const cl_int wait_status = env->api->WaitForEvents(1, &done);
    env->api->ReleaseEvent(done);
    if (wait_status != CL_SUCCESS) {
        error = "gamut did not complete";
        return RT_STAGE_ERROR;
    }

    if (residency->readback(image, lab) != RT_STAGE_OK) {
        error = "gamut readback failed";
        return RT_STAGE_ERROR;
    }

    return RT_STAGE_OK;
}


/*
 * The working.trc node. Argument order:
 *   [0] Lab three-plane F32 buffer, read and written in place,
 *   [1] PTR host context (RtWorkingTrcContext), host-only,
 *   [2] int support flag, mirrored from the context descriptor (1 only when
 *       the configuration is supported and the host setup admitted it),
 *   [3] double saturation (icm.wapsat),
 *   [4] three-plane F32 second-pass XYZ image from the host prefix,
 *   [5] three-plane F32 linear pre-TRC reference for apsatur().
 *
 * The device work starts at the second pass: the host prefix has already run
 * lab2rgb, the fixed sRGB inverse, the probe/profile, toxyz and the optional
 * gamut map. The Lab buffer is read back only after every pass succeeded, so a
 * refusal leaves the host image untouched for the builtin.
 *
 * A clear support flag, in either the argument or the context descriptor,
 * refuses before the upload: that is the residual path, and it makes the CPU
 * builtin - the original host block - execute and record impl=host. The
 * per-pixel arguments are checked here; the context is not consulted for
 * them.
 */
RtStageStatus run_working_trc(const std::shared_ptr<OpenCLEnv>& env,
                              const std::shared_ptr<OpenCLResidency>& residency,
                              const std::vector<RtStageArg>& args,
                              std::string& error)
{
    constexpr std::size_t ARG_COUNT = 6;
    if (args.size() != ARG_COUNT) {
        error = "working.trc expects three buffers, a context and a scalar";
        return RT_STAGE_INVALID;
    }
    if (args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_PTR ||
        args[2].kind != RT_STAGE_ARG_INT ||
        args[3].kind != RT_STAGE_ARG_DOUBLE) {
        error = "working.trc has an argument of the wrong kind";
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        error = "working.trc needs a three-plane F32 Lab buffer";
        return RT_STAGE_INVALID;
    }

    const auto* context =
        static_cast<const RtWorkingTrcContext*>(args[1].u.ptr);
    if (context == nullptr || context->size != sizeof(RtWorkingTrcContext) ||
        context->version != RT_WORKING_TRC_CONTEXT_VERSION ||
        context->magic != RT_WORKING_TRC_CONTEXT_MAGIC ||
        context->build == nullptr) {
        error = "working.trc has an invalid host context";
        return RT_STAGE_INVALID;
    }

    const std::int64_t supported = args[2].u.i;
    if (supported != 0 && supported != 1) {
        error = "working.trc support flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    if (supported != 1 || context->supported != 1) {
        error = "working.trc support flag is not set";
        return RT_STAGE_FALLBACK;
    }

    const double saturation_double = args[3].u.f;
    if (!std::isfinite(saturation_double) || saturation_double < 0.0 ||
        saturation_double > 1.0) {
        error = "working.trc saturation is outside 0..1";
        return RT_STAGE_INVALID;
    }
    if (args[4].kind != RT_STAGE_ARG_BUFFER ||
        args[5].kind != RT_STAGE_ARG_BUFFER) {
        error = "working.trc has a pass-2 argument of the wrong kind";
        return RT_STAGE_INVALID;
    }
    const RtStageBuffer& xyz = args[4].u.buffer;
    const RtStageBuffer& linear = args[5].u.buffer;
    const StageBufferShape image_shape{RT_STAGE_F32, lab.width, lab.height, 3};
    for (const RtStageBuffer* buffer : {&xyz, &linear}) {
        if (!valid(*buffer) || buffer->format != RT_STAGE_F32 ||
            buffer->planes != 3 || buffer->width != lab.width ||
            buffer->height != lab.height) {
            error = "working.trc needs second-pass and linear RGB buffers";
            return RT_STAGE_INVALID;
        }
    }

    const KernelDef* def = find_kernel("working.trc");
    assert(def != nullptr);
    cl_kernel second_kernel =
        compiled_kernel_entry(env, def, "rt_working_trc_second", error);
    cl_kernel apsatur_kernel =
        compiled_kernel_entry(env, def, "rt_working_trc_apsatur", error);
    cl_kernel lab_kernel =
        compiled_kernel_entry(env, def, "rt_working_trc_lab", error);
    if (second_kernel == nullptr || apsatur_kernel == nullptr ||
        lab_kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const int width = lab.width;
    const int height = lab.height;

    StageDeviceBufferPtr lab_device =
        residency->acquire(lab.data, image_shape, &error);
    StageDeviceBufferPtr xyz_device =
        residency->acquire(xyz.data, image_shape, &error);
    StageDeviceBufferPtr linear_device =
        residency->acquire(linear.data, image_shape, &error);
    char post_key = 0;
    StageDeviceBufferPtr post =
        residency->acquire(&post_key, image_shape, &error);
    if (!lab_device || !xyz_device || !linear_device || !post) {
        return RT_STAGE_FALLBACK;
    }

    if (residency->upload(lab_device, lab) != RT_STAGE_OK ||
        residency->upload(xyz_device, xyz) != RT_STAGE_OK ||
        residency->upload(linear_device, linear) != RT_STAGE_OK) {
        error = "working.trc input upload failed";
        return RT_STAGE_FALLBACK;
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };
    const auto pitch_of = [](const StageDeviceBufferPtr& buffer) {
        return buffer->row_pitch() / sizeof(float);
    };
    const auto plane_pitch_of = [](const StageDeviceBufferPtr& buffer) {
        return buffer->plane_pitch() / sizeof(float);
    };

    const StageDeviceBufferPtr buffers[] = {lab_device, xyz_device,
                                           linear_device, post};
    for (const StageDeviceBufferPtr& buffer : buffers) {
        if (buffer->row_pitch() % sizeof(float) != 0 ||
            buffer->plane_pitch() % sizeof(float) != 0 ||
            pitch_of(buffer) < static_cast<std::size_t>(width) ||
            plane_pitch_of(buffer) < static_cast<std::size_t>(height) *
                                           pitch_of(buffer)) {
            error = "working.trc row or plane pitch does not cover the image";
            return RT_STAGE_FALLBACK;
        }
    }
    /* The two RGB inputs and the post scratch have one layout. */
    if (pitch_of(linear_device) != pitch_of(post) ||
        plane_pitch_of(linear_device) != plane_pitch_of(post)) {
        error = "working.trc RGB buffers have different pitches";
        return RT_STAGE_FALLBACK;
    }

    const std::int64_t lab_pitch =
        static_cast<std::int64_t>(pitch_of(lab_device));
    const std::int64_t lab_plane_pitch =
        static_cast<std::int64_t>(plane_pitch_of(lab_device));
    const std::int64_t xyz_pitch =
        static_cast<std::int64_t>(pitch_of(xyz_device));
    const std::int64_t xyz_plane_pitch =
        static_cast<std::int64_t>(plane_pitch_of(xyz_device));
    const std::int64_t rgb_pitch =
        static_cast<std::int64_t>(pitch_of(post));
    const std::int64_t rgb_plane_pitch =
        static_cast<std::int64_t>(plane_pitch_of(post));
    if (lab_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        lab_plane_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        xyz_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        xyz_plane_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        rgb_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        rgb_plane_pitch > std::numeric_limits<cl_int>::max() / 3) {
        error = "working.trc plane pitch does not fit the kernel's int";
        return RT_STAGE_FALLBACK;
    }

    RtWorkingTrcResult setup{};
    if (context->build(context->self, &setup) != 0 ||
        setup.toxyz == nullptr || setup.matrix == nullptr ||
        setup.gamma == nullptr || setup.curve == nullptr) {
        error = "working.trc setup did not build the output profile";
        return RT_STAGE_FALLBACK;
    }

    for (const float* table : {setup.toxyz, setup.matrix}) {
        for (int entry = 0; entry < 9; ++entry) {
            if (!std::isfinite(table[entry])) {
                error = "working.trc setup produced a non-finite matrix";
                return RT_STAGE_FALLBACK;
            }
        }
    }
    if (!std::isfinite(setup.gamma[0]) ||
        !std::isfinite(setup.gamma[RT_WORKING_TRC_GAMMA_ENTRIES - 1])) {
        error = "working.trc setup produced a non-finite gamma table";
        return RT_STAGE_FALLBACK;
    }

    RtStageBuffer toxyz_host{};
    toxyz_host.size = sizeof(RtStageBuffer);
    toxyz_host.data = const_cast<float*>(setup.toxyz);
    toxyz_host.width = 9;
    toxyz_host.height = 1;
    toxyz_host.row_pitch = 9 * sizeof(float);
    toxyz_host.plane_pitch = toxyz_host.row_pitch;
    toxyz_host.planes = 1;
    toxyz_host.format = RT_STAGE_F32;
    toxyz_host.flags = RT_STAGE_BUFFER_READ;

    RtStageBuffer matrix_host = toxyz_host;
    matrix_host.data = const_cast<float*>(setup.matrix);

    RtStageBuffer gamma_host{};
    gamma_host.size = sizeof(RtStageBuffer);
    gamma_host.data = const_cast<float*>(setup.gamma);
    gamma_host.width = RT_WORKING_TRC_GAMMA_ENTRIES;
    gamma_host.height = 1;
    gamma_host.row_pitch = RT_WORKING_TRC_GAMMA_ENTRIES * sizeof(float);
    gamma_host.plane_pitch = gamma_host.row_pitch;
    gamma_host.planes = 1;
    gamma_host.format = RT_STAGE_F32;
    gamma_host.flags = RT_STAGE_BUFFER_READ;

    RtStageBuffer curve_host{};
    curve_host.size = sizeof(RtStageBuffer);
    curve_host.data = const_cast<float*>(setup.curve);
    curve_host.width = 8;
    curve_host.height = 1;
    curve_host.row_pitch = 8 * sizeof(float);
    curve_host.plane_pitch = curve_host.row_pitch;
    curve_host.planes = 1;
    curve_host.format = RT_STAGE_F32;
    curve_host.flags = RT_STAGE_BUFFER_READ;

    /* The host's Lab caches: the same 65536 samples Color::XYZ2Lab reads. */
    std::vector<float> cachef_lut(RT_WORKING_TRC_GAMMA_ENTRIES);
    std::vector<float> cachefy_lut(RT_WORKING_TRC_GAMMA_ENTRIES);
    for (std::size_t i = 0; i < cachef_lut.size(); ++i) {
        cachef_lut[i] = Color::cachef[static_cast<int>(i)];
        cachefy_lut[i] = Color::cachefy[static_cast<int>(i)];
    }
    RtStageBuffer cachef_host = gamma_host;
    cachef_host.data = cachef_lut.data();
    RtStageBuffer cachefy_host = gamma_host;
    cachefy_host.data = cachefy_lut.data();

    const StageBufferShape matrix_shape{RT_STAGE_F32, 9, 1, 1};
    const StageBufferShape curve_shape{RT_STAGE_F32, 8, 1, 1};
    const StageBufferShape gamma_shape{
        RT_STAGE_F32, static_cast<std::int32_t>(RT_WORKING_TRC_GAMMA_ENTRIES),
        1, 1};
    char toxyz_key = 0;
    char matrix_key = 0;
    char gamma_key = 0;
    char curve_key = 0;
    char cachef_key = 0;
    char cachefy_key = 0;
    StageDeviceBufferPtr toxyz_device =
        residency->acquire(&toxyz_key, matrix_shape, &error);
    StageDeviceBufferPtr matrix_device =
        residency->acquire(&matrix_key, matrix_shape, &error);
    StageDeviceBufferPtr gamma_device =
        residency->acquire(&gamma_key, gamma_shape, &error);
    StageDeviceBufferPtr curve_device =
        residency->acquire(&curve_key, curve_shape, &error);
    StageDeviceBufferPtr cachef_device =
        residency->acquire(&cachef_key, gamma_shape, &error);
    StageDeviceBufferPtr cachefy_device =
        residency->acquire(&cachefy_key, gamma_shape, &error);
    if (!toxyz_device || !matrix_device || !gamma_device ||
        !curve_device || !cachef_device || !cachefy_device ||
        residency->upload(toxyz_device, toxyz_host) != RT_STAGE_OK ||
        residency->upload(matrix_device, matrix_host) != RT_STAGE_OK ||
        residency->upload(gamma_device, gamma_host) != RT_STAGE_OK ||
        residency->upload(curve_device, curve_host) != RT_STAGE_OK ||
        residency->upload(cachef_device, cachef_host) != RT_STAGE_OK ||
        residency->upload(cachefy_device, cachefy_host) != RT_STAGE_OK) {
        error = "working.trc setup table upload failed";
        return RT_STAGE_FALLBACK;
    }

    const auto set_arg = [&](cl_kernel kernel, cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };
    const auto launch = [&](cl_kernel kernel, const char* label) {
        const std::size_t global[2] = {static_cast<std::size_t>(width),
                                       static_cast<std::size_t>(height)};
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr,
                                           global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            error = std::string(label) + " launch failed";
            return false;
        }
        const cl_int wait_status = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        if (wait_status != CL_SUCCESS) {
            error = std::string(label) + " did not complete";
            return false;
        }
        return true;
    };

    const cl_float saturation = static_cast<cl_float>(saturation_double);

    {
        std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

        cl_mem xyz_memory = memory_of(xyz_device);
        cl_mem linear_memory = memory_of(linear_device);
        cl_mem post_memory = memory_of(post);
        cl_mem toxyz_memory = memory_of(toxyz_device);
        cl_mem matrix_memory = memory_of(matrix_device);
        cl_mem gamma_memory = memory_of(gamma_device);
        cl_mem curve_memory = memory_of(curve_device);
        cl_mem cachef_memory = memory_of(cachef_device);
        cl_mem cachefy_memory = memory_of(cachefy_device);
        cl_mem lab_memory = memory_of(lab_device);
        if (xyz_memory == nullptr || linear_memory == nullptr ||
            post_memory == nullptr || toxyz_memory == nullptr ||
            matrix_memory == nullptr ||
            gamma_memory == nullptr || curve_memory == nullptr ||
            cachef_memory == nullptr || cachefy_memory == nullptr ||
            lab_memory == nullptr) {
            error = "working.trc could not address its device buffers";
            return RT_STAGE_FALLBACK;
        }

        const cl_int cl_width = width;
        const cl_int cl_height = height;
        const cl_int cl_lab_pitch = static_cast<cl_int>(lab_pitch);
        const cl_int cl_lab_plane = static_cast<cl_int>(lab_plane_pitch);
        const cl_int cl_xyz_pitch = static_cast<cl_int>(xyz_pitch);
        const cl_int cl_xyz_plane = static_cast<cl_int>(xyz_plane_pitch);
        const cl_int cl_rgb_pitch = static_cast<cl_int>(rgb_pitch);
        const cl_int cl_rgb_plane = static_cast<cl_int>(rgb_plane_pitch);

        cl_uint arg = 0;
        bool ok = true;
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_mem), &xyz_memory);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_mem), &post_memory);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_mem), &matrix_memory);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_mem), &gamma_memory);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_mem), &curve_memory);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_int), &cl_xyz_pitch);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_int), &cl_xyz_plane);
        ok = ok && set_arg(second_kernel, arg++, sizeof(cl_int), &cl_rgb_pitch);
        ok = ok &&
             set_arg(second_kernel, arg++, sizeof(cl_int), &cl_rgb_plane);
        if (!ok) {
            error = "working.trc second-pass clSetKernelArg failed";
            return RT_STAGE_FALLBACK;
        }
        if (!launch(second_kernel, "working.trc second pass")) {
            return RT_STAGE_FALLBACK;
        }

        if (saturation_double > 0.0) {
            arg = 0;
            ok = true;
            ok = ok &&
                 set_arg(apsatur_kernel, arg++, sizeof(cl_mem), &linear_memory);
            ok = ok &&
                 set_arg(apsatur_kernel, arg++, sizeof(cl_mem), &post_memory);
            ok = ok && set_arg(apsatur_kernel, arg++, sizeof(cl_mem), &toxyz_memory);
            ok = ok &&
                 set_arg(apsatur_kernel, arg++, sizeof(cl_float), &saturation);
            ok = ok && set_arg(apsatur_kernel, arg++, sizeof(cl_int), &cl_width);
            ok = ok && set_arg(apsatur_kernel, arg++, sizeof(cl_int), &cl_height);
            ok = ok &&
                 set_arg(apsatur_kernel, arg++, sizeof(cl_int), &cl_rgb_pitch);
            ok = ok &&
                 set_arg(apsatur_kernel, arg++, sizeof(cl_int), &cl_rgb_plane);
            if (!ok) {
                error = "working.trc saturation clSetKernelArg failed";
                return RT_STAGE_FALLBACK;
            }
            if (!launch(apsatur_kernel, "working.trc saturation")) {
                return RT_STAGE_FALLBACK;
            }
        }

        arg = 0;
        ok = true;
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_mem), &post_memory);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_mem), &lab_memory);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_mem), &toxyz_memory);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_mem), &cachef_memory);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_mem), &cachefy_memory);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_int), &cl_rgb_pitch);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_int), &cl_rgb_plane);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_int), &cl_lab_pitch);
        ok = ok && set_arg(lab_kernel, arg++, sizeof(cl_int), &cl_lab_plane);
        if (!ok) {
            error = "working.trc Lab clSetKernelArg failed";
            return RT_STAGE_FALLBACK;
        }
        if (!launch(lab_kernel, "working.trc Lab pass")) {
            return RT_STAGE_FALLBACK;
        }
    }

    if (residency->readback(lab_device, lab) != RT_STAGE_OK) {
        error = "working.trc Lab readback failed";
        return RT_STAGE_FALLBACK;
    }

    return RT_STAGE_OK;
}


/*
 * The lab.tools node. Argument order:
 *   [0] Lab three-plane F32 buffer, read and written in place,
 *   [1] PTR host context (RtLabToolsContext), host-only,
 *   [2] int support flag, mirrored from the context descriptor (1 only when
 *       the configuration is the node's supported set and the host setup
 *       built its tables),
 *   [3] int LabGrid flag,
 *   [4..7] doubles a_scale, a_base, b_scale, b_base of the LabGrid pass,
 *   [8] int shadows/highlights enabled,
 *   [9] int highlights, [10] int shadows,
 *   [11] int htonalwidth, [12] int stonalwidth,
 *   [13] int radius, [14] int Lab mode, [15] int scale,
 *   [16] BUFFER highlight tone table, [17] BUFFER shadow tone table,
 *   [18] BUFFER working RGB -> XYZ, [19] BUFFER XYZ -> working RGB,
 *   [20] BUFFER Color::cachef, [21] BUFFER Color::cachefy,
 *   [22] int vibrance flag (1 only when ImProcFunctions::vibrance() would
 *        run and the host setup built its tables),
 *   [23] int pastels, [24] int saturated,
 *   [25] int pastels/saturated threshold top, [26] int threshold bottom,
 *   [27] int protectskins, [28] int avoidcolorshift,
 *   [29] int skin curve set, [30] int highlight (toneCurve.hrenabled),
 *   [31] BUFFER the flat Munsell table set (197 x 140 floats),
 *   [32] BUFFER one float per Munsell table: that table's own length,
 *   [33] BUFFER the vibrance skin curve (65536 floats when [29] is 1),
 *   [34] int Lab-curve flag (1 when chromiLuminanceCurve()'s pass runs for an
 *        enabled Lab curve with contrast at zero),
 *   [35] int L table active, [36] int a table active, [37] int b table active,
 *   [38] int C=f(C) table active, [39] int L=f(C) table active,
 *   [40] int C=f(L) table active,
 *   [41] int Munsell method (RT_LAB_TOOLS_MUNSELL_NONE or _MUN),
 *   [42] int lcredsk,
 *   [43] double 100 - rstprotection, [44] double settings->protectred,
 *   [45] double settings->protectredh, [46] double settings->amchroma,
 *   [47] double the working-profile chroma scale,
 *   [48] BUFFER the L table (32770 floats),
 *   [49] BUFFER the a table, [50] BUFFER the b table,
 *   [51] BUFFER the C=f(C) table, [52] BUFFER the L=f(C) table,
 *   [53] BUFFER the C=f(L) table (65536 floats each),
 *   [54] int L table length, [55] int the other five tables' length,
 *   [56] int local-contrast flag (1 when ImProcFunctions::localContrast()
 *        would run),
 *   [57] int radius, [58] double amount, [59] double darkness,
 *   [60] double lightness, [61] int the block's scale,
 *   [62] PTR host-built RtLabToolsIir coefficients (host-only, never uploaded),
 *   [63] int region count (0 or 1..4), [64] BUFFER the region mask tables,
 *   [65] int their stride, [66] BUFFER the per-region parameters,
 *   [67] and [68] PTR the working and inverse matrices as nine doubles each
 *        (host-only, copied to a device buffer).
 *
 * The pass order is the block's own: LabGrid, then the highlight round, then
 * the shadow round, then local contrast, then the Lab-curve pass, then
 * vibrance, then the regions in region order. Each tool writes the full image,
 * so the work is sequential in that order and no pass can run out of order.
 *
 * A clear support flag refuses before the upload: that is the residual path,
 * and it makes the CPU builtin - the extracted host block - execute and record
 * impl=host. The Lab buffer is read back only after every pass succeeded, so a
 * refused or failed launch leaves the host image untouched for the builtin.
 */
RtStageStatus run_lab_tools(const std::shared_ptr<OpenCLEnv>& env,
                            const std::shared_ptr<OpenCLResidency>& residency,
                            const std::vector<RtStageArg>& args,
                            std::string& error)
{
    constexpr std::size_t ARG_COUNT = 69;
    constexpr int SUBSAMPLING = 4;
    /* guidedfilter.h's epsilon, passed to f_mean() by shadowsHighlights(). */
    constexpr float LAB_TOOLS_EPSILON = 0.075f;
    /* The block's slider ranges; a value outside them is not a device case.
     * The amounts have no geometric limit (their tone table is built on the
     * host), but a negative or absurd value is refused rather than clamped. */
    constexpr int MAX_SLIDER = 100;
    constexpr int MAX_AMOUNT = 10000;
    /* ipvibrance.cc's pastels/saturated range, and its threshold range. */
    constexpr int MIN_VIBRANCE = -100;
    constexpr int MAX_VIBRANCE = 100;

    if (args.size() != ARG_COUNT) {
        error = "lab.tools expects a Lab buffer, a context and its parameters";
        return RT_STAGE_INVALID;
    }

    bool kinds = args[0].kind == RT_STAGE_ARG_BUFFER &&
                 args[1].kind == RT_STAGE_ARG_PTR &&
                 args[2].kind == RT_STAGE_ARG_INT &&
                 args[3].kind == RT_STAGE_ARG_INT;
    for (std::size_t index = 4; index <= 7; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_DOUBLE;
    }
    for (std::size_t index = 8; index <= 15; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_INT;
    }
    for (std::size_t index = 16; index <= 21; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_BUFFER;
    }
    for (std::size_t index = 22; index <= 30; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_INT;
    }
    for (std::size_t index = 31; index <= 33; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_BUFFER;
    }
    for (std::size_t index = 34; index <= 42; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_INT;
    }
    for (std::size_t index = 43; index <= 47; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_DOUBLE;
    }
    for (std::size_t index = 48; index <= 53; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_BUFFER;
    }
    for (std::size_t index = 54; index <= 55; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_INT;
    }
    kinds = kinds && args[56].kind == RT_STAGE_ARG_INT &&
            args[57].kind == RT_STAGE_ARG_INT;
    for (std::size_t index = 58; index <= 60; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_DOUBLE;
    }
    kinds = kinds && args[61].kind == RT_STAGE_ARG_INT &&
            args[62].kind == RT_STAGE_ARG_PTR;
    kinds = kinds && args[63].kind == RT_STAGE_ARG_INT &&
            args[64].kind == RT_STAGE_ARG_BUFFER &&
            args[65].kind == RT_STAGE_ARG_INT &&
            args[66].kind == RT_STAGE_ARG_BUFFER &&
            args[67].kind == RT_STAGE_ARG_PTR &&
            args[68].kind == RT_STAGE_ARG_PTR;
    if (!kinds) {
        error = "lab.tools has an argument of the wrong kind";
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        error = "lab.tools needs a three-plane F32 Lab buffer";
        return RT_STAGE_INVALID;
    }

    const auto* context = static_cast<const RtLabToolsContext*>(args[1].u.ptr);
    if (context == nullptr || context->size != sizeof(RtLabToolsContext) ||
        context->version != RT_LAB_TOOLS_CONTEXT_VERSION ||
        context->magic != RT_LAB_TOOLS_CONTEXT_MAGIC ||
        context->run_host == nullptr) {
        error = "lab.tools has an invalid host context";
        return RT_STAGE_INVALID;
    }

    const std::int64_t supported = args[2].u.i;
    if (supported != 0 && supported != 1) {
        error = "lab.tools support flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    if (supported != 1 || context->supported != 1) {
        error = "lab.tools support flag is not set";
        return RT_STAGE_FALLBACK;
    }

    const std::int64_t labgrid_flag = args[3].u.i;
    if (labgrid_flag != 0 && labgrid_flag != 1) {
        error = "lab.tools LabGrid flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    const bool labgrid = labgrid_flag != 0;
    float labgrid_values[4] = {};
    for (int index = 0; index < 4; ++index) {
        const double value = args[4 + index].u.f;
        if (!std::isfinite(value)) {
            error = "lab.tools has a non-finite LabGrid parameter";
            return RT_STAGE_INVALID;
        }
        labgrid_values[index] = static_cast<float>(value);
    }

    const auto int_arg = [&](std::size_t index, int low, int high) {
        const std::int64_t value = args[index].u.i;
        return value >= low && value <= high ? static_cast<int>(value) : -1;
    };
    const int sh_enabled = int_arg(8, 0, 1);
    const int highlights = int_arg(9, 0, MAX_AMOUNT);
    const int shadows = int_arg(10, 0, MAX_AMOUNT);
    const int htonalwidth = int_arg(11, 0, MAX_SLIDER);
    const int stonalwidth = int_arg(12, 0, MAX_SLIDER);
    const int radius = int_arg(13, 0, MAX_SLIDER);
    const int lab_mode = int_arg(14, 0, 1);
    const int scale = int_arg(15, 1, 1);
    if (sh_enabled < 0 || highlights < 0 || shadows < 0 ||
        htonalwidth < 0 || stonalwidth < 0 || radius < 0 || lab_mode < 0 ||
        scale != 1) {
        error = "lab.tools has a shadows/highlights parameter outside its range";
        return RT_STAGE_INVALID;
    }
    const bool sh_active = sh_enabled != 0 && (highlights > 0 || shadows > 0);

    /*
     * Vibrance. The flag is the call site's labToolsVibranceActive(): it is 1
     * only when the block would touch a pixel, so a vibrance block with both
     * sliders at zero and an identity skin curve stays on the host.
     */
    const std::int64_t vibrance_flag = args[22].u.i;
    if (vibrance_flag != 0 && vibrance_flag != 1) {
        error = "lab.tools vibrance flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    const bool vibrance = vibrance_flag != 0;
    int pastels = 0;
    int saturated = 0;
    int threshold_top = 0;
    int threshold_bottom = 0;
    int protectskins = 0;
    int avoidcolorshift = 0;
    int skin_curve_set = 0;
    int vibrance_highlight = 0;
    if (vibrance) {
        const auto in = [&](std::size_t index, int low, int high) {
            const std::int64_t value = args[index].u.i;
            return value >= low && value <= high ? static_cast<int>(value)
                                                 : low - 1;
        };
        pastels = in(23, MIN_VIBRANCE, MAX_VIBRANCE);
        saturated = in(24, MIN_VIBRANCE, MAX_VIBRANCE);
        threshold_top = in(25, 0, MAX_SLIDER);
        threshold_bottom = in(26, 0, MAX_SLIDER);
        protectskins = in(27, 0, 1);
        avoidcolorshift = in(28, 0, 1);
        skin_curve_set = in(29, 0, 1);
        vibrance_highlight = in(30, 0, 1);
        if (pastels < MIN_VIBRANCE || saturated < MIN_VIBRANCE ||
            threshold_top < 0 || threshold_bottom < 0 || protectskins < 0 ||
            avoidcolorshift < 0 || skin_curve_set < 0 ||
            vibrance_highlight < 0) {
            error = "lab.tools has a vibrance parameter outside its range";
            return RT_STAGE_INVALID;
        }
    }

    /*
     * The Lab-curve pass. `lab_curve` is the call site's
     * labToolsLabCurveActive(): the enabled curve with the residual contrast
     * at zero. The six flags are the six host-built tables' activity, and the
     * method is the block's gamutmunselmethod mapping (NONE or MUN only).
     */
    const std::int64_t lab_curve_flag = args[34].u.i;
    if (lab_curve_flag != 0 && lab_curve_flag != 1) {
        error = "lab.tools Lab-curve flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    const bool lab_curve = lab_curve_flag != 0;
    int lab_curve_l = 0;
    int lab_curve_a = 0;
    int lab_curve_b = 0;
    int lab_curve_cc = 0;
    int lab_curve_lc = 0;
    int lab_curve_cl = 0;
    int lab_curve_method = 0;
    int lcredsk = 0;
    float rst_protection = 0.f;
    float protect_red_raw = 0.f;
    float protect_red_h_raw = 0.f;
    float adjust = 1.f;
    int lcurve_entries = 0;
    int curve_entries = 0;
    if (lab_curve) {
        const auto in = [&](std::size_t index, int low, int high) {
            const std::int64_t value = args[index].u.i;
            return value >= low && value <= high ? static_cast<int>(value)
                                                 : low - 1;
        };
        lab_curve_l = in(35, 0, 1);
        lab_curve_a = in(36, 0, 1);
        lab_curve_b = in(37, 0, 1);
        lab_curve_cc = in(38, 0, 1);
        lab_curve_lc = in(39, 0, 1);
        lab_curve_cl = in(40, 0, 1);
        lcredsk = in(42, 0, 1);
        if (lab_curve_l < 0 || lab_curve_a < 0 || lab_curve_b < 0 ||
            lab_curve_cc < 0 || lab_curve_lc < 0 || lab_curve_cl < 0 ||
            lcredsk < 0) {
            error = "lab.tools has a Lab-curve flag outside its range";
            return RT_STAGE_INVALID;
        }

        const std::int64_t method = args[41].u.i;
        if (method != static_cast<std::int64_t>(RT_LAB_TOOLS_MUNSELL_NONE) &&
            method != static_cast<std::int64_t>(RT_LAB_TOOLS_MUNSELL_MUN)) {
            error = "lab.tools has a Lab-curve Munsell method outside its range";
            return RT_STAGE_INVALID;
        }
        lab_curve_method = static_cast<int>(method);

        const auto double_arg = [&](std::size_t index) {
            const double value = args[index].u.f;
            if (!std::isfinite(value)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return value;
        };
        const double rst = double_arg(43);
        const double protect_red = double_arg(44);
        const double protect_red_h = double_arg(45);
        const double amchroma = double_arg(46);
        const double adjust_value = double_arg(47);
        if (!std::isfinite(rst) || !std::isfinite(protect_red) ||
            !std::isfinite(protect_red_h) || !std::isfinite(amchroma) ||
            !std::isfinite(adjust_value) || rst < 0.0 || rst > 100.0 ||
            protect_red < 0.0 || protect_red > 1000.0 ||
            protect_red_h < 0.0 || protect_red_h > 10.0 ||
            amchroma < 0.0 || amchroma > 10000.0 || adjust_value <= 0.0) {
            error = "lab.tools has a Lab-curve red/skin setting outside its "
                    "range";
            return RT_STAGE_INVALID;
        }
        rst_protection = static_cast<float>(rst);
        protect_red_raw = static_cast<float>(protect_red);
        protect_red_h_raw = static_cast<float>(protect_red_h);
        adjust = static_cast<float>(adjust_value);

        const std::int64_t lcurve_length = args[54].u.i;
        const std::int64_t curve_length = args[55].u.i;
        if (lcurve_length !=
                static_cast<std::int64_t>(RT_LAB_TOOLS_LUMA_LUT_ENTRIES) ||
            curve_length !=
                static_cast<std::int64_t>(RT_LAB_TOOLS_CURVE_LUT_ENTRIES)) {
            error = "lab.tools has a Lab-curve table length outside its range";
            return RT_STAGE_INVALID;
        }
        lcurve_entries = static_cast<int>(lcurve_length);
        curve_entries = static_cast<int>(curve_length);
    }

    /*
     * Local contrast. The flag is the call site's
     * labToolsLocalContrastActive(). The context exports the block's float
     * narrowing of the three sliders and the IIR coefficients gauss.cc derives
     * for the block's sigma; the launcher repeats the range checks so a direct
     * submission cannot feed the kernel a value the predicate would refuse.
     */
    const std::int64_t local_contrast_flag = args[56].u.i;
    if (local_contrast_flag != 0 && local_contrast_flag != 1) {
        error = "lab.tools local contrast flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    const bool local_contrast = local_contrast_flag != 0;
    int lc_radius = 0;
    float lc_amount = 0.f;
    float lc_darkness = 1.f;
    float lc_lightness = 1.f;
    const RtLabToolsIir* lc_iir = nullptr;
    if (local_contrast) {
        const std::int64_t radius = args[57].u.i;
        if (radius < LAB_TOOLS_LC_MIN_RADIUS ||
            radius > LAB_TOOLS_LC_MAX_RADIUS) {
            error = "lab.tools local contrast radius is outside its range";
            return RT_STAGE_INVALID;
        }
        lc_radius = static_cast<int>(radius);

        const auto local_double = [&](std::size_t index, double high) {
            const double value = args[index].u.f;
            if (!std::isfinite(value) || value < 0.0 || value > high) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return value;
        };
        const double amount = local_double(58, LAB_TOOLS_LC_MAX_AMOUNT);
        const double darkness = local_double(59, LAB_TOOLS_LC_MAX_DARKNESS);
        const double lightness =
            local_double(60, LAB_TOOLS_LC_MAX_LIGHTNESS);
        if (!std::isfinite(amount) || !std::isfinite(darkness) ||
            !std::isfinite(lightness)) {
            error = "lab.tools local contrast setting is outside its range";
            return RT_STAGE_INVALID;
        }
        lc_amount = static_cast<float>(amount);
        lc_darkness = static_cast<float>(darkness);
        lc_lightness = static_cast<float>(lightness);

        const std::int64_t scale = args[61].u.i;
        if (scale != LAB_TOOLS_EXPORT_SCALE) {
            error = "lab.tools local contrast scale is not the export scale";
            return RT_STAGE_INVALID;
        }

        lc_iir = static_cast<const RtLabToolsIir*>(args[62].u.ptr);
        if (lc_iir == nullptr || lc_iir->size != sizeof(RtLabToolsIir) ||
            lc_iir->version != RT_LAB_TOOLS_IIR_VERSION) {
            error = "lab.tools has an invalid local-contrast coefficient block";
            return RT_STAGE_INVALID;
        }
        if (lc_iir->double_path > 1u ||
            lc_iir->double_path !=
                (static_cast<double>(lc_radius) >=
                         LAB_TOOLS_LC_DOUBLE_SIGMA
                     ? 1u
                     : 0u)) {
            error = "lab.tools local contrast coefficients use the wrong path";
            return RT_STAGE_INVALID;
        }
        for (const RtLabToolsIirDirection* direction :
             {&lc_iir->horizontal, &lc_iir->vertical}) {
            for (std::size_t index = 0; index < RT_LAB_TOOLS_IIR_TERMS;
                 ++index) {
                if (!std::isfinite(direction->lane[index]) ||
                    !std::isfinite(direction->border[index]) ||
                    !std::isfinite(direction->wide[index])) {
                    error = "lab.tools has a non-finite local-contrast "
                            "coefficient";
                    return RT_STAGE_INVALID;
                }
            }
        }
    }

    /*
     * Lab regions. A nonzero count is the call site's
     * labToolsRegionsActive(): the LabRegions method inside the bounded set.
     * The host ships the mask tables and the per-region floats; the launcher
     * revalidates every value so a direct submission cannot feed the kernel
     * something the predicate would refuse.
     */
    const std::int64_t region_count_arg = args[63].u.i;
    if (region_count_arg < 0 ||
        region_count_arg >
            static_cast<std::int64_t>(RT_LAB_TOOLS_MAX_REGIONS)) {
        error = "lab.tools region count is outside its range";
        return RT_STAGE_INVALID;
    }
    const int region_count = static_cast<int>(region_count_arg);
    const bool regions = region_count > 0;
    int region_lut_stride = 0;
    if (regions) {
        const std::int64_t stride = args[65].u.i;
        if (stride !=
            static_cast<std::int64_t>(RT_LAB_TOOLS_REGION_LUT_STRIDE)) {
            error = "lab.tools region table length is outside its range";
            return RT_STAGE_INVALID;
        }
        region_lut_stride = static_cast<int>(stride);

        const RtStageBuffer& luts = args[64].u.buffer;
        const std::int64_t expected_luts =
            static_cast<std::int64_t>(region_count) *
            static_cast<std::int64_t>(RT_LAB_TOOLS_REGION_CURVES) *
            static_cast<std::int64_t>(region_lut_stride);
        if (!valid(luts) || luts.format != RT_STAGE_F32 ||
            luts.planes != 1 || luts.height != 1 ||
            luts.width != expected_luts) {
            error = "lab.tools needs the region mask tables";
            return RT_STAGE_INVALID;
        }
        const RtStageBuffer& table = args[66].u.buffer;
        const std::int64_t expected_params =
            static_cast<std::int64_t>(region_count) *
            static_cast<std::int64_t>(RT_LAB_TOOLS_REGION_PARAM_STRIDE);
        if (!valid(table) || table.format != RT_STAGE_F32 ||
            table.planes != 1 || table.height != 1 ||
            table.width != expected_params) {
            error = "lab.tools needs the region parameter table";
            return RT_STAGE_INVALID;
        }
        if (args[67].u.ptr == nullptr || args[68].u.ptr == nullptr) {
            error = "lab.tools needs the region working matrices";
            return RT_STAGE_INVALID;
        }
        const float* values = static_cast<const float*>(table.data);
        for (int region = 0; region < region_count; ++region) {
            const float* p =
                values + region * RT_LAB_TOOLS_REGION_PARAM_STRIDE;
            for (std::size_t slot = 0;
                 slot < RT_LAB_TOOLS_REGION_PARAM_STRIDE; ++slot) {
                if (!std::isfinite(p[slot])) {
                    error = "lab.tools has a non-finite region parameter";
                    return RT_STAGE_INVALID;
                }
            }
            const int channel = static_cast<int>(p[RT_LAB_TOOLS_REGION_CHANNEL]);
            const float radius_ab = p[RT_LAB_TOOLS_REGION_RADIUS_AB];
            const float radius_l = p[RT_LAB_TOOLS_REGION_RADIUS_L];
            if (channel < -1 || channel > 2 ||
                p[RT_LAB_TOOLS_REGION_RS] < 0.0f ||
                p[RT_LAB_TOOLS_REGION_RS] > 3.0f ||
                p[RT_LAB_TOOLS_REGION_SLOPE] <= 0.0f ||
                p[RT_LAB_TOOLS_REGION_POWER] <= 0.0f ||
                radius_ab < 1.0f || radius_l < 1.0f ||
                radius_ab != std::floor(radius_ab) ||
                radius_l != std::floor(radius_l)) {
                error = "lab.tools has a region parameter outside its range";
                return RT_STAGE_INVALID;
            }
            const std::size_t active_slots[] = {
                static_cast<std::size_t>(RT_LAB_TOOLS_REGION_HUE_ACTIVE),
                static_cast<std::size_t>(RT_LAB_TOOLS_REGION_CHROMA_ACTIVE),
                static_cast<std::size_t>(RT_LAB_TOOLS_REGION_LIGHT_ACTIVE)};
            for (std::size_t slot : active_slots) {
                if (p[slot] != 0.0f && p[slot] != 1.0f) {
                    error = "lab.tools has a region mask flag outside its "
                            "range";
                    return RT_STAGE_INVALID;
                }
            }

            const float* tables =
                static_cast<const float*>(luts.data);
            for (int curve = 0;
                 curve < static_cast<int>(RT_LAB_TOOLS_REGION_CURVES);
                 ++curve) {
                if (p[active_slots[curve]] == 0.0f) {
                    continue;
                }
                const float* table =
                    tables +
                    (region * RT_LAB_TOOLS_REGION_CURVES + curve) *
                        region_lut_stride;
                const int nodes = static_cast<int>(table[0]);
                if (nodes < 2 ||
                    nodes >
                        static_cast<int>(RT_LAB_TOOLS_REGION_LUT_NODES)) {
                    error = "lab.tools has a region table with a bad node "
                            "count";
                    return RT_STAGE_INVALID;
                }
                const float* xs = table + 1;
                const float* ys = table + 1 + nodes;
                for (int node = 0; node < nodes; ++node) {
                    if (!std::isfinite(xs[node]) ||
                        !std::isfinite(ys[node]) ||
                        (node > 0 && xs[node] <= xs[node - 1])) {
                        error = "lab.tools has a malformed region table";
                        return RT_STAGE_INVALID;
                    }
                }
            }
        }
    }

    if (!labgrid && !sh_active && !vibrance && !lab_curve &&
        !local_contrast && !regions) {
        error = "lab.tools has no enabled tool";
        return RT_STAGE_FALLBACK;
    }

    /*
     * Vibrance, the Lab curves and local contrast need the host's correctly
     * rounded divide and sqrt at every branch decision. A GPU OpenCL
     * implementation may round one of them differently, which moves a pixel
     * whose hue sits on a Munsell zone limit to the other side of the gate;
     * the Radeon RX 550 was measured moving two vibrance pixels by up to
     * 2,329 LSB16 on the PEF chart. Local contrast's IIR recurrence would
     * likewise take a different boundary path. The node therefore runs all
     * three only on a CPU OpenCL device (POCL), the deterministic device the
     * project uses for parity, and records impl=host elsewhere.
     */
    if (vibrance || lab_curve || local_contrast || regions) {
        cl_device_type device_type = 0;
        if (env->api->GetDeviceInfo(env->device, CL_DEVICE_TYPE,
                                    sizeof(device_type), &device_type,
                                    nullptr) != CL_SUCCESS ||
            device_type != CL_DEVICE_TYPE_CPU) {
            error = "lab.tools needs a deterministic CPU device";
            return RT_STAGE_FALLBACK;
        }
    }

    /*
     * The Lab-curve kernel repeats the host's double-precision square root and
     * divide, and local contrast repeats gaussianBlur()'s double IIR
     * recurrence, so both need an fp64 device. A device without fp64 cannot
     * build either program; the pass stays on the host.
     */
    if (lab_curve || local_contrast || regions) {
        cl_device_fp_config fp_config = 0;
        if (env->api->GetDeviceInfo(env->device, CL_DEVICE_DOUBLE_FP_CONFIG,
                                    sizeof(fp_config), &fp_config,
                                    nullptr) != CL_SUCCESS ||
            fp_config == 0) {
            error = "lab.tools needs device fp64";
            return RT_STAGE_FALLBACK;
        }
    }

    /*
     * When the a and b tables are inactive the host feeds sincosval from the
     * SSE2 precompute, which is single precision; the kernel's first chroma
     * follows that path. A host built without SSE2 (and without RT_SIMDE)
     * would compute the scalar double value instead, so the pass is refused
     * rather than diverging from the block.
     */
#if !defined(__SSE2__) && !defined(RT_SIMDE)
    if (lab_curve) {
        error = "lab.tools Lab curves need the host's SSE2 precompute";
        return RT_STAGE_FALLBACK;
    }
    /*
     * Local contrast below sigma 25 is the SSE float path, and the exported
     * coefficients describe that path. A host built without SSE2 (and without
     * RT_SIMDE) runs gaussianBlur()'s double path for the same sigma, so the
     * device pass would diverge; keep the block on the host. At sigma 25 and
     * above both sides use the double path and the pass stays available.
     */
    if (local_contrast && lc_iir->double_path == 0) {
        error = "lab.tools local contrast needs the host's SSE2 blur path";
        return RT_STAGE_FALLBACK;
    }
#endif

    const RtStageBuffer& lut_highlight = args[16].u.buffer;
    const RtStageBuffer& lut_shadow = args[17].u.buffer;
    const RtStageBuffer& ws = args[18].u.buffer;
    const RtStageBuffer& iws = args[19].u.buffer;
    const RtStageBuffer& cachef = args[20].u.buffer;
    const RtStageBuffer& cachefy = args[21].u.buffer;

    const std::uint32_t lut_entries = lab_mode != 0
                                          ? RT_LAB_TOOLS_LAB_LUT_ENTRIES
                                          : RT_LAB_TOOLS_RGB_LUT_ENTRIES;
    for (const RtStageBuffer* table : {&lut_highlight, &lut_shadow}) {
        if (!valid(*table) || table->format != RT_STAGE_F32 ||
            table->planes != 1 || table->height != 1 ||
            static_cast<std::uint32_t>(table->width) != lut_entries) {
            error = "lab.tools needs two tone tables of the mode's length";
            return RT_STAGE_INVALID;
        }
    }
    for (const RtStageBuffer* matrix : {&ws, &iws}) {
        if (!valid(*matrix) || matrix->format != RT_STAGE_F32 ||
            matrix->planes != 1 || matrix->width != 9 ||
            matrix->height != 1) {
            error = "lab.tools needs two working matrices of nine floats";
            return RT_STAGE_INVALID;
        }
    }
    for (const RtStageBuffer* cache : {&cachef, &cachefy}) {
        if (!valid(*cache) || cache->format != RT_STAGE_F32 ||
            cache->planes != 1 || cache->height != 1 ||
            static_cast<std::uint32_t>(cache->width) !=
                RT_LAB_TOOLS_CACHE_ENTRIES) {
            error = "lab.tools needs the two Lab caches";
            return RT_STAGE_INVALID;
        }
    }

    const RtStageBuffer& munsell = args[31].u.buffer;
    const RtStageBuffer& munsell_sizes = args[32].u.buffer;
    const RtStageBuffer& skin_curve = args[33].u.buffer;
    const bool lab_curve_munsell =
        lab_curve && lab_curve_method == static_cast<int>(RT_LAB_TOOLS_MUNSELL_MUN);
    if ((vibrance && avoidcolorshift != 0) || lab_curve_munsell) {
        const std::uint32_t flats =
            RT_LAB_TOOLS_MUNSELL_TABLES * RT_LAB_TOOLS_MUNSELL_STRIDE;
        if (!valid(munsell) || munsell.format != RT_STAGE_F32 ||
            munsell.planes != 1 || munsell.height != 1 ||
            static_cast<std::uint32_t>(munsell.width) != flats ||
            !valid(munsell_sizes) ||
            munsell_sizes.format != RT_STAGE_F32 ||
            munsell_sizes.planes != 1 || munsell_sizes.height != 1 ||
            static_cast<std::uint32_t>(munsell_sizes.width) !=
                RT_LAB_TOOLS_MUNSELL_TABLES) {
            error = "lab.tools needs the flat Munsell table set";
            return RT_STAGE_INVALID;
        }
    }
    if (vibrance) {
        if (!valid(skin_curve) || skin_curve.format != RT_STAGE_F32 ||
            skin_curve.planes != 1 || skin_curve.height != 1 ||
            (skin_curve_set != 0 &&
             static_cast<std::uint32_t>(skin_curve.width) !=
                 RT_LAB_TOOLS_SKIN_CURVE_ENTRIES) ||
            (skin_curve_set == 0 && skin_curve.width < 1)) {
            error = "lab.tools needs the vibrance skin curve";
            return RT_STAGE_INVALID;
        }
    }

    /*
     * The six host-built Lab-curve tables. The L table is 32770 entries long
     * (LUTf::operator[](float) reads index 32768 and its neighbour); the a, b,
     * C=f(C), L=f(C) and C=f(L) tables are the block's 65536-entry diagonal
     * tables.
     */
    const RtStageBuffer& lcurve = args[48].u.buffer;
    const RtStageBuffer& acurve = args[49].u.buffer;
    const RtStageBuffer& bcurve = args[50].u.buffer;
    const RtStageBuffer& satcurve = args[51].u.buffer;
    const RtStageBuffer& lhskcurve = args[52].u.buffer;
    const RtStageBuffer& clcurve = args[53].u.buffer;
    if (lab_curve) {
        const RtStageBuffer* tables[] = {&lcurve, &acurve, &bcurve,
                                         &satcurve, &lhskcurve, &clcurve};
        for (std::size_t index = 0; index < 6; ++index) {
            const std::uint32_t expected =
                index == 0
                    ? static_cast<std::uint32_t>(lcurve_entries)
                    : static_cast<std::uint32_t>(curve_entries);
            if (!valid(*tables[index]) ||
                tables[index]->format != RT_STAGE_F32 ||
                tables[index]->planes != 1 || tables[index]->height != 1 ||
                static_cast<std::uint32_t>(tables[index]->width) != expected) {
                error = "lab.tools needs the six Lab-curve tables";
                return RT_STAGE_INVALID;
            }
        }
    }

    const int width = lab.width;
    const int height = lab.height;
    if (lab.row_pitch % sizeof(float) != 0 ||
        lab.plane_pitch < lab.row_pitch * static_cast<std::size_t>(height)) {
        error = "lab.tools Lab buffer pitch does not cover the image";
        return RT_STAGE_INVALID;
    }
    /*
     * The region pass picks guidedFilter()'s default subsampling from the
     * image size and each radius. The buffers are sized for the smallest
     * subsampling any pass needs, so the shadows/highlights explicit
     * subsampling of four and every region radius share one set of scratch
     * planes.
     */
    std::vector<int> region_radius_ab;
    std::vector<int> region_radius_l;
    std::vector<int> region_subsampling_ab;
    std::vector<int> region_subsampling_l;
    int sub_subsampling = SUBSAMPLING;
    if (regions) {
        const float* values =
            static_cast<const float*>(args[66].u.buffer.data);
        for (int region = 0; region < region_count; ++region) {
            const float* p =
                values + region * RT_LAB_TOOLS_REGION_PARAM_STRIDE;
            const int radius_ab =
                static_cast<int>(p[RT_LAB_TOOLS_REGION_RADIUS_AB]);
            const int radius_l =
                static_cast<int>(p[RT_LAB_TOOLS_REGION_RADIUS_L]);
            const int subsampling_ab =
                labToolsRegionSubsampling(width, height, radius_ab);
            const int subsampling_l =
                labToolsRegionSubsampling(width, height, radius_l);
            region_radius_ab.push_back(radius_ab);
            region_radius_l.push_back(radius_l);
            region_subsampling_ab.push_back(subsampling_ab);
            region_subsampling_l.push_back(subsampling_l);
            sub_subsampling = std::min(
                sub_subsampling, std::min(subsampling_ab, subsampling_l));
        }
    }
    const int sw = width / sub_subsampling;
    const int sh = height / sub_subsampling;
    if (sw < 3 || sh < 3) {
        error = "lab.tools image is too small for the guided filter";
        return RT_STAGE_FALLBACK;
    }
    /* gaussianBlur()'s recurrence reads the last three samples and seeds from
     * three states, so a line shorter than four would be out of bounds. */
    if (local_contrast && (width < 4 || height < 4)) {
        error = "lab.tools local contrast needs at least four rows and columns";
        return RT_STAGE_FALLBACK;
    }

    const KernelDef* def = find_kernel("lab.tools");
    assert(def != nullptr);
    cl_kernel mask_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_mask", error);
    cl_kernel subsample_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_subsample", error);
    cl_kernel box_h_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_box_h", error);
    cl_kernel box_v_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_box_v", error);
    cl_kernel combine_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_combine", error);
    cl_kernel upsample_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_upsample", error);
    cl_kernel apply_kernel = compiled_kernel_entry(
        env, def,
        lab_mode != 0 ? "rt_lab_tools_apply" : "rt_lab_tools_apply_rgb",
        error);
    cl_kernel labgrid_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_labgrid", error);
    cl_kernel vibrance_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_vibrance", error);
    const KernelDef* curve_def = find_kernel("lab.tools.labcurve");
    assert(curve_def != nullptr);
    cl_kernel lab_curve_kernel =
        lab_curve ? compiled_kernel_entry(env, curve_def,
                                          "rt_lab_tools_lab_curve", error)
                  : nullptr;
    const KernelDef* lc_def = find_kernel("lab.tools.localcontrast");
    assert(lc_def != nullptr);
    cl_kernel lc_line_kernel =
        local_contrast
            ? compiled_kernel_entry(env, lc_def, "rt_lab_tools_lc_line", error)
            : nullptr;
    cl_kernel lc_blend_kernel =
        local_contrast ? compiled_kernel_entry(env, lc_def,
                                               "rt_lab_tools_lc_blend", error)
                       : nullptr;
    const KernelDef* region_def = find_kernel("lab.tools.regions");
    assert(region_def != nullptr);
    cl_kernel region_snapshot_kernel =
        regions ? compiled_kernel_entry(env, region_def,
                                        "rt_lab_tools_region_snapshot",
                                        error)
                : nullptr;
    cl_kernel region_mask_kernel =
        regions ? compiled_kernel_entry(env, region_def,
                                        "rt_lab_tools_region_mask", error)
                : nullptr;
    cl_kernel region_upsample_kernel =
        regions ? compiled_kernel_entry(env, region_def,
                                        "rt_lab_tools_region_upsample",
                                        error)
                : nullptr;
    cl_kernel region_apply_kernel =
        regions ? compiled_kernel_entry(env, region_def,
                                        "rt_lab_tools_region_apply", error)
                : nullptr;
    if (mask_kernel == nullptr || subsample_kernel == nullptr ||
        box_h_kernel == nullptr || box_v_kernel == nullptr ||
        combine_kernel == nullptr || upsample_kernel == nullptr ||
        apply_kernel == nullptr || labgrid_kernel == nullptr ||
        vibrance_kernel == nullptr || (lab_curve && lab_curve_kernel == nullptr) ||
        (local_contrast && (lc_line_kernel == nullptr ||
                            lc_blend_kernel == nullptr)) ||
        (regions && (region_snapshot_kernel == nullptr ||
                     region_mask_kernel == nullptr ||
                     region_upsample_kernel == nullptr ||
                     region_apply_kernel == nullptr))) {
        return RT_STAGE_FALLBACK;
    }

    const StageBufferShape lab_shape{RT_STAGE_F32, width, height, 3};
    const StageBufferShape full_shape{RT_STAGE_F32, width, height, 2};
    /* guidedFilter()'s five subsampled arrays. Each is its own buffer because
     * the residency admits at most three planes, and separate buffers keep the
     * kernel arguments explicit. */
    const StageBufferShape sub_shape{RT_STAGE_F32, sw, sh, 1};
    StageDeviceBufferPtr lab_device =
        residency->acquire(lab.data, lab_shape, &error);
    char full_key = 0;
    char i1_key = 0;
    char p1_key = 0;
    char meani_key = 0;
    char meanp_key = 0;
    char temp_key = 0;
    char hl_key = 0;
    char sh_key = 0;
    char ws_key = 0;
    char iws_key = 0;
    char cachef_key = 0;
    char cachefy_key = 0;
    char munsell_key = 0;
    char munsell_sizes_key = 0;
    char skin_curve_key = 0;
    StageDeviceBufferPtr full =
        residency->acquire(&full_key, full_shape, &error);
    StageDeviceBufferPtr sub_i1 =
        residency->acquire(&i1_key, sub_shape, &error);
    StageDeviceBufferPtr sub_p1 =
        residency->acquire(&p1_key, sub_shape, &error);
    StageDeviceBufferPtr sub_meani =
        residency->acquire(&meani_key, sub_shape, &error);
    StageDeviceBufferPtr sub_meanp =
        residency->acquire(&meanp_key, sub_shape, &error);
    StageDeviceBufferPtr sub_temp =
        residency->acquire(&temp_key, sub_shape, &error);
    const StageBufferShape lut_shape{
        RT_STAGE_F32, static_cast<std::int32_t>(lut_entries), 1, 1};
    const StageBufferShape matrix_shape{RT_STAGE_F32, 9, 1, 1};
    const StageBufferShape cache_shape{
        RT_STAGE_F32, static_cast<std::int32_t>(RT_LAB_TOOLS_CACHE_ENTRIES),
        1, 1};
    StageDeviceBufferPtr lut_highlight_device =
        residency->acquire(&hl_key, lut_shape, &error);
    StageDeviceBufferPtr lut_shadow_device =
        residency->acquire(&sh_key, lut_shape, &error);
    StageDeviceBufferPtr ws_device =
        residency->acquire(&ws_key, matrix_shape, &error);
    StageDeviceBufferPtr iws_device =
        residency->acquire(&iws_key, matrix_shape, &error);
    StageDeviceBufferPtr cachef_device =
        residency->acquire(&cachef_key, cache_shape, &error);
    StageDeviceBufferPtr cachefy_device =
        residency->acquire(&cachefy_key, cache_shape, &error);

    /* Vibrance's three host-built tables, acquired only when vibrance runs. */
    const StageBufferShape munsell_shape{
        RT_STAGE_F32,
        static_cast<std::int32_t>(RT_LAB_TOOLS_MUNSELL_TABLES *
                                  RT_LAB_TOOLS_MUNSELL_STRIDE),
        1, 1};
    const StageBufferShape munsell_sizes_shape{
        RT_STAGE_F32,
        static_cast<std::int32_t>(RT_LAB_TOOLS_MUNSELL_TABLES), 1, 1};
    /* The host passes one float when the curve is the identity, which the
     * kernel never reads; the buffer shape follows that width. */
    const StageBufferShape skin_curve_shape{
        RT_STAGE_F32,
        skin_curve_set != 0
            ? static_cast<std::int32_t>(RT_LAB_TOOLS_SKIN_CURVE_ENTRIES)
            : 1,
        1, 1};
    StageDeviceBufferPtr munsell_device;
    StageDeviceBufferPtr munsell_sizes_device;
    StageDeviceBufferPtr skin_curve_device;
    if (vibrance || lab_curve_munsell) {
        munsell_device = residency->acquire(&munsell_key, munsell_shape, &error);
        munsell_sizes_device =
            residency->acquire(&munsell_sizes_key, munsell_sizes_shape, &error);
    }
    if (vibrance) {
        skin_curve_device =
            residency->acquire(&skin_curve_key, skin_curve_shape, &error);
    }

    /* The six Lab-curve tables, acquired only when the pass runs. */
    char lcurve_key = 0;
    char acurve_key = 0;
    char bcurve_key = 0;
    char satcurve_key = 0;
    char lhskcurve_key = 0;
    char clcurve_key = 0;
    const StageBufferShape lcurve_shape{
        RT_STAGE_F32, static_cast<std::int32_t>(lcurve_entries), 1, 1};
    const StageBufferShape curve_shape{
        RT_STAGE_F32, static_cast<std::int32_t>(curve_entries), 1, 1};
    StageDeviceBufferPtr lcurve_device;
    StageDeviceBufferPtr acurve_device;
    StageDeviceBufferPtr bcurve_device;
    StageDeviceBufferPtr satcurve_device;
    StageDeviceBufferPtr lhskcurve_device;
    StageDeviceBufferPtr clcurve_device;
    if (lab_curve) {
        lcurve_device = residency->acquire(&lcurve_key, lcurve_shape, &error);
        acurve_device = residency->acquire(&acurve_key, curve_shape, &error);
        bcurve_device = residency->acquire(&bcurve_key, curve_shape, &error);
        satcurve_device =
            residency->acquire(&satcurve_key, curve_shape, &error);
        lhskcurve_device =
            residency->acquire(&lhskcurve_key, curve_shape, &error);
        clcurve_device = residency->acquire(&clcurve_key, curve_shape, &error);
    }

    /*
     * Local contrast's two image-sized planes: the horizontal pass writes the
     * first and the vertical pass the second, which becomes the blur the blend
     * reads. The residency refuses them over budget, which falls back cleanly.
     */
    char lc_h_key = 0;
    char lc_v_key = 0;
    const StageBufferShape lc_shape{RT_STAGE_F32, width, height, 1};
    StageDeviceBufferPtr lc_h_device;
    StageDeviceBufferPtr lc_v_device;
    if (local_contrast) {
        lc_h_device = residency->acquire(&lc_h_key, lc_shape, &error);
        lc_v_device = residency->acquire(&lc_v_key, lc_shape, &error);
    }

    /*
     * The region pass keeps a snapshot of the Lab as it stands when the
     * regions pass starts (after every preceding tool), for every region's
     * mask, the two filtered masks, and the host-built tables.
     */
    char snapshot_key = 0;
    char region_masks_key = 0;
    char region_luts_key = 0;
    char region_params_key = 0;
    const StageBufferShape region_masks_shape{RT_STAGE_F32, width, height, 2};
    const StageBufferShape region_luts_shape{
        RT_STAGE_F32,
        region_count * static_cast<int>(RT_LAB_TOOLS_REGION_CURVES) *
            region_lut_stride,
        1, 1};
    const StageBufferShape region_params_shape{
        RT_STAGE_F32,
        region_count *
            static_cast<int>(RT_LAB_TOOLS_REGION_PARAM_STRIDE),
        1, 1};
    StageDeviceBufferPtr snapshot_device;
    StageDeviceBufferPtr region_masks_device;
    StageDeviceBufferPtr region_luts_device;
    StageDeviceBufferPtr region_params_device;
    if (regions) {
        snapshot_device = residency->acquire(&snapshot_key, lab_shape, &error);
        region_masks_device =
            residency->acquire(&region_masks_key, region_masks_shape, &error);
        region_luts_device =
            residency->acquire(&region_luts_key, region_luts_shape, &error);
        region_params_device = residency->acquire(
            &region_params_key, region_params_shape, &error);
    }
    if (!lab_device || !full || !sub_i1 || !sub_p1 || !sub_meani ||
        !sub_meanp || !sub_temp || !lut_highlight_device ||
        !lut_shadow_device || !ws_device || !iws_device || !cachef_device ||
        !cachefy_device ||
        ((vibrance || lab_curve_munsell) &&
         (!munsell_device || !munsell_sizes_device)) ||
        (vibrance && !skin_curve_device) ||
        (lab_curve &&
         (!lcurve_device || !acurve_device || !bcurve_device ||
          !satcurve_device || !lhskcurve_device || !clcurve_device)) ||
        (local_contrast && (!lc_h_device || !lc_v_device)) ||
        (regions && (!snapshot_device || !region_masks_device ||
                     !region_luts_device || !region_params_device))) {
        return RT_STAGE_FALLBACK;
    }

    const auto upload = [&](const StageDeviceBufferPtr& buffer,
                            const RtStageBuffer& host, const char* label) {
        if (residency->upload(buffer, host) != RT_STAGE_OK) {
            error = std::string("lab.tools setup upload failed: ") + label;
            return false;
        }
        return true;
    };
    if (!upload(lab_device, lab, "lab") ||
        !upload(lut_highlight_device, lut_highlight, "highlight") ||
        !upload(lut_shadow_device, lut_shadow, "shadow") ||
        !upload(ws_device, ws, "ws") || !upload(iws_device, iws, "iws") ||
        !upload(cachef_device, cachef, "cachef") ||
        !upload(cachefy_device, cachefy, "cachefy") ||
        ((vibrance || lab_curve_munsell) &&
         (!upload(munsell_device, munsell, "munsell") ||
          !upload(munsell_sizes_device, munsell_sizes, "munsell sizes"))) ||
        (vibrance && !upload(skin_curve_device, skin_curve, "skin curve")) ||
        (lab_curve &&
         (!upload(lcurve_device, lcurve, "lcurve") ||
          !upload(acurve_device, acurve, "acurve") ||
          !upload(bcurve_device, bcurve, "bcurve") ||
          !upload(satcurve_device, satcurve, "satcurve") ||
          !upload(lhskcurve_device, lhskcurve, "lhskcurve") ||
          !upload(clcurve_device, clcurve, "clcurve"))) ||
        (regions &&
         (!upload(region_luts_device, args[64].u.buffer, "region tables") ||
          !upload(region_params_device, args[66].u.buffer,
                  "region parameters")))) {
        return RT_STAGE_FALLBACK;
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };
    const auto pitch_of = [](const StageDeviceBufferPtr& buffer) {
        return buffer->row_pitch() / sizeof(float);
    };
    const auto plane_pitch_of = [](const StageDeviceBufferPtr& buffer) {
        return buffer->plane_pitch() / sizeof(float);
    };

    const StageDeviceBufferPtr buffers[] = {
        lab_device, full, sub_i1, sub_p1, sub_meani, sub_meanp, sub_temp,
        lut_highlight_device, lut_shadow_device, ws_device, iws_device,
        cachef_device, cachefy_device};
    const StageDeviceBufferPtr region_buffers[] = {
        snapshot_device, region_masks_device, region_luts_device,
        region_params_device};
    for (const StageDeviceBufferPtr& buffer : region_buffers) {
        if (buffer == nullptr) {
            continue;
        }
        if (buffer->row_pitch() % sizeof(float) != 0 ||
            buffer->plane_pitch() % sizeof(float) != 0 ||
            pitch_of(buffer) <
                static_cast<std::size_t>(buffer->shape().width) ||
            plane_pitch_of(buffer) <
                static_cast<std::size_t>(buffer->shape().height) *
                    pitch_of(buffer)) {
            error = "lab.tools region buffer pitch does not cover its table";
            return RT_STAGE_FALLBACK;
        }
    }
    for (const StageDeviceBufferPtr& buffer : buffers) {
        if (buffer->row_pitch() % sizeof(float) != 0 ||
            buffer->plane_pitch() % sizeof(float) != 0 ||
            pitch_of(buffer) <
                static_cast<std::size_t>(buffer->shape().width) ||
            plane_pitch_of(buffer) <
                static_cast<std::size_t>(buffer->shape().height) *
                    pitch_of(buffer)) {
            error = "lab.tools row or plane pitch does not cover the image";
            return RT_STAGE_FALLBACK;
        }
    }
    for (const StageDeviceBufferPtr* buffer :
         {&munsell_device, &munsell_sizes_device, &skin_curve_device,
          &lcurve_device, &acurve_device, &bcurve_device, &satcurve_device,
          &lhskcurve_device, &clcurve_device}) {
        if (*buffer == nullptr) {
            continue;
        }
        if ((*buffer)->row_pitch() % sizeof(float) != 0 ||
            pitch_of(*buffer) <
                static_cast<std::size_t>((*buffer)->shape().width)) {
            error = "lab.tools table pitch does not cover the table";
            return RT_STAGE_FALLBACK;
        }
    }
    for (const StageDeviceBufferPtr* buffer : {&lc_h_device, &lc_v_device}) {
        if (*buffer == nullptr) {
            continue;
        }
        if ((*buffer)->row_pitch() % sizeof(float) != 0 ||
            (*buffer)->plane_pitch() % sizeof(float) != 0 ||
            pitch_of(*buffer) < static_cast<std::size_t>(width) ||
            plane_pitch_of(*buffer) <
                static_cast<std::size_t>(height) * pitch_of(*buffer)) {
            error = "lab.tools local-contrast plane pitch does not cover the "
                    "image";
            return RT_STAGE_FALLBACK;
        }
    }

    cl_mem lab_memory = memory_of(lab_device);
    cl_mem full_memory = memory_of(full);
    cl_mem i1_memory = memory_of(sub_i1);
    cl_mem p1_memory = memory_of(sub_p1);
    cl_mem meani_memory = memory_of(sub_meani);
    cl_mem meanp_memory = memory_of(sub_meanp);
    cl_mem temp_memory = memory_of(sub_temp);
    cl_mem hl_memory = memory_of(lut_highlight_device);
    cl_mem sh_memory = memory_of(lut_shadow_device);
    cl_mem ws_memory = memory_of(ws_device);
    cl_mem iws_memory = memory_of(iws_device);
    cl_mem cachef_memory = memory_of(cachef_device);
    cl_mem cachefy_memory = memory_of(cachefy_device);
    cl_mem munsell_memory =
        (vibrance || lab_curve_munsell) ? memory_of(munsell_device) : nullptr;
    cl_mem munsell_sizes_memory =
        (vibrance || lab_curve_munsell) ? memory_of(munsell_sizes_device)
                                        : nullptr;
    cl_mem skin_curve_memory =
        vibrance ? memory_of(skin_curve_device) : nullptr;
    cl_mem lcurve_memory = lab_curve ? memory_of(lcurve_device) : nullptr;
    cl_mem acurve_memory = lab_curve ? memory_of(acurve_device) : nullptr;
    cl_mem bcurve_memory = lab_curve ? memory_of(bcurve_device) : nullptr;
    cl_mem satcurve_memory = lab_curve ? memory_of(satcurve_device) : nullptr;
    cl_mem lhskcurve_memory =
        lab_curve ? memory_of(lhskcurve_device) : nullptr;
    cl_mem clcurve_memory = lab_curve ? memory_of(clcurve_device) : nullptr;
    cl_mem lc_h_memory = local_contrast ? memory_of(lc_h_device) : nullptr;
    cl_mem lc_v_memory = local_contrast ? memory_of(lc_v_device) : nullptr;
    cl_mem snapshot_memory = regions ? memory_of(snapshot_device) : nullptr;
    cl_mem region_masks_memory =
        regions ? memory_of(region_masks_device) : nullptr;
    cl_mem region_luts_memory =
        regions ? memory_of(region_luts_device) : nullptr;
    cl_mem region_params_memory =
        regions ? memory_of(region_params_device) : nullptr;
    if (lab_memory == nullptr || full_memory == nullptr ||
        i1_memory == nullptr || p1_memory == nullptr ||
        meani_memory == nullptr || meanp_memory == nullptr ||
        temp_memory == nullptr || hl_memory == nullptr ||
        sh_memory == nullptr || ws_memory == nullptr ||
        iws_memory == nullptr || cachef_memory == nullptr ||
        cachefy_memory == nullptr ||
        ((vibrance || lab_curve_munsell) &&
         (munsell_memory == nullptr || munsell_sizes_memory == nullptr)) ||
        (vibrance && skin_curve_memory == nullptr) ||
        (lab_curve &&
         (lcurve_memory == nullptr || acurve_memory == nullptr ||
          bcurve_memory == nullptr || satcurve_memory == nullptr ||
          lhskcurve_memory == nullptr || clcurve_memory == nullptr)) ||
        (local_contrast && (lc_h_memory == nullptr || lc_v_memory == nullptr)) ||
        (regions && (snapshot_memory == nullptr ||
                     region_masks_memory == nullptr ||
                     region_luts_memory == nullptr ||
                     region_params_memory == nullptr))) {
        error = "lab.tools could not address its device buffers";
        return RT_STAGE_FALLBACK;
    }

    const cl_int cl_width = width;
    const cl_int cl_height = height;
    /* shadows/highlights passes an explicit subsampling of four. */
    const cl_int cl_sw_sh = width / SUBSAMPLING;
    const cl_int cl_sh_sh = height / SUBSAMPLING;
    const cl_int lab_pitch = static_cast<cl_int>(pitch_of(lab_device));
    const cl_int lab_plane = static_cast<cl_int>(plane_pitch_of(lab_device));
    const cl_int full_pitch = static_cast<cl_int>(pitch_of(full));
    const cl_int full_plane = static_cast<cl_int>(plane_pitch_of(full));
    const cl_int region_mask_pitch =
        regions ? static_cast<cl_int>(pitch_of(region_masks_device)) : 0;
    const cl_int region_mask_plane =
        regions ? static_cast<cl_int>(plane_pitch_of(region_masks_device)) : 0;
    const cl_int sub_pitch = static_cast<cl_int>(pitch_of(sub_i1));
    /* The five subsampled buffers are allocated from the same shape, so the
     * layout helper gives them one pitch; refuse rather than mix layouts. */
    if (pitch_of(sub_p1) != pitch_of(sub_i1) ||
        pitch_of(sub_meani) != pitch_of(sub_i1) ||
        pitch_of(sub_meanp) != pitch_of(sub_i1) ||
        pitch_of(sub_temp) != pitch_of(sub_i1)) {
        error = "lab.tools subsampled buffers have different pitches";
        return RT_STAGE_FALLBACK;
    }
    const cl_int lut_size = static_cast<cl_int>(lut_entries);
    if (lab_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        lab_plane > std::numeric_limits<cl_int>::max() / 3 ||
        full_pitch > std::numeric_limits<cl_int>::max() / 3 ||
        full_plane > std::numeric_limits<cl_int>::max() / 3 ||
        region_mask_pitch > std::numeric_limits<cl_int>::max() / 2 ||
        region_mask_plane > std::numeric_limits<cl_int>::max() / 2 ||
        sub_pitch > std::numeric_limits<cl_int>::max() / 2 ||
        sw > std::numeric_limits<cl_int>::max() / 2 ||
        sh > std::numeric_limits<cl_int>::max() / 2) {
        error = "lab.tools pitch does not fit the kernel's int";
        return RT_STAGE_FALLBACK;
    }

    const auto set_arg = [&](cl_kernel kernel, cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };
    const auto launch = [&](cl_kernel kernel, const std::size_t* global,
                            const char* label) {
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr,
                                           global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            error = std::string(label) + " launch failed";
            return false;
        }
        const cl_int wait_status = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        if (wait_status != CL_SUCCESS) {
            error = std::string(label) + " did not complete";
            return false;
        }
        return true;
    };

    const auto launch_1d = [&](cl_kernel kernel, std::size_t global,
                               const char* label) {
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 1, nullptr,
                                           &global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            error = std::string(label) + " launch failed";
            return false;
        }
        const cl_int wait_status = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        if (wait_status != CL_SUCCESS) {
            error = std::string(label) + " did not complete";
            return false;
        }
        return true;
    };

    /*
     * gaussianBlur()'s double path keeps a whole line of double forward states
     * for the boundary correction and the backward recurrence. The stage
     * formats are fp32 only, so this one buffer is created directly and checked
     * against the residency's remaining budget; an over-budget image falls
     * back before any pass runs.
     */
    cl_mem lc_state_memory = nullptr;
    std::shared_ptr<void> lc_state_owner;
    if (local_contrast && lc_iir->double_path != 0) {
        const std::size_t samples =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (samples >
            std::numeric_limits<std::size_t>::max() / sizeof(cl_double)) {
            error = "lab.tools local-contrast state size overflows";
            return RT_STAGE_FALLBACK;
        }
        const std::size_t state_bytes = samples * sizeof(cl_double);
        const StageResidencyStats stats = residency->stats();
        if (stats.used_bytes > stats.budget_bytes ||
            state_bytes > stats.budget_bytes - stats.used_bytes) {
            error = "lab.tools local-contrast state exceeds the device budget";
            return RT_STAGE_FALLBACK;
        }
        cl_int state_error = CL_SUCCESS;
        cl_mem memory = env->api->CreateBuffer(env->context, CL_MEM_READ_WRITE,
                                               state_bytes, nullptr,
                                               &state_error);
        if (memory == nullptr) {
            error = "lab.tools local-contrast state allocation failed";
            return RT_STAGE_FALLBACK;
        }
        lc_state_owner = std::shared_ptr<void>(
            static_cast<void*>(memory), [api = env->api](void* pointer) {
                api->ReleaseMemObject(static_cast<cl_mem>(pointer));
            });
        lc_state_memory = memory;
    }

    const std::size_t image_global[2] = {static_cast<std::size_t>(width),
                                         static_cast<std::size_t>(height)};
    const auto box_blur = [&](cl_mem src, cl_mem dst, cl_int box_radius,
                              cl_int grid_width, cl_int grid_height) {
        cl_mem temp = temp_memory;
        const std::size_t row[2] = {1,
                                    static_cast<std::size_t>(grid_height)};
        const std::size_t column[2] = {
            static_cast<std::size_t>(grid_width), 1};
        cl_uint arg = 0;
        bool ok = true;
        ok = ok && set_arg(box_h_kernel, arg++, sizeof(cl_mem), &src);
        ok = ok && set_arg(box_h_kernel, arg++, sizeof(cl_mem), &temp);
        ok = ok && set_arg(box_h_kernel, arg++, sizeof(cl_int), &box_radius);
        ok = ok && set_arg(box_h_kernel, arg++, sizeof(cl_int),
                           &grid_width);
        ok = ok && set_arg(box_h_kernel, arg++, sizeof(cl_int),
                           &grid_height);
        ok = ok && set_arg(box_h_kernel, arg++, sizeof(cl_int), &sub_pitch);
        if (!ok) {
            error = "lab.tools box blur arguments failed";
            return false;
        }
        if (!launch(box_h_kernel, row, "lab.tools horizontal box blur")) {
            return false;
        }

        /* The vertical pass reads the horizontal result and writes the
         * destination, so a source that is also the destination is safe. */
        arg = 0;
        ok = true;
        ok = ok && set_arg(box_v_kernel, arg++, sizeof(cl_mem), &temp);
        ok = ok && set_arg(box_v_kernel, arg++, sizeof(cl_mem), &dst);
        ok = ok && set_arg(box_v_kernel, arg++, sizeof(cl_int), &box_radius);
        ok = ok && set_arg(box_v_kernel, arg++, sizeof(cl_int),
                           &grid_width);
        ok = ok && set_arg(box_v_kernel, arg++, sizeof(cl_int),
                           &grid_height);
        ok = ok && set_arg(box_v_kernel, arg++, sizeof(cl_int), &sub_pitch);
        if (!ok) {
            error = "lab.tools box blur arguments failed";
            return false;
        }
        return launch(box_v_kernel, column,
                      "lab.tools vertical box blur");
    };

    /*
     * guidedfilter.cc's element-wise operations:
     *   0: dst = a * b, 1: dst = c - a * b, 2: dst = a / (b + epsilon).
     * `c` is ignored by 0 and 2; the caller passes dst for those.
     */
    const auto combine = [&](cl_mem dst, cl_mem a, cl_mem b, cl_mem c,
                             cl_int op, cl_float epsilon, cl_int grid_width,
                             cl_int grid_height) {
        cl_uint arg = 0;
        bool ok = true;
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_mem), &dst);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_mem), &a);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_mem), &b);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_mem), &c);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_int), &op);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_float), &epsilon);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_int), &sub_pitch);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_int),
                           &grid_width);
        ok = ok && set_arg(combine_kernel, arg++, sizeof(cl_int),
                           &grid_height);
        if (!ok) {
            error = "lab.tools guided filter arguments failed";
            return false;
        }
        const std::size_t global[2] = {
            static_cast<std::size_t>(grid_width),
            static_cast<std::size_t>(grid_height)};
        return launch(combine_kernel, global, "lab.tools guided filter");
    };

    /*
     * One shadows/highlights round: the mask, guidedFilter(), then the blend.
     * `lut_memory` is the host-built tone table of this round's amount.
     */
    const auto run_round = [&](bool hl, int tonalwidth, cl_mem lut_memory) {
        const cl_int hl_flag = hl ? 1 : 0;
        const float thresh = shadowsHighlightsThreshold(tonalwidth);
        const float scale_value = shadowsHighlightsScale(thresh, hl);
        const cl_int sh_sw = cl_sw_sh;
        const cl_int sh_sh = cl_sh_sh;
        const std::size_t sh_sub_global[2] = {
            static_cast<std::size_t>(sh_sw),
            static_cast<std::size_t>(sh_sh)};
        const float sh_sub_col_scale =
            static_cast<float>(width) / static_cast<float>(sh_sw);
        const float sh_sub_row_scale =
            static_cast<float>(height) / static_cast<float>(sh_sh);
        const float sh_up_col_scale =
            static_cast<float>(sh_sw) / static_cast<float>(width);
        const float sh_up_row_scale =
            static_cast<float>(sh_sh) / static_cast<float>(height);

        cl_uint arg = 0;
        bool ok = true;
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_mem), &lab_memory);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_mem), &full_memory);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &lab_pitch);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &lab_plane);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &full_pitch);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &full_plane);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_int), &hl_flag);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_float), &thresh);
        ok = ok && set_arg(mask_kernel, arg++, sizeof(cl_float), &scale_value);
        if (!ok) {
            error = "lab.tools mask arguments failed";
            return false;
        }
        if (!launch(mask_kernel, image_global, "lab.tools mask")) {
            return false;
        }

        arg = 0;
        ok = true;
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_mem),
                           &full_memory);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_mem),
                           &i1_memory);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_mem),
                           &p1_memory);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int), &sh_sw);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int), &sh_sh);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &full_pitch);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &full_plane);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int), &sub_pitch);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_float),
                           &sh_sub_col_scale);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_float),
                           &sh_sub_row_scale);
        if (!ok) {
            error = "lab.tools subsample arguments failed";
            return false;
        }
        if (!launch(subsample_kernel, sh_sub_global,
                    "lab.tools subsample")) {
            return false;
        }

        const cl_int rad = guidedFilterBoxRadius(
            shadowsHighlightsRadius(radius, scale), SUBSAMPLING, sh_sw,
            sh_sh);
        const cl_float epsilon = LAB_TOOLS_EPSILON;

        /* meanI and meanp from I1 and p1, which stay intact. */
        if (!box_blur(i1_memory, meani_memory, rad, sh_sw, sh_sh) ||
            !box_blur(p1_memory, meanp_memory, rad, sh_sw, sh_sh)) {
            return false;
        }
        /* corrIp = I1 * p1, then its box mean, in place. */
        if (!combine(p1_memory, i1_memory, p1_memory, p1_memory, 0, epsilon,
                     sh_sw, sh_sh) ||
            !box_blur(p1_memory, p1_memory, rad, sh_sw, sh_sh)) {
            return false;
        }
        /* corrI = I1 * I1, then its box mean, in place. */
        if (!combine(i1_memory, i1_memory, i1_memory, i1_memory, 0,
                     epsilon, sh_sw, sh_sh) ||
            !box_blur(i1_memory, i1_memory, rad, sh_sw, sh_sh)) {
            return false;
        }
        /* varI = corrI - meanI^2, covIp = corrIp - meanI * meanp. */
        if (!combine(i1_memory, meani_memory, meani_memory, i1_memory, 1,
                     epsilon, sh_sw, sh_sh) ||
            !combine(p1_memory, meani_memory, meanp_memory, p1_memory, 1,
                     epsilon, sh_sw, sh_sh)) {
            return false;
        }
        /* a = covIp / (varI + eps), b = meanp - a * meanI. The block's
         * SUBMUL reads its fourth array, so b overwrites covIp while meanp
         * stays intact. */
        if (!combine(i1_memory, p1_memory, i1_memory, i1_memory, 2, epsilon,
                     sh_sw, sh_sh) ||
            !combine(p1_memory, i1_memory, meani_memory, meanp_memory, 1,
                     epsilon, sh_sw, sh_sh)) {
            return false;
        }
        /* meana and meanb, in place. */
        if (!box_blur(i1_memory, i1_memory, rad, sh_sw, sh_sh) ||
            !box_blur(p1_memory, p1_memory, rad, sh_sw, sh_sh)) {
            return false;
        }

        arg = 0;
        ok = true;
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_mem),
                           &i1_memory);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_mem),
                           &p1_memory);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_mem),
                           &full_memory);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int), &sh_sw);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int), &sh_sh);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int), &sub_pitch);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int),
                           &full_pitch);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_int),
                           &full_plane);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_float),
                           &sh_up_col_scale);
        ok = ok && set_arg(upsample_kernel, arg++, sizeof(cl_float),
                           &sh_up_row_scale);
        if (!ok) {
            error = "lab.tools upsample arguments failed";
            return false;
        }
        if (!launch(upsample_kernel, image_global, "lab.tools upsample")) {
            return false;
        }

        arg = 0;
        ok = true;
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem), &lab_memory);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem), &full_memory);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem), &lut_memory);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &lut_size);
        if (lab_mode == 0) {
            ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem),
                               &ws_memory);
            ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem),
                               &iws_memory);
            ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem),
                               &cachef_memory);
            ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_mem),
                               &cachefy_memory);
        }
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &lab_pitch);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &lab_plane);
        ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &full_pitch);
        if (lab_mode != 0) {
            ok = ok && set_arg(apply_kernel, arg++, sizeof(cl_int), &hl_flag);
        }
        if (!ok) {
            error = "lab.tools blend arguments failed";
            return false;
        }
        return launch(apply_kernel, image_global, "lab.tools blend");
    };

    /*
     * One region's guidedFilter() with the block's implicit subsampling. The
     * initial mask and the guide stay in `full` (plane 0 and plane 1), so the
     * same mask can feed the second radius; the filtered result is written to
     * one plane of the region-mask buffer.
     */
    const auto region_guided_filter = [&](cl_int radius_value,
                                          cl_float epsilon,
                                          cl_int subsampling,
                                          cl_int dst_plane) {
        const cl_int grid_width = width / subsampling;
        const cl_int grid_height = height / subsampling;
        if (grid_width < 3 || grid_height < 3) {
            error = "lab.tools region grid is too small";
            return false;
        }
        const std::size_t grid_global[2] = {
            static_cast<std::size_t>(grid_width),
            static_cast<std::size_t>(grid_height)};
        const float sub_col =
            static_cast<float>(width) / static_cast<float>(grid_width);
        const float sub_row =
            static_cast<float>(height) / static_cast<float>(grid_height);
        const float up_col =
            static_cast<float>(grid_width) / static_cast<float>(width);
        const float up_row =
            static_cast<float>(grid_height) / static_cast<float>(height);

        cl_uint arg = 0;
        bool ok = true;
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_mem),
                           &full_memory);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_mem),
                           &i1_memory);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_mem),
                           &p1_memory);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &grid_width);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &grid_height);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &cl_width);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &cl_height);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &full_pitch);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &full_plane);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_int),
                           &sub_pitch);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_float),
                           &sub_col);
        ok = ok && set_arg(subsample_kernel, arg++, sizeof(cl_float),
                           &sub_row);
        if (!ok) {
            error = "lab.tools region subsample arguments failed";
            return false;
        }
        if (!launch(subsample_kernel, grid_global,
                    "lab.tools region subsample")) {
            return false;
        }

        const cl_int rad = guidedFilterBoxRadius(
            radius_value, subsampling, grid_width, grid_height);

        if (!box_blur(i1_memory, meani_memory, rad, grid_width,
                      grid_height) ||
            !box_blur(p1_memory, meanp_memory, rad, grid_width,
                      grid_height)) {
            return false;
        }
        if (!combine(p1_memory, i1_memory, p1_memory, p1_memory, 0, epsilon,
                     grid_width, grid_height) ||
            !box_blur(p1_memory, p1_memory, rad, grid_width,
                      grid_height)) {
            return false;
        }
        if (!combine(i1_memory, i1_memory, i1_memory, i1_memory, 0,
                     epsilon, grid_width, grid_height) ||
            !box_blur(i1_memory, i1_memory, rad, grid_width,
                      grid_height)) {
            return false;
        }
        if (!combine(i1_memory, meani_memory, meani_memory, i1_memory, 1,
                     epsilon, grid_width, grid_height) ||
            !combine(p1_memory, meani_memory, meanp_memory, p1_memory, 1,
                     epsilon, grid_width, grid_height)) {
            return false;
        }
        if (!combine(i1_memory, p1_memory, i1_memory, i1_memory, 2,
                     epsilon, grid_width, grid_height) ||
            !combine(p1_memory, i1_memory, meani_memory, meanp_memory, 1,
                     epsilon, grid_width, grid_height)) {
            return false;
        }
        if (!box_blur(i1_memory, i1_memory, rad, grid_width,
                      grid_height) ||
            !box_blur(p1_memory, p1_memory, rad, grid_width,
                      grid_height)) {
            return false;
        }

        arg = 0;
        ok = true;
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_mem),
                           &i1_memory);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_mem),
                           &p1_memory);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_mem),
                           &full_memory);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_mem),
                           &region_masks_memory);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &grid_width);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &grid_height);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &cl_width);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &cl_height);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &sub_pitch);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &full_pitch);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &full_plane);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &region_mask_pitch);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_int),
                           &dst_plane);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_float),
                           &up_col);
        ok = ok && set_arg(region_upsample_kernel, arg++, sizeof(cl_float),
                           &up_row);
        if (!ok) {
            error = "lab.tools region upsample arguments failed";
            return false;
        }
        return launch(region_upsample_kernel, image_global,
                      "lab.tools region upsample");
    };

    /*
     * The region CDL and channel copy repeat the host's double matrix
     * products. The float ws/iws buffers are the other kernels' inputs, so the
     * region program gets its own copies at the host's precision.
     */
    cl_mem region_ws_memory = nullptr;
    cl_mem region_iws_memory = nullptr;
    std::shared_ptr<void> region_ws_owner;
    std::shared_ptr<void> region_iws_owner;
    if (regions) {
        for (int matrix = 0; matrix < 2; ++matrix) {
            const double* host = static_cast<const double*>(
                matrix == 0 ? args[67].u.ptr : args[68].u.ptr);
            cl_int matrix_error = CL_SUCCESS;
            cl_mem memory = env->api->CreateBuffer(
                env->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                9 * sizeof(cl_double), const_cast<double*>(host),
                &matrix_error);
            if (memory == nullptr) {
                error = "lab.tools region matrix upload failed";
                return RT_STAGE_FALLBACK;
            }
            std::shared_ptr<void> owner(
                static_cast<void*>(memory), [api = env->api](void* pointer) {
                    api->ReleaseMemObject(static_cast<cl_mem>(pointer));
                });
            if (matrix == 0) {
                region_ws_memory = memory;
                region_ws_owner = owner;
            } else {
                region_iws_memory = memory;
                region_iws_owner = owner;
            }
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

        if (labgrid) {
            const cl_float a_scale = labgrid_values[0];
            const cl_float a_base = labgrid_values[1];
            const cl_float b_scale = labgrid_values[2];
            const cl_float b_base = labgrid_values[3];
            cl_uint arg = 0;
            bool ok = true;
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_int),
                               &cl_width);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_int),
                               &cl_height);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_int),
                               &lab_pitch);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_int),
                               &lab_plane);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_float),
                               &a_scale);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_float),
                               &a_base);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_float),
                               &b_scale);
            ok = ok && set_arg(labgrid_kernel, arg++, sizeof(cl_float),
                               &b_base);
            if (!ok) {
                error = "lab.tools LabGrid arguments failed";
                return RT_STAGE_FALLBACK;
            }
            if (!launch(labgrid_kernel, image_global, "lab.tools LabGrid")) {
                return RT_STAGE_FALLBACK;
            }
        }

        if (sh_enabled != 0 && highlights > 0 &&
            !run_round(true, htonalwidth, hl_memory)) {
            return RT_STAGE_FALLBACK;
        }
        if (sh_enabled != 0 && shadows > 0 &&
            !run_round(false, stonalwidth, sh_memory)) {
            return RT_STAGE_FALLBACK;
        }

        /*
         * ImProcFunctions::localContrast(), between the shadows/highlights
         * rounds and the Lab curves, as the host block orders it. The
         * horizontal pass turns the L plane into the first scratch plane and
         * the vertical pass turns that into the blur; the blend then rewrites
         * L in place.
         */
        if (local_contrast) {
            if (pitch_of(lc_h_device) >
                    static_cast<std::size_t>(
                        std::numeric_limits<cl_int>::max()) ||
                pitch_of(lc_v_device) >
                    static_cast<std::size_t>(
                        std::numeric_limits<cl_int>::max())) {
                error = "lab.tools local-contrast pitch does not fit the "
                        "kernel's int";
                return RT_STAGE_FALLBACK;
            }
            const cl_int lc_h_pitch =
                static_cast<cl_int>(pitch_of(lc_h_device));
            const cl_int lc_v_pitch =
                static_cast<cl_int>(pitch_of(lc_v_device));
            const cl_int lc_mode = static_cast<cl_int>(lc_iir->double_path);
            const cl_int h_vector_lines =
                lc_mode != 0 ? 0 : height - (height % 4);
            const cl_int v_vector_lines =
                lc_mode != 0 ? 0 : width - (width % 8);

            const auto set_line_args =
                [&](const RtLabToolsIirDirection& direction, cl_mem source,
                    cl_mem target, cl_int length, cl_int lines,
                    cl_int src_base_step, cl_int dst_base_step,
                    cl_int src_stride, cl_int dst_stride,
                    cl_int vector_lines) {
                    cl_uint arg = 0;
                    cl_int src_base = 0;
                    cl_int dst_base = 0;
                    bool ok = true;
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_mem),
                                       &source);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_mem),
                                       &target);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_mem),
                                       &lc_state_memory);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &length);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &lines);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &src_base_step);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &dst_base_step);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &src_stride);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &dst_stride);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &src_base);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &dst_base);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &lc_mode);
                    ok = ok && set_arg(lc_line_kernel, arg++, sizeof(cl_int),
                                       &vector_lines);
                    for (std::size_t index = 0;
                         index < RT_LAB_TOOLS_IIR_TERMS; ++index) {
                        ok = ok &&
                             set_arg(lc_line_kernel, arg++, sizeof(cl_float),
                                     &direction.lane[index]);
                    }
                    for (std::size_t index = 0;
                         index < RT_LAB_TOOLS_IIR_TERMS; ++index) {
                        ok = ok &&
                             set_arg(lc_line_kernel, arg++, sizeof(cl_double),
                                     &direction.border[index]);
                    }
                    for (std::size_t index = 0;
                         index < RT_LAB_TOOLS_IIR_TERMS; ++index) {
                        ok = ok &&
                             set_arg(lc_line_kernel, arg++, sizeof(cl_double),
                                     &direction.wide[index]);
                    }
                    return ok;
                };

            if (!set_line_args(lc_iir->horizontal, lab_memory, lc_h_memory,
                               width, height, lab_pitch, lc_h_pitch, 1, 1,
                               h_vector_lines) ||
                !launch_1d(lc_line_kernel, static_cast<std::size_t>(height),
                           "lab.tools local-contrast horizontal")) {
                error = "lab.tools local-contrast horizontal pass failed";
                return RT_STAGE_FALLBACK;
            }
            if (!set_line_args(lc_iir->vertical, lc_h_memory, lc_v_memory,
                               height, width, 1, 1, lc_h_pitch, lc_v_pitch,
                               v_vector_lines) ||
                !launch_1d(lc_line_kernel, static_cast<std::size_t>(width),
                           "lab.tools local-contrast vertical")) {
                error = "lab.tools local-contrast vertical pass failed";
                return RT_STAGE_FALLBACK;
            }

            cl_uint arg = 0;
            bool ok = true;
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_mem),
                               &lc_v_memory);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_int),
                               &cl_width);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_int),
                               &cl_height);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_int),
                               &lab_pitch);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_int),
                               &lab_plane);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_int),
                               &lc_v_pitch);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_float),
                               &lc_amount);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_float),
                               &lc_darkness);
            ok = ok && set_arg(lc_blend_kernel, arg++, sizeof(cl_float),
                               &lc_lightness);
            if (!ok) {
                error = "lab.tools local-contrast blend arguments failed";
                return RT_STAGE_FALLBACK;
            }
            if (!launch(lc_blend_kernel, image_global,
                        "lab.tools local contrast")) {
                return RT_STAGE_FALLBACK;
            }
        }

        /*
         * chromiLuminanceCurve(), between local contrast and vibrance, as the
         * host block orders it.
         */
        if (lab_curve) {
            const cl_int cl_lcurve_entries = lcurve_entries;
            const cl_int cl_curve_entries = curve_entries;
            const cl_int cl_l_active = lab_curve_l;
            const cl_int cl_a_active = lab_curve_a;
            const cl_int cl_b_active = lab_curve_b;
            const cl_int cl_cc_active = lab_curve_cc;
            const cl_int cl_lc_active = lab_curve_lc;
            const cl_int cl_cl_active = lab_curve_cl;
            const cl_int cl_method = lab_curve_method;
            const cl_float cl_rst = rst_protection;
            const cl_float cl_protect_red = protect_red_raw;
            const cl_float cl_protect_red_h = protect_red_h_raw;
            const cl_float cl_adjust = adjust;
            const cl_int cl_lcredsk = lcredsk;

            cl_uint arg = 0;
            bool ok = true;
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &lcurve_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_lcurve_entries);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &acurve_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &bcurve_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &satcurve_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &lhskcurve_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &clcurve_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_curve_entries);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_l_active);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_a_active);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_b_active);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_cc_active);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_lc_active);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_cl_active);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_method);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_float),
                               &cl_rst);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_float),
                               &cl_protect_red);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_float),
                               &cl_protect_red_h);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_float),
                               &cl_adjust);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_lcredsk);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &munsell_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_mem),
                               &munsell_sizes_memory);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_width);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &cl_height);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &lab_pitch);
            ok = ok && set_arg(lab_curve_kernel, arg++, sizeof(cl_int),
                               &lab_plane);
            if (!ok) {
                error = "lab.tools Lab-curve arguments failed";
                return RT_STAGE_FALLBACK;
            }
            if (!launch(lab_curve_kernel, image_global,
                        "lab.tools Lab curves")) {
                return RT_STAGE_FALLBACK;
            }
        }

        if (vibrance) {
            const cl_int cl_pastels = pastels;
            const cl_int cl_saturated = saturated;
            const cl_int cl_threshold_top = threshold_top;
            const cl_int cl_threshold_bottom = threshold_bottom;
            const cl_int cl_protectskins = protectskins;
            const cl_int cl_avoidcolorshift = avoidcolorshift;
            const cl_int cl_skin_curve_set = skin_curve_set;
            const cl_int cl_vibrance_highlight = vibrance_highlight;
            cl_uint arg = 0;
            bool ok = true;
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_width);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_height);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &lab_pitch);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &lab_plane);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_mem),
                               &iws_memory);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_mem),
                               &skin_curve_memory);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_skin_curve_set);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_mem),
                               &munsell_memory);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_mem),
                               &munsell_sizes_memory);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_pastels);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_saturated);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_threshold_top);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_threshold_bottom);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_protectskins);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_avoidcolorshift);
            ok = ok && set_arg(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_vibrance_highlight);
            if (!ok) {
                error = "lab.tools vibrance arguments failed";
                return RT_STAGE_FALLBACK;
            }
            if (!launch(vibrance_kernel, image_global, "lab.tools vibrance")) {
                return RT_STAGE_FALLBACK;
            }
        }

        /*
         * ImProcFunctions::labColorCorrectionRegions(), after vibrance, as
         * the host block orders it. Every region mask is built from the Lab
         * snapshot taken at the start of this pass; the region applications
         * then run in region order on the previous result.
         */
        if (regions) {
            /*
             * The block builds the masks from the Lab as it stands when
             * labColorCorrectionRegions() is entered, after LabGrid, the two
             * shadows/highlights rounds, local contrast, the Lab curves and
             * vibrance. Copy the working Lab into the snapshot now; the
             * region applications then run on the working Lab in region
             * order.
             */
            {
                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_mem), &lab_memory);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_mem), &snapshot_memory);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_int), &cl_width);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_int), &cl_height);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_int), &lab_pitch);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_int), &lab_plane);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_int), &lab_pitch);
                ok = ok && set_arg(region_snapshot_kernel, arg++,
                                   sizeof(cl_int), &lab_plane);
                if (!ok) {
                    error = "lab.tools region snapshot arguments failed";
                    return RT_STAGE_FALLBACK;
                }
                if (!launch(region_snapshot_kernel, image_global,
                            "lab.tools region snapshot")) {
                    return RT_STAGE_FALLBACK;
                }
            }

            const cl_float region_c_factor = 327.68f / 48000.f;
            for (int region = 0; region < region_count; ++region) {
                const cl_int cl_region = region;
                if (region_subsampling_ab[region] < 1 ||
                    region_subsampling_l[region] < 1) {
                    error = "lab.tools region subsampling is invalid";
                    return RT_STAGE_FALLBACK;
                }

                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_mem),
                                   &snapshot_memory);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_mem),
                                   &full_memory);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_mem),
                                   &region_luts_memory);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_mem),
                                   &region_params_memory);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &cl_region);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &region_lut_stride);
                ok = ok && set_arg(region_mask_kernel, arg++,
                                   sizeof(cl_float), &region_c_factor);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &cl_width);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &cl_height);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &lab_pitch);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &lab_plane);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &full_pitch);
                ok = ok && set_arg(region_mask_kernel, arg++, sizeof(cl_int),
                                   &full_plane);
                if (!ok) {
                    error = "lab.tools region mask arguments failed";
                    return RT_STAGE_FALLBACK;
                }
                if (!launch(region_mask_kernel, image_global,
                            "lab.tools region mask")) {
                    return RT_STAGE_FALLBACK;
                }

                const cl_int cl_radius_ab =
                    region_radius_ab[region];
                const cl_int cl_radius_l = region_radius_l[region];
                const cl_float epsilon_ab = 0.001f;
                const cl_float epsilon_l = 0.0001f;
                if (!region_guided_filter(
                        cl_radius_ab, epsilon_ab,
                        region_subsampling_ab[region], 0) ||
                    !region_guided_filter(
                        cl_radius_l, epsilon_l,
                        region_subsampling_l[region],
                        region_mask_plane)) {
                    return RT_STAGE_FALLBACK;
                }

                arg = 0;
                ok = true;
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &lab_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &region_masks_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &region_params_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &region_ws_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &region_iws_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &cachef_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_mem), &cachefy_memory);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &cl_region);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &cl_width);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &cl_height);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &lab_pitch);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &lab_plane);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &region_mask_pitch);
                ok = ok && set_arg(region_apply_kernel, arg++,
                                   sizeof(cl_int), &region_mask_plane);
                if (!ok) {
                    error = "lab.tools region apply arguments failed";
                    return RT_STAGE_FALLBACK;
                }
                if (!launch(region_apply_kernel, image_global,
                            "lab.tools region apply")) {
                    return RT_STAGE_FALLBACK;
                }
            }
        }
    }

    if (residency->readback(lab_device, lab) != RT_STAGE_OK) {
        error = "lab.tools Lab readback failed";
        return RT_STAGE_FALLBACK;
    }

    return RT_STAGE_OK;
}

/*
 * The lab.wavelet node. Argument order:
 *   [0] Lab three-plane F32 buffer, read and written in place (only L changes
 *       unless the gamutlab pre-pass also repairs a and b),
 *   [1] PTR host context (RtLabWaveletContext), host-only,
 *   [2] int support flag, mirrored from the context descriptor,
 *   [3..8] doubles mult[0..5] (level 0 is the finest),
 *   [9] double threshold, [10] double skinprotect,
 *   [11..13] doubles b_l, t_l and t_r (the block's hueskin points / 100),
 *   [14] int gamutlab, [15] int the block's scale,
 *   [16] int active levels (1..6),
 *   [17] BUFFER the host-built range tables, levels * 0x20000 floats,
 *   [18] BUFFER the host-built hue mask, [19] BUFFER the chroma mask.
 *
 * The pass order is the block's own: the host prepares the gamutlab
 * BadpixelsLab() pre-pass and the hue/chroma masks on a private copy, then
 * the device decomposes levels 0..levels-1 and reconstructs from the coarsest
 * level down to 0. The last reconstruction level writes the source L plane,
 * which is the block's final copy.
 *
 * A clear support flag refuses before the upload: that is the residual path,
 * and it makes the CPU builtin - the extracted host block - execute and record
 * impl=host. Every refusal leaves the caller's Lab buffer untouched.
 */
RtStageStatus run_lab_wavelet(const std::shared_ptr<OpenCLEnv>& env,
                              const std::shared_ptr<OpenCLResidency>& residency,
                              const std::vector<RtStageArg>& args,
                              std::string& error)
{
    constexpr std::size_t ARG_COUNT = 22;

    if (args.size() != ARG_COUNT) {
        error = "lab.wavelet expects a Lab buffer, a context and its "
                "parameters";
        return RT_STAGE_INVALID;
    }

    bool kinds = args[0].kind == RT_STAGE_ARG_BUFFER &&
                 args[1].kind == RT_STAGE_ARG_PTR &&
                 args[2].kind == RT_STAGE_ARG_INT;
    for (std::size_t index = 3; index <= 13; ++index) {
        kinds = kinds && args[index].kind == RT_STAGE_ARG_DOUBLE;
    }
    kinds = kinds && args[14].kind == RT_STAGE_ARG_INT &&
            args[15].kind == RT_STAGE_ARG_INT &&
            args[16].kind == RT_STAGE_ARG_INT &&
            args[17].kind == RT_STAGE_ARG_BUFFER &&
            args[18].kind == RT_STAGE_ARG_BUFFER &&
            args[19].kind == RT_STAGE_ARG_BUFFER &&
            args[20].kind == RT_STAGE_ARG_INT &&
            args[21].kind == RT_STAGE_ARG_BUFFER;
    if (!kinds) {
        error = "lab.wavelet has an argument of the wrong kind";
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        error = "lab.wavelet needs a three-plane F32 Lab buffer";
        return RT_STAGE_INVALID;
    }

    const auto* context = static_cast<const RtLabWaveletContext*>(
        args[1].u.ptr);
    if (context == nullptr || context->size != sizeof(RtLabWaveletContext) ||
        context->version != RT_LAB_WAVELET_CONTEXT_VERSION ||
        context->magic != RT_LAB_WAVELET_CONTEXT_MAGIC ||
        context->build == nullptr || context->prepare == nullptr ||
        context->run_host == nullptr) {
        error = "lab.wavelet has an invalid host context";
        return RT_STAGE_INVALID;
    }

    const std::int64_t supported = args[2].u.i;
    if (supported != 0 && supported != 1) {
        error = "lab.wavelet support flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    if (supported != 1 || context->supported != 1) {
        error = "lab.wavelet support flag is not set";
        return RT_STAGE_FALLBACK;
    }

    const std::int64_t wavelet_flag = args[20].u.i;
    if (wavelet_flag != 0 && wavelet_flag != 1) {
        error = "lab.wavelet wavelet flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    const bool wavelet = wavelet_flag == 1;

    double mult[RT_LAB_WAVELET_MAX_LEVELS] = {};
    for (int level = 0; level < static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS);
         ++level) {
        const double value = args[3 + level].u.f;
        if (!std::isfinite(value) || value < LAB_WAVELET_MIN_MULT ||
            value > LAB_WAVELET_MAX_MULT) {
            error = "lab.wavelet has a multiplier outside its range";
            return RT_STAGE_INVALID;
        }
        mult[level] = value;
    }
    const double threshold = args[9].u.f;
    const double skinprotect = args[10].u.f;
    const double b_l = args[11].u.f;
    const double t_l = args[12].u.f;
    const double t_r = args[13].u.f;
    if (!std::isfinite(threshold) ||
        threshold < LAB_WAVELET_MIN_THRESHOLD ||
        threshold > LAB_WAVELET_MAX_THRESHOLD ||
        !std::isfinite(skinprotect) ||
        skinprotect < LAB_WAVELET_MIN_SKIN ||
        skinprotect > LAB_WAVELET_MAX_SKIN) {
        error = "lab.wavelet has a threshold or skin setting outside its "
                "range";
        return RT_STAGE_INVALID;
    }
    if (!std::isfinite(b_l) || !std::isfinite(t_l) || !std::isfinite(t_r) ||
        b_l < LAB_WAVELET_MIN_HUE || b_l > LAB_WAVELET_MAX_HUE ||
        t_l < LAB_WAVELET_MIN_HUE || t_l > LAB_WAVELET_MAX_HUE ||
        t_r < LAB_WAVELET_MIN_HUE || t_r > LAB_WAVELET_MAX_HUE) {
        error = "lab.wavelet has a hue point outside its range";
        return RT_STAGE_INVALID;
    }
    const std::int64_t gamutlab_flag = args[14].u.i;
    const std::int64_t scale = args[15].u.i;
    const std::int64_t levels_value = args[16].u.i;
    if ((gamutlab_flag != 0 && gamutlab_flag != 1) ||
        scale != RT_LAB_WAVELET_EXPORT_SCALE || levels_value < 0 ||
        levels_value > static_cast<std::int64_t>(
                           RT_LAB_WAVELET_MAX_LEVELS)) {
        error = "lab.wavelet has a flag or level count outside its range";
        return RT_STAGE_INVALID;
    }
    const int levels = static_cast<int>(levels_value);

    if (levels > 0) {
        const int expected_levels = labWaveletLastLevel(mult);
        if (expected_levels != levels) {
            error = "lab.wavelet level count disagrees with its multipliers";
            return RT_STAGE_INVALID;
        }
        if (static_cast<std::uint32_t>(levels) != context->levels) {
            error = "lab.wavelet equaliser level count disagrees with the "
                    "context";
            return RT_STAGE_INVALID;
        }
    }
    if (levels == 0 && !wavelet) {
        error = "lab.wavelet has nothing to run";
        return RT_STAGE_FALLBACK;
    }

    /*
     * dirpyr_channel()'s first and last column blocks read up to
     * 2 * scalewin without a lower clamp; the block is only defined when the
     * image is at least that wide. Fail closed rather than reproduce an
     * out-of-bounds read.
     */
    const int width = lab.width;
    const int height = lab.height;
    if (levels > 0) {
        int widest = 1;
        for (int level = 0; level < levels; ++level) {
            const int step = 1 << level;
            const int window = level > 1 ? 2 * step : step;
            widest = std::max(widest, window);
        }
        if (width < 8 || height < 8 || width < 2 * widest) {
            error = "lab.wavelet image is too small for its pyramid";
            return RT_STAGE_FALLBACK;
        }
    }
    if (lab.row_pitch % sizeof(float) != 0 ||
        lab.plane_pitch < lab.row_pitch * static_cast<std::size_t>(height)) {
        error = "lab.wavelet Lab buffer pitch does not cover the image";
        return RT_STAGE_INVALID;
    }

    RtStageBuffer luts{};
    if (levels > 0) {
        luts = args[17].u.buffer;
        if (!valid(luts) || luts.format != RT_STAGE_F32 || luts.planes != 1 ||
            luts.height != 1 ||
            static_cast<std::uint32_t>(luts.width) !=
                static_cast<std::uint32_t>(levels) *
                    RT_LAB_WAVELET_LUT_ENTRIES) {
            error = "lab.wavelet needs one range table per level";
            return RT_STAGE_INVALID;
        }
    }

    const bool skin = levels > 0 && skinprotect != 0.0;
    const RtStageBuffer& hue = args[18].u.buffer;
    const RtStageBuffer& chrom = args[19].u.buffer;
    if (skin) {
        for (const RtStageBuffer* mask : {&hue, &chrom}) {
            if (!valid(*mask) || mask->format != RT_STAGE_F32 ||
                mask->planes != 1 || mask->height != height ||
                mask->width != width) {
                error = "lab.wavelet needs two hue/chroma masks";
                return RT_STAGE_INVALID;
            }
        }
    }

    const RtStageBuffer& wavelet_params_buffer = args[21].u.buffer;
    int wavelet_levels = 0;
    int wavelet_entries = 0;
    const float* wavelet_params = nullptr;
    if (wavelet) {
        if (context->wavelet.active != 1 ||
            context->wavelet.compute_mea == nullptr ||
            context->wavelet.params == nullptr ||
            context->wavelet.taps != 8) {
            error = "lab.wavelet wavelet pass is not set up";
            return RT_STAGE_FALLBACK;
        }
        wavelet_levels = static_cast<int>(context->wavelet.levels);
        wavelet_entries = static_cast<int>(context->wavelet.entries);
        if (wavelet_levels < 1 ||
            wavelet_levels >
                static_cast<int>(RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) ||
            wavelet_entries != static_cast<int>(RT_LAB_WAVELET_WAVELET_PARAMS) +
                                   wavelet_levels *
                                       static_cast<int>(
                                           RT_LAB_WAVELET_MEA_ENTRIES)) {
            error = "lab.wavelet wavelet level count is outside its range";
            return RT_STAGE_INVALID;
        }
        if (!valid(wavelet_params_buffer) ||
            wavelet_params_buffer.format != RT_STAGE_F32 ||
            wavelet_params_buffer.planes != 1 ||
            wavelet_params_buffer.height != 1 ||
            wavelet_params_buffer.width != wavelet_entries ||
            wavelet_params_buffer.data == nullptr) {
            error = "lab.wavelet needs the wavelet parameter table";
            return RT_STAGE_INVALID;
        }
        wavelet_params =
            static_cast<const float*>(wavelet_params_buffer.data);
        if (static_cast<int>(wavelet_params[RT_LW_PARAM_LEVELS]) !=
                wavelet_levels ||
            static_cast<int>(wavelet_params[RT_LW_PARAM_TAPS]) != 8) {
            error = "lab.wavelet parameter table disagrees with its context";
            return RT_STAGE_INVALID;
        }

        /*
         * The Haar border leaves the rows and columns in
         * [size - skip, skip) unwritten, so every undecimated level needs a
         * half-size grid at least as large as its stride.
         */
        const int half_w = (width + 1) / 2;
        const int half_h = (height + 1) / 2;
        if (wavelet_levels >= 2) {
            const int reach = 1 << (wavelet_levels - 1);
            if (half_w < reach || half_h < reach) {
                error = "lab.wavelet image is too small for its wavelet bands";
                return RT_STAGE_FALLBACK;
            }
        }
    }

    /*
     * The equaliser's weights and its LUT lookup use float division and
     * square root. A GPU OpenCL implementation may round either differently,
     * so the node runs only on a deterministic CPU device (POCL), the device
     * the project uses for parity, and records impl=host elsewhere.
     */
    cl_device_type device_type = 0;
    if (env->api->GetDeviceInfo(env->device, CL_DEVICE_TYPE,
                                sizeof(device_type), &device_type,
                                nullptr) != CL_SUCCESS ||
        device_type != CL_DEVICE_TYPE_CPU) {
        error = "lab.wavelet needs a deterministic CPU device";
        return RT_STAGE_FALLBACK;
    }

    /*
     * The statistics accumulate in double, so the wavelet pass needs fp64.
     */
    if (wavelet) {
        cl_device_fp_config fp_config = 0;
        if (env->api->GetDeviceInfo(env->device, CL_DEVICE_DOUBLE_FP_CONFIG,
                                    sizeof(fp_config), &fp_config,
                                    nullptr) != CL_SUCCESS ||
            fp_config == 0) {
            error = "lab.wavelet needs device fp64";
            return RT_STAGE_FALLBACK;
        }
    }

    /*
     * The gamutlab protection is a host pre-pass. Run it, and the mask
     * precompute, on a private copy so a later refusal leaves the caller's
     * image untouched for the host builtin.
     */
    std::vector<float> scratch;
    RtStageBuffer prepared = lab;
    if (levels > 0) {
        if (context->needs_scratch != 0) {
            const std::size_t pixels = static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height);
            scratch.resize(pixels * 3);
            prepared.data = scratch.data();
            prepared.row_pitch =
                static_cast<std::size_t>(width) * sizeof(float);
            prepared.plane_pitch = prepared.row_pitch * height;
            prepared.planes = 3;
            const auto* source = static_cast<const unsigned char*>(lab.data);
            auto* target = static_cast<unsigned char*>(prepared.data);
            for (std::uint32_t plane = 0; plane < 3; ++plane) {
                for (int y = 0; y < height; ++y) {
                    std::memcpy(target + plane * prepared.plane_pitch +
                                    static_cast<std::size_t>(y) *
                                        prepared.row_pitch,
                                source + plane * lab.plane_pitch +
                                    static_cast<std::size_t>(y) * lab.row_pitch,
                                static_cast<std::size_t>(width) *
                                    sizeof(float));
                }
            }
        }
        if (context->prepare(context->self, &prepared) != 0) {
            error = "lab.wavelet host preparation failed";
            return RT_STAGE_FALLBACK;
        }
    }

    /* The L plane is the only input both passes read. */
    char l_key = 0;
    char pyramid_keys[RT_LAB_WAVELET_MAX_LEVELS] = {};
    char lut_key = 0;
    char hue_key = 0;
    char chrom_key = 0;
    const StageBufferShape plane_shape{RT_STAGE_F32, width, height, 1};
    const StageBufferShape lut_shape{
        RT_STAGE_F32,
        static_cast<std::int32_t>(levels) *
            static_cast<std::int32_t>(RT_LAB_WAVELET_LUT_ENTRIES),
        1, 1};
    StageDeviceBufferPtr l_device = residency->acquire(&l_key, plane_shape,
                                                       &error);
    StageDeviceBufferPtr pyramid[RT_LAB_WAVELET_MAX_LEVELS];
    for (int level = 0;
         level < static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS); ++level) {
        pyramid[level] = nullptr;
    }
    for (int level = 0; level < levels; ++level) {
        pyramid[level] = residency->acquire(&pyramid_keys[level], plane_shape,
                                            &error);
    }
    StageDeviceBufferPtr lut_device;
    if (levels > 0) {
        lut_device = residency->acquire(&lut_key, lut_shape, &error);
    }
    StageDeviceBufferPtr hue_device;
    StageDeviceBufferPtr chrom_device;
    if (skin) {
        hue_device = residency->acquire(&hue_key, plane_shape, &error);
        chrom_device = residency->acquire(&chrom_key, plane_shape, &error);
    }

    /*
     * Wavelet buffers. They are acquired before the equaliser runs so a
     * budget refusal falls back with the caller's image still untouched.
     */
    const int half_w = (width + 1) / 2;
    const int half_h = (height + 1) / 2;
    constexpr int WAVELET_KEY_BANDS = RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS * 7;
    char wavelet_keys[WAVELET_KEY_BANDS + 6] = {};
    StageDeviceBufferPtr bands[RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS][3];
    StageDeviceBufferPtr wavelet_low[2];
    StageDeviceBufferPtr wavelet_tmp[2];
    StageDeviceBufferPtr stats_device;
    StageDeviceBufferPtr wavelet_params_device;
    const StageBufferShape band_shape{RT_STAGE_F32, half_w, half_h, 1};
    const StageBufferShape full_shape{RT_STAGE_F32, width, half_h, 1};
    const StageBufferShape stats_shape{
        RT_STAGE_F32,
        static_cast<std::int32_t>(
            wavelet_levels * 3 *
            static_cast<int>(RT_LAB_WAVELET_STATS_PER_BAND)),
        1, 1};
    const StageBufferShape wavelet_params_shape{
        RT_STAGE_F32, static_cast<std::int32_t>(wavelet_entries), 1, 1};
    bool wavelet_acquired = true;
    if (wavelet) {
        for (int level = 0; level < wavelet_levels; ++level) {
            for (int dir = 0; dir < 3; ++dir) {
                bands[level][dir] = residency->acquire(
                    &wavelet_keys[level * 7 + dir], band_shape, &error);
                wavelet_acquired = wavelet_acquired && bands[level][dir];
            }
        }
        wavelet_low[0] =
            residency->acquire(&wavelet_keys[WAVELET_KEY_BANDS + 0],
                               band_shape, &error);
        wavelet_low[1] =
            residency->acquire(&wavelet_keys[WAVELET_KEY_BANDS + 1],
                               band_shape, &error);
        wavelet_tmp[0] =
            residency->acquire(&wavelet_keys[WAVELET_KEY_BANDS + 2],
                               full_shape, &error);
        wavelet_tmp[1] =
            residency->acquire(&wavelet_keys[WAVELET_KEY_BANDS + 3],
                               full_shape, &error);
        stats_device =
            residency->acquire(&wavelet_keys[WAVELET_KEY_BANDS + 4],
                               stats_shape, &error);
        wavelet_params_device =
            residency->acquire(&wavelet_keys[WAVELET_KEY_BANDS + 5],
                               wavelet_params_shape, &error);
        wavelet_acquired = wavelet_acquired && wavelet_low[0] &&
                           wavelet_low[1] && wavelet_tmp[0] &&
                           wavelet_tmp[1] && stats_device &&
                           wavelet_params_device;
    }

    bool acquired = l_device != nullptr;
    if (levels > 0) {
        acquired = acquired && lut_device != nullptr;
        for (int level = 0; level < levels; ++level) {
            acquired = acquired && pyramid[level] != nullptr;
        }
    }
    if (skin) {
        acquired = acquired && hue_device && chrom_device;
    }
    if (!acquired || (wavelet && !wavelet_acquired)) {
        return RT_STAGE_FALLBACK;
    }

    if (residency->upload(l_device, plane_view(prepared, 0)) != RT_STAGE_OK ||
        (levels > 0 && residency->upload(lut_device, luts) != RT_STAGE_OK) ||
        (skin &&
         (residency->upload(hue_device, hue) != RT_STAGE_OK ||
          residency->upload(chrom_device, chrom) != RT_STAGE_OK))) {
        error = "lab.wavelet setup upload failed";
        return RT_STAGE_FALLBACK;
    }
    /*
     * The forward analysis reads the filter rows from the parameter table, so
     * the scalar and filter part must be resident before the transform. The
     * table is uploaded again after compute_mea() fills the mea tail.
     */
    if (wavelet &&
        residency->upload(wavelet_params_device,
                          wavelet_params_buffer) != RT_STAGE_OK) {
        error = "lab.wavelet parameter upload failed";
        return RT_STAGE_FALLBACK;
    }

    const KernelDef* def = find_kernel("lab.wavelet");
    if (def == nullptr) {
        error = "lab.wavelet kernel is not registered";
        return RT_STAGE_INVALID;
    }
    cl_kernel decompose_kernel = nullptr;
    cl_kernel equalize_kernel = nullptr;
    if (levels > 0) {
        decompose_kernel = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_decompose", error);
        equalize_kernel = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_equalize", error);
        if (decompose_kernel == nullptr || equalize_kernel == nullptr) {
            return RT_STAGE_FALLBACK;
        }
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };
    const auto pitch_of = [](const StageDeviceBufferPtr& buffer) {
        return static_cast<cl_int>(buffer->row_pitch() / sizeof(float));
    };
    const auto set_arg = [&](cl_kernel kernel, cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };
    const auto launch = [&](cl_kernel kernel, const std::size_t* global,
                            const char* label) {
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr,
                                           global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            error = std::string(label) + " launch failed";
            return false;
        }
        const cl_int wait_status = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        if (wait_status != CL_SUCCESS) {
            error = std::string(label) + " did not complete";
            return false;
        }
        return true;
    };

    const cl_int cl_width = width;
    const cl_int cl_height = height;
#if defined(__SSE2__) || defined(RT_SIMDE)
    const cl_int cl_vectorised = 1;
#else
    const cl_int cl_vectorised = 0;
#endif
    const std::size_t image_global[2] = {
        static_cast<std::size_t>(width), static_cast<std::size_t>(height)};
    const cl_mem l_memory = memory_of(l_device);
    const cl_int l_pitch = pitch_of(l_device);
    std::vector<cl_mem> pyramid_memory(levels);
    std::vector<cl_int> pyramid_pitch(levels);
    for (int level = 0; level < levels; ++level) {
        pyramid_memory[level] = memory_of(pyramid[level]);
        pyramid_pitch[level] = pitch_of(pyramid[level]);
    }
    const cl_mem lut_memory = levels > 0 ? memory_of(lut_device) : l_memory;
    const cl_mem hue_memory = skin ? memory_of(hue_device) : l_memory;
    const cl_mem chrom_memory = skin ? memory_of(chrom_device) : l_memory;
    const cl_int mask_pitch = skin ? pitch_of(hue_device) : 0;

    /* dirpyr_channel(), finest level first. */
    for (int level = 0; level < levels; ++level) {
        const cl_mem fine_memory =
            level == 0 ? l_memory : pyramid_memory[level - 1];
        const cl_int fine_pitch =
            level == 0 ? l_pitch : pyramid_pitch[level - 1];
        const cl_int cl_level = level;
        const cl_int cl_scale = 1 << level;
        bool ok = true;
        ok = ok && set_arg(decompose_kernel, 0, sizeof(cl_mem), &fine_memory);
        ok = ok && set_arg(decompose_kernel, 1, sizeof(cl_int), &fine_pitch);
        ok = ok && set_arg(decompose_kernel, 2, sizeof(cl_mem),
                           &pyramid_memory[level]);
        ok = ok && set_arg(decompose_kernel, 3, sizeof(cl_int),
                           &pyramid_pitch[level]);
        ok = ok && set_arg(decompose_kernel, 4, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(decompose_kernel, 5, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(decompose_kernel, 6, sizeof(cl_int), &cl_level);
        ok = ok && set_arg(decompose_kernel, 7, sizeof(cl_int), &cl_scale);
        ok = ok && set_arg(decompose_kernel, 8, sizeof(cl_int),
                           &cl_vectorised);
        if (!ok || !launch(decompose_kernel, image_global,
                           "lab.wavelet decompose")) {
            error = "lab.wavelet decompose arguments failed";
            return RT_STAGE_FALLBACK;
        }
    }

    /*
     * idirpyr_eq_channel(), coarsest level first. The last pyramid level is
     * the reconstruction buffer, exactly as the block reuses it; level 0
     * writes the source plane directly.
     */
    const int buffer_level = levels - 1;
    const double skinprotect_value = skinprotect;
    const cl_int cl_skin = skin ? 1 : 0;
    const cl_float cl_skinprotect = static_cast<cl_float>(skinprotect_value);
    const cl_float cl_b_l = static_cast<cl_float>(b_l);
    const cl_float cl_t_l = static_cast<cl_float>(t_l);
    const cl_float cl_t_r = static_cast<cl_float>(t_r);
    for (int level = buffer_level; level >= 0; --level) {
        const cl_mem fine_memory =
            level == 0 ? l_memory : pyramid_memory[level - 1];
        const cl_int fine_pitch =
            level == 0 ? l_pitch : pyramid_pitch[level - 1];
        const cl_int cl_level = level;
        const cl_int to_dst = level == 0 ? 1 : 0;
        bool ok = true;
        ok = ok && set_arg(equalize_kernel, 0, sizeof(cl_mem), &fine_memory);
        ok = ok && set_arg(equalize_kernel, 1, sizeof(cl_int), &fine_pitch);
        ok = ok && set_arg(equalize_kernel, 2, sizeof(cl_mem),
                           &pyramid_memory[level]);
        ok = ok && set_arg(equalize_kernel, 3, sizeof(cl_int),
                           &pyramid_pitch[level]);
        ok = ok && set_arg(equalize_kernel, 4, sizeof(cl_mem),
                           &pyramid_memory[buffer_level]);
        ok = ok && set_arg(equalize_kernel, 5, sizeof(cl_int),
                           &pyramid_pitch[buffer_level]);
        ok = ok && set_arg(equalize_kernel, 6, sizeof(cl_mem), &l_memory);
        ok = ok && set_arg(equalize_kernel, 7, sizeof(cl_int), &l_pitch);
        ok = ok && set_arg(equalize_kernel, 8, sizeof(cl_mem), &lut_memory);
        ok = ok && set_arg(equalize_kernel, 9, sizeof(cl_int), &cl_level);
        ok = ok && set_arg(equalize_kernel, 10, sizeof(cl_mem), &hue_memory);
        ok = ok && set_arg(equalize_kernel, 11, sizeof(cl_int), &mask_pitch);
        ok = ok && set_arg(equalize_kernel, 12, sizeof(cl_mem),
                           &chrom_memory);
        ok = ok && set_arg(equalize_kernel, 13, sizeof(cl_int), &mask_pitch);
        ok = ok && set_arg(equalize_kernel, 14, sizeof(cl_int), &cl_width);
        ok = ok && set_arg(equalize_kernel, 15, sizeof(cl_int), &cl_height);
        ok = ok && set_arg(equalize_kernel, 16, sizeof(cl_int), &cl_skin);
        ok = ok && set_arg(equalize_kernel, 17, sizeof(cl_float),
                           &cl_skinprotect);
        ok = ok && set_arg(equalize_kernel, 18, sizeof(cl_float), &cl_b_l);
        ok = ok && set_arg(equalize_kernel, 19, sizeof(cl_float), &cl_t_l);
        ok = ok && set_arg(equalize_kernel, 20, sizeof(cl_float), &cl_t_r);
        ok = ok && set_arg(equalize_kernel, 21, sizeof(cl_int), &to_dst);
        if (!ok || !launch(equalize_kernel, image_global,
                           "lab.wavelet equalize")) {
            error = "lab.wavelet equalize arguments failed";
            return RT_STAGE_FALLBACK;
        }
    }

    /*
     * The wavelet tool. Its forward decomposition, statistics, contrast
     * multiply and reconstruction reproduce ip_wavelet() for the contrast-only
     * configuration; the parameter table and the mea blocks are host-built.
     */
    if (wavelet) {
        cl_kernel analysis_v = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_analysis_v", error);
        cl_kernel analysis_h = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_analysis_h", error);
        cl_kernel haar_v = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_haar_v", error);
        cl_kernel haar_h = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_haar_h", error);
        cl_kernel synth_h = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_synth_h", error);
        cl_kernel synth_v = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_synth_v", error);
        cl_kernel haar_synth_h = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_haar_synth_h", error);
        cl_kernel haar_synth_v = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_haar_synth_v", error);
        cl_kernel stats_kernel = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_stats", error);
        cl_kernel contrast_kernel = compiled_kernel_entry(
            env, def, "rt_lab_wavelet_w_contrast", error);
        if (analysis_v == nullptr || analysis_h == nullptr ||
            haar_v == nullptr || haar_h == nullptr || synth_h == nullptr ||
            synth_v == nullptr || haar_synth_h == nullptr ||
            haar_synth_v == nullptr || stats_kernel == nullptr ||
            contrast_kernel == nullptr) {
            return RT_STAGE_FALLBACK;
        }

        const cl_mem params_memory = memory_of(wavelet_params_device);
        const cl_int cl_taps = 8;
        const cl_int cl_offset = Daub4_offset;
        const cl_int cl_half_w = half_w;
        const cl_int cl_half_h = half_h;
        const std::size_t half_global[2] = {
            static_cast<std::size_t>(half_w),
            static_cast<std::size_t>(half_h)};
        const std::size_t full_global[2] = {
            static_cast<std::size_t>(width),
            static_cast<std::size_t>(half_h)};
        const std::size_t band_global[2] = {
            static_cast<std::size_t>(half_w),
            static_cast<std::size_t>(half_h)};
        const std::size_t stats_global[2] = {3, 1};

        const auto tmp_lo = memory_of(wavelet_tmp[0]);
        const auto tmp_hi = memory_of(wavelet_tmp[1]);
        const cl_int tmp_lo_pitch = pitch_of(wavelet_tmp[0]);
        const cl_int tmp_hi_pitch = pitch_of(wavelet_tmp[1]);

        /* Level 0: the decimated 8-tap Daubechies analysis. */
        {
            bool ok = true;
            ok = ok && set_arg(analysis_v, 0, sizeof(cl_mem), &l_memory);
            ok = ok && set_arg(analysis_v, 1, sizeof(cl_int), &l_pitch);
            ok = ok && set_arg(analysis_v, 2, sizeof(cl_int), &cl_height);
            ok = ok && set_arg(analysis_v, 3, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(analysis_v, 4, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(analysis_v, 5, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(analysis_v, 6, sizeof(cl_int), &tmp_hi_pitch);
            ok = ok && set_arg(analysis_v, 7, sizeof(cl_mem), &params_memory);
            ok = ok && set_arg(analysis_v, 8, sizeof(cl_int), &cl_taps);
            ok = ok && set_arg(analysis_v, 9, sizeof(cl_int), &cl_offset);
            ok = ok && set_arg(analysis_v, 10, sizeof(cl_int), &cl_width);
            ok = ok && set_arg(analysis_v, 11, sizeof(cl_int), &cl_half_h);
            if (!ok || !launch(analysis_v, full_global,
                               "lab.wavelet analysis vertical")) {
                error = "lab.wavelet analysis vertical arguments failed";
                return RT_STAGE_FALLBACK;
            }
        }
        {
            cl_mem low_memory = memory_of(wavelet_low[0]);
            cl_int low_pitch = pitch_of(wavelet_low[0]);
            cl_mem band1 = memory_of(bands[0][0]);
            cl_mem band2 = memory_of(bands[0][1]);
            cl_mem band3 = memory_of(bands[0][2]);
            cl_int bp1 = pitch_of(bands[0][0]);
            cl_int bp2 = pitch_of(bands[0][1]);
            cl_int bp3 = pitch_of(bands[0][2]);
            bool ok = true;
            ok = ok && set_arg(analysis_h, 0, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(analysis_h, 1, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(analysis_h, 2, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(analysis_h, 3, sizeof(cl_int), &tmp_hi_pitch);
            ok = ok && set_arg(analysis_h, 4, sizeof(cl_mem), &low_memory);
            ok = ok && set_arg(analysis_h, 5, sizeof(cl_int), &low_pitch);
            ok = ok && set_arg(analysis_h, 6, sizeof(cl_mem), &band1);
            ok = ok && set_arg(analysis_h, 7, sizeof(cl_int), &bp1);
            ok = ok && set_arg(analysis_h, 8, sizeof(cl_mem), &band2);
            ok = ok && set_arg(analysis_h, 9, sizeof(cl_int), &bp2);
            ok = ok && set_arg(analysis_h, 10, sizeof(cl_mem), &band3);
            ok = ok && set_arg(analysis_h, 11, sizeof(cl_int), &bp3);
            ok = ok && set_arg(analysis_h, 12, sizeof(cl_mem), &params_memory);
            ok = ok && set_arg(analysis_h, 13, sizeof(cl_int), &cl_taps);
            ok = ok && set_arg(analysis_h, 14, sizeof(cl_int), &cl_offset);
            ok = ok && set_arg(analysis_h, 15, sizeof(cl_int), &cl_width);
            ok = ok && set_arg(analysis_h, 16, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(analysis_h, 17, sizeof(cl_int), &cl_half_h);
            if (!ok || !launch(analysis_h, half_global,
                               "lab.wavelet analysis horizontal")) {
                error = "lab.wavelet analysis horizontal arguments failed";
                return RT_STAGE_FALLBACK;
            }
        }

        /* Levels 1..levels-1: the undecimated Haar analysis. */
        for (int level = 1; level < wavelet_levels; ++level) {
            const int skip = 1 << (level - 1);
            cl_mem src = memory_of(wavelet_low[(level - 1) % 2]);
            cl_int src_pitch = pitch_of(wavelet_low[(level - 1) % 2]);
            cl_int cl_skip = skip;
            bool ok = true;
            ok = ok && set_arg(haar_v, 0, sizeof(cl_mem), &src);
            ok = ok && set_arg(haar_v, 1, sizeof(cl_int), &src_pitch);
            ok = ok && set_arg(haar_v, 2, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(haar_v, 3, sizeof(cl_int), &cl_half_h);
            ok = ok && set_arg(haar_v, 4, sizeof(cl_int), &cl_skip);
            ok = ok && set_arg(haar_v, 5, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(haar_v, 6, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(haar_v, 7, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(haar_v, 8, sizeof(cl_int), &tmp_hi_pitch);
            if (!ok || !launch(haar_v, band_global,
                               "lab.wavelet haar vertical")) {
                error = "lab.wavelet haar vertical arguments failed";
                return RT_STAGE_FALLBACK;
            }

            cl_mem low_memory = memory_of(wavelet_low[level % 2]);
            cl_int low_pitch = pitch_of(wavelet_low[level % 2]);
            cl_mem band1 = memory_of(bands[level][0]);
            cl_mem band2 = memory_of(bands[level][1]);
            cl_mem band3 = memory_of(bands[level][2]);
            cl_int bp1 = pitch_of(bands[level][0]);
            cl_int bp2 = pitch_of(bands[level][1]);
            cl_int bp3 = pitch_of(bands[level][2]);
            ok = true;
            ok = ok && set_arg(haar_h, 0, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(haar_h, 1, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(haar_h, 2, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(haar_h, 3, sizeof(cl_int), &tmp_hi_pitch);
            ok = ok && set_arg(haar_h, 4, sizeof(cl_mem), &low_memory);
            ok = ok && set_arg(haar_h, 5, sizeof(cl_int), &low_pitch);
            ok = ok && set_arg(haar_h, 6, sizeof(cl_mem), &band1);
            ok = ok && set_arg(haar_h, 7, sizeof(cl_int), &bp1);
            ok = ok && set_arg(haar_h, 8, sizeof(cl_mem), &band2);
            ok = ok && set_arg(haar_h, 9, sizeof(cl_int), &bp2);
            ok = ok && set_arg(haar_h, 10, sizeof(cl_mem), &band3);
            ok = ok && set_arg(haar_h, 11, sizeof(cl_int), &bp3);
            ok = ok && set_arg(haar_h, 12, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(haar_h, 13, sizeof(cl_int), &cl_half_h);
            ok = ok && set_arg(haar_h, 14, sizeof(cl_int), &cl_skip);
            if (!ok || !launch(haar_h, band_global,
                               "lab.wavelet haar horizontal")) {
                error = "lab.wavelet haar horizontal arguments failed";
                return RT_STAGE_FALLBACK;
            }
        }

        /* Per-band statistics, one work item per band. */
        std::vector<float> stats_host(
            static_cast<std::size_t>(wavelet_levels) * 3 *
                RT_LAB_WAVELET_STATS_PER_BAND,
            0.f);
        RtStageBuffer stats_view{};
        stats_view.size = sizeof(RtStageBuffer);
        stats_view.data = stats_host.data();
        stats_view.width = static_cast<std::int32_t>(stats_host.size());
        stats_view.height = 1;
        stats_view.row_pitch = stats_host.size() * sizeof(float);
        stats_view.plane_pitch = stats_view.row_pitch;
        stats_view.planes = 1;
        stats_view.format = RT_STAGE_F32;
        stats_view.flags = RT_STAGE_BUFFER_READ;
        const cl_mem stats_memory = memory_of(stats_device);
        for (int level = 0; level < wavelet_levels; ++level) {
            cl_mem band1 = memory_of(bands[level][0]);
            cl_mem band2 = memory_of(bands[level][1]);
            cl_mem band3 = memory_of(bands[level][2]);
            cl_int bp1 = pitch_of(bands[level][0]);
            cl_int bp2 = pitch_of(bands[level][1]);
            cl_int bp3 = pitch_of(bands[level][2]);
            cl_int cl_bands = 3;
            bool ok = true;
            ok = ok && set_arg(stats_kernel, 0, sizeof(cl_mem), &band1);
            ok = ok && set_arg(stats_kernel, 1, sizeof(cl_int), &bp1);
            ok = ok && set_arg(stats_kernel, 2, sizeof(cl_mem), &band2);
            ok = ok && set_arg(stats_kernel, 3, sizeof(cl_int), &bp2);
            ok = ok && set_arg(stats_kernel, 4, sizeof(cl_mem), &band3);
            ok = ok && set_arg(stats_kernel, 5, sizeof(cl_int), &bp3);
            cl_int stats_offset = level * 3 * static_cast<int>(
                                                    RT_LAB_WAVELET_STATS_PER_BAND);
            ok = ok && set_arg(stats_kernel, 6, sizeof(cl_mem),
                               &stats_memory);
            ok = ok && set_arg(stats_kernel, 7, sizeof(cl_int), &stats_offset);
            ok = ok && set_arg(stats_kernel, 8, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(stats_kernel, 9, sizeof(cl_int), &cl_half_h);
            ok = ok && set_arg(stats_kernel, 10, sizeof(cl_int), &cl_bands);
            if (!ok || !launch(stats_kernel, stats_global,
                               "lab.wavelet statistics")) {
                error = "lab.wavelet statistics arguments failed";
                return RT_STAGE_FALLBACK;
            }
        }
        if (residency->readback(stats_device, stats_view) != RT_STAGE_OK) {
            error = "lab.wavelet statistics readback failed";
            return RT_STAGE_FALLBACK;
        }
        if (context->wavelet.compute_mea(
                context->self, stats_host.data(),
                static_cast<uint32_t>(wavelet_levels)) != 0) {
            error = "lab.wavelet mea build failed";
            return RT_STAGE_FALLBACK;
        }
        wavelet_params = static_cast<const float*>(wavelet_params_buffer.data);
        if (residency->upload(wavelet_params_device,
                              wavelet_params_buffer) != RT_STAGE_OK) {
            error = "lab.wavelet parameter upload failed";
            return RT_STAGE_FALLBACK;
        }

        /* The contrast multiply, one launch per level. */
        for (int level = 0; level < wavelet_levels; ++level) {
            cl_mem band1 = memory_of(bands[level][0]);
            cl_mem band2 = memory_of(bands[level][1]);
            cl_mem band3 = memory_of(bands[level][2]);
            cl_int bp1 = pitch_of(bands[level][0]);
            cl_int bp2 = pitch_of(bands[level][1]);
            cl_int bp3 = pitch_of(bands[level][2]);
            cl_mem coeff = memory_of(wavelet_low[(wavelet_levels - 1) % 2]);
            cl_int coeff_pitch =
                pitch_of(wavelet_low[(wavelet_levels - 1) % 2]);
            cl_int cl_mea_offset = static_cast<cl_int>(
                RT_LAB_WAVELET_WAVELET_PARAMS);
            cl_int cl_level = level;
            cl_int cl_max = wavelet_levels;
            bool ok = true;
            ok = ok && set_arg(contrast_kernel, 0, sizeof(cl_mem), &band1);
            ok = ok && set_arg(contrast_kernel, 1, sizeof(cl_int), &bp1);
            ok = ok && set_arg(contrast_kernel, 2, sizeof(cl_mem), &band2);
            ok = ok && set_arg(contrast_kernel, 3, sizeof(cl_int), &bp2);
            ok = ok && set_arg(contrast_kernel, 4, sizeof(cl_mem), &band3);
            ok = ok && set_arg(contrast_kernel, 5, sizeof(cl_int), &bp3);
            ok = ok && set_arg(contrast_kernel, 6, sizeof(cl_mem), &coeff);
            ok = ok && set_arg(contrast_kernel, 7, sizeof(cl_int),
                               &coeff_pitch);
            ok = ok && set_arg(contrast_kernel, 8, sizeof(cl_mem), &l_memory);
            ok = ok && set_arg(contrast_kernel, 9, sizeof(cl_int), &l_pitch);
            ok = ok && set_arg(contrast_kernel, 10, sizeof(cl_mem),
                               &params_memory);
            ok = ok && set_arg(contrast_kernel, 11, sizeof(cl_int),
                               &cl_mea_offset);
            ok = ok && set_arg(contrast_kernel, 12, sizeof(cl_int), &cl_level);
            ok = ok && set_arg(contrast_kernel, 13, sizeof(cl_int),
                               &cl_half_w);
            ok = ok && set_arg(contrast_kernel, 14, sizeof(cl_int),
                               &cl_half_h);
            ok = ok && set_arg(contrast_kernel, 15, sizeof(cl_int), &cl_max);
            if (!ok || !launch(contrast_kernel, band_global,
                               "lab.wavelet contrast")) {
                error = "lab.wavelet contrast arguments failed";
                return RT_STAGE_FALLBACK;
            }
        }

        /* Reconstruction: the Haar levels first, then level 0. */
        const int coeff_index = (wavelet_levels - 1) % 2;
        for (int level = wavelet_levels - 1; level >= 1; --level) {
            const int skip = 1 << (level - 1);
            cl_int cl_skip = skip;
            cl_mem low_memory = memory_of(wavelet_low[coeff_index]);
            cl_int low_pitch = pitch_of(wavelet_low[coeff_index]);
            cl_mem band1 = memory_of(bands[level][0]);
            cl_mem band2 = memory_of(bands[level][1]);
            cl_mem band3 = memory_of(bands[level][2]);
            cl_int bp1 = pitch_of(bands[level][0]);
            cl_int bp2 = pitch_of(bands[level][1]);
            cl_int bp3 = pitch_of(bands[level][2]);
            bool ok = true;
            ok = ok && set_arg(haar_synth_h, 0, sizeof(cl_mem), &low_memory);
            ok = ok && set_arg(haar_synth_h, 1, sizeof(cl_int), &low_pitch);
            ok = ok && set_arg(haar_synth_h, 2, sizeof(cl_mem), &band1);
            ok = ok && set_arg(haar_synth_h, 3, sizeof(cl_int), &bp1);
            ok = ok && set_arg(haar_synth_h, 4, sizeof(cl_mem), &band2);
            ok = ok && set_arg(haar_synth_h, 5, sizeof(cl_int), &bp2);
            ok = ok && set_arg(haar_synth_h, 6, sizeof(cl_mem), &band3);
            ok = ok && set_arg(haar_synth_h, 7, sizeof(cl_int), &bp3);
            ok = ok && set_arg(haar_synth_h, 8, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(haar_synth_h, 9, sizeof(cl_int),
                               &tmp_lo_pitch);
            ok = ok && set_arg(haar_synth_h, 10, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(haar_synth_h, 11, sizeof(cl_int),
                               &tmp_hi_pitch);
            ok = ok && set_arg(haar_synth_h, 12, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(haar_synth_h, 13, sizeof(cl_int), &cl_half_h);
            ok = ok && set_arg(haar_synth_h, 14, sizeof(cl_int), &cl_skip);
            if (!ok || !launch(haar_synth_h, band_global,
                               "lab.wavelet haar synthesis horizontal")) {
                error = "lab.wavelet haar synthesis horizontal arguments "
                        "failed";
                return RT_STAGE_FALLBACK;
            }

            ok = true;
            ok = ok && set_arg(haar_synth_v, 0, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(haar_synth_v, 1, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(haar_synth_v, 2, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(haar_synth_v, 3, sizeof(cl_int), &tmp_hi_pitch);
            ok = ok && set_arg(haar_synth_v, 4, sizeof(cl_mem), &low_memory);
            ok = ok && set_arg(haar_synth_v, 5, sizeof(cl_int), &low_pitch);
            ok = ok && set_arg(haar_synth_v, 6, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(haar_synth_v, 7, sizeof(cl_int), &cl_half_h);
            ok = ok && set_arg(haar_synth_v, 8, sizeof(cl_int), &cl_skip);
            if (!ok || !launch(haar_synth_v, band_global,
                               "lab.wavelet haar synthesis vertical")) {
                error = "lab.wavelet haar synthesis vertical arguments "
                        "failed";
                return RT_STAGE_FALLBACK;
            }
        }

        {
            cl_mem low_memory = memory_of(wavelet_low[coeff_index]);
            cl_int low_pitch = pitch_of(wavelet_low[coeff_index]);
            cl_mem band1 = memory_of(bands[0][0]);
            cl_mem band2 = memory_of(bands[0][1]);
            cl_mem band3 = memory_of(bands[0][2]);
            cl_int bp1 = pitch_of(bands[0][0]);
            cl_int bp2 = pitch_of(bands[0][1]);
            cl_int bp3 = pitch_of(bands[0][2]);
            bool ok = true;
            ok = ok && set_arg(synth_h, 0, sizeof(cl_mem), &low_memory);
            ok = ok && set_arg(synth_h, 1, sizeof(cl_int), &low_pitch);
            ok = ok && set_arg(synth_h, 2, sizeof(cl_mem), &band1);
            ok = ok && set_arg(synth_h, 3, sizeof(cl_int), &bp1);
            ok = ok && set_arg(synth_h, 4, sizeof(cl_mem), &band2);
            ok = ok && set_arg(synth_h, 5, sizeof(cl_int), &bp2);
            ok = ok && set_arg(synth_h, 6, sizeof(cl_mem), &band3);
            ok = ok && set_arg(synth_h, 7, sizeof(cl_int), &bp3);
            ok = ok && set_arg(synth_h, 8, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(synth_h, 9, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(synth_h, 10, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(synth_h, 11, sizeof(cl_int), &tmp_hi_pitch);
            ok = ok && set_arg(synth_h, 12, sizeof(cl_mem), &params_memory);
            ok = ok && set_arg(synth_h, 13, sizeof(cl_int), &cl_taps);
            ok = ok && set_arg(synth_h, 14, sizeof(cl_int), &cl_offset);
            ok = ok && set_arg(synth_h, 15, sizeof(cl_int), &cl_half_w);
            ok = ok && set_arg(synth_h, 16, sizeof(cl_int), &cl_width);
            ok = ok && set_arg(synth_h, 17, sizeof(cl_int), &cl_half_h);
            if (!ok || !launch(synth_h, full_global,
                               "lab.wavelet synthesis horizontal")) {
                error = "lab.wavelet synthesis horizontal arguments failed";
                return RT_STAGE_FALLBACK;
            }

            const float blend = wavelet_params[RT_LW_PARAM_STRENGTH];
            cl_float cl_blend = blend;
            ok = true;
            ok = ok && set_arg(synth_v, 0, sizeof(cl_mem), &tmp_lo);
            ok = ok && set_arg(synth_v, 1, sizeof(cl_int), &tmp_lo_pitch);
            ok = ok && set_arg(synth_v, 2, sizeof(cl_mem), &tmp_hi);
            ok = ok && set_arg(synth_v, 3, sizeof(cl_int), &tmp_hi_pitch);
            ok = ok && set_arg(synth_v, 4, sizeof(cl_mem), &l_memory);
            ok = ok && set_arg(synth_v, 5, sizeof(cl_int), &l_pitch);
            ok = ok && set_arg(synth_v, 6, sizeof(cl_mem), &params_memory);
            ok = ok && set_arg(synth_v, 7, sizeof(cl_int), &cl_taps);
            ok = ok && set_arg(synth_v, 8, sizeof(cl_int), &cl_offset);
            ok = ok && set_arg(synth_v, 9, sizeof(cl_int), &cl_width);
            ok = ok && set_arg(synth_v, 10, sizeof(cl_int), &cl_half_h);
            ok = ok && set_arg(synth_v, 11, sizeof(cl_int), &cl_height);
            ok = ok && set_arg(synth_v, 12, sizeof(cl_float), &cl_blend);
            if (!ok || !launch(synth_v, image_global,
                               "lab.wavelet synthesis vertical")) {
                error = "lab.wavelet synthesis vertical arguments failed";
                return RT_STAGE_FALLBACK;
            }
        }
    }

    if (residency->readback(l_device, plane_view(lab, 0)) != RT_STAGE_OK) {
        error = "lab.wavelet L readback failed";
        return RT_STAGE_FALLBACK;
    }

    /*
     * BadpixelsLab() also repairs a and b. The prepared copy owns those
     * repaired planes; the device only wrote L.
     */
    if (levels > 0 && context->needs_scratch != 0) {
        auto* target = static_cast<unsigned char*>(lab.data);
        const auto* source = static_cast<const unsigned char*>(prepared.data);
        for (std::uint32_t plane = 1; plane < 3; ++plane) {
            for (int y = 0; y < height; ++y) {
                std::memcpy(target + plane * lab.plane_pitch +
                                static_cast<std::size_t>(y) * lab.row_pitch,
                            source + plane * prepared.plane_pitch +
                                static_cast<std::size_t>(y) *
                                    prepared.row_pitch,
                            static_cast<std::size_t>(width) * sizeof(float));
            }
        }
    }

    return RT_STAGE_OK;
}

/*
 * Frame drawing. Argument order:
 *   [0] source RGB buffer, [1] destination RGB buffer,
 *   [2..4] doubles border red, green and blue,
 *   [5..6] ints row and column offset of the source inside the frame.
 *
 * The framed dimensions come from the destination buffer; the source and
 * destination pitches are inserted by the launcher when it sets the kernel
 * arguments. The node is only valid when the source is strictly smaller than
 * the frame in at least one dimension and fits at the given offset, which is
 * exactly the region where drawFrame() inserts a border. The three
 * destination planes are read back after a wait, so the node reports a
 * completed device event.
 */
RtStageStatus run_framing(const std::shared_ptr<OpenCLEnv>& env,
                          const std::shared_ptr<OpenCLResidency>& residency,
                          const std::vector<RtStageArg>& args,
                          std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 2;
    constexpr std::size_t DOUBLE_ARGS = 3;
    constexpr std::size_t INT_ARGS = 2;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + DOUBLE_ARGS + INT_ARGS;
    constexpr std::size_t FIRST_INT = BUFFER_ARGS + DOUBLE_ARGS;

    if (args.size() != ARG_COUNT) {
        error = "framing expects two buffers, three doubles and two ints";
        return RT_STAGE_INVALID;
    }
    for (std::size_t i = 0; i < BUFFER_ARGS; ++i) {
        if (args[i].kind != RT_STAGE_ARG_BUFFER) {
            error = "framing expects two buffers";
            return RT_STAGE_INVALID;
        }
    }
    for (std::size_t i = BUFFER_ARGS; i < FIRST_INT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_DOUBLE) {
            error = "framing has a non-double border colour";
            return RT_STAGE_INVALID;
        }
    }
    for (std::size_t i = FIRST_INT; i < ARG_COUNT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_INT) {
            error = "framing has a non-int offset";
            return RT_STAGE_INVALID;
        }
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (!valid(src) || !valid(dst) || src.format != RT_STAGE_F32 ||
        dst.format != RT_STAGE_F32 || src.planes != 3 || dst.planes != 3) {
        error = "framing needs two three-plane F32 buffers";
        return RT_STAGE_INVALID;
    }
    if (src.data == dst.data) {
        error = "framing source and destination are the same buffer";
        return RT_STAGE_INVALID;
    }

    /*
     * drawFrame() returns the source unchanged when it is larger than the
     * frame or exactly the frame size, so the node must never see either
     * shape. Every dimension and offset is signed and checked before it
     * reaches the kernel.
     */
    if (dst.width < src.width || dst.height < src.height) {
        error = "framing destination is smaller than the source";
        return RT_STAGE_INVALID;
    }
    if (dst.width == src.width && dst.height == src.height) {
        error = "framing source already fills the frame";
        return RT_STAGE_INVALID;
    }

    const std::int64_t row_offset = args[FIRST_INT].u.i;
    const std::int64_t col_offset = args[FIRST_INT + 1].u.i;
    if (row_offset < 0 || col_offset < 0 ||
        row_offset + src.height > dst.height ||
        col_offset + src.width > dst.width) {
        error = "framing source does not fit at the given offset";
        return RT_STAGE_INVALID;
    }

    const KernelDef* def = find_kernel("framing");
    assert(def != nullptr);
    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    StageDeviceBufferPtr src_devices[3];
    StageDeviceBufferPtr dst_devices[3];
    const StageBufferShape src_shape{RT_STAGE_F32, src.width, src.height, 1};
    const StageBufferShape dst_shape{RT_STAGE_F32, dst.width, dst.height, 1};
    for (int plane = 0; plane < 3; ++plane) {
        const RtStageBuffer src_plane = plane_view(src, plane);
        const RtStageBuffer dst_plane = plane_view(dst, plane);
        src_devices[plane] = residency->acquire(src_plane.data, src_shape, &error);
        dst_devices[plane] = residency->acquire(dst_plane.data, dst_shape, &error);
        if (!src_devices[plane] || !dst_devices[plane]) {
            return RT_STAGE_FALLBACK;
        }
        if (residency->upload(src_devices[plane], src_plane) != RT_STAGE_OK) {
            error = "framing source upload failed";
            return RT_STAGE_FALLBACK;
        }
    }

    auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device != nullptr ? device->memory() : nullptr;
    };

    const std::size_t src_pitch_bytes = src_devices[0]->row_pitch();
    const std::size_t dst_pitch_bytes = dst_devices[0]->row_pitch();
    for (int plane = 1; plane < 3; ++plane) {
        if (src_devices[plane]->row_pitch() != src_pitch_bytes ||
            dst_devices[plane]->row_pitch() != dst_pitch_bytes) {
            error = "framing planes disagree on the row pitch";
            return RT_STAGE_FALLBACK;
        }
    }
    if (src_pitch_bytes % sizeof(float) != 0 ||
        dst_pitch_bytes % sizeof(float) != 0 ||
        src_pitch_bytes / sizeof(float) < static_cast<std::size_t>(src.width) ||
        dst_pitch_bytes / sizeof(float) < static_cast<std::size_t>(dst.width)) {
        error = "framing row pitch does not cover the image width";
        return RT_STAGE_FALLBACK;
    }

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const cl_float border_r = static_cast<cl_float>(args[2].u.f);
    const cl_float border_g = static_cast<cl_float>(args[3].u.f);
    const cl_float border_b = static_cast<cl_float>(args[4].u.f);
    const cl_int src_width = src.width;
    const cl_int src_height = src.height;
    const cl_int dst_width = dst.width;
    const cl_int dst_height = dst.height;
    const cl_int src_pitch =
        static_cast<cl_int>(src_pitch_bytes / sizeof(float));
    const cl_int dst_pitch =
        static_cast<cl_int>(dst_pitch_bytes / sizeof(float));
    const cl_int row = static_cast<cl_int>(row_offset);
    const cl_int col = static_cast<cl_int>(col_offset);

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
    ok = ok &&
         set_arg(arg++, sizeof(cl_float), &border_r) &&
         set_arg(arg++, sizeof(cl_float), &border_g) &&
         set_arg(arg++, sizeof(cl_float), &border_b) &&
         set_arg(arg++, sizeof(cl_int), &src_width) &&
         set_arg(arg++, sizeof(cl_int), &src_height) &&
         set_arg(arg++, sizeof(cl_int), &dst_width) &&
         set_arg(arg++, sizeof(cl_int), &dst_height) &&
         set_arg(arg++, sizeof(cl_int), &src_pitch) &&
         set_arg(arg++, sizeof(cl_int), &dst_pitch) &&
         set_arg(arg++, sizeof(cl_int), &row) &&
         set_arg(arg++, sizeof(cl_int), &col);
    if (!ok) {
        error = "clSetKernelArg failed";
        return RT_STAGE_FALLBACK;
    }

    const std::size_t global[2] = {static_cast<std::size_t>(dst_width),
                                   static_cast<std::size_t>(dst_height)};
    cl_event done = nullptr;
    if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr, global,
                                       nullptr, 0, nullptr,
                                       &done) != CL_SUCCESS ||
        done == nullptr) {
        error = "framing launch failed";
        return RT_STAGE_ERROR;
    }

    const cl_int wait_status = env->api->WaitForEvents(1, &done);
    env->api->ReleaseEvent(done);
    if (wait_status != CL_SUCCESS) {
        error = "framing did not complete";
        return RT_STAGE_ERROR;
    }

    for (int plane = 0; plane < 3; ++plane) {
        if (residency->readback(dst_devices[plane], plane_view(dst, plane)) !=
            RT_STAGE_OK) {
            error = "framing readback failed";
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

    /*
     * The kernel addresses all three planes through one buffer, so the whole
     * image is one allocation. Uploading each plane into its own compact
     * buffer would put the plane writes past the first allocation.
     */
    const StageBufferShape shape{RT_STAGE_F32, image.width, image.height, 3};
    StageDeviceBufferPtr device_image =
        residency->acquire(image.data, shape, &error);
    if (!device_image) {
        return RT_STAGE_FALLBACK;
    }
    if (residency->upload(device_image, image) != RT_STAGE_OK) {
        error = "thumbnail.whitebalance upload failed";
        return RT_STAGE_FALLBACK;
    }

    auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(device_image);
    if (!device) {
        error = "thumbnail.whitebalance device buffer mismatch";
        return RT_STAGE_FALLBACK;
    }

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    cl_mem memory = device->memory();
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

    if (residency->readback(device_image, image) != RT_STAGE_OK) {
        error = "thumbnail.whitebalance readback failed";
        return RT_STAGE_ERROR;
    }

    return RT_STAGE_OK;
}


/*
 * Thumbnail coarse transform. Arguments: [0] source, [1] destination,
 * [2..4] ints deg, hflip, vflip. The destination has the rotated dimensions;
 * the kernel maps each destination pixel back to its source.
 */
RtStageStatus run_thumbnail_permute(
    const std::shared_ptr<OpenCLEnv>& env,
    const std::shared_ptr<OpenCLResidency>& residency,
    const std::vector<RtStageArg>& args, const char* node, bool eight_bit,
    std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 2;
    constexpr std::size_t INT_ARGS = 3;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + INT_ARGS;

    if (args.size() != ARG_COUNT ||
        args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_BUFFER) {
        error = "thumbnail transform expects two buffers and three ints";
        return RT_STAGE_INVALID;
    }
    for (std::size_t i = BUFFER_ARGS; i < ARG_COUNT; ++i) {
        if (args[i].kind != RT_STAGE_ARG_INT) {
            error = "thumbnail transform has a non-int scalar";
            return RT_STAGE_INVALID;
        }
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (eight_bit) {
        if (!valid(src) || !valid(dst) || src.format != RT_STAGE_U8 ||
            dst.format != RT_STAGE_U8 || src.planes != 1 || dst.planes != 1) {
            error = "thumbnail transform needs two packed U8 buffers";
            return RT_STAGE_INVALID;
        }
    } else if (!valid(src) || !valid(dst) || src.format != RT_STAGE_F32 ||
               dst.format != RT_STAGE_F32 || src.planes != 3 ||
               dst.planes != 3) {
        error = "thumbnail transform needs two three-plane F32 buffers";
        return RT_STAGE_INVALID;
    }
    if (src.data == dst.data) {
        error = "thumbnail transform needs distinct buffers";
        return RT_STAGE_INVALID;
    }

    const int deg = static_cast<int>(args[2].u.i);
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
        error = "thumbnail transform needs a rotation of 0, 90, 180 or 270";
        return RT_STAGE_INVALID;
    }
    const int rotated_w = deg == 90 || deg == 270 ? src.height : src.width;
    const int rotated_h = deg == 90 || deg == 270 ? src.width : src.height;
    if (dst.width != rotated_w || dst.height != rotated_h) {
        error = "thumbnail transform destination has the wrong dimensions";
        return RT_STAGE_INVALID;
    }

    const KernelDef* def = find_kernel(node);
    assert(def != nullptr);
    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const StageBufferShape src_shape = eight_bit
        ? StageBufferShape{RT_STAGE_U8, src.width, src.height, 1}
        : StageBufferShape{RT_STAGE_F32, src.width, src.height, 3};
    const StageBufferShape dst_shape = eight_bit
        ? StageBufferShape{RT_STAGE_U8, dst.width, dst.height, 1}
        : StageBufferShape{RT_STAGE_F32, dst.width, dst.height, 3};
    StageDeviceBufferPtr src_device =
        residency->acquire(src.data, src_shape, &error);
    StageDeviceBufferPtr dst_device =
        residency->acquire(dst.data, dst_shape, &error);
    if (!src_device || !dst_device) {
        return RT_STAGE_FALLBACK;
    }
    if (residency->upload(src_device, src) != RT_STAGE_OK) {
        error = "thumbnail transform upload failed";
        return RT_STAGE_FALLBACK;
    }

    auto device_src = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(src_device);
    auto device_dst = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(dst_device);
    if (!device_src || !device_dst) {
        error = "thumbnail transform device buffer mismatch";
        return RT_STAGE_FALLBACK;
    }

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const cl_int src_w = src.width;
    const cl_int src_h = src.height;
    const cl_int dst_w = dst.width;
    const cl_int dst_h = dst.height;
    const cl_int rotation = deg;
    const cl_int hflip = static_cast<cl_int>(args[3].u.i != 0);
    const cl_int vflip = static_cast<cl_int>(args[4].u.i != 0);

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    cl_mem src_memory = device_src->memory();
    cl_mem dst_memory = device_dst->memory();
    bool args_ok =
        set_arg(0, sizeof(cl_mem), &src_memory) &&
        set_arg(1, sizeof(cl_mem), &dst_memory) &&
        set_arg(2, sizeof(cl_int), &src_w) &&
        set_arg(3, sizeof(cl_int), &src_h) &&
        set_arg(4, sizeof(cl_int), &dst_w) &&
        set_arg(5, sizeof(cl_int), &dst_h) &&
        set_arg(6, sizeof(cl_int), &rotation) &&
        set_arg(7, sizeof(cl_int), &hflip) &&
        set_arg(8, sizeof(cl_int), &vflip);
    if (eight_bit) {
        const cl_int src_pitch = static_cast<cl_int>(device_src->row_pitch());
        const cl_int dst_pitch = static_cast<cl_int>(device_dst->row_pitch());
        args_ok = args_ok && set_arg(9, sizeof(cl_int), &src_pitch) &&
                  set_arg(10, sizeof(cl_int), &dst_pitch);
    } else {
        const cl_int src_pitch =
            static_cast<cl_int>(device_src->row_pitch() / sizeof(float));
        const cl_int src_plane_pitch =
            static_cast<cl_int>(device_src->plane_pitch() / sizeof(float));
        const cl_int dst_pitch =
            static_cast<cl_int>(device_dst->row_pitch() / sizeof(float));
        const cl_int dst_plane_pitch =
            static_cast<cl_int>(device_dst->plane_pitch() / sizeof(float));
        args_ok = args_ok && set_arg(9, sizeof(cl_int), &src_pitch) &&
                  set_arg(10, sizeof(cl_int), &src_plane_pitch) &&
                  set_arg(11, sizeof(cl_int), &dst_pitch) &&
                  set_arg(12, sizeof(cl_int), &dst_plane_pitch);
    }
    if (!args_ok) {
        error = "clSetKernelArg failed";
        return RT_STAGE_FALLBACK;
    }

    const std::size_t global =
        static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h);
    cl_event done = nullptr;
    if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 1, nullptr, &global,
                                       nullptr, 0, nullptr,
                                       &done) != CL_SUCCESS ||
        done == nullptr) {
        error = "thumbnail transform launch failed";
        return RT_STAGE_ERROR;
    }
    const cl_int wait_status = env->api->WaitForEvents(1, &done);
    env->api->ReleaseEvent(done);
    if (wait_status != CL_SUCCESS) {
        error = "thumbnail transform did not complete";
        return RT_STAGE_ERROR;
    }
    if (residency->readback(dst_device, dst) != RT_STAGE_OK) {
        error = "thumbnail transform readback failed";
        return RT_STAGE_ERROR;
    }

    return RT_STAGE_OK;
}

/*
 * Thumbnail scale. Arguments: [0] source, [1] destination, [2] interpolation
 * (0 nearest, 1 bilinear). The source may be packed 8-bit, planar 16-bit or
 * planar float; the destination is three-plane float, or packed 8-bit for the
 * `thumbnail.scale.u8` node.
 */

/* ABI interpolation values: TI_Nearest and TI_Bilinear. */
constexpr std::int64_t SCALE_INTERP_NEAREST = 0;
constexpr std::int64_t SCALE_INTERP_BILINEAR = 1;

/* Kernel interpolation modes; copy is the reference's same-size short cut. */
constexpr int SCALE_MODE_NEAREST = 0;
constexpr int SCALE_MODE_BILINEAR = 1;
constexpr int SCALE_MODE_COPY = 2;

/* Kernel source-format selectors. */
constexpr int SCALE_SRC_F32 = 0;
constexpr int SCALE_SRC_U16 = 1;
constexpr int SCALE_SRC_U8 = 2;

struct ScaleAxis {
    std::int32_t index = 0;
    std::int32_t next = 0;
    float fraction = 0.0f;
};

/* The OpenCL struct is three 32-bit words, packed without padding. */
static_assert(sizeof(ScaleAxis) == 3 * sizeof(std::uint32_t),
              "ScaleAxis must match the kernel struct");

/*
 * PlanarRGBData::resizeImgTo() advances the source coordinate with a float
 * accumulator, so the host repeats the same additions; recomputing
 * `i * step` drifts by an ulp and changes the interpolated sample.
 */
bool build_planar_axis(int src_length, int dst_length, std::vector<ScaleAxis>& axis)
{
    assert(src_length > 0 && dst_length > 0);
    const float step = static_cast<float>(src_length) /
                       static_cast<float>(dst_length);
    float position = 0.0f;
    axis.resize(static_cast<std::size_t>(dst_length));
    for (int i = 0; i < dst_length; ++i, position += step) {
        const int index = static_cast<int>(position);
        if (index < 0 || index >= src_length) {
            return false;
        }
        const int next = index < src_length - 1 ? index + 1 : index;
        axis[static_cast<std::size_t>(i)] = ScaleAxis{
            index, next, position - static_cast<float>(index)};
    }
    return true;
}

/*
 * ChunkyRGBData::resizeImgTo() derives the source index with integer
 * multiplication and the fraction from that index. The thumbnail range keeps
 * `i * length` inside 64 bits; the reference's int expression cannot overflow
 * there either.
 */
bool build_chunky_axis(int src_length, int dst_length, std::vector<ScaleAxis>& axis)
{
    assert(src_length > 0 && dst_length > 0);
    axis.resize(static_cast<std::size_t>(dst_length));
    for (int i = 0; i < dst_length; ++i) {
        const std::int64_t product =
            static_cast<std::int64_t>(i) * static_cast<std::int64_t>(src_length);
        const int index = static_cast<int>(product / dst_length);
        if (index < 0 || index >= src_length) {
            return false;
        }
        const float position = static_cast<float>(i) *
                               static_cast<float>(src_length) /
                               static_cast<float>(dst_length);
        const int next = index + 1 < src_length ? index + 1 : index;
        axis[static_cast<std::size_t>(i)] = ScaleAxis{
            index, next, position - static_cast<float>(index)};
    }
    return true;
}

/* Nearest and the same-size copy take one source sample per axis. */
void build_nearest_axis(int src_length, int dst_length, std::vector<ScaleAxis>& axis)
{
    assert(src_length > 0 && dst_length > 0);
    axis.resize(static_cast<std::size_t>(dst_length));
    for (int i = 0; i < dst_length; ++i) {
        const std::int64_t product =
            static_cast<std::int64_t>(i) * static_cast<std::int64_t>(src_length);
        const int index = static_cast<int>(product / dst_length);
        assert(index >= 0 && index < src_length);
        axis[static_cast<std::size_t>(i)] = ScaleAxis{index, index, 0.0f};
    }
}

bool build_scale_axis(int source_format, int mode, int src_length, int dst_length,
                      std::vector<ScaleAxis>& axis)
{
    if (mode == SCALE_MODE_COPY || mode == SCALE_MODE_NEAREST) {
        build_nearest_axis(src_length, dst_length, axis);
        return true;
    }
    if (source_format == SCALE_SRC_U8) {
        return build_chunky_axis(src_length, dst_length, axis);
    }
    return build_planar_axis(src_length, dst_length, axis);
}

RtStageStatus run_thumbnail_scale(
    const std::shared_ptr<OpenCLEnv>& env,
    const std::shared_ptr<OpenCLResidency>& residency,
    const std::vector<RtStageArg>& args, const char* node, bool eight_bit,
    std::string& error)
{
    constexpr std::size_t BUFFER_ARGS = 2;
    constexpr std::size_t INT_ARGS = 1;
    constexpr std::size_t ARG_COUNT = BUFFER_ARGS + INT_ARGS;

    if (args.size() != ARG_COUNT ||
        args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_BUFFER ||
        args[2].kind != RT_STAGE_ARG_INT) {
        error = "thumbnail scale expects two buffers and an int";
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (!valid(src) || !valid(dst)) {
        error = "thumbnail scale has an invalid buffer";
        return RT_STAGE_INVALID;
    }
    if (src.data == dst.data) {
        error = "thumbnail scale needs distinct buffers";
        return RT_STAGE_INVALID;
    }

    const std::int64_t interp = args[2].u.i;
    if (interp != SCALE_INTERP_NEAREST && interp != SCALE_INTERP_BILINEAR) {
        error = "thumbnail scale has an unknown interpolation value " +
                std::to_string(interp);
        return RT_STAGE_INVALID;
    }

    int source_format = SCALE_SRC_F32;
    if (eight_bit) {
        if (src.format != RT_STAGE_U8 || src.planes != 1 ||
            dst.format != RT_STAGE_U8 || dst.planes != 1) {
            error = "thumbnail scale (8-bit) needs two packed U8 buffers";
            return RT_STAGE_INVALID;
        }
        source_format = SCALE_SRC_U8;
    } else {
        if (dst.format != RT_STAGE_F32 || dst.planes != 3) {
            error = "thumbnail scale needs a three-plane F32 destination";
            return RT_STAGE_INVALID;
        }
        if (src.format == RT_STAGE_F32 || src.format == RT_STAGE_U16) {
            if (src.planes != 3) {
                error = "thumbnail scale needs a three-plane planar source";
                return RT_STAGE_INVALID;
            }
            source_format =
                src.format == RT_STAGE_F32 ? SCALE_SRC_F32 : SCALE_SRC_U16;
        } else if (src.format == RT_STAGE_U8) {
            if (src.planes != 1) {
                error = "thumbnail scale needs a packed U8 source";
                return RT_STAGE_INVALID;
            }
            source_format = SCALE_SRC_U8;
        } else {
            error = "thumbnail scale has an unsupported source format";
            return RT_STAGE_INVALID;
        }
    }

    /*
     * The reference converts the same-size case without resampling, before it
     * looks at the interpolation, so the mode encodes that priority.
     */
    const bool same_size = src.width == dst.width && src.height == dst.height;
    const int mode = same_size
        ? SCALE_MODE_COPY
        : (interp == SCALE_INTERP_NEAREST ? SCALE_MODE_NEAREST
                                          : SCALE_MODE_BILINEAR);

    std::vector<ScaleAxis> rows;
    std::vector<ScaleAxis> cols;
    if (!build_scale_axis(source_format, mode, src.height, dst.height, rows) ||
        !build_scale_axis(source_format, mode, src.width, dst.width, cols)) {
        error = "thumbnail scale cannot map the source onto the destination";
        return RT_STAGE_INVALID;
    }

    const KernelDef* def = find_kernel(node);
    assert(def != nullptr);
    cl_kernel kernel = compiled_kernel(env, def, error);
    if (kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const StageBufferShape src_shape{src.format, src.width, src.height,
                                     src.planes};
    const StageBufferShape dst_shape{dst.format, dst.width, dst.height,
                                     dst.planes};
    StageDeviceBufferPtr src_device = residency->acquire(src.data, src_shape, &error);
    StageDeviceBufferPtr dst_device = residency->acquire(dst.data, dst_shape, &error);
    if (!src_device || !dst_device) {
        return RT_STAGE_FALLBACK;
    }
    if (residency->upload(src_device, src) != RT_STAGE_OK) {
        error = "thumbnail scale upload failed";
        return RT_STAGE_FALLBACK;
    }

    auto device_src = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(src_device);
    auto device_dst = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(dst_device);
    if (!device_src || !device_dst) {
        error = "thumbnail scale device buffer mismatch";
        return RT_STAGE_FALLBACK;
    }

    /* The two axes travel as scratch buffers and die with the submission. */
    const auto create_scratch = [&](std::size_t bytes, const void* data,
                                    cl_mem& out) -> bool {
        cl_int scratch_error = CL_SUCCESS;
        out = env->api->CreateBuffer(env->context,
                                     CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     bytes, const_cast<void*>(data),
                                     &scratch_error);
        return out != nullptr;
    };
    constexpr std::size_t AXIS_BUFFERS = 2;
    std::vector<cl_mem> scratch(AXIS_BUFFERS, nullptr);
    if (!create_scratch(rows.size() * sizeof(ScaleAxis), rows.data(),
                        scratch[0]) ||
        !create_scratch(cols.size() * sizeof(ScaleAxis), cols.data(),
                        scratch[1])) {
        for (cl_mem memory : scratch) {
            if (memory != nullptr) {
                env->api->ReleaseMemObject(memory);
            }
        }
        error = "thumbnail scale scratch allocation failed";
        return RT_STAGE_FALLBACK;
    }

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const cl_mem src_memory = device_src->memory();
    const cl_mem dst_memory = device_dst->memory();
    const cl_int src_format = source_format;
    const cl_int scale_mode = mode;
    const cl_int dst_width = dst.width;
    const cl_int dst_height = dst.height;
    /* Pitch units follow the element type each kernel indexes with. */
    const cl_int src_element_bytes = static_cast<cl_int>(bytes_per_pixel(src.format));
    const cl_int src_pitch =
        static_cast<cl_int>(device_src->row_pitch()) / src_element_bytes;
    const cl_int src_plane_pitch =
        static_cast<cl_int>(device_src->plane_pitch()) / src_element_bytes;
    const cl_int dst_element_bytes = static_cast<cl_int>(bytes_per_pixel(dst.format));
    const cl_int dst_pitch =
        static_cast<cl_int>(device_dst->row_pitch()) / dst_element_bytes;
    const cl_int dst_plane_pitch =
        static_cast<cl_int>(device_dst->plane_pitch()) / dst_element_bytes;

    const auto set_arg = [&](cl_uint index, std::size_t bytes,
                             const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };

    bool args_ok =
        set_arg(0, sizeof(cl_mem), &src_memory) &&
        set_arg(1, sizeof(cl_mem), &dst_memory) &&
        set_arg(2, sizeof(cl_mem), &scratch[0]) &&
        set_arg(3, sizeof(cl_mem), &scratch[1]);
    if (eight_bit) {
        args_ok = args_ok &&
                  set_arg(4, sizeof(cl_int), &scale_mode) &&
                  set_arg(5, sizeof(cl_int), &dst_width) &&
                  set_arg(6, sizeof(cl_int), &dst_height) &&
                  set_arg(7, sizeof(cl_int), &src_pitch) &&
                  set_arg(8, sizeof(cl_int), &dst_pitch);
    } else {
        args_ok = args_ok &&
                  set_arg(4, sizeof(cl_int), &src_format) &&
                  set_arg(5, sizeof(cl_int), &scale_mode) &&
                  set_arg(6, sizeof(cl_int), &dst_width) &&
                  set_arg(7, sizeof(cl_int), &dst_height) &&
                  set_arg(8, sizeof(cl_int), &src_pitch) &&
                  set_arg(9, sizeof(cl_int), &src_plane_pitch) &&
                  set_arg(10, sizeof(cl_int), &dst_pitch) &&
                  set_arg(11, sizeof(cl_int), &dst_plane_pitch);
    }
    if (!args_ok) {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        error = "clSetKernelArg failed";
        return RT_STAGE_FALLBACK;
    }

    const std::size_t global = static_cast<std::size_t>(dst.height);
    cl_event done = nullptr;
    if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 1, nullptr, &global,
                                       nullptr, 0, nullptr,
                                       &done) != CL_SUCCESS ||
        done == nullptr) {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        error = "thumbnail scale launch failed";
        return RT_STAGE_ERROR;
    }
    const cl_int wait_status = env->api->WaitForEvents(1, &done);
    env->api->ReleaseEvent(done);
    for (cl_mem memory : scratch) {
        env->api->ReleaseMemObject(memory);
    }
    if (wait_status != CL_SUCCESS) {
        error = "thumbnail scale did not complete";
        return RT_STAGE_ERROR;
    }
    if (residency->readback(dst_device, dst) != RT_STAGE_OK) {
        error = "thumbnail scale readback failed";
        return RT_STAGE_ERROR;
    }

    return RT_STAGE_OK;
}

/*
 * Output sharpening, USM and undamped RLD. Arguments are documented in
 * stage/algorithms/sharpen_cl.h. The L plane is the only transfer: the mask,
 * the blur, the RLD estimate and the RLD intermediate live in device scratch.
 */
RtStageStatus run_sharpen(const std::shared_ptr<OpenCLEnv>& env,
                          const std::shared_ptr<OpenCLResidency>& residency,
                          const std::vector<RtStageArg>& args,
                          std::string& error)
{
    SharpenNodeParams p;
    RtStageBuffer image{};
    if (!sharpen_node_decode(args.data(),
                             static_cast<std::uint32_t>(args.size()), p, image,
                             error)) {
        return RT_STAGE_INVALID;
    }
    if (image.width < 8 || image.height < 8) {
        error = "output.sharpen needs at least 8x8 pixels";
        return RT_STAGE_INVALID;
    }
    if (!sharpen_device_supported(p, error)) {
        return RT_STAGE_FALLBACK;
    }

    const int width = image.width;
    const int height = image.height;

    /*
     * The CPU deconvolution returns before it clamps L when the amount is zero
     * and there is no blur-radius pre-pass, so the plane is unchanged. Report
     * OK without touching it; this is the same no-op on the host.
     */
    if (p.method == SHARPEN_METHOD_RLD && p.deconvamount == 0) {
        return RT_STAGE_OK;
    }

    const KernelDef* def = find_kernel("output.sharpen");
    assert(def != nullptr);
    cl_kernel mask_kernel =
        compiled_kernel_entry(env, def, "rt_sharpen_mask", error);
    cl_kernel iir_kernel =
        compiled_kernel_entry(env, def, "rt_sharpen_iir_line", error);
    if (mask_kernel == nullptr || iir_kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    StageDeviceBufferPtr image_device =
        residency->acquire(image.data, {RT_STAGE_F32, width, height, 1}, &error);
    if (!image_device) {
        return RT_STAGE_FALLBACK;
    }
    if (residency->upload(image_device, image) != RT_STAGE_OK) {
        error = "output.sharpen upload failed";
        return RT_STAGE_FALLBACK;
    }

    auto device = std::dynamic_pointer_cast<OpenCLDeviceBuffer>(image_device);
    if (!device) {
        error = "output.sharpen device buffer type mismatch";
        return RT_STAGE_FALLBACK;
    }
    if (device->row_pitch() % sizeof(float) != 0) {
        error = "output.sharpen row pitch is not sample aligned";
        return RT_STAGE_FALLBACK;
    }
    const int pitch = static_cast<int>(device->row_pitch() / sizeof(float));
    if (pitch < width) {
        error = "output.sharpen row pitch does not cover the width";
        return RT_STAGE_FALLBACK;
    }
    cl_mem image_memory = device->memory();

    std::vector<cl_mem> scratch;
    const std::size_t scratch_bytes = device->row_pitch() *
                                      static_cast<std::size_t>(height);
    const auto release_scratch = [&]() {
        for (cl_mem memory : scratch) {
            env->api->ReleaseMemObject(memory);
        }
        scratch.clear();
    };
    const auto create_scratch = [&]() -> bool {
        for (int i = 0; i < 3; ++i) {
            cl_int scratch_error = CL_SUCCESS;
            cl_mem memory =
                env->api->CreateBuffer(env->context, CL_MEM_READ_WRITE,
                                       scratch_bytes, nullptr, &scratch_error);
            if (memory == nullptr) {
                return false;
            }
            scratch.push_back(memory);
        }
        return true;
    };
    if (!create_scratch()) {
        release_scratch();
        error = "output.sharpen device scratch allocation failed";
        return RT_STAGE_FALLBACK;
    }
    cl_mem mask_memory = scratch[0];
    cl_mem temp_memory = scratch[1];
    cl_mem blur_memory = scratch[2];

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const auto set_arg = [&](cl_kernel kernel, cl_uint index,
                             std::size_t bytes, const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) == CL_SUCCESS;
    };
    const auto run_2d = [&](cl_kernel kernel, int global_width,
                            int global_height) -> bool {
        const std::size_t global[2] = {
            static_cast<std::size_t>(global_width),
            static_cast<std::size_t>(global_height)};
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr,
                                           global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            return false;
        }
        const cl_int wait_status = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        return wait_status == CL_SUCCESS;
    };
    const auto run_1d = [&](cl_kernel kernel, int lines) -> bool {
        const std::size_t global = static_cast<std::size_t>(lines);
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 1, nullptr,
                                           &global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            return false;
        }
        const cl_int wait_status = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        return wait_status == CL_SUCCESS;
    };

    const cl_int cl_width = width;
    const cl_int cl_height = height;
    const cl_int cl_pitch = pitch;

    /* Contrast mask, then its sigma-2 blur (skipped when the mask is flat). */
    const cl_float contrast_threshold =
        static_cast<cl_float>(p.contrast / 100.0);
    const cl_float contrast_scale =
        static_cast<cl_float>(SHARPEN_MASK_GRADIENT_SCALE);
    const bool mask_ok =
        set_arg(mask_kernel, 0, sizeof(cl_mem), &image_memory) &&
        set_arg(mask_kernel, 1, sizeof(cl_mem), &mask_memory) &&
        set_arg(mask_kernel, 2, sizeof(cl_int), &cl_width) &&
        set_arg(mask_kernel, 3, sizeof(cl_int), &cl_height) &&
        set_arg(mask_kernel, 4, sizeof(cl_int), &cl_pitch) &&
        set_arg(mask_kernel, 5, sizeof(cl_float), &contrast_threshold) &&
        set_arg(mask_kernel, 6, sizeof(cl_float), &contrast_scale) &&
        run_2d(mask_kernel, width, height);
    if (!mask_ok) {
        release_scratch();
        error = "output.sharpen mask failed";
        return RT_STAGE_ERROR;
    }

    const auto run_iir = [&](cl_mem source, cl_mem target, int length,
                             int src_base_step, int dst_base_step,
                             int src_stride, int dst_stride, int lines,
                             const SharpenIirFactors& factors) -> bool {
        const cl_int cl_length = length;
        const cl_int cl_src_base_step = src_base_step;
        const cl_int cl_dst_base_step = dst_base_step;
        const cl_int cl_src_stride = src_stride;
        const cl_int cl_dst_stride = dst_stride;
        cl_float m[9];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                m[i * 3 + j] = factors.m[i][j];
            }
        }
        const bool ok =
            set_arg(iir_kernel, 0, sizeof(cl_mem), &source) &&
            set_arg(iir_kernel, 1, sizeof(cl_mem), &target) &&
            set_arg(iir_kernel, 2, sizeof(cl_int), &cl_length) &&
            set_arg(iir_kernel, 3, sizeof(cl_int), &cl_src_base_step) &&
            set_arg(iir_kernel, 4, sizeof(cl_int), &cl_dst_base_step) &&
            set_arg(iir_kernel, 5, sizeof(cl_int), &cl_src_stride) &&
            set_arg(iir_kernel, 6, sizeof(cl_int), &cl_dst_stride) &&
            set_arg(iir_kernel, 7, sizeof(cl_float), &factors.B) &&
            set_arg(iir_kernel, 8, sizeof(cl_float), &factors.b1) &&
            set_arg(iir_kernel, 9, sizeof(cl_float), &factors.b2) &&
            set_arg(iir_kernel, 10, sizeof(cl_float), &factors.b3);
        if (!ok) {
            return false;
        }
        for (int i = 0; i < 9; ++i) {
            if (!set_arg(iir_kernel, static_cast<cl_uint>(11 + i),
                         sizeof(cl_float), &m[i])) {
                return false;
            }
        }
        return run_1d(iir_kernel, lines);
    };

    if (contrast_threshold != 0.0f) {
        SharpenIirFactors mask_factors;
        sharpen_iir_factors(SHARPEN_MASK_SIGMA, mask_factors);
        if (!run_iir(mask_memory, temp_memory, width, pitch, pitch, 1, 1,
                     height, mask_factors) ||
            !run_iir(temp_memory, mask_memory, height, 1, 1, pitch, pitch,
                     width, mask_factors)) {
            release_scratch();
            error = "output.sharpen mask blur failed";
            return RT_STAGE_ERROR;
        }
    }

    if (p.method == SHARPEN_METHOD_RLD) {
        const double rld_sigma = p.deconvradius / p.scale;

        cl_kernel init_kernel =
            compiled_kernel_entry(env, def, "rt_sharpen_rld_init", error);
        cl_kernel final_kernel =
            compiled_kernel_entry(env, def, "rt_sharpen_rld_final", error);
        if (init_kernel == nullptr || final_kernel == nullptr) {
            release_scratch();
            return RT_STAGE_FALLBACK;
        }

        /*
         * gaussianBlur() skips below GAUSS_SKIP, uses 3x3 below 0.6, 5x5 up
         * to 0.84 and 7x7 up to 1.15. The support check has already excluded
         * the separable IIR range.
         */
        enum class RldClass { Skip, Fir3, Kernel5, Kernel7 };
        RldClass rld_class = RldClass::Skip;
        if (rld_sigma >= SHARPEN_GAUSS_SKIP) {
            if (rld_sigma < SHARPEN_FIR_LIMIT) {
                rld_class = RldClass::Fir3;
            } else if (rld_sigma <= SHARPEN_5X5_LIMIT) {
                rld_class = RldClass::Kernel5;
            } else {
                rld_class = RldClass::Kernel7;
            }
        }

        cl_kernel div_kernel = nullptr;
        cl_kernel mult_kernel = nullptr;
        SharpenFirFactors fir_factors;
        SharpenKernel5 kernel5;
        SharpenKernel7 kernel7;
        switch (rld_class) {
        case RldClass::Skip:
            break;
        case RldClass::Fir3:
            sharpen_fir_factors(rld_sigma, fir_factors);
            div_kernel =
                compiled_kernel_entry(env, def, "rt_sharpen_div3", error);
            mult_kernel =
                compiled_kernel_entry(env, def, "rt_sharpen_mult3", error);
            break;
        case RldClass::Kernel5:
            sharpen_kernel5(rld_sigma, kernel5);
            div_kernel =
                compiled_kernel_entry(env, def, "rt_sharpen_div5", error);
            mult_kernel =
                compiled_kernel_entry(env, def, "rt_sharpen_mult5", error);
            break;
        case RldClass::Kernel7:
            sharpen_kernel7(rld_sigma, kernel7);
            div_kernel =
                compiled_kernel_entry(env, def, "rt_sharpen_div7", error);
            mult_kernel =
                compiled_kernel_entry(env, def, "rt_sharpen_mult7", error);
            break;
        }
        if (rld_class != RldClass::Skip &&
            (div_kernel == nullptr || mult_kernel == nullptr)) {
            release_scratch();
            return RT_STAGE_FALLBACK;
        }

        cl_mem estimate_memory = temp_memory;
        cl_mem tmp_memory = blur_memory;
        const bool init_ok =
            set_arg(init_kernel, 0, sizeof(cl_mem), &image_memory) &&
            set_arg(init_kernel, 1, sizeof(cl_mem), &estimate_memory) &&
            set_arg(init_kernel, 2, sizeof(cl_int), &cl_width) &&
            set_arg(init_kernel, 3, sizeof(cl_int), &cl_height) &&
            set_arg(init_kernel, 4, sizeof(cl_int), &cl_pitch) &&
            run_2d(init_kernel, width, height);
        if (!init_ok) {
            release_scratch();
            error = "output.sharpen RLD initialisation failed";
            return RT_STAGE_ERROR;
        }

        if (rld_class != RldClass::Skip) {
            const auto run_div = [&]() -> bool {
                bool ok =
                    set_arg(div_kernel, 0, sizeof(cl_mem), &estimate_memory) &&
                    set_arg(div_kernel, 1, sizeof(cl_mem), &tmp_memory) &&
                    set_arg(div_kernel, 2, sizeof(cl_mem), &image_memory) &&
                    set_arg(div_kernel, 3, sizeof(cl_int), &cl_width) &&
                    set_arg(div_kernel, 4, sizeof(cl_int), &cl_height) &&
                    set_arg(div_kernel, 5, sizeof(cl_int), &cl_pitch);
                cl_uint arg = 6;
                if (rld_class == RldClass::Fir3) {
                    ok = ok &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.c0) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.c1) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.c2) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.b0) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.b1);
                } else if (rld_class == RldClass::Kernel5) {
                    ok = ok &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c00) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c10) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c11) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c20) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c21);
                } else {
                    ok = ok &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c00) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c10) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c11) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c20) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c21) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c22) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c30) &&
                         set_arg(div_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c31);
                }
                return ok && run_2d(div_kernel, width, height);
            };
            const auto run_mult = [&]() -> bool {
                bool ok =
                    set_arg(mult_kernel, 0, sizeof(cl_mem), &tmp_memory) &&
                    set_arg(mult_kernel, 1, sizeof(cl_mem), &estimate_memory) &&
                    set_arg(mult_kernel, 2, sizeof(cl_int), &cl_width) &&
                    set_arg(mult_kernel, 3, sizeof(cl_int), &cl_height) &&
                    set_arg(mult_kernel, 4, sizeof(cl_int), &cl_pitch);
                cl_uint arg = 5;
                if (rld_class == RldClass::Fir3) {
                    ok = ok &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.c0) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.c1) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.c2) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.b0) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &fir_factors.b1);
                } else if (rld_class == RldClass::Kernel5) {
                    ok = ok &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c00) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c10) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c11) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c20) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel5.c21);
                } else {
                    ok = ok &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c00) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c10) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c11) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c20) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c21) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c22) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c30) &&
                         set_arg(mult_kernel, arg++, sizeof(cl_float),
                                 &kernel7.c31);
                }
                return ok && run_2d(mult_kernel, width, height);
            };

            for (int iteration = 0; iteration < p.deconviter; ++iteration) {
                if (!run_div() || !run_mult()) {
                    release_scratch();
                    error = "output.sharpen RLD iteration failed";
                    return RT_STAGE_ERROR;
                }
            }
        }

        const cl_float rld_amount =
            static_cast<cl_float>(p.deconvamount) / 100.0f;
        const bool final_ok =
            set_arg(final_kernel, 0, sizeof(cl_mem), &image_memory) &&
            set_arg(final_kernel, 1, sizeof(cl_mem), &estimate_memory) &&
            set_arg(final_kernel, 2, sizeof(cl_mem), &mask_memory) &&
            set_arg(final_kernel, 3, sizeof(cl_int), &cl_width) &&
            set_arg(final_kernel, 4, sizeof(cl_int), &cl_height) &&
            set_arg(final_kernel, 5, sizeof(cl_int), &cl_pitch) &&
            set_arg(final_kernel, 6, sizeof(cl_float), &rld_amount) &&
            run_2d(final_kernel, width, height);
        if (!final_ok) {
            release_scratch();
            error = "output.sharpen RLD final blend failed";
            return RT_STAGE_ERROR;
        }

        const RtStageStatus readback = residency->readback(image_device, image);
        release_scratch();
        if (readback != RT_STAGE_OK) {
            error = "output.sharpen RLD readback failed";
            return RT_STAGE_ERROR;
        }
        return RT_STAGE_OK;
    }

    const double sigma = p.radius / p.scale;
    cl_kernel fir_kernel =
        compiled_kernel_entry(env, def, "rt_sharpen_fir3", error);
    cl_kernel apply_kernel =
        compiled_kernel_entry(env, def, "rt_sharpen_apply", error);
    if (fir_kernel == nullptr || apply_kernel == nullptr) {
        release_scratch();
        return RT_STAGE_FALLBACK;
    }

    /* Main USM blur. */
    int use_blur = 1;
    if (sigma < SHARPEN_GAUSS_SKIP) {
        use_blur = 0;
    } else if (sigma < SHARPEN_FIR_LIMIT) {
        SharpenFirFactors factors;
        sharpen_fir_factors(sigma, factors);
        const cl_int src_pitch = pitch;
        const cl_int dst_pitch = pitch;
        const bool ok =
            set_arg(fir_kernel, 0, sizeof(cl_mem), &image_memory) &&
            set_arg(fir_kernel, 1, sizeof(cl_mem), &blur_memory) &&
            set_arg(fir_kernel, 2, sizeof(cl_int), &cl_width) &&
            set_arg(fir_kernel, 3, sizeof(cl_int), &cl_height) &&
            set_arg(fir_kernel, 4, sizeof(cl_int), &src_pitch) &&
            set_arg(fir_kernel, 5, sizeof(cl_int), &dst_pitch) &&
            set_arg(fir_kernel, 6, sizeof(cl_float), &factors.c0) &&
            set_arg(fir_kernel, 7, sizeof(cl_float), &factors.c1) &&
            set_arg(fir_kernel, 8, sizeof(cl_float), &factors.c2) &&
            set_arg(fir_kernel, 9, sizeof(cl_float), &factors.b0) &&
            set_arg(fir_kernel, 10, sizeof(cl_float), &factors.b1) &&
            run_2d(fir_kernel, width, height);
        if (!ok) {
            release_scratch();
            error = "output.sharpen FIR blur failed";
            return RT_STAGE_ERROR;
        }
    } else {
        SharpenIirFactors factors;
        sharpen_iir_factors(sigma, factors);
        if (!run_iir(image_memory, temp_memory, width, pitch, pitch, 1, 1,
                     height, factors) ||
            !run_iir(temp_memory, blur_memory, height, 1, 1, pitch, pitch,
                     width, factors)) {
            release_scratch();
            error = "output.sharpen IIR blur failed";
            return RT_STAGE_ERROR;
        }
    }

    const cl_int use_blur_flag = use_blur;
    const cl_int cl_amount = p.amount;
    const cl_int cl_bl = p.threshold_bottom_left;
    const cl_int cl_tl = p.threshold_top_left;
    const cl_int cl_br = p.threshold_bottom_right;
    const cl_int cl_tr = p.threshold_top_right;
    const cl_int cl_is_double = p.threshold_is_double ? 1 : 0;
    const cl_int cl_start_at_one = p.threshold_start_at_one ? 1 : 0;
    const bool apply_ok =
        set_arg(apply_kernel, 0, sizeof(cl_mem), &image_memory) &&
        set_arg(apply_kernel, 1, sizeof(cl_mem), &blur_memory) &&
        set_arg(apply_kernel, 2, sizeof(cl_mem), &mask_memory) &&
        set_arg(apply_kernel, 3, sizeof(cl_int), &cl_width) &&
        set_arg(apply_kernel, 4, sizeof(cl_int), &cl_height) &&
        set_arg(apply_kernel, 5, sizeof(cl_int), &cl_pitch) &&
        set_arg(apply_kernel, 6, sizeof(cl_int), &use_blur_flag) &&
        set_arg(apply_kernel, 7, sizeof(cl_int), &cl_amount) &&
        set_arg(apply_kernel, 8, sizeof(cl_int), &cl_bl) &&
        set_arg(apply_kernel, 9, sizeof(cl_int), &cl_tl) &&
        set_arg(apply_kernel, 10, sizeof(cl_int), &cl_br) &&
        set_arg(apply_kernel, 11, sizeof(cl_int), &cl_tr) &&
        set_arg(apply_kernel, 12, sizeof(cl_int), &cl_is_double) &&
        set_arg(apply_kernel, 13, sizeof(cl_int), &cl_start_at_one) &&
        run_2d(apply_kernel, width, height);
    if (!apply_ok) {
        release_scratch();
        error = "output.sharpen apply failed";
        return RT_STAGE_ERROR;
    }

    const RtStageStatus readback = residency->readback(image_device, image);
    release_scratch();
    if (readback != RT_STAGE_OK) {
        error = "output.sharpen readback failed";
        return RT_STAGE_ERROR;
    }

    return RT_STAGE_OK;
}

/*
 * The local.adjustments node, one spot at a time.
 *
 *   [0] Lab three-plane F32 buffer, read and written in place,
 *   [1] PTR host context (RtLocalAdjustmentsContext), host-only,
 *   [2] int support flag, mirrored from the context descriptor.
 *
 * The context build runs once per spot, before that spot's upload, on the Lab
 * image the call site produced with rgb2lab(baseImg) and the read-back result
 * of the previous spot. It exports the per-spot record, the host-built light,
 * chroma-slider and vibrance-skin tables, the working matrices, the Lab caches
 * and the Gaussian coefficients for origblur and local contrast. calc_ref()
 * and complexCurvelocal() stay host-side; the local-contrast coefficients are
 * built from the spot's lcradius.
 *
 * Pass order matches Lab_Local(): the optional negative-value prefilter, then,
 * per active spot, local CBDL, vibrance, shadows/highlights, soft light, local
 * contrast and Colour & Light, each with its own ROI copy and deltaE reference
 * blur. The full-image tail clamp and the Munsell pass follow. Every pass writes
 * the device image;
 * the host Lab is read back only after all passes succeeded, so a refusal or
 * a failed launch leaves it untouched for the builtin.
 */
RtStageStatus run_local_adjustments(const std::shared_ptr<OpenCLEnv>& env,
                                    const std::shared_ptr<OpenCLResidency>& residency,
                                    const std::vector<RtStageArg>& args,
                                    std::string& error)
{
    constexpr std::size_t ARG_COUNT = 3;

    if (args.size() != ARG_COUNT) {
        error = "local.adjustments expects a Lab buffer, a context and a "
                "support flag";
        return RT_STAGE_INVALID;
    }
    if (args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_PTR ||
        args[2].kind != RT_STAGE_ARG_INT) {
        error = "local.adjustments has an argument of the wrong kind";
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        error = "local.adjustments needs a three-plane F32 Lab buffer";
        return RT_STAGE_INVALID;
    }
    const auto* context =
        static_cast<const RtLocalAdjustmentsContext*>(args[1].u.ptr);
    if (context == nullptr ||
        context->size != sizeof(RtLocalAdjustmentsContext) ||
        context->version != RT_LOCAL_ADJUSTMENTS_CONTEXT_VERSION ||
        context->magic != RT_LOCAL_ADJUSTMENTS_CONTEXT_MAGIC ||
        context->build == nullptr || context->run_host == nullptr ||
        context->spot_count == 0) {
        error = "local.adjustments has an invalid host context";
        return RT_STAGE_INVALID;
    }
    if (args[2].u.i != 0 && args[2].u.i != 1) {
        error = "local.adjustments support flag must be 0 or 1";
        return RT_STAGE_INVALID;
    }
    if (args[2].u.i != 1 || context->supported != 1) {
        error = "local.adjustments support flag is not set";
        return RT_STAGE_FALLBACK;
    }
    if (lab.row_pitch % sizeof(float) != 0 ||
        lab.plane_pitch <
            lab.row_pitch * static_cast<std::size_t>(lab.height)) {
        error = "local.adjustments Lab buffer pitch does not cover the image";
        return RT_STAGE_INVALID;
    }

    /*
     * The device path is CPU-only: the colour core uses pow()/cos()/sin()
     * where the host uses its own SLEEF helpers, the deltaE scope amplifies
     * small differences, and the Munsell pass needs the same CPU arithmetic.
     */
    StageOpenCLDeviceInfo device;
    if (!opencl_stage_device_info(&device) || !device.cpu) {
        error = "local.adjustments requires a CPU OpenCL device";
        return RT_STAGE_FALLBACK;
    }
    {
        cl_device_fp_config fp_config = 0;
        if (env->api->GetDeviceInfo(env->device, CL_DEVICE_DOUBLE_FP_CONFIG,
                                    sizeof(fp_config), &fp_config,
                                    nullptr) != CL_SUCCESS ||
            fp_config == 0) {
            error = "local.adjustments needs device fp64";
            return RT_STAGE_FALLBACK;
        }
    }

    const int width = lab.width;
    const int height = lab.height;
    const KernelDef* def = find_kernel("local.adjustments");
    assert(def != nullptr);
    cl_kernel apply_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_apply", error);
    cl_kernel clamp_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_clamp", error);
    cl_kernel roi_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_roi", error);
    cl_kernel munsell_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_munsell", error);
    cl_kernel line_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_lc_line", error);
    cl_kernel vibrance_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_vibrance", error);
    cl_kernel lc_blend_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_lc_blend", error);
    cl_kernel blend_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_blend", error);
    cl_kernel softlight_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_softlight",
                              error);
    cl_kernel cbdl_fill_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_cbdl_fill",
                              error);
    cl_kernel cbdl_copy_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_cbdl_copy",
                              error);
    cl_kernel cbdl_finish_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_cbdl_finish",
                              error);
    cl_kernel cbdl_metric_kernel =
        compiled_kernel_entry(env, def, "rt_local_adjustments_cbdl_metric",
                              error);
    cl_kernel cbdl_luma_kernel = compiled_kernel_entry(
        env, def, "rt_local_adjustments_cbdl_blend_luma", error);
    cl_kernel cbdl_chroma_kernel = compiled_kernel_entry(
        env, def, "rt_local_adjustments_cbdl_blend_chroma", error);
    cl_kernel cbdl_decompose_kernel =
        compiled_kernel_entry(env, def, "rt_lab_wavelet_decompose", error);
    cl_kernel cbdl_equalize_kernel =
        compiled_kernel_entry(env, def, "rt_lab_wavelet_equalize", error);
    /* The shadows/highlights kernels are the lab.tools fragments that the
     * local-adjustments program already concatenates. */
    cl_kernel sh_mask_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_mask", error);
    cl_kernel sh_subsample_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_subsample", error);
    cl_kernel sh_box_h_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_box_h", error);
    cl_kernel sh_box_v_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_box_v", error);
    cl_kernel sh_combine_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_combine", error);
    cl_kernel sh_upsample_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_upsample", error);
    cl_kernel sh_apply_kernel =
        compiled_kernel_entry(env, def, "rt_lab_tools_apply", error);
    if (apply_kernel == nullptr || clamp_kernel == nullptr ||
        roi_kernel == nullptr || munsell_kernel == nullptr ||
        line_kernel == nullptr || vibrance_kernel == nullptr ||
        lc_blend_kernel == nullptr || blend_kernel == nullptr ||
        softlight_kernel == nullptr ||
        cbdl_fill_kernel == nullptr || cbdl_copy_kernel == nullptr ||
        cbdl_finish_kernel == nullptr || cbdl_metric_kernel == nullptr ||
        cbdl_luma_kernel == nullptr || cbdl_chroma_kernel == nullptr ||
        cbdl_decompose_kernel == nullptr || cbdl_equalize_kernel == nullptr ||
        sh_mask_kernel == nullptr || sh_subsample_kernel == nullptr ||
        sh_box_h_kernel == nullptr || sh_box_v_kernel == nullptr ||
        sh_combine_kernel == nullptr || sh_upsample_kernel == nullptr ||
        sh_apply_kernel == nullptr) {
        return RT_STAGE_FALLBACK;
    }

    const auto memory_of = [](const StageDeviceBufferPtr& buffer) {
        auto device_buffer =
            std::dynamic_pointer_cast<OpenCLDeviceBuffer>(buffer);
        return device_buffer != nullptr ? device_buffer->memory() : nullptr;
    };
    const auto pitch_of = [](const StageDeviceBufferPtr& buffer) -> std::size_t {
        return buffer != nullptr ? buffer->row_pitch() / sizeof(float) : 0;
    };
    const auto plane_pitch_of =
        [](const StageDeviceBufferPtr& buffer) -> std::size_t {
        return buffer != nullptr ? buffer->plane_pitch() / sizeof(float) : 0;
    };

    const StageBufferShape image_shape{RT_STAGE_F32, width, height, 3};
    StageDeviceBufferPtr image =
        residency->acquire(lab.data, image_shape, &error);
    char reserved_key = 0;
    StageDeviceBufferPtr reserved =
        residency->acquire(&reserved_key, image_shape, &error);
    if (!image || !reserved || residency->upload(image, lab) != RT_STAGE_OK ||
        residency->upload(reserved, lab) != RT_STAGE_OK) {
        error = "local.adjustments Lab upload failed";
        return RT_STAGE_FALLBACK;
    }

    /*
     * A contiguous staging copy is the host Lab handed to build() between
     * spots. The caller's Lab buffer is not written until every spot has
     * succeeded.
     */
    std::vector<float> staging(static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 3u);
    const auto copy_to_staging = [&]() {
        const auto* base = static_cast<const unsigned char*>(lab.data);
        for (int y = 0; y < height; ++y) {
            for (int plane = 0; plane < 3; ++plane) {
                const auto* row = reinterpret_cast<const float*>(
                    base + static_cast<std::size_t>(plane) * lab.plane_pitch +
                    static_cast<std::size_t>(y) * lab.row_pitch);
                std::memcpy(staging.data() +
                                (static_cast<std::size_t>(plane) * height + y) *
                                    width,
                            row,
                            static_cast<std::size_t>(width) * sizeof(float));
            }
        }
    };
    const auto copy_from_staging = [&]() {
        auto* base = static_cast<unsigned char*>(lab.data);
        for (int y = 0; y < height; ++y) {
            for (int plane = 0; plane < 3; ++plane) {
                auto* row = reinterpret_cast<float*>(
                    base + static_cast<std::size_t>(plane) * lab.plane_pitch +
                    static_cast<std::size_t>(y) * lab.row_pitch);
                std::memcpy(row,
                            staging.data() +
                                (static_cast<std::size_t>(plane) * height + y) *
                                    width,
                            static_cast<std::size_t>(width) * sizeof(float));
            }
        }
    };
    copy_to_staging();

    RtStageBuffer staging_buffer{};
    staging_buffer.size = sizeof(RtStageBuffer);
    staging_buffer.data = staging.data();
    staging_buffer.width = width;
    staging_buffer.height = height;
    staging_buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
    staging_buffer.plane_pitch =
        staging_buffer.row_pitch * static_cast<std::size_t>(height);
    staging_buffer.planes = 3;
    staging_buffer.format = RT_STAGE_F32;
    staging_buffer.flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;

    const int lab_pitch = static_cast<int>(pitch_of(image));
    const int lab_plane = static_cast<int>(plane_pitch_of(image));
    if (pitch_of(image) >
            static_cast<std::size_t>(std::numeric_limits<cl_int>::max()) ||
        plane_pitch_of(image) >
            static_cast<std::size_t>(std::numeric_limits<cl_int>::max()) / 3) {
        error = "local.adjustments pitch does not fit the kernel's int";
        return RT_STAGE_FALLBACK;
    }

    /* A table that no tool reads gets a one-float placeholder so every
     * kernel argument is a valid buffer. */
    std::vector<float> placeholder(1, 0.f);
    char placeholder_key = 0;
    StageDeviceBufferPtr placeholder_device = residency->acquire(
        &placeholder_key, StageBufferShape{RT_STAGE_F32, 1, 1, 1}, &error);
    if (!placeholder_device ||
        residency->upload(placeholder_device,
                          RtStageBuffer{sizeof(RtStageBuffer),
                                        placeholder.data(), 1, 1,
                                        sizeof(float), sizeof(float), 1,
                                        RT_STAGE_F32,
                                        RT_STAGE_BUFFER_READ}) != RT_STAGE_OK) {
        error = "local.adjustments placeholder upload failed";
        return RT_STAGE_FALLBACK;
    }

    const auto upload_table = [&](const float* data, int count, char* key,
                                  const char* label)
        -> StageDeviceBufferPtr {
        StageDeviceBufferPtr device_buffer = residency->acquire(
            key, StageBufferShape{RT_STAGE_F32, count, 1, 1}, &error);
        if (!device_buffer) {
            error = std::string("local.adjustments ") + label +
                    " allocation failed";
            return nullptr;
        }
        RtStageBuffer host{};
        host.size = sizeof(RtStageBuffer);
        host.data = const_cast<float*>(data);
        host.width = count;
        host.height = 1;
        host.row_pitch = static_cast<std::size_t>(count) * sizeof(float);
        host.plane_pitch = host.row_pitch;
        host.planes = 1;
        host.format = RT_STAGE_F32;
        host.flags = RT_STAGE_BUFFER_READ;
        if (residency->upload(device_buffer, host) != RT_STAGE_OK) {
            error = std::string("local.adjustments ") + label +
                    " upload failed";
            return nullptr;
        }
        return device_buffer;
    };

    char ws_key = 0;
    char iws_key = 0;
    char cachef_key = 0;
    char cachefy_key = 0;
    char gamma_srgb_key = 0;
    char igamma_srgb_key = 0;
    char munsell_key = 0;
    char munsell_sizes_key = 0;
    StageDeviceBufferPtr ws_device;
    StageDeviceBufferPtr iws_device;
    StageDeviceBufferPtr cachef_device;
    StageDeviceBufferPtr cachefy_device;
    StageDeviceBufferPtr gamma_srgb_device;
    StageDeviceBufferPtr igamma_srgb_device;
    StageDeviceBufferPtr munsell_device;
    StageDeviceBufferPtr munsell_sizes_device;

    char record_key = 0;
    char luts_key = 0;
    char light_key = 0;
    char satur_key = 0;
    char saturmoins_key = 0;
    char skin_key = 0;
    char sh_hl_key = 0;
    char sh_sh_key = 0;
    char cbdl_luma_key = 0;
    char cbdl_chroma_key = 0;
    char work_key = 0;
    char scratch_key = 0;
    char blur_keys[3] = {0, 0, 0};
    char lc_h_key = 0;
    char lc_v_key = 0;
    char sh_full_key = 0;
    char sh_i1_key = 0;
    char sh_p1_key = 0;
    char sh_meani_key = 0;
    char sh_meanp_key = 0;
    char sh_temp_key = 0;
    char cbdl_src_key = 0;
    char cbdl_out_key = 0;
    char cbdl_resid_key = 0;
    char cbdl_rowmax_key = 0;
    char cbdl_pyr_keys[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS] = {};
    StageDeviceBufferPtr blur_device[3];

    const auto finite = [](float value) { return std::isfinite(value); };

    std::lock_guard<std::recursive_mutex> lock(env->queue_mutex);

    const auto set = [&](cl_kernel kernel, cl_uint index, std::size_t bytes,
                         const void* value) {
        return env->api->SetKernelArg(kernel, index, bytes, value) ==
               CL_SUCCESS;
    };
    const auto launch2d = [&](cl_kernel kernel, std::size_t gx,
                              std::size_t gy, const char* label) {
        const std::size_t global[2] = {gx, gy};
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 2, nullptr,
                                           global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            error = std::string("local.adjustments ") + label +
                    " launch failed";
            return false;
        }
        const cl_int wait = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        if (wait != CL_SUCCESS) {
            error = std::string("local.adjustments ") + label +
                    " did not complete";
            return false;
        }
        return true;
    };
    const auto launch1d = [&](cl_kernel kernel, std::size_t gx,
                              const char* label) {
        const std::size_t global[1] = {gx};
        cl_event done = nullptr;
        if (env->api->EnqueueNDRangeKernel(env->queue, kernel, 1, nullptr,
                                           global, nullptr, 0, nullptr,
                                           &done) != CL_SUCCESS ||
            done == nullptr) {
            error = std::string("local.adjustments ") + label +
                    " launch failed";
            return false;
        }
        const cl_int wait = env->api->WaitForEvents(1, &done);
        env->api->ReleaseEvent(done);
        if (wait != CL_SUCCESS) {
            error = std::string("local.adjustments ") + label +
                    " did not complete";
            return false;
        }
        return true;
    };

    const auto run_clamp = [&](float epsi, int mode, const char* label) {
        cl_uint arg = 0;
        cl_mem lab_memory = memory_of(image);
        cl_mem ws_memory = memory_of(ws_device);
        cl_mem iws_memory = memory_of(iws_device);
        cl_mem cachef_memory = memory_of(cachef_device);
        cl_mem cachefy_memory = memory_of(cachefy_device);
        cl_int mode_value = mode;
        cl_float epsi_value = epsi;
        bool ok = true;
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_mem), &lab_memory);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_mem), &ws_memory);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_mem), &iws_memory);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_mem), &cachef_memory);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_mem), &cachefy_memory);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_int), &width);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_int), &height);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_int), &lab_pitch);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_int), &lab_plane);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_int), &mode_value);
        ok = ok && set(clamp_kernel, arg++, sizeof(cl_float), &epsi_value);
        if (!ok) {
            error = std::string("local.adjustments ") + label +
                    " arguments failed";
            return false;
        }
        return launch2d(clamp_kernel, static_cast<std::size_t>(width),
                        static_cast<std::size_t>(height), label);
    };

    const auto run_line =
        [&](cl_kernel kernel, cl_mem src, cl_mem dst, cl_mem state,
            cl_int length, cl_int lines, cl_int src_base_step,
            cl_int dst_base_step, cl_int src_stride, cl_int dst_stride,
            cl_int src_base, cl_int dst_base, cl_int mode,
            cl_int vector_lines, const RtLabToolsIirDirection& direction,
            const char* label) {
        cl_uint arg = 0;
        bool ok = true;
        ok = ok && set(kernel, arg++, sizeof(cl_mem), &src);
        ok = ok && set(kernel, arg++, sizeof(cl_mem), &dst);
        ok = ok && set(kernel, arg++, sizeof(cl_mem), &state);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &length);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &lines);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &src_base_step);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &dst_base_step);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &src_stride);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &dst_stride);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &src_base);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &dst_base);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &mode);
        ok = ok && set(kernel, arg++, sizeof(cl_int), &vector_lines);
        for (std::size_t index = 0;
             index < RT_LAB_TOOLS_IIR_TERMS && ok; ++index) {
            ok = ok && set(kernel, arg++, sizeof(cl_float),
                           &direction.lane[index]);
        }
        for (std::size_t index = 0;
             index < RT_LAB_TOOLS_IIR_TERMS && ok; ++index) {
            ok = ok && set(kernel, arg++, sizeof(cl_double),
                           &direction.border[index]);
        }
        for (std::size_t index = 0;
             index < RT_LAB_TOOLS_IIR_TERMS && ok; ++index) {
            ok = ok && set(kernel, arg++, sizeof(cl_double),
                           &direction.wide[index]);
        }
        if (!ok) {
            error = std::string("local.adjustments ") + label +
                    " arguments failed";
            return false;
        }
        return launch1d(kernel, static_cast<std::size_t>(lines), label);
    };

    for (uint32_t spot = 0; spot < context->spot_count; ++spot) {
        RtLocalAdjustmentsResult setup{};
        if (context->build(context->self, static_cast<int32_t>(spot),
                           &staging_buffer, &setup) != 0) {
            error = "local.adjustments host setup failed";
            return RT_STAGE_FALLBACK;
        }
        if (setup.spots == nullptr || setup.spot_count != 1 ||
            setup.spot_stride != RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE ||
            setup.ws == nullptr || setup.iws == nullptr ||
            setup.cachef == nullptr || setup.cachefy == nullptr ||
            setup.gamma_srgb == nullptr || setup.igamma_srgb == nullptr ||
            setup.iir == nullptr) {
            error = "local.adjustments setup is incomplete";
            return RT_STAGE_FALLBACK;
        }
        const float* rec = setup.spots;
        for (std::size_t slot = 0; slot < setup.spot_stride; ++slot) {
            if (!finite(rec[slot])) {
                error = "local.adjustments record has a non-finite value";
                return RT_STAGE_FALLBACK;
            }
        }

        const bool active = rec[RT_LOCAL_ADJUSTMENTS_SLOT_ACTIV] == 1.0f;
        const bool colour = rec[RT_LOCAL_ADJUSTMENTS_SLOT_COLORENA] == 1.0f;
        const bool islocal = rec[RT_LOCAL_ADJUSTMENTS_SLOT_ISLOCAL] == 1.0f;
        const bool vibrance =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPVIBRANCE] == 1.0f;
        const bool localContrast =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCONTRAST] == 1.0f;
        const bool shadowsHighlights =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHADHIGHLIGHTS] == 1.0f;
        const bool softLight =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSOFT] == 1.0f;
        const bool cbdl =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCBDL] == 1.0f;
        const uint32_t shape =
            static_cast<uint32_t>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_SHAPE]);
        const uint32_t avoid = static_cast<uint32_t>(
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDGAMUT]);
        const uint32_t qcurve =
            static_cast<uint32_t>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_QCURVE]);
        if (shape > RT_LOCAL_ADJUSTMENTS_SHAPE_RECT ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_ACTIV] != 0.0f && !active) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_ISLOCAL] != 0.0f && !islocal) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_COLORENA] != 0.0f && !colour) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPVIBRANCE] != 0.0f &&
             !vibrance) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCONTRAST] != 0.0f &&
             !localContrast) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHADHIGHLIGHTS] != 0.0f &&
             !shadowsHighlights) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSOFT] != 0.0f &&
             !softLight) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCBDL] != 0.0f && !cbdl) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_MERGE] != 0.0f) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_HRENABLED] != 0.0f) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_REPARCOL] != 100.0f) ||
            (qcurve != 0u && qcurve != 1u) ||
            (avoid != RT_LOCAL_ADJUSTMENTS_AVOID_NONE &&
             avoid != RT_LOCAL_ADJUSTMENTS_AVOID_MUNS) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDNEG] != 0.0f &&
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDNEG] != 1.0f) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_DELTAE] != 0.0f &&
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_DELTAE] != 1.0f) ||
            (active && !islocal) ||
            (active && colour &&
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCOLOR] != 1.0f) ||
            (!active && (colour || vibrance || localContrast ||
                         shadowsHighlights || softLight || cbdl || islocal))) {
            error = "local.adjustments record is outside the supported set";
            return RT_STAGE_FALLBACK;
        }
        const uint32_t clamp_mode =
            static_cast<uint32_t>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_CLAMPMODE]);
        if (clamp_mode != RT_LOCAL_ADJUSTMENTS_CLAMP_NOTZERO &&
            clamp_mode != RT_LOCAL_ADJUSTMENTS_CLAMP_LAPLACIAN) {
            error = "local.adjustments has no clamp arm";
            return RT_STAGE_FALLBACK;
        }
        if (rec[RT_LOCAL_ADJUSTMENTS_SLOT_CHRO] < -100.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CHRO] > 100.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_LIGH] < -100.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_LIGH] > 100.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CONT] < -100.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CONT] > 100.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_BLURCOL] < 0.0f ||
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_BLURCOL] > 100.0f) {
            error = "local.adjustments slider is outside its range";
            return RT_STAGE_FALLBACK;
        }
        if (vibrance &&
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PASTELS] < -100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PASTELS] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SATURATED] < -100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SATURATED] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_TOP] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_TOP] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_BOTTOM] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_BOTTOM] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSV] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSV] > 100.0f)) {
            error = "local.adjustments vibrance setting is outside its range";
            return RT_STAGE_FALLBACK;
        }
        if (localContrast &&
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_AMOUNT] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_AMOUNT] > 1.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_DARKNESS] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_DARKNESS] > 3.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_LIGHTNESS] < -100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_LIGHTNESS] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_RADIUS] < 10.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_RADIUS] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSLC] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSLC] > 100.0f)) {
            error = "local.adjustments local-contrast setting is outside its "
                    "range";
            return RT_STAGE_FALLBACK;
        }
        if (shadowsHighlights &&
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_TONAL] < 10.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_TONAL] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_TONAL] < 10.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_TONAL] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_RADIUS] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_RADIUS] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_BLUR] < 2.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_BLUR] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSH] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSH] > 100.0f ||
             (rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS] == 0.0f &&
              rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS] == 0.0f))) {
            error = "local.adjustments shadows/highlights setting is outside "
                    "its range";
            return RT_STAGE_FALLBACK;
        }
        if (softLight &&
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_SOFT_STRENGTH] <= 1.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SOFT_STRENGTH] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSOFT] < 1.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSOFT] > 100.0f)) {
            error = "local.adjustments soft-light setting is outside its "
                    "range";
            return RT_STAGE_FALLBACK;
        }
        const float cbdlLumaLevelsValue =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_LEVELS];
        const float cbdlChromaLevelsValue =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_LEVELS];
        if (cbdlLumaLevelsValue < 0.0f ||
            cbdlLumaLevelsValue >
                static_cast<float>(RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS) ||
            cbdlChromaLevelsValue < 0.0f ||
            cbdlChromaLevelsValue >
                static_cast<float>(RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS)) {
            error = "local.adjustments CBDL level count is out of range";
            return RT_STAGE_FALLBACK;
        }
        const uint32_t cbdlLumaLevels =
            static_cast<uint32_t>(cbdlLumaLevelsValue);
        const uint32_t cbdlChromaLevels =
            static_cast<uint32_t>(cbdlChromaLevelsValue);
        if (cbdl &&
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSCB] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSCB] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_THRESHOLD] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_THRESHOLD] > 1.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CLARITY] < 0.1f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CLARITY] > 100.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA] < 0.0f ||
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA] > 1.5f)) {
            error = "local.adjustments CBDL setting is outside its range";
            return RT_STAGE_FALLBACK;
        }
        for (int level = 0; level < static_cast<int>(
                 RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS);
             ++level) {
            if (rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT0 + level] < 0.0f ||
                rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT0 + level] > 4.0f) {
                error = "local.adjustments CBDL multiplier is outside its "
                        "range";
                return RT_STAGE_FALLBACK;
            }
        }
        for (const int flag : {RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PROTECTSKINS,
                               RT_LOCAL_ADJUSTMENTS_SLOT_VIB_AVOIDCOLORSHIFT,
                               RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE}) {
            if (rec[flag] != 0.0f && rec[flag] != 1.0f) {
                error = "local.adjustments vibrance flag is not 0 or 1";
                return RT_STAGE_FALLBACK;
            }
        }
        const bool skinActive =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE] == 1.0f;
        if (skinActive &&
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_OFFSET] < 0.0f ||
             static_cast<std::size_t>(
                 rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_OFFSET]) +
                     RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES >
                 setup.lut_count)) {
            error = "local.adjustments skin curve is out of range";
            return RT_STAGE_FALLBACK;
        }
        if (setup.iir->size != sizeof(RtLabToolsIir) ||
            setup.iir->version != RT_LAB_TOOLS_IIR_VERSION ||
            setup.iir->double_path != 0 ||
            setup.iir_vibrance == nullptr ||
            setup.iir_vibrance->size != sizeof(RtLabToolsIir) ||
            setup.iir_vibrance->version != RT_LAB_TOOLS_IIR_VERSION ||
            setup.iir_vibrance->double_path != 0 ||
            setup.iir_local_contrast == nullptr ||
            setup.iir_local_contrast->size != sizeof(RtLabToolsIir) ||
            setup.iir_local_contrast->version != RT_LAB_TOOLS_IIR_VERSION ||
            setup.iir_local_contrast->double_path > 1u ||
            setup.iir_shadows_highlights == nullptr ||
            setup.iir_shadows_highlights->size != sizeof(RtLabToolsIir) ||
            setup.iir_shadows_highlights->version !=
                RT_LAB_TOOLS_IIR_VERSION ||
            setup.iir_shadows_highlights->double_path != 0) {
            error = "local.adjustments blur needs the SSE coefficient path";
            return RT_STAGE_FALLBACK;
        }
        if (localContrast &&
            setup.iir_local_contrast->double_path !=
                (rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_RADIUS] >=
                         LAB_TOOLS_LC_DOUBLE_SIGMA
                     ? 1u
                     : 0u)) {
            error = "local.adjustments local-contrast coefficients use the "
                    "wrong path";
            return RT_STAGE_FALLBACK;
        }
#if !defined(__SSE2__) && !defined(RT_SIMDE)
        /*
         * Below sigma 25 the host takes the SSE float recurrence on an SSE2
         * build and the double one otherwise; the exported `double_path` is
         * the sigma test alone. A host without SSE2 would therefore run a
         * different recurrence from the one the kernel's mode selects.
         */
        if (vibrance || localContrast) {
            error = "local.adjustments vibrance and local contrast need the "
                    "host's SSE2 blur path";
            return RT_STAGE_FALLBACK;
        }
#endif

        /* Curve table ranges. A table that is not active must not be read. */
        struct TableSpec {
            int offset;
            int active;
            int entries;
        };
        const TableSpec curves[] = {
            {RT_LOCAL_ADJUSTMENTS_SLOT_LL_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LL_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_CL_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CL_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_LC_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LC_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_CC_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CC_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_LH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LH_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_HH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_HH_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_CH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CH_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LIGHT_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES)},
            {RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_ACTIVE,
             static_cast<int>(RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES)}};
        for (const TableSpec& table : curves) {
            const bool table_active = rec[table.active] == 1.0f;
            if (rec[table.active] != 0.0f && !table_active) {
                error = "local.adjustments curve flag is not 0 or 1";
                return RT_STAGE_FALLBACK;
            }
            if (qcurve == 0u &&
                table.active >=
                    static_cast<int>(RT_LOCAL_ADJUSTMENTS_SLOT_LL_ACTIVE) &&
                table.active <=
                    static_cast<int>(RT_LOCAL_ADJUSTMENTS_SLOT_CH_ACTIVE) &&
                table_active) {
                error = "local.adjustments has a curve outside the curve "
                        "pass";
                return RT_STAGE_FALLBACK;
            }
            if (table_active) {
                const float offset = rec[table.offset];
                if (offset < 0.0f ||
                    static_cast<std::size_t>(offset) +
                            static_cast<std::size_t>(table.entries) >
                        setup.lut_count) {
                    error = "local.adjustments curve table is out of range";
                    return RT_STAGE_FALLBACK;
                }
            }
        }
        const bool cbdlLumaActive =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_ACTIVE] == 1.0f;
        const bool cbdlChromaActive =
            rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_ACTIVE] == 1.0f;
        if ((rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_ACTIVE] != 0.0f &&
             !cbdlLumaActive) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_ACTIVE] != 0.0f &&
             !cbdlChromaActive) ||
            (!cbdl && (cbdlLumaActive || cbdlChromaActive))) {
            error = "local.adjustments CBDL table flag is not 0 or 1";
            return RT_STAGE_FALLBACK;
        }
        const auto cbdlTableRange = [&](int offsetSlot, bool tableActive,
                                        uint32_t levels,
                                        const char* label) -> bool {
            if (!tableActive) {
                return true;
            }
            if (levels == 0 ||
                levels > RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS) {
                error = std::string("local.adjustments ") + label +
                        " has no levels";
                return false;
            }
            const float offset = rec[offsetSlot];
            const std::size_t entries =
                static_cast<std::size_t>(levels) *
                RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES;
            if (offset < 0.0f ||
                static_cast<std::size_t>(offset) + entries >
                    setup.lut_count) {
                error = std::string("local.adjustments ") + label +
                        " table is out of range";
                return false;
            }
            return true;
        };
        if (!cbdlTableRange(RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_OFFSET,
                            cbdlLumaActive, cbdlLumaLevels, "CBDL luma") ||
            !cbdlTableRange(RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_OFFSET,
                            cbdlChromaActive, cbdlChromaLevels,
                            "CBDL chroma")) {
            return RT_STAGE_FALLBACK;
        }
        if ((rec[RT_LOCAL_ADJUSTMENTS_SLOT_CHRO] > 0.0f &&
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_ACTIVE] != 1.0f) ||
            (rec[RT_LOCAL_ADJUSTMENTS_SLOT_CHRO] < 0.0f &&
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_ACTIVE] != 1.0f) ||
            ((rec[RT_LOCAL_ADJUSTMENTS_SLOT_LIGH] != 0.0f ||
              rec[RT_LOCAL_ADJUSTMENTS_SLOT_CONT] != 0.0f) &&
             rec[RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_ACTIVE] != 1.0f) ||
            (shadowsHighlights &&
             ((rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS] > 0.0f &&
               rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_ACTIVE] != 1.0f) ||
              (rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS] > 0.0f &&
               rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_ACTIVE] != 1.0f)))) {
            error = "local.adjustments slider table is missing";
            return RT_STAGE_FALLBACK;
        }

        int xstart = 0;
        int ystart = 0;
        int bfw = 0;
        int bfh = 0;
        if (active) {
            xstart = std::max(
                static_cast<int>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_XC] -
                                 rec[RT_LOCAL_ADJUSTMENTS_SLOT_LXL]),
                0);
            ystart = std::max(
                static_cast<int>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_YC] -
                                 rec[RT_LOCAL_ADJUSTMENTS_SLOT_LYT]),
                0);
            const int xend = std::min(
                static_cast<int>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_XC] +
                                 rec[RT_LOCAL_ADJUSTMENTS_SLOT_LX]),
                width);
            const int yend = std::min(
                static_cast<int>(rec[RT_LOCAL_ADJUSTMENTS_SLOT_YC] +
                                 rec[RT_LOCAL_ADJUSTMENTS_SLOT_LY]),
                height);
            bfw = xend - xstart;
            bfh = yend - ystart;
            /* iplocallab.cc's mSP is 5: no colour work below it. */
            if (bfw < 5 || bfh < 5 || bfw > width || bfh > height) {
                error = "local.adjustments spot ROI is below the colour "
                        "floor";
                return RT_STAGE_FALLBACK;
            }
            /* shadowsHighlights() passes an explicit guided-filter
             * subsampling of four; the lab.tools port needs at least three
             * subsampled samples per axis. A smaller ROI is a host
             * execution. */
            if (shadowsHighlights &&
                (bfw < RT_LOCAL_ADJUSTMENTS_SH_MIN_ROI ||
                 bfh < RT_LOCAL_ADJUSTMENTS_SH_MIN_ROI)) {
                error = "local.adjustments shadows/highlights ROI is too "
                        "small for the guided filter";
                return RT_STAGE_FALLBACK;
            }
        }

        /* The matrices, caches and Munsell tables are block-global. */
        if (spot == 0) {
            if (setup.munsell == nullptr || setup.munsell_sizes == nullptr ||
                setup.munsell_tables != RT_LAB_TOOLS_MUNSELL_TABLES) {
                error = "local.adjustments needs the Munsell tables";
                return RT_STAGE_FALLBACK;
            }
            ws_device = upload_table(setup.ws, 9, &ws_key, "working matrix");
            iws_device = upload_table(setup.iws, 9, &iws_key, "inverse matrix");
            cachef_device = upload_table(setup.cachef,
                                         RT_LAB_TOOLS_CACHE_ENTRIES,
                                         &cachef_key, "cachef");
            cachefy_device = upload_table(setup.cachefy,
                                          RT_LAB_TOOLS_CACHE_ENTRIES,
                                          &cachefy_key, "cachefy");
            gamma_srgb_device = upload_table(
                setup.gamma_srgb,
                RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES, &gamma_srgb_key,
                "sRGB gamma");
            igamma_srgb_device = upload_table(
                setup.igamma_srgb,
                RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES, &igamma_srgb_key,
                "inverse sRGB gamma");
            munsell_device = upload_table(
                setup.munsell,
                RT_LAB_TOOLS_MUNSELL_TABLES * RT_LAB_TOOLS_MUNSELL_STRIDE,
                &munsell_key, "Munsell tables");
            munsell_sizes_device = upload_table(
                setup.munsell_sizes, RT_LAB_TOOLS_MUNSELL_TABLES,
                &munsell_sizes_key, "Munsell sizes");
            if (!ws_device || !iws_device || !cachef_device ||
                !cachefy_device || !gamma_srgb_device ||
                !igamma_srgb_device || !munsell_device ||
                !munsell_sizes_device) {
                return RT_STAGE_FALLBACK;
            }
        }

        StageDeviceBufferPtr record_device =
            upload_table(rec, RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE, &record_key,
                         "record");
        const int lut_count =
            setup.lut_count == 0 ? 1 : static_cast<int>(setup.lut_count);
        StageDeviceBufferPtr luts_device = upload_table(
            setup.lut_count == 0 ? placeholder.data() : setup.luts,
            lut_count, &luts_key, "colour tables");
        if (!record_device || !luts_device) {
            return RT_STAGE_FALLBACK;
        }

        const auto table_slice = [&](int offset_slot, int active_slot,
                                     int entries, const char* label,
                                     char* key)
            -> StageDeviceBufferPtr {
            const bool table_active = rec[active_slot] == 1.0f;
            if (!table_active) {
                return placeholder_device;
            }
            const float offset = rec[offset_slot];
            if (offset < 0.0f ||
                static_cast<std::size_t>(offset) +
                        static_cast<std::size_t>(entries) >
                    setup.lut_count) {
                error = std::string("local.adjustments ") + label +
                        " table is out of range";
                return nullptr;
            }
            return upload_table(setup.luts + static_cast<std::size_t>(offset),
                                entries, key, label);
        };
        StageDeviceBufferPtr light_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_ACTIVE,
            RT_LOCAL_ADJUSTMENTS_LIGHT_LUT_ENTRIES, "light", &light_key);
        StageDeviceBufferPtr satur_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_ACTIVE,
            RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, "satur", &satur_key);
        StageDeviceBufferPtr saturmoins_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_ACTIVE,
            RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, "saturmoins",
            &saturmoins_key);
        StageDeviceBufferPtr skin_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE,
            RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, "skin", &skin_key);
        StageDeviceBufferPtr sh_hl_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_ACTIVE,
            RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES, "shadows/highlights "
            "highlight", &sh_hl_key);
        StageDeviceBufferPtr sh_sh_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_ACTIVE,
            RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES, "shadows/highlights "
            "shadow", &sh_sh_key);
        StageDeviceBufferPtr cbdl_luma_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_ACTIVE,
            static_cast<int>(cbdlLumaLevels) *
                static_cast<int>(RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES),
            "CBDL luminance", &cbdl_luma_key);
        StageDeviceBufferPtr cbdl_chroma_device = table_slice(
            RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_OFFSET,
            RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_ACTIVE,
            static_cast<int>(cbdlChromaLevels) *
                static_cast<int>(RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES),
            "CBDL chroma", &cbdl_chroma_key);
        if (!light_device || !satur_device || !saturmoins_device ||
            !skin_device || !sh_hl_device || !sh_sh_device ||
            !cbdl_luma_device || !cbdl_chroma_device) {
            return RT_STAGE_FALLBACK;
        }

        if (rec[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDNEG] != 0.0f &&
            !run_clamp(RT_LOCAL_ADJUSTMENTS_EPSI0,
                       RT_LOCAL_ADJUSTMENTS_CLAMP_NOTZERO, "prefilter")) {
            return RT_STAGE_FALLBACK;
        }

        if (active) {
            StageDeviceBufferPtr work_device = residency->acquire(
                &work_key, StageBufferShape{RT_STAGE_F32, bfw, bfh, 3},
                &error);
            for (int plane = 0; plane < 3; ++plane) {
                blur_device[plane] = residency->acquire(
                    &blur_keys[plane],
                    StageBufferShape{RT_STAGE_F32, bfw, bfh, 1}, &error);
            }
            StageDeviceBufferPtr scratch_device = residency->acquire(
                &scratch_key, StageBufferShape{RT_STAGE_F32, bfw, bfh, 1},
                &error);
            /*
             * shadows/highlights' mask/guide pair and guidedFilter()'s five
             * subsampled arrays. They exist only when the tool runs.
             */
            const int sh_sw = bfw / RT_LOCAL_ADJUSTMENTS_SH_SUBSAMPLING;
            const int sh_sh = bfh / RT_LOCAL_ADJUSTMENTS_SH_SUBSAMPLING;
            StageDeviceBufferPtr sh_full_device;
            StageDeviceBufferPtr sh_i1_device;
            StageDeviceBufferPtr sh_p1_device;
            StageDeviceBufferPtr sh_meani_device;
            StageDeviceBufferPtr sh_meanp_device;
            StageDeviceBufferPtr sh_temp_device;
            /*
             * Local CBDL: the tool source, the level result, the
             * reconstruction buffer, the six pyramid planes and the chroma
             * normaliser's per-row magnitudes.
             */
            StageDeviceBufferPtr cbdl_src_device;
            StageDeviceBufferPtr cbdl_out_device;
            StageDeviceBufferPtr cbdl_resid_device;
            StageDeviceBufferPtr cbdl_rowmax_device;
            StageDeviceBufferPtr cbdl_pyr_device[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS];
            std::vector<float> cbdl_rowmax(static_cast<std::size_t>(bfh), 0.f);
            if (cbdl) {
                const StageBufferShape plane_shape{RT_STAGE_F32, bfw, bfh, 1};
                cbdl_src_device =
                    residency->acquire(&cbdl_src_key, plane_shape, &error);
                cbdl_out_device =
                    residency->acquire(&cbdl_out_key, plane_shape, &error);
                cbdl_resid_device =
                    residency->acquire(&cbdl_resid_key, plane_shape, &error);
                for (uint32_t level = 0;
                     level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS; ++level) {
                    cbdl_pyr_device[level] = residency->acquire(
                        &cbdl_pyr_keys[level], plane_shape, &error);
                }
                cbdl_rowmax_device = residency->acquire(
                    &cbdl_rowmax_key, StageBufferShape{RT_STAGE_F32, bfh, 1, 1},
                    &error);
            }
            if (shadowsHighlights) {
                const StageBufferShape sh_sub_shape{RT_STAGE_F32, sh_sw,
                                                    sh_sh, 1};
                sh_full_device = residency->acquire(
                    &sh_full_key, StageBufferShape{RT_STAGE_F32, bfw, bfh, 2},
                    &error);
                sh_i1_device =
                    residency->acquire(&sh_i1_key, sh_sub_shape, &error);
                sh_p1_device =
                    residency->acquire(&sh_p1_key, sh_sub_shape, &error);
                sh_meani_device =
                    residency->acquire(&sh_meani_key, sh_sub_shape, &error);
                sh_meanp_device =
                    residency->acquire(&sh_meanp_key, sh_sub_shape, &error);
                sh_temp_device =
                    residency->acquire(&sh_temp_key, sh_sub_shape, &error);
            }
            bool cbdl_acquired = true;
            if (cbdl) {
                cbdl_acquired = cbdl_src_device && cbdl_out_device &&
                                cbdl_resid_device && cbdl_rowmax_device;
                for (uint32_t level = 0;
                     level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS; ++level) {
                    cbdl_acquired =
                        cbdl_acquired && cbdl_pyr_device[level];
                }
            }
            if (!work_device || !scratch_device || !blur_device[0] ||
                !blur_device[1] || !blur_device[2] || !cbdl_acquired ||
                (shadowsHighlights &&
                 (!sh_full_device || !sh_i1_device || !sh_p1_device ||
                  !sh_meani_device || !sh_meanp_device ||
                  !sh_temp_device))) {
                error = "local.adjustments ROI allocation failed";
                return RT_STAGE_FALLBACK;
            }

            const auto int_pitch =
                [&error](std::size_t pitch, const char* label) -> int {
                if (pitch >
                    static_cast<std::size_t>(
                        std::numeric_limits<cl_int>::max())) {
                    error = std::string("local.adjustments ") + label +
                            " pitch does not fit the kernel's int";
                    return -1;
                }
                return static_cast<int>(pitch);
            };
            const int work_pitch = int_pitch(pitch_of(work_device), "ROI");
            const int work_plane =
                int_pitch(plane_pitch_of(work_device), "ROI plane");
            const int blur_pitch = int_pitch(pitch_of(blur_device[0]), "blur");
            const int scratch_pitch =
                int_pitch(pitch_of(scratch_device), "scratch");
            int cbdl_src_pitch = 0;
            int cbdl_out_pitch = 0;
            int cbdl_resid_pitch = 0;
            int cbdl_pyr_pitch[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS] = {};
            if (cbdl) {
                cbdl_src_pitch =
                    int_pitch(pitch_of(cbdl_src_device), "CBDL source");
                cbdl_out_pitch =
                    int_pitch(pitch_of(cbdl_out_device), "CBDL result");
                cbdl_resid_pitch =
                    int_pitch(pitch_of(cbdl_resid_device), "CBDL residual");
                for (uint32_t level = 0;
                     level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS; ++level) {
                    cbdl_pyr_pitch[level] = int_pitch(
                        pitch_of(cbdl_pyr_device[level]), "CBDL pyramid");
                }
                if (cbdl_src_pitch < 0 || cbdl_out_pitch < 0 ||
                    cbdl_resid_pitch < 0) {
                    return RT_STAGE_FALLBACK;
                }
                for (uint32_t level = 0;
                     level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS; ++level) {
                    if (cbdl_pyr_pitch[level] < 0) {
                        return RT_STAGE_FALLBACK;
                    }
                }
            }
            int sh_full_pitch = 0;
            int sh_full_plane = 0;
            int sh_sub_pitch = 0;
            if (shadowsHighlights) {
                sh_full_pitch =
                    int_pitch(pitch_of(sh_full_device), "shadows/highlights");
                sh_full_plane = int_pitch(plane_pitch_of(sh_full_device),
                                          "shadows/highlights plane");
                sh_sub_pitch = int_pitch(pitch_of(sh_i1_device),
                                         "shadows/highlights sub");
                if (pitch_of(sh_p1_device) != pitch_of(sh_i1_device) ||
                    pitch_of(sh_meani_device) != pitch_of(sh_i1_device) ||
                    pitch_of(sh_meanp_device) != pitch_of(sh_i1_device) ||
                    pitch_of(sh_temp_device) != pitch_of(sh_i1_device)) {
                    error = "local.adjustments shadows/highlights subsampled "
                            "buffers have different pitches";
                    return RT_STAGE_FALLBACK;
                }
                if (sh_full_pitch < 0 || sh_full_plane < 0 ||
                    sh_sub_pitch < 0) {
                    return RT_STAGE_FALLBACK;
                }
                if (sh_sw < 3 || sh_sh < 3) {
                    error = "local.adjustments shadows/highlights grid is too "
                            "small";
                    return RT_STAGE_FALLBACK;
                }
            }
            if (work_pitch < 0 || work_plane < 0 || blur_pitch < 0 ||
                scratch_pitch < 0) {
                return RT_STAGE_FALLBACK;
            }
            cl_mem work_memory = memory_of(work_device);
            cl_mem scratch_memory = memory_of(scratch_device);
            cl_mem sh_full_memory =
                shadowsHighlights ? memory_of(sh_full_device) : nullptr;
            cl_mem sh_i1_memory =
                shadowsHighlights ? memory_of(sh_i1_device) : nullptr;
            cl_mem sh_p1_memory =
                shadowsHighlights ? memory_of(sh_p1_device) : nullptr;
            cl_mem sh_meani_memory =
                shadowsHighlights ? memory_of(sh_meani_device) : nullptr;
            cl_mem sh_meanp_memory =
                shadowsHighlights ? memory_of(sh_meanp_device) : nullptr;
            cl_mem sh_temp_memory =
                shadowsHighlights ? memory_of(sh_temp_device) : nullptr;

            /*
             * The host's tool blocks each start from a fresh copy of the
             * current image: bufcolorig, bufexporig, bufgb and tmp1. The
             * three-plane `work` buffer is that copy.
             */
            const auto copy_roi = [&]() {
                cl_uint arg = 0;
                cl_mem lab_memory = memory_of(image);
                cl_int x0 = xstart;
                cl_int y0 = ystart;
                cl_int rw = bfw;
                cl_int rh = bfh;
                cl_int lp = lab_pitch;
                cl_int lpl = lab_plane;
                cl_int wp = work_pitch;
                cl_int wpl = work_plane;
                bool ok = true;
                ok = ok && set(roi_kernel, arg++, sizeof(cl_mem), &lab_memory);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_mem),
                               &work_memory);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &x0);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &y0);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &rw);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &rh);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &lp);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &lpl);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &wp);
                ok = ok && set(roi_kernel, arg++, sizeof(cl_int), &wpl);
                if (!ok) {
                    error = "local.adjustments ROI copy arguments failed";
                    return false;
                }
                return launch2d(roi_kernel, static_cast<std::size_t>(bfw),
                                static_cast<std::size_t>(bfh), "ROI copy");
            };

            /*
             * transit_shapedetect2()'s origblur over the work copy. The
             * colour, vibrance and local-contrast deltaE references all take
             * the SSE path here, so the float scratch plane doubles as the
             * kernel's unused double-state argument.
             */
            const auto blur_roi = [&](const RtLabToolsIir& iir,
                                      const char* label) {
                const cl_int horizontal_lines = bfh - (bfh % 4);
                const cl_int vertical_lines = bfw - (bfw % 8);
                for (int plane = 0; plane < 3; ++plane) {
                    cl_mem dst = memory_of(blur_device[plane]);
                    const cl_int base =
                        static_cast<cl_int>(plane) * work_plane;
                    if (!run_line(line_kernel, work_memory, scratch_memory,
                                  scratch_memory, bfw, bfh, work_pitch,
                                  scratch_pitch, 1, 1, base, 0, 0,
                                  horizontal_lines, iir.horizontal, label) ||
                        !run_line(line_kernel, scratch_memory, dst,
                                  scratch_memory, bfh, bfw, 1, 1,
                                  scratch_pitch, blur_pitch, 0, 0, 0,
                                  vertical_lines, iir.vertical, label)) {
                        return false;
                    }
                }
                return true;
            };

            /* transit_shapedetect2()'s blend for a tool whose output is in
             * `work`: `varsens` is the tool's own scope sensitivity. */
            const auto run_blend = [&](const char* label,
                                       cl_float varsens) {
                cl_uint arg = 0;
                cl_mem lab_memory = memory_of(image);
                cl_mem fin_memory = work_memory;
                cl_mem blur_l = memory_of(blur_device[0]);
                cl_mem blur_a = memory_of(blur_device[1]);
                cl_mem blur_b = memory_of(blur_device[2]);
                cl_mem record_memory = memory_of(record_device);
                cl_int lp = lab_pitch;
                cl_int lpl = lab_plane;
                cl_int fp = work_pitch;
                cl_int fpl = work_plane;
                cl_int rw = bfw;
                cl_int rh = bfh;
                cl_int x0 = xstart;
                cl_int y0 = ystart;
                cl_int bp = blur_pitch;
                bool ok = true;
                ok = ok && set(blend_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_mem),
                               &fin_memory);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_mem), &blur_l);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_mem), &blur_a);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_mem), &blur_b);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_mem),
                               &record_memory);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &lp);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &lpl);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &fp);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &fpl);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &rw);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &rh);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &x0);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &y0);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_int), &bp);
                ok = ok && set(blend_kernel, arg++, sizeof(cl_float),
                               &varsens);
                if (!ok) {
                    error = std::string("local.adjustments ") + label +
                            " blend arguments failed";
                    return false;
                }
                return launch2d(blend_kernel, static_cast<std::size_t>(bfw),
                                static_cast<std::size_t>(bfh), label);
            };

            /*
             * shadows/highlights. The Lab-mode "std" method is
             * ImProcFunctions::shadowsHighlights() on the ROI: a highlight
             * round, then a shadow round. Each round builds the mask and
             * guide, runs guidedFilter() with an explicit subsampling of
             * four, and applies the host-built tone table. The kernels are
             * the lab.tools fragments.
             */
            const auto sh_box_blur =
                [&](cl_mem src, cl_mem dst, cl_int radius_value,
                    cl_int grid_width, cl_int grid_height) {
                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set(sh_box_h_kernel, arg++, sizeof(cl_mem), &src);
                ok = ok && set(sh_box_h_kernel, arg++, sizeof(cl_mem),
                               &sh_temp_memory);
                ok = ok && set(sh_box_h_kernel, arg++, sizeof(cl_int),
                               &radius_value);
                ok = ok && set(sh_box_h_kernel, arg++, sizeof(cl_int),
                               &grid_width);
                ok = ok && set(sh_box_h_kernel, arg++, sizeof(cl_int),
                               &grid_height);
                ok = ok && set(sh_box_h_kernel, arg++, sizeof(cl_int),
                               &sh_sub_pitch);
                if (!ok) {
                    error = "local.adjustments shadows/highlights box blur "
                            "arguments failed";
                    return false;
                }
                if (!launch2d(sh_box_h_kernel, 1,
                              static_cast<std::size_t>(grid_height),
                              "shadows/highlights horizontal box blur")) {
                    return false;
                }
                arg = 0;
                ok = true;
                ok = ok && set(sh_box_v_kernel, arg++, sizeof(cl_mem),
                               &sh_temp_memory);
                ok = ok && set(sh_box_v_kernel, arg++, sizeof(cl_mem), &dst);
                ok = ok && set(sh_box_v_kernel, arg++, sizeof(cl_int),
                               &radius_value);
                ok = ok && set(sh_box_v_kernel, arg++, sizeof(cl_int),
                               &grid_width);
                ok = ok && set(sh_box_v_kernel, arg++, sizeof(cl_int),
                               &grid_height);
                ok = ok && set(sh_box_v_kernel, arg++, sizeof(cl_int),
                               &sh_sub_pitch);
                if (!ok) {
                    error = "local.adjustments shadows/highlights box blur "
                            "arguments failed";
                    return false;
                }
                return launch2d(sh_box_v_kernel,
                                static_cast<std::size_t>(grid_width), 1,
                                "shadows/highlights vertical box blur");
            };

            const auto sh_combine =
                [&](cl_mem dst, cl_mem a, cl_mem b, cl_mem c, cl_int op,
                    cl_float epsilon, cl_int grid_width,
                    cl_int grid_height) {
                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_mem), &dst);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_mem), &a);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_mem), &b);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_mem), &c);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_int), &op);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_float),
                               &epsilon);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_int),
                               &sh_sub_pitch);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_int),
                               &grid_width);
                ok = ok && set(sh_combine_kernel, arg++, sizeof(cl_int),
                               &grid_height);
                if (!ok) {
                    error = "local.adjustments shadows/highlights guided "
                            "filter arguments failed";
                    return false;
                }
                return launch2d(sh_combine_kernel,
                                static_cast<std::size_t>(grid_width),
                                static_cast<std::size_t>(grid_height),
                                "shadows/highlights guided filter");
            };

            const auto run_shadows_highlights_round =
                [&](bool hl, int tonalwidth, cl_mem lut,
                    const char* label) {
                const cl_int hl_flag = hl ? 1 : 0;
                const cl_float thresh =
                    shadowsHighlightsThreshold(tonalwidth);
                const cl_float scale_value =
                    shadowsHighlightsScale(thresh, hl);
                const cl_int sw = sh_sw;
                const cl_int sh = sh_sh;
                const std::size_t sub_global[2] = {
                    static_cast<std::size_t>(sw),
                    static_cast<std::size_t>(sh)};
                const cl_float sub_col_scale =
                    static_cast<cl_float>(bfw) / static_cast<cl_float>(sw);
                const cl_float sub_row_scale =
                    static_cast<cl_float>(bfh) / static_cast<cl_float>(sh);
                const cl_float up_col_scale =
                    static_cast<cl_float>(sw) / static_cast<cl_float>(bfw);
                const cl_float up_row_scale =
                    static_cast<cl_float>(sh) / static_cast<cl_float>(bfh);

                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_mem),
                               &work_memory);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_mem),
                               &sh_full_memory);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int), &bfw);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int), &bfh);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int),
                               &work_pitch);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int),
                               &work_plane);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int),
                               &sh_full_pitch);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int),
                               &sh_full_plane);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_int),
                               &hl_flag);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_float),
                               &thresh);
                ok = ok && set(sh_mask_kernel, arg++, sizeof(cl_float),
                               &scale_value);
                if (!ok) {
                    error = std::string("local.adjustments ") + label +
                            " mask arguments failed";
                    return false;
                }
                if (!launch2d(sh_mask_kernel,
                              static_cast<std::size_t>(bfw),
                              static_cast<std::size_t>(bfh),
                              "shadows/highlights mask")) {
                    return false;
                }

                arg = 0;
                ok = true;
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_mem),
                               &sh_full_memory);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_mem),
                               &sh_i1_memory);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_mem),
                               &sh_p1_memory);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &sw);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &sh);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &bfw);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &bfh);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &sh_full_pitch);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &sh_full_plane);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_int),
                               &sh_sub_pitch);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_float),
                               &sub_col_scale);
                ok = ok && set(sh_subsample_kernel, arg++, sizeof(cl_float),
                               &sub_row_scale);
                if (!ok) {
                    error = std::string("local.adjustments ") + label +
                            " subsample arguments failed";
                    return false;
                }
                if (!launch2d(sh_subsample_kernel, sub_global[0],
                              sub_global[1], "shadows/highlights subsample")) {
                    return false;
                }

                const cl_int radius_value = guidedFilterBoxRadius(
                    shadowsHighlightsRadius(
                        static_cast<int>(
                            rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_RADIUS]),
                        1),
                    RT_LOCAL_ADJUSTMENTS_SH_SUBSAMPLING, sw, sh);
                const cl_float epsilon = RT_LOCAL_ADJUSTMENTS_SH_EPSILON;

                if (!sh_box_blur(sh_i1_memory, sh_meani_memory,
                                 radius_value, sw, sh) ||
                    !sh_box_blur(sh_p1_memory, sh_meanp_memory,
                                 radius_value, sw, sh)) {
                    return false;
                }
                if (!sh_combine(sh_p1_memory, sh_i1_memory, sh_p1_memory,
                                sh_p1_memory, 0, epsilon, sw, sh) ||
                    !sh_box_blur(sh_p1_memory, sh_p1_memory, radius_value, sw,
                                 sh)) {
                    return false;
                }
                if (!sh_combine(sh_i1_memory, sh_i1_memory, sh_i1_memory,
                                sh_i1_memory, 0, epsilon, sw, sh) ||
                    !sh_box_blur(sh_i1_memory, sh_i1_memory, radius_value, sw,
                                 sh)) {
                    return false;
                }
                if (!sh_combine(sh_i1_memory, sh_meani_memory, sh_meani_memory,
                                sh_i1_memory, 1, epsilon, sw, sh) ||
                    !sh_combine(sh_p1_memory, sh_meani_memory, sh_meanp_memory,
                                sh_p1_memory, 1, epsilon, sw, sh)) {
                    return false;
                }
                if (!sh_combine(sh_i1_memory, sh_p1_memory, sh_i1_memory,
                                sh_i1_memory, 2, epsilon, sw, sh) ||
                    !sh_combine(sh_p1_memory, sh_i1_memory, sh_meani_memory,
                                sh_meanp_memory, 1, epsilon, sw, sh)) {
                    return false;
                }
                if (!sh_box_blur(sh_i1_memory, sh_i1_memory, radius_value, sw,
                                 sh) ||
                    !sh_box_blur(sh_p1_memory, sh_p1_memory, radius_value, sw,
                                 sh)) {
                    return false;
                }

                arg = 0;
                ok = true;
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_mem),
                               &sh_i1_memory);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_mem),
                               &sh_p1_memory);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_mem),
                               &sh_full_memory);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int), &sw);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int), &sh);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int),
                               &bfw);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int),
                               &bfh);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int),
                               &sh_sub_pitch);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int),
                               &sh_full_pitch);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_int),
                               &sh_full_plane);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_float),
                               &up_col_scale);
                ok = ok && set(sh_upsample_kernel, arg++, sizeof(cl_float),
                               &up_row_scale);
                if (!ok) {
                    error = std::string("local.adjustments ") + label +
                            " upsample arguments failed";
                    return false;
                }
                if (!launch2d(sh_upsample_kernel,
                              static_cast<std::size_t>(bfw),
                              static_cast<std::size_t>(bfh),
                              "shadows/highlights upsample")) {
                    return false;
                }

                arg = 0;
                ok = true;
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_mem),
                               &work_memory);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_mem),
                               &sh_full_memory);
                const cl_int sh_lut_size =
                    static_cast<cl_int>(RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_mem), &lut);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int),
                               &sh_lut_size);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int), &bfw);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int), &bfh);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int),
                               &work_pitch);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int),
                               &work_plane);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int),
                               &sh_full_pitch);
                ok = ok && set(sh_apply_kernel, arg++, sizeof(cl_int),
                               &hl_flag);
                if (!ok) {
                    error = std::string("local.adjustments ") + label +
                            " blend arguments failed";
                    return false;
                }
                return launch2d(sh_apply_kernel,
                                static_cast<std::size_t>(bfw),
                                static_cast<std::size_t>(bfh),
                                "shadows/highlights blend");
            };

            /*
             * Local CBDL. The host's block runs before vibrance and only when
             * the ROI is wider and taller than 65 pixels. The luminance pass
             * reconstructs the directional pyramid and blends with
             * transit_shapedetect(6); the chroma pass repeats it on the
             * chroma magnitude and blends with transit_shapedetect(7). The
             * range tables and the pyramid kernels are the lab.wavelet
             * node's.
             */
            if (cbdl && bfw > 65 && bfh > 65) {
#if defined(__SSE2__) || defined(RT_SIMDE)
                const cl_int cbdl_vectorised = 1;
#else
                const cl_int cbdl_vectorised = 0;
#endif
                const cl_float clarL =
                    0.01f * rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CLARITY];
                const cl_float clarC =
                    0.01f *
                    (0.5f * rec[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CLARITY]);

                const auto cbdl_run_pyramid =
                    [&](cl_mem src, int src_pitch, cl_mem luts,
                        uint32_t levels, cl_float clar,
                        const char* label) -> bool {
                    for (uint32_t level = 0; level < levels; ++level) {
                        cl_mem fine = level == 0
                                          ? src
                                          : memory_of(
                                                cbdl_pyr_device[level - 1]);
                        cl_int fine_pitch =
                            level == 0 ? src_pitch : cbdl_pyr_pitch[level - 1];
                        cl_mem coarse = memory_of(cbdl_pyr_device[level]);
                        cl_int coarse_pitch = cbdl_pyr_pitch[level];
                        cl_int cl_level = static_cast<cl_int>(level);
                        const cl_int cl_scale = 1 << level;
                        cl_uint arg = 0;
                        bool ok = true;
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_mem), &fine);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &fine_pitch);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_mem), &coarse);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &coarse_pitch);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &bfw);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &bfh);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &cl_level);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &cl_scale);
                        ok = ok && set(cbdl_decompose_kernel, arg++,
                                       sizeof(cl_int), &cbdl_vectorised);
                        if (!ok ||
                            !launch2d(cbdl_decompose_kernel,
                                      static_cast<std::size_t>(bfw),
                                      static_cast<std::size_t>(bfh), label)) {
                            error = std::string("local.adjustments ") +
                                    label + " decompose failed";
                            return false;
                        }
                    }

                    /* residbuff starts as the coarsest level. */
                    {
                        cl_mem seed_src =
                            memory_of(cbdl_pyr_device[levels - 1]);
                        cl_mem seed_dst = memory_of(cbdl_resid_device);
                        cl_int seed_src_pitch = cbdl_pyr_pitch[levels - 1];
                        cl_uint arg = 0;
                        bool ok = true;
                        ok = ok && set(cbdl_copy_kernel, arg++,
                                       sizeof(cl_mem), &seed_src);
                        ok = ok && set(cbdl_copy_kernel, arg++,
                                       sizeof(cl_int), &seed_src_pitch);
                        ok = ok && set(cbdl_copy_kernel, arg++,
                                       sizeof(cl_mem), &seed_dst);
                        ok = ok && set(cbdl_copy_kernel, arg++,
                                       sizeof(cl_int), &cbdl_resid_pitch);
                        ok = ok && set(cbdl_copy_kernel, arg++,
                                       sizeof(cl_int), &bfw);
                        ok = ok && set(cbdl_copy_kernel, arg++,
                                       sizeof(cl_int), &bfh);
                        if (!ok ||
                            !launch2d(cbdl_copy_kernel,
                                      static_cast<std::size_t>(bfw),
                                      static_cast<std::size_t>(bfh), label)) {
                            error = std::string("local.adjustments ") +
                                    label + " seed failed";
                            return false;
                        }
                    }

                    cl_mem resid = memory_of(cbdl_resid_device);
                    cl_mem mask = memory_of(placeholder_device);
                    const cl_int mask_pitch = 0;
                    const cl_int cl_skin = 0;
                    const cl_float cl_zero = 0.0f;
                    for (int level = static_cast<int>(levels) - 1; level >= 0;
                         --level) {
                        cl_mem fine = level == 0
                                          ? src
                                          : memory_of(
                                                cbdl_pyr_device[level - 1]);
                        cl_int fine_pitch =
                            level == 0 ? src_pitch : cbdl_pyr_pitch[level - 1];
                        cl_mem coarse = memory_of(cbdl_pyr_device[level]);
                        cl_int coarse_pitch = cbdl_pyr_pitch[level];
                        cl_int cl_level = level;
                        const cl_int to_dst = 0;
                        cl_uint arg = 0;
                        bool ok = true;
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &fine);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &fine_pitch);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &coarse);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &coarse_pitch);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &resid);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &cbdl_resid_pitch);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &resid);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &cbdl_resid_pitch);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &luts);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &cl_level);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &mask);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &mask_pitch);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_mem), &mask);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &mask_pitch);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &bfw);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &bfh);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &cl_skin);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_float), &cl_zero);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_float), &cl_zero);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_float), &cl_zero);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_float), &cl_zero);
                        ok = ok && set(cbdl_equalize_kernel, arg++,
                                       sizeof(cl_int), &to_dst);
                        if (!ok ||
                            !launch2d(cbdl_equalize_kernel,
                                      static_cast<std::size_t>(bfw),
                                      static_cast<std::size_t>(bfh), label)) {
                            error = std::string("local.adjustments ") +
                                    label + " equalize failed";
                            return false;
                        }
                    }

                    cl_mem resid5 =
                        memory_of(cbdl_pyr_device[levels - 1]);
                    cl_mem out = memory_of(cbdl_out_device);
                    cl_int resid5_pitch = cbdl_pyr_pitch[levels - 1];
                    cl_uint arg = 0;
                    bool ok = true;
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_mem),
                                   &resid);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_int),
                                   &cbdl_resid_pitch);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_mem),
                                   &resid5);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_int),
                                   &resid5_pitch);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_mem),
                                   &out);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_int),
                                   &cbdl_out_pitch);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_int),
                                   &bfw);
                    ok = ok && set(cbdl_finish_kernel, arg++, sizeof(cl_int),
                                   &bfh);
                    ok = ok && set(cbdl_finish_kernel, arg++,
                                   sizeof(cl_float), &clar);
                    if (!ok ||
                        !launch2d(cbdl_finish_kernel,
                                  static_cast<std::size_t>(bfw),
                                  static_cast<std::size_t>(bfh), label)) {
                        error = std::string("local.adjustments ") + label +
                                " finish failed";
                        return false;
                    }
                    return true;
                };

                /* Luminance: the source is the ROI's L plane. */
                if (cbdlLumaActive) {
                    if (!copy_roi() ||
                        !blur_roi(*setup.iir_vibrance, "CBDL blur")) {
                        return RT_STAGE_FALLBACK;
                    }
                    cl_mem src_memory = memory_of(cbdl_src_device);
                    cl_mem cbdl_work_memory = memory_of(work_device);
                    cl_int mode = 0;
                    cl_uint arg = 0;
                    bool ok = true;
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_mem),
                                   &cbdl_work_memory);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &work_pitch);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &work_plane);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_mem),
                                   &src_memory);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &cbdl_src_pitch);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &bfw);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &bfh);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &mode);
                    if (!ok ||
                        !launch2d(cbdl_fill_kernel,
                                  static_cast<std::size_t>(bfw),
                                  static_cast<std::size_t>(bfh),
                                  "CBDL luminance source")) {
                        error = "local.adjustments CBDL luminance source "
                                "failed";
                        return RT_STAGE_FALLBACK;
                    }
                    if (!cbdl_run_pyramid(
                            src_memory, cbdl_src_pitch,
                            memory_of(cbdl_luma_device), cbdlLumaLevels,
                            clarL, "CBDL luminance")) {
                        return RT_STAGE_FALLBACK;
                    }

                    cl_mem lab_memory = memory_of(image);
                    cl_mem fin_memory = memory_of(cbdl_out_device);
                    cl_mem blur_l = memory_of(blur_device[0]);
                    cl_mem blur_a = memory_of(blur_device[1]);
                    cl_mem blur_b = memory_of(blur_device[2]);
                    cl_mem record_memory = memory_of(record_device);
                    cl_int cl_lab_pitch = lab_pitch;
                    cl_int cl_lab_plane = lab_plane;
                    cl_int cl_fin_pitch = cbdl_out_pitch;
                    cl_int rw = bfw;
                    cl_int rh = bfh;
                    cl_int x0 = xstart;
                    cl_int y0 = ystart;
                    cl_int bp = blur_pitch;
                    arg = 0;
                    ok = true;
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_mem),
                                   &lab_memory);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_mem),
                                   &fin_memory);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_mem),
                                   &blur_l);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_mem),
                                   &blur_a);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_mem),
                                   &blur_b);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_mem),
                                   &record_memory);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &cl_lab_pitch);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &cl_lab_plane);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &cl_fin_pitch);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &rw);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &rh);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &x0);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &y0);
                    ok = ok && set(cbdl_luma_kernel, arg++, sizeof(cl_int),
                                   &bp);
                    if (!ok ||
                        !launch2d(cbdl_luma_kernel,
                                  static_cast<std::size_t>(bfw),
                                  static_cast<std::size_t>(bfh),
                                  "CBDL luminance blend")) {
                        error = "local.adjustments CBDL luminance blend "
                                "failed";
                        return RT_STAGE_FALLBACK;
                    }
                }

                /* Chroma: the source is sqrt(a*a + b*b), and the block
                 * normalises the result by 1 / (0.01 * max |value|). */
                if (cbdlChromaActive) {
                    if (!copy_roi() ||
                        !blur_roi(*setup.iir_vibrance, "CBDL chroma blur")) {
                        return RT_STAGE_FALLBACK;
                    }
                    cl_mem src_memory = memory_of(cbdl_src_device);
                    cl_mem cbdl_work_memory = memory_of(work_device);
                    cl_int mode = 1;
                    cl_uint arg = 0;
                    bool ok = true;
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_mem),
                                   &cbdl_work_memory);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &work_pitch);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &work_plane);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_mem),
                                   &src_memory);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &cbdl_src_pitch);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &bfw);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &bfh);
                    ok = ok && set(cbdl_fill_kernel, arg++, sizeof(cl_int),
                                   &mode);
                    if (!ok ||
                        !launch2d(cbdl_fill_kernel,
                                  static_cast<std::size_t>(bfw),
                                  static_cast<std::size_t>(bfh),
                                  "CBDL chroma source")) {
                        error = "local.adjustments CBDL chroma source failed";
                        return RT_STAGE_FALLBACK;
                    }
                    if (!cbdl_run_pyramid(
                            src_memory, cbdl_src_pitch,
                            memory_of(cbdl_chroma_device),
                            cbdlChromaLevels, clarC, "CBDL chroma")) {
                        return RT_STAGE_FALLBACK;
                    }

                    cl_mem lab_memory = memory_of(image);
                    cl_mem out_memory = memory_of(cbdl_out_device);
                    cl_mem rowmax_memory = memory_of(cbdl_rowmax_device);
                    cl_int cl_lab_pitch = lab_pitch;
                    cl_int cl_lab_plane = lab_plane;
                    cl_int cl_out_pitch = cbdl_out_pitch;
                    cl_int rw = bfw;
                    cl_int rh = bfh;
                    cl_int x0 = xstart;
                    cl_int y0 = ystart;
                    arg = 0;
                    ok = true;
                    ok = ok && set(cbdl_metric_kernel, arg++, sizeof(cl_mem),
                                   &out_memory);
                    ok = ok && set(cbdl_metric_kernel, arg++,
                                   sizeof(cl_int), &cl_out_pitch);
                    ok = ok && set(cbdl_metric_kernel, arg++, sizeof(cl_mem),
                                   &lab_memory);
                    ok = ok && set(cbdl_metric_kernel, arg++,
                                   sizeof(cl_int), &cl_lab_pitch);
                    ok = ok && set(cbdl_metric_kernel, arg++,
                                   sizeof(cl_int), &cl_lab_plane);
                    ok = ok && set(cbdl_metric_kernel, arg++,
                                   sizeof(cl_int), &x0);
                    ok = ok && set(cbdl_metric_kernel, arg++,
                                   sizeof(cl_int), &y0);
                    ok = ok && set(cbdl_metric_kernel, arg++, sizeof(cl_int),
                                   &rw);
                    ok = ok && set(cbdl_metric_kernel, arg++, sizeof(cl_int),
                                   &rh);
                    ok = ok && set(cbdl_metric_kernel, arg++, sizeof(cl_mem),
                                   &rowmax_memory);
                    if (!ok ||
                        !launch1d(cbdl_metric_kernel,
                                  static_cast<std::size_t>(bfh),
                                  "CBDL chroma metric")) {
                        error = "local.adjustments CBDL chroma metric failed";
                        return RT_STAGE_FALLBACK;
                    }

                    RtStageBuffer rowmax_host{};
                    rowmax_host.size = sizeof(RtStageBuffer);
                    rowmax_host.data = cbdl_rowmax.data();
                    rowmax_host.width = bfh;
                    rowmax_host.height = 1;
                    rowmax_host.row_pitch =
                        static_cast<std::size_t>(bfh) * sizeof(float);
                    rowmax_host.plane_pitch = rowmax_host.row_pitch;
                    rowmax_host.planes = 1;
                    rowmax_host.format = RT_STAGE_F32;
                    rowmax_host.flags = RT_STAGE_BUFFER_READ;
                    if (residency->readback(cbdl_rowmax_device, rowmax_host) !=
                        RT_STAGE_OK) {
                        error = "local.adjustments CBDL chroma metric "
                                "readback failed";
                        return RT_STAGE_FALLBACK;
                    }
                    float maxAbs = 0.f;
                    for (int row = 0; row < bfh; ++row) {
                        maxAbs = std::max(
                            maxAbs,
                            cbdl_rowmax[static_cast<std::size_t>(row)]);
                    }
                    cl_float coefC = 0.01f * maxAbs;
                    if (coefC > 0.f) {
                        coefC = 1.f / coefC;
                    }

                    cl_mem fin_memory = out_memory;
                    cl_mem blur_l = memory_of(blur_device[0]);
                    cl_mem blur_a = memory_of(blur_device[1]);
                    cl_mem blur_b = memory_of(blur_device[2]);
                    cl_mem record_memory = memory_of(record_device);
                    cl_int cl_fin_pitch = cbdl_out_pitch;
                    cl_int bp = blur_pitch;
                    arg = 0;
                    ok = true;
                    ok = ok && set(cbdl_chroma_kernel, arg++, sizeof(cl_mem),
                                   &lab_memory);
                    ok = ok && set(cbdl_chroma_kernel, arg++, sizeof(cl_mem),
                                   &fin_memory);
                    ok = ok && set(cbdl_chroma_kernel, arg++, sizeof(cl_mem),
                                   &blur_l);
                    ok = ok && set(cbdl_chroma_kernel, arg++, sizeof(cl_mem),
                                   &blur_a);
                    ok = ok && set(cbdl_chroma_kernel, arg++, sizeof(cl_mem),
                                   &blur_b);
                    ok = ok && set(cbdl_chroma_kernel, arg++, sizeof(cl_mem),
                                   &record_memory);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &cl_lab_pitch);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &cl_lab_plane);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &cl_fin_pitch);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &rw);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &rh);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &x0);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &y0);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_int), &bp);
                    ok = ok && set(cbdl_chroma_kernel, arg++,
                                   sizeof(cl_float), &coefC);
                    if (!ok ||
                        !launch2d(cbdl_chroma_kernel,
                                  static_cast<std::size_t>(bfw),
                                  static_cast<std::size_t>(bfh),
                                  "CBDL chroma blend")) {
                        error = "local.adjustments CBDL chroma blend failed";
                        return RT_STAGE_FALLBACK;
                    }
                }
            }

            /*
             * Vibrance. ImProcFunctions::vibrance() runs on the ROI copy;
             * the blend is transit_shapedetect2() with the vibrance scope
             * sensitivity (lp.sensv). The skin curve and the Munsell set are
             * host-built tables.
             */
            if (vibrance) {
                if (!copy_roi() ||
                    !blur_roi(*setup.iir_vibrance, "vibrance blur")) {
                    return RT_STAGE_FALLBACK;
                }

                const cl_int cl_pastels = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PASTELS]);
                const cl_int cl_saturated = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SATURATED]);
                const cl_int cl_threshold_top = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_TOP]);
                const cl_int cl_threshold_bottom = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_BOTTOM]);
                const cl_int cl_protectskins = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PROTECTSKINS]);
                const cl_int cl_avoidcolorshift = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_AVOIDCOLORSHIFT]);
                const cl_int cl_skin_curve_set = skinActive ? 1 : 0;
                const cl_int cl_vibrance_highlight = 0;
                cl_mem lab_memory = work_memory;
                cl_mem iws_memory = memory_of(iws_device);
                cl_mem skin_memory = memory_of(skin_device);
                cl_mem munsell_memory = memory_of(munsell_device);
                cl_mem munsell_sizes_memory =
                    memory_of(munsell_sizes_device);
                cl_int rw = bfw;
                cl_int rh = bfh;
                cl_int wp = work_pitch;
                cl_int wpl = work_plane;
                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int), &rw);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int), &rh);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int), &wp);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int), &wpl);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_mem),
                               &iws_memory);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_mem),
                               &skin_memory);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_skin_curve_set);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_mem),
                               &munsell_memory);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_mem),
                               &munsell_sizes_memory);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_pastels);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_saturated);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_threshold_top);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_threshold_bottom);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_protectskins);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_avoidcolorshift);
                ok = ok && set(vibrance_kernel, arg++, sizeof(cl_int),
                               &cl_vibrance_highlight);
                if (!ok || !launch2d(vibrance_kernel,
                                     static_cast<std::size_t>(bfw),
                                     static_cast<std::size_t>(bfh),
                                     "vibrance")) {
                    error = "local.adjustments vibrance failed";
                    return RT_STAGE_FALLBACK;
                }
                if (!run_blend("vibrance",
                               rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSV])) {
                    return RT_STAGE_FALLBACK;
                }
            }

            /*
             * shadows/highlights. The Lab-mode "std" method is
             * ImProcFunctions::shadowsHighlights() on the ROI, after vibrance
             * and before local contrast, as in Lab_Local(). The pass takes a
             * fresh ROI copy of the image as vibrance left it.
             */
            if (shadowsHighlights) {
                if (!copy_roi() ||
                    !blur_roi(*setup.iir_shadows_highlights,
                              "shadows/highlights blur")) {
                    return RT_STAGE_FALLBACK;
                }
                const cl_int highlight_amount = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS]);
                const cl_int shadow_amount = static_cast<cl_int>(
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS]);
                if (highlight_amount > 0 &&
                    !run_shadows_highlights_round(
                        true,
                        static_cast<int>(
                            rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_TONAL]),
                        memory_of(sh_hl_device), "highlight")) {
                    return RT_STAGE_FALLBACK;
                }
                if (shadow_amount > 0 &&
                    !run_shadows_highlights_round(
                        false,
                        static_cast<int>(
                            rec[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_TONAL]),
                        memory_of(sh_sh_device), "shadow")) {
                    return RT_STAGE_FALLBACK;
                }
                if (!run_blend("shadows/highlights",
                               rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSH])) {
                    return RT_STAGE_FALLBACK;
                }
            }

            /*
             * Soft light. ImProcFunctions::softLight() runs per pixel on the
             * ROI copy, after shadows/highlights and before local contrast,
             * as in Lab_Local(). The blend is transit_shapedetect2() with the
             * soft-light scope sensitivity (lp.senssf) and the default
             * 3/sk deltaE reference blur (iir_vibrance).
             */
            if (softLight) {
                if (!copy_roi() ||
                    !blur_roi(*setup.iir_vibrance, "soft-light blur")) {
                    return RT_STAGE_FALLBACK;
                }

                cl_mem gamma_memory = memory_of(gamma_srgb_device);
                cl_mem igamma_memory = memory_of(igamma_srgb_device);
                cl_mem ws_memory = memory_of(ws_device);
                cl_mem iws_memory = memory_of(iws_device);
                cl_mem cachef_memory = memory_of(cachef_device);
                cl_mem cachefy_memory = memory_of(cachefy_device);
                cl_int rw = bfw;
                cl_int rh = bfh;
                cl_int wp = work_pitch;
                cl_int wpl = work_plane;
                cl_float strength =
                    rec[RT_LOCAL_ADJUSTMENTS_SLOT_SOFT_STRENGTH];
                cl_uint arg = 0;
                bool ok = true;
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &work_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &gamma_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &igamma_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &ws_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &iws_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &cachef_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_mem),
                               &cachefy_memory);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_int), &rw);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_int), &rh);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_int), &wp);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_int), &wpl);
                ok = ok && set(softlight_kernel, arg++, sizeof(cl_float),
                               &strength);
                if (!ok || !launch2d(softlight_kernel,
                                     static_cast<std::size_t>(bfw),
                                     static_cast<std::size_t>(bfh),
                                     "soft light")) {
                    error = "local.adjustments soft light failed";
                    return RT_STAGE_FALLBACK;
                }
                if (!run_blend("soft light",
                               rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSOFT])) {
                    return RT_STAGE_FALLBACK;
                }
            }

            /*
             * Local contrast. The host's block is guarded by mSPwav = 32; a
             * smaller ROI does no local-contrast work, so the pass is skipped
             * and the spot keeps the earlier tools' output.
             */
            if (localContrast && bfw >= 32 && bfh >= 32) {
                StageDeviceBufferPtr lc_h_device = residency->acquire(
                    &lc_h_key, StageBufferShape{RT_STAGE_F32, bfw, bfh, 1},
                    &error);
                StageDeviceBufferPtr lc_v_device = residency->acquire(
                    &lc_v_key, StageBufferShape{RT_STAGE_F32, bfw, bfh, 1},
                    &error);
                if (!lc_h_device || !lc_v_device) {
                    error = "local.adjustments local-contrast allocation "
                            "failed";
                    return RT_STAGE_FALLBACK;
                }
                const int lc_h_pitch = int_pitch(pitch_of(lc_h_device),
                                                 "local-contrast");
                const int lc_v_pitch = int_pitch(pitch_of(lc_v_device),
                                                 "local-contrast");
                if (lc_h_pitch < 0 || lc_v_pitch < 0) {
                    return RT_STAGE_FALLBACK;
                }
                cl_mem lc_h_memory = memory_of(lc_h_device);
                cl_mem lc_v_memory = memory_of(lc_v_device);

                /*
                 * gaussianBlur()'s double path keeps a whole line of double
                 * forward states. The stage formats are fp32 only, so this one
                 * buffer is created directly and checked against the
                 * residency's remaining budget; an over-budget ROI falls back
                 * before any pass runs.
                 */
                cl_mem lc_state_memory = scratch_memory;
                std::shared_ptr<void> lc_state_owner;
                if (setup.iir_local_contrast->double_path != 0) {
                    const std::size_t samples =
                        static_cast<std::size_t>(bfw) *
                        static_cast<std::size_t>(bfh);
                    const std::size_t state_bytes = samples * sizeof(cl_double);
                    const StageResidencyStats stats = residency->stats();
                    if (stats.used_bytes > stats.budget_bytes ||
                        state_bytes > stats.budget_bytes - stats.used_bytes) {
                        error = "local.adjustments local-contrast state "
                                "exceeds the device budget";
                        return RT_STAGE_FALLBACK;
                    }
                    cl_int state_error = CL_SUCCESS;
                    cl_mem memory = env->api->CreateBuffer(
                        env->context, CL_MEM_READ_WRITE, state_bytes, nullptr,
                        &state_error);
                    if (memory == nullptr) {
                        error = "local.adjustments local-contrast state "
                                "allocation failed";
                        return RT_STAGE_FALLBACK;
                    }
                    lc_state_owner = std::shared_ptr<void>(
                        static_cast<void*>(memory),
                        [api = env->api](void* pointer) {
                            api->ReleaseMemObject(
                                static_cast<cl_mem>(pointer));
                        });
                    lc_state_memory = memory;
                }

                if (!copy_roi() ||
                    !blur_roi(*setup.iir_vibrance,
                              "local-contrast deltaE blur")) {
                    return RT_STAGE_FALLBACK;
                }

                const cl_int lc_mode =
                    static_cast<cl_int>(
                        setup.iir_local_contrast->double_path);
                const cl_int lc_h_lines =
                    lc_mode != 0 ? 0 : bfh - (bfh % 4);
                const cl_int lc_v_lines =
                    lc_mode != 0 ? 0 : bfw - (bfw % 8);
                if (!run_line(line_kernel, work_memory, lc_h_memory,
                              lc_state_memory, bfw, bfh, work_pitch,
                              lc_h_pitch, 1, 1, 0, 0, lc_mode, lc_h_lines,
                              setup.iir_local_contrast->horizontal,
                              "local-contrast horizontal") ||
                    !run_line(line_kernel, lc_h_memory, lc_v_memory,
                              lc_state_memory, bfh, bfw, 1, 1, lc_h_pitch,
                              lc_v_pitch, 0, 0, lc_mode, lc_v_lines,
                              setup.iir_local_contrast->vertical,
                              "local-contrast vertical")) {
                    return RT_STAGE_FALLBACK;
                }

                {
                    cl_uint arg = 0;
                    cl_mem lab_memory = work_memory;
                    cl_mem blur_memory = lc_v_memory;
                    cl_int rw = bfw;
                    cl_int rh = bfh;
                    cl_int wp = work_pitch;
                    cl_int wpl = work_plane;
                    cl_int lp = lc_v_pitch;
                    cl_float amount =
                        rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_AMOUNT];
                    cl_float darkness =
                        rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_DARKNESS];
                    cl_float lightness =
                        rec[RT_LOCAL_ADJUSTMENTS_SLOT_LC_LIGHTNESS];
                    bool ok = true;
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_mem),
                                   &lab_memory);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_mem),
                                   &blur_memory);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_int),
                                   &rw);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_int),
                                   &rh);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_int),
                                   &wp);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_int),
                                   &wpl);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_int),
                                   &lp);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_float),
                                   &amount);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_float),
                                   &darkness);
                    ok = ok && set(lc_blend_kernel, arg++, sizeof(cl_float),
                                   &lightness);
                    if (!ok || !launch2d(lc_blend_kernel,
                                         static_cast<std::size_t>(bfw),
                                         static_cast<std::size_t>(bfh),
                                         "local-contrast blend")) {
                        error = "local.adjustments local-contrast blend "
                                "failed";
                        return RT_STAGE_FALLBACK;
                    }
                }
                if (!run_blend("local-contrast",
                               rec[RT_LOCAL_ADJUSTMENTS_SLOT_SENSLC])) {
                    return RT_STAGE_FALLBACK;
                }
            }

            /*
             * Colour & Light runs last in Lab_Local(): after vibrance and
             * local contrast. Its fused apply reads the image as the earlier
             * tools left it and its own sigma blur.
             */
            if (colour) {
                if (!copy_roi() || !blur_roi(*setup.iir, "colour blur")) {
                    return RT_STAGE_FALLBACK;
                }

                cl_uint arg = 0;
                cl_mem lab_memory = memory_of(image);
                cl_mem blur_l = memory_of(blur_device[0]);
                cl_mem blur_a = memory_of(blur_device[1]);
                cl_mem blur_b = memory_of(blur_device[2]);
                cl_mem record_memory = memory_of(record_device);
                cl_mem luts_memory = memory_of(luts_device);
                cl_mem light_memory = memory_of(light_device);
                cl_mem satur_memory = memory_of(satur_device);
                cl_mem saturmoins_memory = memory_of(saturmoins_device);
                cl_int x0 = xstart;
                cl_int y0 = ystart;
                cl_int rw = bfw;
                cl_int rh = bfh;
                cl_int lp = lab_pitch;
                cl_int lpl = lab_plane;
                cl_int bp = blur_pitch;
                bool ok = true;
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem),
                               &lab_memory);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem), &blur_l);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem), &blur_a);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem), &blur_b);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem),
                               &record_memory);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem),
                               &luts_memory);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem),
                               &light_memory);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem),
                               &satur_memory);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_mem),
                               &saturmoins_memory);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &lp);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &lpl);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &rw);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &rh);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &x0);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &y0);
                ok = ok && set(apply_kernel, arg++, sizeof(cl_int), &bp);
                if (!ok || !launch2d(apply_kernel,
                                     static_cast<std::size_t>(bfw),
                                     static_cast<std::size_t>(bfh),
                                     "colour apply")) {
                    error = "local.adjustments colour apply failed";
                    return RT_STAGE_FALLBACK;
                }
            }
        }

        if (!run_clamp(RT_LOCAL_ADJUSTMENTS_EPSI, static_cast<int>(clamp_mode),
                       "tail clamp")) {
            return RT_STAGE_FALLBACK;
        }

        if (active && islocal && avoid == RT_LOCAL_ADJUSTMENTS_AVOID_MUNS) {
            cl_uint arg = 0;
            cl_mem lab_memory = memory_of(image);
            cl_mem reserved_memory = memory_of(reserved);
            cl_mem record_memory = memory_of(record_device);
            cl_mem tables_memory = memory_of(munsell_device);
            cl_mem sizes_memory = memory_of(munsell_sizes_device);
            bool ok = true;
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_mem),
                           &lab_memory);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_mem),
                           &reserved_memory);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_mem),
                           &record_memory);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_mem),
                           &tables_memory);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_mem),
                           &sizes_memory);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_int), &width);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_int), &height);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_int), &lab_pitch);
            ok = ok && set(munsell_kernel, arg++, sizeof(cl_int), &lab_plane);
            if (!ok || !launch2d(munsell_kernel,
                                 static_cast<std::size_t>(width),
                                 static_cast<std::size_t>(height),
                                 "Munsell")) {
                error = "local.adjustments Munsell pass failed";
                return RT_STAGE_FALLBACK;
            }
        }

        if (residency->readback(image, staging_buffer) != RT_STAGE_OK) {
            error = "local.adjustments readback failed";
            return RT_STAGE_ERROR;
        }
    }

    copy_from_staging();
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
        if (!request.impl.empty()) {
            StageEvent event = StageQueue::submit(request, profiling);
            event.note = "an explicit implementation is a host program";
            return event;
        }

        if (find_kernel(request.name) == nullptr) {
            StageEvent event = StageQueue::submit(request, profiling);
            event.note = "no OpenCL implementation for stage '" +
                         request.name + "'";
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
        } else if (request.name == "thumbnail.transform") {
            const RtStageStatus status =
                run_thumbnail_permute(env_, residency_, request.args,
                                      request.name.c_str(), false, error);
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
        } else if (request.name == "thumbnail.transform.u8") {
            const RtStageStatus status =
                run_thumbnail_permute(env_, residency_, request.args,
                                      request.name.c_str(), true, error);
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
        } else if (request.name == "thumbnail.scale") {
            const RtStageStatus status =
                run_thumbnail_scale(env_, residency_, request.args,
                                    request.name.c_str(), false, error);
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
        } else if (request.name == "thumbnail.scale.u8") {
            const RtStageStatus status =
                run_thumbnail_scale(env_, residency_, request.args,
                                    request.name.c_str(), true, error);
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
        } else if (request.name == "gamut") {
            const RtStageStatus status =
                run_gamut(env_, residency_, request.args, error);
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
        } else if (request.name == "working.trc") {
            const RtStageStatus status =
                run_working_trc(env_, residency_, request.args, error);
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
        } else if (request.name == "lab.tools") {
            const RtStageStatus status =
                run_lab_tools(env_, residency_, request.args, error);
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
        } else if (request.name == "local.adjustments") {
            const RtStageStatus status =
                run_local_adjustments(env_, residency_, request.args, error);
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
        } else if (request.name == "lab.wavelet") {
            const RtStageStatus status =
                run_lab_wavelet(env_, residency_, request.args, error);
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
        } else if (request.name == "output.sharpen") {
            const RtStageStatus status =
                run_sharpen(env_, residency_, request.args, error);
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
        } else if (request.name == "framing") {
            const RtStageStatus status =
                run_framing(env_, residency_, request.args, error);
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
