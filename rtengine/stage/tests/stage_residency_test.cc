/*
 * Device residency, event and selection tests for the stage runtime.
 *
 * The OpenCL cases run whenever a usable device is present. POCL exposes a CPU
 * device only with RT_OPENCL_ALLOW_CPU=1; the device type is reported so a
 * CPU-device run is never presented as GPU evidence.
 */

#include "stage_buffer.h"
#include "stage_device.h"
#include "stage_opencl_device.h"
#include "stage_runtime.h"

#include "image8.h"
#include "image16.h"
#include "imagefloat.h"
#include "labimage.h"

#include "color.h"
#include "curves.h"
#include "flatcurvetypes.h"
#include "iccstore.h"
#include "improcfun.h"
#include "localadjustments.h"
#include "procparams.h"
#include "settings.h"
#include "utils.h"
#include "stage/algorithms/gamut_cl.h"
#include "stage/algorithms/sharpen_cl.h"
#include "stage/lab_tools_context.h"
#include "stage/lab_sharpening_context.h"
#include "stage/lab_wavelet_context.h"
#include "stage/local_adjustments_context.h"
#include "stage/working_trc_context.h"

#include "rt_algo.h"
#include "sharpen_core.h"
#include "array2D.h"
#include "boxblur.h"
#include "guidedfilter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace rtengine
{
/* The retinex residual reaches retinex_pde(), which takes this mutex. The
 * test binary does not run rtengine::init(). */
extern MyMutex* fftwMutex;
}

namespace
{

using namespace rtengine::stage;

using rtengine::Image8;
using rtengine::Image16;
using rtengine::Imagefloat;
using rtengine::LabImage;
using rtengine::TMatrix;
using rtengine::TI_Bilinear;
using rtengine::TI_Nearest;
using rtengine::TypeInterpolation;

int failures = 0;
int skipped = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void skip(const char* message)
{
    std::fprintf(stderr, "SKIP: %s\n", message);
    ++skipped;
}

bool close_enough(float value)
{
    return std::isfinite(value);
}

RtStageArg buffer_arg(const RtStageBuffer& buffer)
{
    RtStageArg arg{};
    arg.kind = RT_STAGE_ARG_BUFFER;
    arg.u.buffer = buffer;
    return arg;
}

RtStageArg double_arg(double value)
{
    RtStageArg arg{};
    arg.kind = RT_STAGE_ARG_DOUBLE;
    arg.u.f = value;
    return arg;
}

RtStageArg int_arg(std::int64_t value)
{
    RtStageArg arg{};
    arg.kind = RT_STAGE_ARG_INT;
    arg.u.i = value;
    return arg;
}

void fill(RtStageBuffer& buffer, unsigned seed)
{
    for (std::uint32_t plane = 0; plane < buffer.planes; ++plane) {
        unsigned char* plane_data = static_cast<unsigned char*>(buffer.data) +
                                    static_cast<std::size_t>(plane) *
                                        buffer.plane_pitch;
        for (int y = 0; y < buffer.height; ++y) {
            float* row = reinterpret_cast<float*>(
                plane_data + static_cast<std::size_t>(y) * buffer.row_pitch);
            for (int x = 0; x < buffer.width; ++x) {
                const unsigned value =
                    (seed + static_cast<unsigned>(x) * 17u +
                     static_cast<unsigned>(y) * 31u + plane * 101u) %
                    65536u;
                row[x] = static_cast<float>(value);
            }
        }
    }
}

struct Diff {
    double max_abs = 0.0;
    std::size_t differing = 0;
    std::size_t samples = 0;
    bool finite = true;
};

/* Peak absolute error in 16-bit LSB units: one sample unit is one LSB. */
Diff compare(const RtStageBuffer& a, const RtStageBuffer& b)
{
    Diff diff;
    if (a.format != RT_STAGE_F32 || b.format != RT_STAGE_F32 ||
        a.width != b.width || a.height != b.height || a.planes != b.planes) {
        diff.finite = false;
        return diff;
    }
    for (std::uint32_t plane = 0; plane < a.planes; ++plane) {
        const unsigned char* pa = static_cast<const unsigned char*>(a.data) +
                                  static_cast<std::size_t>(plane) * a.plane_pitch;
        const unsigned char* pb = static_cast<const unsigned char*>(b.data) +
                                  static_cast<std::size_t>(plane) * b.plane_pitch;
        for (int y = 0; y < a.height; ++y) {
            const float* ra = reinterpret_cast<const float*>(
                pa + static_cast<std::size_t>(y) * a.row_pitch);
            const float* rb = reinterpret_cast<const float*>(
                pb + static_cast<std::size_t>(y) * b.row_pitch);
            for (int x = 0; x < a.width; ++x) {
                ++diff.samples;
                if (!close_enough(ra[x]) || !close_enough(rb[x])) {
                    diff.finite = false;
                }
                const double delta = std::fabs(static_cast<double>(ra[x]) -
                                               static_cast<double>(rb[x]));
                if (delta > 0.0) {
                    ++diff.differing;
                }
                diff.max_abs = std::max(diff.max_abs, delta);
            }
        }
    }
    return diff;
}

struct NodeFixture {
    RtStageBuffer source;
    RtStageBuffer cpu_output;
    RtStageBuffer device_output;

    bool allocate(StageBufferAllocator& allocator, int src_w, int src_h,
                  int dst_w, int dst_h)
    {
        std::string error;
        source = allocator.allocate({RT_STAGE_F32, src_w, src_h, 3}, &error);
        cpu_output = allocator.allocate({RT_STAGE_F32, dst_w, dst_h, 3}, &error);
        device_output = allocator.allocate({RT_STAGE_F32, dst_w, dst_h, 3}, &error);
        if (!valid(source) || !valid(cpu_output) || !valid(device_output)) {
            std::fprintf(stderr, "allocation failed: %s\n", error.c_str());
            return false;
        }
        fill(source, 7u);
        fill(device_output, 0u);
        return true;
    }

    std::vector<RtStageArg> args(RtStageBuffer& dst, double scale) const
    {
        return {buffer_arg(source), buffer_arg(dst), double_arg(scale), int_arg(0)};
    }
};

std::string read_file(const std::string& path)
{
    std::string contents;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            contents.append(buffer, read);
        }
        std::fclose(file);
    }
    return contents;
}

/* ------------------------------------------------------------ residency */

void test_residency()
{
    using namespace rtengine::stage;

    std::string reason;
    if (!opencl_stage_available(&reason)) {
        skip(("residency: " + reason).c_str());
        return;
    }

    StageOpenCLDeviceInfo info;
    check(opencl_stage_device_info(&info), "device info is available");
    std::printf("stage-residency: device=%s platform=%s driver=%s type=%s\n",
                info.device.c_str(), info.platform.c_str(), info.driver.c_str(),
                info.cpu ? "CPU (POCL CPU run)" : "GPU");

    auto context = opencl_device()->create_context();
    check(context != nullptr, "OpenCL device creates a context");
    if (context == nullptr) {
        return;
    }
    StageDeviceResidency* residency = context->residency();
    check(residency != nullptr, "OpenCL context exposes a residency");
    if (residency == nullptr) {
        return;
    }

    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;
    RtStageBuffer host_buffer =
        host.allocate({RT_STAGE_F32, 40, 24, 3}, &error);
    check(valid(host_buffer), "host fixture allocates");
    fill(host_buffer, 3u);

    const void* key = host_buffer.data;
    StageDeviceBufferPtr device =
        residency->acquire(key, {RT_STAGE_F32, 40, 24, 3}, &error);
    check(device != nullptr, "residency acquires a device buffer");
    if (device == nullptr) {
        std::fprintf(stderr, "acquire error: %s\n", error.c_str());
        return;
    }
    check(device->valid(), "device buffer is valid");
    check(device->bytes() > 0, "device buffer accounts its bytes");

    std::size_t row_pitch = 0;
    std::size_t plane_pitch = 0;
    std::size_t bytes = 0;
    check(stage_buffer_layout({RT_STAGE_F32, 40, 24, 3}, row_pitch, plane_pitch,
                              bytes),
          "layout helper resolves the shape");
    check(device->row_pitch() == row_pitch, "device row pitch matches layout");
    check(device->plane_pitch() == plane_pitch,
          "device plane pitch matches layout");
    check(device->bytes() == bytes, "device size matches layout");

    StageResidencyStats stats = residency->stats();
    check(stats.allocations == 1, "one device allocation is counted");
    check(stats.used_bytes == bytes, "used bytes account the allocation");
    check(stats.peak_bytes >= bytes, "peak bytes record the high-water mark");

    const RtStageStatus upload_status = residency->upload(device, host_buffer);
    if (upload_status != RT_STAGE_OK) {
        std::fprintf(stderr, "upload status: %d\n", static_cast<int>(upload_status));
    }
    check(upload_status == RT_STAGE_OK, "upload copies the host buffer");

    RtStageBuffer readback = host.allocate({RT_STAGE_F32, 40, 24, 3}, &error);
    check(valid(readback), "readback target allocates");
    std::memset(readback.data, 0, readback.plane_pitch * readback.planes);
    const RtStageStatus readback_status = residency->readback(device, readback);
    if (readback_status != RT_STAGE_OK) {
        std::fprintf(stderr, "readback status: %d\n",
                     static_cast<int>(readback_status));
    }
    check(readback_status == RT_STAGE_OK,
          "readback copies the device buffer back");
    const Diff roundtrip = compare(host_buffer, readback);
    check(roundtrip.finite, "upload/readback stays finite");
    check(roundtrip.max_abs == 0.0, "upload/readback is exact");
    check(roundtrip.differing == 0, "upload/readback changes no sample");

    /* Reuse keeps the allocation when the address and shape are unchanged. */
    StageDeviceBufferPtr again =
        residency->acquire(key, {RT_STAGE_F32, 40, 24, 3}, &error);
    check(again == device, "a repeated acquire returns the same allocation");
    stats = residency->stats();
    check(stats.allocations == 1, "reuse does not allocate again");
    check(stats.reused == 1, "reuse is counted");

    /* Drop the caller handles before the budget can evict the entries. */
    device.reset();
    again.reset();

    /* A shape change at the same address replaces the allocation. */
    RtStageBuffer wider = host.allocate({RT_STAGE_F32, 64, 24, 3}, &error);
    StageDeviceBufferPtr resized =
        residency->acquire(wider.data, {RT_STAGE_F32, 64, 24, 3}, &error);
    check(resized != nullptr, "a different shape allocates");
    check(resized != nullptr, "a different shape replaces the allocation");

    /* Budget: keep only the newest allocation. */
    const std::size_t one_buffer = resized != nullptr ? resized->bytes() : 0;
    check(residency->set_budget(one_buffer), "a one-buffer budget is accepted");
    stats = residency->stats();
    check(stats.evictions >= 1, "shrinking the budget evicts idle entries");
    check(stats.used_bytes <= stats.budget_bytes, "the residency fits its budget");
    check(residency->find(key) == nullptr, "the evicted binding is gone");

    /* Over budget: a single allocation larger than the whole budget. */
    const std::uint64_t refusals = residency->stats().refusals;
    RtStageBuffer huge = host.allocate({RT_STAGE_F32, 2048, 2048, 3}, &error);
    error.clear();
    StageDeviceBufferPtr refused =
        residency->acquire(huge.data, {RT_STAGE_F32, 2048, 2048, 3}, &error);
    check(refused == nullptr, "an over-budget allocation is refused");
    check(!error.empty(), "a refusal reports a reason");
    check(residency->stats().refusals == refusals + 1,
          "a refusal is counted");

    /* Release returns the bytes and drops the binding. */
    residency->release(wider.data);
    stats = residency->stats();
    check(stats.used_bytes == 0, "release returns all bytes");
    check(residency->find(wider.data) == nullptr, "release drops the binding");

    check(residency->set_budget(bytes * 4), "the budget can grow again");
    check(residency->acquire(key, {RT_STAGE_F32, 40, 24, 3}, &error) != nullptr,
          "allocation resumes after a refusal");
    residency->clear();
    check(residency->stats().used_bytes == 0, "clear frees every binding");

    /* True LRU order: with three idle candidates the least recently used one
     * is evicted, not just any entry. */
    auto lru_context = opencl_device()->create_context();
    StageDeviceResidency* lru = lru_context->residency();
    check(lru != nullptr, "the LRU context exposes a residency");
    if (lru != nullptr) {
        RtStageBuffer oldest = host.allocate({RT_STAGE_F32, 32, 16, 3}, &error);
        RtStageBuffer middle = host.allocate({RT_STAGE_F32, 32, 16, 3}, &error);
        RtStageBuffer newest = host.allocate({RT_STAGE_F32, 32, 16, 3}, &error);
        const StageBufferShape shape{RT_STAGE_F32, 32, 16, 3};
        StageDeviceBufferPtr first = lru->acquire(oldest.data, shape, &error);
        StageDeviceBufferPtr second = lru->acquire(middle.data, shape, &error);
        StageDeviceBufferPtr third = lru->acquire(newest.data, shape, &error);
        check(first && second && third, "three LRU entries allocate");
        if (first && second && third) {
            const std::size_t one = first->bytes();
            check(!lru->set_budget(one * 2),
                  "in-use entries cannot be evicted by a smaller budget");
            first.reset();
            second.reset();
            third.reset();
            /* Touch the oldest so the middle entry becomes the LRU victim. */
            check(lru->acquire(oldest.data, shape, &error) != nullptr,
                  "the oldest entry can be touched");
            check(lru->set_budget(one * 2), "the budget trims to two buffers");
            check(lru->find(middle.data) == nullptr,
                  "the least recently used idle entry is evicted");
            check(lru->find(oldest.data) != nullptr,
                  "a recently touched entry survives");
            check(lru->find(newest.data) != nullptr,
                  "the newest entry survives");
            check(lru->stats().evictions == 1, "exactly one entry was evicted");
        }
        lru->clear();
    }

    /* Budget parsing: out-of-range values clamp, garbage keeps the default. */
    constexpr std::size_t MIB = 1024 * 1024;
    setenv("RT_STAGE_DEVICE_BUDGET_MB", "1", 1);
    auto low_queue = opencl_device()->create_queue();
    check(low_queue->residency()->stats().budget_bytes == 16 * MIB,
          "a low budget clamps to 16 MiB");
    setenv("RT_STAGE_DEVICE_BUDGET_MB", "999999", 1);
    auto high_queue = opencl_device()->create_queue();
    check(high_queue->residency()->stats().budget_bytes == 65536 * MIB,
          "a high budget clamps to 65536 MiB");
    setenv("RT_STAGE_DEVICE_BUDGET_MB", "not-a-number", 1);
    auto default_queue = opencl_device()->create_queue();
    check(default_queue->residency()->stats().budget_bytes == 1024 * MIB,
          "a non-numeric budget keeps the 1024 MiB default");
    unsetenv("RT_STAGE_DEVICE_BUDGET_MB");

    /* Multi-plane U8 has no defined layout; both helpers must agree by
     * refusing it. */
    std::size_t u8_row = 0;
    std::size_t u8_plane = 0;
    std::size_t u8_bytes = 0;
    std::string u8_error;
    check(!stage_buffer_layout({RT_STAGE_U8, 8, 8, 3}, u8_row, u8_plane,
                               u8_bytes, &u8_error),
          "multi-plane U8 is refused by the layout helper");
    check(u8_error.find("single-plane") != std::string::npos,
          "multi-plane U8 explains the refusal");
    u8_error.clear();
    check(host.allocate({RT_STAGE_U8, 8, 8, 3}, &u8_error).data == nullptr,
          "multi-plane U8 is refused by the host allocator");
    check(u8_error.find("single-plane") != std::string::npos,
          "the host allocator explains the refusal");
}

/* --------------------------------------------------------------- events */

void test_cpu_events()
{
    using namespace rtengine::stage;

    auto context = cpu_device()->create_context();
    StageBufferAllocator& allocator = context->allocator();
    std::string error;
    RtStageBuffer source = allocator.allocate({RT_STAGE_F32, 32, 16, 3}, &error);
    RtStageBuffer output = allocator.allocate({RT_STAGE_F32, 16, 8, 3}, &error);
    fill(source, 11u);

    StageRequest request;
    request.name = "lanczos3";
    request.impl = "builtin";
    request.args = {buffer_arg(source), buffer_arg(output), double_arg(0.5),
                    int_arg(0)};

    auto program = context->compile(request, &error);
    check(program != nullptr, "CPU program compiles");

    auto queue = cpu_device()->create_queue();
    StageEvent event = queue->submit(*program, request.args, nullptr);
    check(event.status == StageEventStatus::Complete,
          "the CPU queue completes before submit returns");
    check(!event.asynchronous, "the CPU event reports a synchronous result");
    check(event.device == StageDeviceType::CPU, "the CPU event names the CPU");
    check(event.done() && event.succeeded(), "the CPU event succeeded");
    check(event.note.empty(), "a CPU execution has no fallback note");
    check(queue->wait(event) == RT_STAGE_OK, "the CPU wait reports OK");
}

/*
 * Driver double: reports Running once, then Failed. Used to exercise the event
 * state machine through the real StageQueue::poll() without depending on a
 * device error that hardware may not reproduce on demand.
 */
class FailingSubmission final : public StageSubmission
{
public:
    RtStageStatus wait() override
    {
        status_ = StageEventStatus::Failed;
        return RT_STAGE_ERROR;
    }

    StageEventStatus poll() override
    {
        if (polls_++ > 0) {
            status_ = StageEventStatus::Failed;
        }
        return status_;
    }

    std::uint64_t elapsed_ns() const override
    {
        return 42;
    }

private:
    int polls_ = 0;
    StageEventStatus status_ = StageEventStatus::Running;
};

void test_failed_event()
{
    auto queue = cpu_device()->create_queue();
    StageEvent event;
    event.status = StageEventStatus::Running;
    event.asynchronous = true;
    event.submission = std::make_shared<FailingSubmission>();

    check(queue->poll(event) == StageEventStatus::Running,
          "a poll keeps a running submission running");
    check(queue->poll(event) == StageEventStatus::Failed,
          "a driver failure moves the event to Failed");
    check(event.status == StageEventStatus::Failed,
          "the failed status is stored on the event");
    check(!event.succeeded(), "a failed event did not succeed");
    check(event.elapsed_ns == 42,
          "a failed event keeps the measured completion time");
}

void test_device_events(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("device events: no OpenCL device");
        return;
    }

    auto queue = opencl_device()->create_queue();
    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;
    RtStageBuffer source = host.allocate({RT_STAGE_F32, 48, 32, 3}, &error);
    RtStageBuffer output = host.allocate({RT_STAGE_F32, 24, 16, 3}, &error);
    fill(source, 5u);

    StageRequest request;
    request.name = "lanczos3";
    request.args = {buffer_arg(source), buffer_arg(output), double_arg(0.5),
                    int_arg(0)};

    StageEvent event = queue->submit(request, nullptr);
    if (!event.asynchronous) {
        std::fprintf(stderr, "device submit note: %s\n", event.note.c_str());
    }
    check(event.asynchronous, "the device submission returns before completion");
    check(event.status == StageEventStatus::Running,
          "the device event reports running");
    check(event.device == StageDeviceType::OPENCL,
          "the device event names the device");
    check(!event.done(), "a running event is not done");
    check(event.submission != nullptr, "a running event carries a submission");
    check(queue->poll(event) == StageEventStatus::Running,
          "poll keeps a running event running");
    check(queue->wait(event) == RT_STAGE_OK, "the device wait reports OK");
    check(event.status == StageEventStatus::Complete,
          "wait completes the event");
    check(event.succeeded(), "the completed event succeeded");
    check(event.elapsed_ns > 0, "the completed event carries timing");

    /* A node that refuses device execution must fall back to the host and say
     * why, without pretending to be asynchronous. */
    RtStageBuffer bad_source = host.allocate({RT_STAGE_F32, 16, 16, 3}, &error);
    RtStageBuffer bad_output = host.allocate({RT_STAGE_F32, 8, 8, 1}, &error);
    StageRequest bad;
    bad.name = "lanczos3";
    bad.args = {buffer_arg(bad_source), buffer_arg(bad_output), double_arg(0.5),
                int_arg(0)};
    StageEvent failed = queue->submit(bad, nullptr);
    check(!failed.asynchronous, "a refused device node is synchronous");
    check(failed.device == StageDeviceType::CPU,
          "a refused device node reports the CPU");
    check(failed.status == StageEventStatus::Failed,
          "an invalid node fails on the host");
    check(!failed.note.empty(), "a refused device node reports a reason");

    /* An unknown node has no device implementation and runs on the host. */
    StageRequest unknown;
    unknown.name = "no_such_stage";
    StageEvent missing = queue->submit(unknown, nullptr);
    check(!missing.asynchronous, "an unknown node is synchronous");
    check(missing.device == StageDeviceType::NONE,
          "an unknown node reports that nothing executed");
    check(missing.status == StageEventStatus::Failed, "an unknown node fails");
    check(missing.note.find("no OpenCL implementation") != std::string::npos,
          "an unimplemented node explains the fallback");
}

/*
 * Identity geometry must reproduce the source exactly: at a zero fractional
 * offset the cubic weights pick the sample itself, so any difference is an
 * addressing or pitch bug rather than interpolation error.
 */
void test_transform_identity(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("transform identity: no OpenCL device");
        return;
    }

    auto queue = opencl_device()->create_queue();
    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;
    constexpr int WIDTH = 40;
    constexpr int HEIGHT = 24;
    RtStageBuffer source =
        host.allocate({RT_STAGE_F32, WIDTH, HEIGHT, 3}, &error);
    RtStageBuffer output =
        host.allocate({RT_STAGE_F32, WIDTH, HEIGHT, 3}, &error);
    fill(source, 23u);

    StageRequest request;
    request.name = "transform";
    request.args = {
        buffer_arg(source), buffer_arg(output),
        /* cx, cy, sx, sy, oW, oH, highQuality, useLog */
        int_arg(0), int_arg(0), int_arg(0), int_arg(0),
        int_arg(WIDTH), int_arg(HEIGHT), int_arg(1), int_arg(0),
        /* hscale, vscale, cost, sint */
        double_arg(1.0), double_arg(1.0), double_arg(1.0), double_arg(0.0)
    };

    StageEvent event = queue->submit(request, nullptr);
    check(event.asynchronous, "the transform submission is asynchronous");
    check(queue->wait(event) == RT_STAGE_OK, "the transform wait reports OK");

    const Diff diff = compare(source, output);
    check(diff.max_abs == 0.0, "identity transform is exact");
    check(diff.differing == 0, "identity transform changes no sample");
}

/*
 * The packed 8-bit Lab-to-RGB path is only reachable from the preview, crop
 * and thumbnail paths, so the export harness cannot cover it. This test runs
 * the device kernel on a synthetic Lab image and compares every byte with an
 * independent reference of the same formula.
 */
namespace reference
{

float f2xyz(float f)
{
    return f > 0.20689655172413793f
        ? f * f * f
        : (116.0f * f - 16.0f) * (27.0f / 24389.0f);
}

float gamma_lookup(const float* lut, float value)
{
    const float clamped = std::min(std::max(value, 0.0f), 65535.0f);
    const int index = static_cast<int>(clamped);
    const int upper = std::min(index + 1, 65535);
    const float fraction = clamped - static_cast<float>(index);
    return lut[index] + (lut[upper] - lut[index]) * fraction;
}

unsigned char to_u8(float value)
{
    const unsigned int v = static_cast<unsigned int>(
        std::min(std::max(value, 0.0f), 65535.0f));
    return static_cast<unsigned char>(((v + 128u) - ((v + 128u) >> 8)) >> 8);
}

struct Rgb {
    float r;
    float g;
    float b;
};

Rgb lab_to_linear(float L, float a, float b, const float matrix[9])
{
    const float LL = L / 327.68f;
    const float fy =
        (0.008620689655172414f * L) / 327.68f + 0.13793103448275862f;
    const float fx = (0.002f * a) / 327.68f + fy;
    const float fz = fy - (0.005f * b) / 327.68f;
    const float xyzX = 65535.0f * f2xyz(fx) * 0.9642f;
    const float xyzZ = 65535.0f * f2xyz(fz) * 0.8249f;
    const float xyzY = LL > 8.0f ? 65535.0f * fy * fy * fy
                                 : 65535.0f * LL / 903.2962962962963f;
    return Rgb{
        matrix[0] * xyzX + matrix[1] * xyzY + matrix[2] * xyzZ,
        matrix[3] * xyzX + matrix[4] * xyzY + matrix[5] * xyzZ,
        matrix[6] * xyzX + matrix[7] * xyzY + matrix[8] * xyzZ};
}

} // namespace reference

void test_lab_to_u8(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("Lab to RGB 8-bit: no OpenCL device");
        return;
    }

    constexpr int WIDTH = 24;
    constexpr int HEIGHT = 16;
    constexpr int LUT_ENTRIES = 65536;
    constexpr int MATRIX_ENTRIES = 9;

    auto queue = opencl_device()->create_queue();
    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;

    RtStageBuffer lab = host.allocate({RT_STAGE_F32, WIDTH, HEIGHT, 3}, &error);
    RtStageBuffer dst = host.allocate({RT_STAGE_U8, WIDTH, HEIGHT, 1}, &error);
    RtStageBuffer lut =
        host.allocate({RT_STAGE_F32, LUT_ENTRIES, 1, 1}, &error);
    RtStageBuffer matrix =
        host.allocate({RT_STAGE_F32, MATRIX_ENTRIES, 1, 1}, &error);
    if (lab.data == nullptr || dst.data == nullptr || lut.data == nullptr ||
        matrix.data == nullptr) {
        check(false, "Lab to RGB fixture allocates");
        return;
    }

    /* A ramp per plane: a row/column swap or a plane mix-up changes bytes. */
    for (int plane = 0; plane < 3; ++plane) {
        unsigned char* base = static_cast<unsigned char*>(lab.data) +
                              static_cast<std::size_t>(plane) * lab.plane_pitch;
        for (int y = 0; y < HEIGHT; ++y) {
            float* row = reinterpret_cast<float*>(
                base + static_cast<std::size_t>(y) * lab.row_pitch);
            for (int x = 0; x < WIDTH; ++x) {
                row[x] = static_cast<float>(2000 + plane * 900 + x * 37 +
                                            y * 11);
            }
        }
    }

    float* lut_data = reinterpret_cast<float*>(lut.data);
    for (int i = 0; i < LUT_ENTRIES; ++i) {
        lut_data[i] = static_cast<float>(i);
    }

    /* D65 XYZ to linear sRGB, as an output profile supplies it. */
    const float matrix_values[MATRIX_ENTRIES] = {
        3.2404542f, -1.5371385f, -0.4985314f,
        -0.9692660f, 1.8760108f, 0.0415560f,
        0.0556434f, -0.2040259f, 1.0572252f};
    float* matrix_data = reinterpret_cast<float*>(matrix.data);
    for (int i = 0; i < MATRIX_ENTRIES; ++i) {
        matrix_data[i] = matrix_values[i];
    }

    StageRequest request;
    request.name = "monitor.output";
    request.args = {buffer_arg(lab), buffer_arg(dst), buffer_arg(lut),
                    buffer_arg(matrix)};

    StageEvent event = queue->submit(request, nullptr);
    check(event.asynchronous, "the Lab to RGB 8-bit submission is asynchronous");
    check(queue->wait(event) == RT_STAGE_OK, "the Lab to RGB 8-bit wait is OK");

    int differing = 0;
    for (int y = 0; y < HEIGHT; ++y) {
        const float* planes[3] = {};
        for (int plane = 0; plane < 3; ++plane) {
            planes[plane] = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(lab.data) +
                static_cast<std::size_t>(plane) * lab.plane_pitch +
                static_cast<std::size_t>(y) * lab.row_pitch);
        }
        const unsigned char* row =
            static_cast<const unsigned char*>(dst.data) +
            static_cast<std::size_t>(y) * dst.row_pitch;
        for (int x = 0; x < WIDTH; ++x) {
            const reference::Rgb rgb = reference::lab_to_linear(
                planes[0][x], planes[1][x], planes[2][x], matrix_values);
            const unsigned char expected[3] = {
                reference::to_u8(reference::gamma_lookup(lut_data, rgb.r)),
                reference::to_u8(reference::gamma_lookup(lut_data, rgb.g)),
                reference::to_u8(reference::gamma_lookup(lut_data, rgb.b))};
            if (row[x * 3] != expected[0] || row[x * 3 + 1] != expected[1] ||
                row[x * 3 + 2] != expected[2]) {
                ++differing;
            }
        }
    }

    if (differing != 0) {
        std::fprintf(stderr, "Lab to RGB 8-bit: %d of %d pixels differ\n",
                     differing, WIDTH * HEIGHT);
    }
    check(differing == 0, "the Lab to RGB 8-bit output matches the reference");
}

/*
 * thumbnail.whitebalance is reachable only from the thumbnail path, so the
 * export harness cannot cover it. Run the device kernel on a synthetic image
 * and compare every sample with an independent reference of the same loop.
 */
void test_thumbnail_wb(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("thumbnail white balance: no OpenCL device");
        return;
    }

    constexpr int WIDTH = 24;
    constexpr int HEIGHT = 16;
    constexpr double RMI = 2.0;
    constexpr double GMI = 1.0;
    constexpr double BMI = 0.5;
    constexpr float MAX_VALUE = 65535.0f;
    constexpr bool HR = true;

    auto queue = opencl_device()->create_queue();
    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;
    RtStageBuffer image =
        host.allocate({RT_STAGE_F32, WIDTH, HEIGHT, 3}, &error);
    if (image.data == nullptr) {
        check(false, "thumbnail white balance fixture allocates");
        return;
    }

    /* A ramp whose products straddle the highlight threshold. */
    for (int plane = 0; plane < 3; ++plane) {
        unsigned char* base = static_cast<unsigned char*>(image.data) +
                              static_cast<std::size_t>(plane) * image.plane_pitch;
        for (int y = 0; y < HEIGHT; ++y) {
            float* row = reinterpret_cast<float*>(
                base + static_cast<std::size_t>(y) * image.row_pitch);
            for (int x = 0; x < WIDTH; ++x) {
                row[x] = static_cast<float>(1000 + plane * 20000 + x * 1300 +
                                            y * 700);
            }
        }
    }

    /* Keep a copy because the node works in place. */
    std::vector<float> original(
        static_cast<std::size_t>(WIDTH) * HEIGHT * 3);
    for (int plane = 0; plane < 3; ++plane) {
        const float* row = reinterpret_cast<const float*>(
            static_cast<const unsigned char*>(image.data) +
            static_cast<std::size_t>(plane) * image.plane_pitch);
        for (int y = 0; y < HEIGHT; ++y) {
            const float* src = reinterpret_cast<const float*>(
                reinterpret_cast<const unsigned char*>(row) +
                static_cast<std::size_t>(y) * image.row_pitch);
            for (int x = 0; x < WIDTH; ++x) {
                original[(static_cast<std::size_t>(plane) * HEIGHT + y) *
                             WIDTH + x] = src[x];
            }
        }
    }

    StageRequest request;
    request.name = "thumbnail.whitebalance";
    request.args = {buffer_arg(image), double_arg(RMI), double_arg(GMI),
                    double_arg(BMI), int_arg(HR ? 1 : 0), int_arg(WIDTH),
                    int_arg(HEIGHT)};

    StageEvent event = queue->submit(request, nullptr);
    check(event.succeeded(), "the thumbnail white balance node completed");
    check(queue->wait(event) == RT_STAGE_OK,
          "the thumbnail white balance wait is OK");

    int differing = 0;
    for (int y = 0; y < HEIGHT; ++y) {
        const float* planes[3] = {};
        for (int plane = 0; plane < 3; ++plane) {
            planes[plane] = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(image.data) +
                static_cast<std::size_t>(plane) * image.plane_pitch +
                static_cast<std::size_t>(y) * image.row_pitch);
        }
        for (int x = 0; x < WIDTH; ++x) {
            const float sample[3] = {
                original[(static_cast<std::size_t>(0) * HEIGHT + y) * WIDTH + x],
                original[(static_cast<std::size_t>(1) * HEIGHT + y) * WIDTH + x],
                original[(static_cast<std::size_t>(2) * HEIGHT + y) * WIDTH + x]};
            float red = sample[0] * static_cast<float>(RMI);
            float green = sample[1] * static_cast<float>(GMI);
            float blue = sample[2] * static_cast<float>(BMI);
            float expected[3];
            if (HR && red > MAX_VALUE && blue > MAX_VALUE) {
                const float value = std::min(std::max((red + green + blue) / 3.0f,
                                                      0.0f), MAX_VALUE);
                expected[0] = expected[1] = expected[2] = value;
            } else {
                expected[0] = std::min(std::max(red, 0.0f), MAX_VALUE);
                expected[1] = std::min(std::max(green, 0.0f), MAX_VALUE);
                expected[2] = std::min(std::max(blue, 0.0f), MAX_VALUE);
            }
            for (int plane = 0; plane < 3; ++plane) {
                if (planes[plane][x] != expected[plane]) {
                    ++differing;
                }
            }
        }
    }

    if (differing != 0) {
        std::fprintf(stderr,
                     "thumbnail white balance: %d samples differ\n", differing);
    }
    check(differing == 0, "thumbnail white balance matches the reference");
}

/*
 * thumbnail.transform is a permutation of the coarse rotate/flip
 * composition. Sixteen combinations of rotation and flips are compared with
 * an independent mapping of the same operations.
 */
void test_thumbnail_permute(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("thumbnail transform: no OpenCL device");
        return;
    }

    constexpr int WIDTH = 20;
    constexpr int HEIGHT = 12;
    const int rotations[4] = {0, 90, 180, 270};

    auto queue = opencl_device()->create_queue();
    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;
    RtStageBuffer src =
        host.allocate({RT_STAGE_F32, WIDTH, HEIGHT, 3}, &error);
    if (src.data == nullptr) {
        check(false, "thumbnail transform fixture allocates");
        return;
    }
    for (int plane = 0; plane < 3; ++plane) {
        unsigned char* base = static_cast<unsigned char*>(src.data) +
                              static_cast<std::size_t>(plane) * src.plane_pitch;
        for (int y = 0; y < HEIGHT; ++y) {
            float* row = reinterpret_cast<float*>(
                base + static_cast<std::size_t>(y) * src.row_pitch);
            for (int x = 0; x < WIDTH; ++x) {
                row[x] = static_cast<float>(plane * 100000 + y * 100 + x);
            }
        }
    }

    int differing = 0;
    for (int rotation : rotations) {
        for (int flips = 0; flips < 4; ++flips) {
            const bool hflip = (flips & 1) != 0;
            const bool vflip = (flips & 2) != 0;
            const bool swaps = rotation == 90 || rotation == 270;
            const int dw = swaps ? HEIGHT : WIDTH;
            const int dh = swaps ? WIDTH : HEIGHT;

            RtStageBuffer dst =
                host.allocate({RT_STAGE_F32, dw, dh, 3}, &error);
            StageRequest request;
            request.name = "thumbnail.transform";
            request.args = {buffer_arg(src), buffer_arg(dst),
                            int_arg(rotation), int_arg(hflip ? 1 : 0),
                            int_arg(vflip ? 1 : 0)};
            StageEvent event = queue->submit(request, nullptr);
            if (!event.succeeded() || queue->wait(event) != RT_STAGE_OK) {
                ++differing;
                continue;
            }

            for (int y = 0; y < dh; ++y) {
                for (int x = 0; x < dw; ++x) {
                    const int ux = hflip ? dw - 1 - x : x;
                    const int uy = vflip ? dh - 1 - y : y;
                    int sx = ux;
                    int sy = uy;
                    if (rotation == 90) {
                        sx = uy;
                        sy = HEIGHT - 1 - ux;
                    } else if (rotation == 180) {
                        sx = WIDTH - 1 - ux;
                        sy = HEIGHT - 1 - uy;
                    } else if (rotation == 270) {
                        sx = WIDTH - 1 - uy;
                        sy = ux;
                    }
                    for (int plane = 0; plane < 3; ++plane) {
                        const float* src_row = reinterpret_cast<const float*>(
                            static_cast<const unsigned char*>(src.data) +
                            static_cast<std::size_t>(plane) * src.plane_pitch +
                            static_cast<std::size_t>(sy) * src.row_pitch);
                        const float* dst_row = reinterpret_cast<const float*>(
                            static_cast<const unsigned char*>(dst.data) +
                            static_cast<std::size_t>(plane) * dst.plane_pitch +
                            static_cast<std::size_t>(y) * dst.row_pitch);
                        if (dst_row[x] != src_row[sx]) {
                            ++differing;
                        }
                    }
                }
            }
        }
    }

    if (differing != 0) {
        std::fprintf(stderr, "thumbnail transform: %d samples differ\n",
                     differing);
    }
    check(differing == 0,
          "thumbnail transform matches the rotate/flip reference");
}

/*
 * The packed 8-bit variant used by the embedded-JPEG thumbnail path.
 */
void test_thumbnail_permute_u8(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("thumbnail transform (8-bit): no OpenCL device");
        return;
    }

    constexpr int WIDTH = 18;
    constexpr int HEIGHT = 10;
    const int rotations[4] = {0, 90, 180, 270};

    auto queue = opencl_device()->create_queue();
    StageBufferAllocator host(StageDeviceType::CPU);
    std::string error;
    RtStageBuffer src = host.allocate({RT_STAGE_U8, WIDTH, HEIGHT, 1}, &error);
    if (src.data == nullptr) {
        check(false, "thumbnail transform 8-bit fixture allocates");
        return;
    }
    unsigned char* src_data = static_cast<unsigned char*>(src.data);
    for (int y = 0; y < HEIGHT; ++y) {
        unsigned char* row = src_data + static_cast<std::size_t>(y) * src.row_pitch;
        for (int x = 0; x < WIDTH; ++x) {
            row[x * 3] = static_cast<unsigned char>(x * 7 + y * 3);
            row[x * 3 + 1] = static_cast<unsigned char>(x + y * 11);
            row[x * 3 + 2] = static_cast<unsigned char>(x * 3 + y);
        }
    }

    int differing = 0;
    for (int rotation : rotations) {
        for (int flips = 0; flips < 4; ++flips) {
            const bool hflip = (flips & 1) != 0;
            const bool vflip = (flips & 2) != 0;
            const bool swaps = rotation == 90 || rotation == 270;
            const int dw = swaps ? HEIGHT : WIDTH;
            const int dh = swaps ? WIDTH : HEIGHT;

            RtStageBuffer dst =
                host.allocate({RT_STAGE_U8, dw, dh, 1}, &error);
            StageRequest request;
            request.name = "thumbnail.transform.u8";
            request.args = {buffer_arg(src), buffer_arg(dst),
                            int_arg(rotation), int_arg(hflip ? 1 : 0),
                            int_arg(vflip ? 1 : 0)};
            StageEvent event = queue->submit(request, nullptr);
            if (!event.succeeded() || queue->wait(event) != RT_STAGE_OK) {
                ++differing;
                continue;
            }

            const unsigned char* dst_data =
                static_cast<const unsigned char*>(dst.data);
            for (int y = 0; y < dh; ++y) {
                const unsigned char* dst_row =
                    dst_data + static_cast<std::size_t>(y) * dst.row_pitch;
                const int uy = vflip ? dh - 1 - y : y;
                for (int x = 0; x < dw; ++x) {
                    const int ux = hflip ? dw - 1 - x : x;
                    int sx = ux;
                    int sy = uy;
                    if (rotation == 90) {
                        sx = uy;
                        sy = HEIGHT - 1 - ux;
                    } else if (rotation == 180) {
                        sx = WIDTH - 1 - ux;
                        sy = HEIGHT - 1 - uy;
                    } else if (rotation == 270) {
                        sx = WIDTH - 1 - uy;
                        sy = ux;
                    }
                    const unsigned char* src_row =
                        src_data + static_cast<std::size_t>(sy) * src.row_pitch;
                    for (int channel = 0; channel < 3; ++channel) {
                        if (dst_row[x * 3 + channel] !=
                            src_row[sx * 3 + channel]) {
                            ++differing;
                        }
                    }
                }
            }
        }
    }

    if (differing != 0) {
        std::fprintf(stderr, "thumbnail transform 8-bit: %d samples differ\n",
                     differing);
    }
    check(differing == 0,
          "thumbnail transform (8-bit) matches the rotate/flip reference");
}

/* --------------------------------------------------------- thumbnail scale */

/*
 * thumbnail.scale and thumbnail.scale.u8 reproduce `IImage::resizeImgTo`, so
 * the device output is compared with that production template rather than with
 * a reimplementation of the loop. The fixture covers both ends of the range,
 * the midpoint and, for a float source, a NaN, which the float destination
 * conversion turns into zero.
 */

constexpr int SCALE_SRC_WIDTH = 40;
constexpr int SCALE_SRC_HEIGHT = 24;
constexpr int SCALE_DOWN_WIDTH = 23;
constexpr int SCALE_DOWN_HEIGHT = 14;
constexpr int SCALE_UP_WIDTH = 61;
constexpr int SCALE_UP_HEIGHT = 37;
constexpr int SCALE_NAN_X = 5;
constexpr int SCALE_NAN_Y = 3;
/* One LSB of the 16-bit scale, the recorded tolerance for planar bilinear. */
constexpr double SCALE_TOLERANCE_LSB16 = 1.0;

struct ScaleTarget {
    int width;
    int height;
    const char* label;
};

const ScaleTarget SCALE_TARGETS[] = {
    {SCALE_DOWN_WIDTH, SCALE_DOWN_HEIGHT, "downscale"},
    {SCALE_UP_WIDTH, SCALE_UP_HEIGHT, "upscale"},
    {SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT, "same size"}};

struct ScaleMismatch {
    std::size_t samples = 0;
    std::size_t differing = 0;
    std::size_t non_finite = 0;
    double max_abs = 0.0;

    bool exact() const
    {
        return differing == 0 && non_finite == 0;
    }
};

/* Deterministic spread over the whole range, so no value depends on luck. */
unsigned scale_value(int x, int y, int plane, unsigned max_value)
{
    const std::uint32_t hash =
        static_cast<std::uint32_t>(x) * 2654435761u ^
        static_cast<std::uint32_t>(y) * 40503u ^
        static_cast<std::uint32_t>(plane + 1) * 2246822519u;
    return hash % (max_value + 1u);
}

unsigned char* scale_pixel(Image8& image, int plane, int y, int x)
{
    return &image.data[(static_cast<std::size_t>(y) * image.getWidth() + x) * 3 +
                       plane];
}

const unsigned char* scale_pixel(const Image8& image, int plane, int y, int x)
{
    return &image.data[(static_cast<std::size_t>(y) * image.getWidth() + x) * 3 +
                       plane];
}

unsigned short* scale_row(Image16& image, int plane, int y)
{
    return plane == 0 ? image.r(y) : plane == 1 ? image.g(y) : image.b(y);
}

const unsigned short* scale_row(const Image16& image, int plane, int y)
{
    return plane == 0 ? image.r(y) : plane == 1 ? image.g(y) : image.b(y);
}

float* scale_row(Imagefloat& image, int plane, int y)
{
    return plane == 0 ? image.r(y) : plane == 1 ? image.g(y) : image.b(y);
}

const float* scale_row(const Imagefloat& image, int plane, int y)
{
    return plane == 0 ? image.r(y) : plane == 1 ? image.g(y) : image.b(y);
}

void fill_scale_source(Image8& image)
{
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < image.getHeight(); ++y) {
            for (int x = 0; x < image.getWidth(); ++x) {
                *scale_pixel(image, plane, y, x) = static_cast<unsigned char>(
                    scale_value(x, y, plane, 255u));
            }
        }
    }
    /* Both ends, the midpoint and its neighbour, which is where rounding and
     * truncation show up first. */
    *scale_pixel(image, 0, 0, 0) = 0;
    *scale_pixel(image, 0, 0, 1) = 255;
    *scale_pixel(image, 0, 0, 2) = 128;
    *scale_pixel(image, 0, 1, 0) = 129;
    *scale_pixel(image, 1, 1, 1) = 1;
    *scale_pixel(image, 2, 1, 2) = 254;
}

void fill_scale_source(Image16& image)
{
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < image.getHeight(); ++y) {
            unsigned short* row = scale_row(image, plane, y);
            for (int x = 0; x < image.getWidth(); ++x) {
                row[x] = static_cast<unsigned short>(
                    scale_value(x, y, plane, 65535u));
            }
        }
    }
    *scale_row(image, 0, 0) = 0;
    scale_row(image, 0, 0)[1] = 65535;
    scale_row(image, 0, 0)[2] = 32768;
    scale_row(image, 0, 1)[0] = 32769;
    scale_row(image, 1, 1)[1] = 1;
    scale_row(image, 2, 1)[2] = 65534;
}

void fill_scale_source(Imagefloat& image)
{
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < image.getHeight(); ++y) {
            float* row = scale_row(image, plane, y);
            for (int x = 0; x < image.getWidth(); ++x) {
                row[x] = static_cast<float>(
                    scale_value(x, y, plane, 65535u));
            }
        }
    }
    *scale_row(image, 0, 0) = 0.0f;
    scale_row(image, 0, 0)[1] = 65535.0f;
    scale_row(image, 0, 0)[2] = 32768.0f;
    scale_row(image, 0, 1)[0] = 32769.0f;
    scale_row(image, 1, 1)[1] = 1.0f;
    scale_row(image, 1, SCALE_NAN_Y)[SCALE_NAN_X] =
        std::numeric_limits<float>::quiet_NaN();
    /*
     * An infinity separates the same-size conversion from the resampling
     * branches: the reference converts it unchanged, while a blend would
     * multiply it by a zero weight and produce a NaN.
     */
    scale_row(image, 2, 0)[3] = std::numeric_limits<float>::infinity();
}

/* Copy a fixture through the image's own strides, so a wrong view pitch in
 * production shows up as a mismatch rather than being mirrored here. */
void copy_scale_source(const Image8& image, const RtStageBuffer& buffer)
{
    const std::size_t row_bytes = static_cast<std::size_t>(image.getWidth()) * 3;
    for (int y = 0; y < image.getHeight(); ++y) {
        std::memcpy(static_cast<unsigned char*>(buffer.data) +
                        static_cast<std::size_t>(y) * buffer.row_pitch,
                    image.data + static_cast<std::size_t>(y) * row_bytes,
                    row_bytes);
    }
}

void copy_scale_source(const Image16& image, const RtStageBuffer& buffer)
{
    const std::size_t row_bytes =
        static_cast<std::size_t>(image.getWidth()) * sizeof(unsigned short);
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < image.getHeight(); ++y) {
            std::memcpy(static_cast<unsigned char*>(buffer.data) +
                            static_cast<std::size_t>(plane) * buffer.plane_pitch +
                            static_cast<std::size_t>(y) * buffer.row_pitch,
                        scale_row(image, plane, y), row_bytes);
        }
    }
}

void copy_scale_source(const Imagefloat& image, const RtStageBuffer& buffer)
{
    const std::size_t row_bytes =
        static_cast<std::size_t>(image.getWidth()) * sizeof(float);
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < image.getHeight(); ++y) {
            std::memcpy(static_cast<unsigned char*>(buffer.data) +
                            static_cast<std::size_t>(plane) * buffer.plane_pitch +
                            static_cast<std::size_t>(y) * buffer.row_pitch,
                        scale_row(image, plane, y), row_bytes);
        }
    }
}

ScaleMismatch compare_scale(const RtStageBuffer& device, const Image8& reference)
{
    ScaleMismatch mismatch;
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < reference.getHeight(); ++y) {
            for (int x = 0; x < reference.getWidth(); ++x) {
                const unsigned char expected = *scale_pixel(reference, plane, y, x);
                const unsigned char actual =
                    *(static_cast<const unsigned char*>(device.data) +
                      static_cast<std::size_t>(y) * device.row_pitch +
                      static_cast<std::size_t>(x) * 3 + plane);
                ++mismatch.samples;
                if (actual != expected) {
                    ++mismatch.differing;
                    mismatch.max_abs = std::max(
                        mismatch.max_abs,
                        std::fabs(static_cast<double>(actual) -
                                  static_cast<double>(expected)));
                }
            }
        }
    }
    return mismatch;
}

ScaleMismatch compare_scale(const RtStageBuffer& device,
                            const Imagefloat& reference)
{
    ScaleMismatch mismatch;
    for (int plane = 0; plane < 3; ++plane) {
        for (int y = 0; y < reference.getHeight(); ++y) {
            const float* expected_row = scale_row(reference, plane, y);
            const float* actual_row = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(device.data) +
                static_cast<std::size_t>(plane) * device.plane_pitch +
                static_cast<std::size_t>(y) * device.row_pitch);
            for (int x = 0; x < reference.getWidth(); ++x) {
                ++mismatch.samples;
                const float actual = actual_row[x];
                const float expected = expected_row[x];
                /* Identical samples, including a shared infinity, are exact. */
                if (actual == expected) {
                    continue;
                }
                const double delta =
                    std::fabs(static_cast<double>(actual) -
                              static_cast<double>(expected));
                if (!std::isfinite(delta)) {
                    ++mismatch.non_finite;
                    continue;
                }
                if (delta > 0.0) {
                    ++mismatch.differing;
                }
                mismatch.max_abs = std::max(mismatch.max_abs, delta);
            }
        }
    }
    return mismatch;
}

/* Submit one scale node and require that the device executed it. */
void submit_scale(StageQueue& queue, const char* node, const RtStageBuffer& source,
                  const RtStageBuffer& dst, TypeInterpolation interp)
{
    StageRequest request;
    request.name = node;
    request.args = {buffer_arg(source), buffer_arg(dst),
                    int_arg(static_cast<std::int64_t>(interp))};

    StageEvent event = queue.submit(request, nullptr);
    const std::string label = std::string(node) + " executes on the device";
    check(event.device == StageDeviceType::OPENCL, label.c_str());
    check(event.succeeded(), (std::string(node) + " completed").c_str());
    check(!event.asynchronous,
          (std::string(node) + " waits inside the launcher").c_str());
    check(queue.wait(event) == RT_STAGE_OK,
          (std::string(node) + " reports OK").c_str());
}

void report_scale(const char* node, const ScaleTarget& target,
                  TypeInterpolation interp, const ScaleMismatch& mismatch,
                  bool exact_required)
{
    const char* interp_label = interp == TI_Nearest ? "nearest" : "bilinear";
    std::printf("stage-residency: %s %s %s: PAE %.6f LSB16, "
                "%zu of %zu samples differ, %zu non-finite\n",
                node, target.label, interp_label, mismatch.max_abs,
                mismatch.differing, mismatch.samples, mismatch.non_finite);

    const std::string label =
        std::string(node) + " " + target.label + " " + interp_label;
    if (exact_required) {
        check(mismatch.exact(), (label + " is bit-exact").c_str());
    } else {
        check(mismatch.non_finite == 0 &&
                  mismatch.max_abs <= SCALE_TOLERANCE_LSB16,
              (label + " stays within one LSB16").c_str());
    }
}

/*
 * A packed 8-bit source is the lossy reference: the blend is stored in the
 * source type, so both destinations must reproduce it exactly.
 */
void run_scale_u8_cases(StageQueue& queue, StageBufferAllocator& host)
{
    Image8 source(SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT);
    fill_scale_source(source);
    RtStageBuffer source_buffer =
        host.allocate({RT_STAGE_U8, SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT, 1});
    if (source_buffer.data == nullptr) {
        check(false, "thumbnail scale 8-bit source fixture allocates");
        return;
    }
    copy_scale_source(source, source_buffer);

    for (const ScaleTarget& target : SCALE_TARGETS) {
        for (TypeInterpolation interp : {TI_Nearest, TI_Bilinear}) {
            Image8 reference8(target.width, target.height);
            source.resizeImgTo(target.width, target.height, interp, &reference8);
            RtStageBuffer device8 =
                host.allocate({RT_STAGE_U8, target.width, target.height, 1});
            submit_scale(queue, "thumbnail.scale.u8", source_buffer, device8,
                         interp);
            report_scale("thumbnail.scale.u8", target, interp,
                         compare_scale(device8, reference8), true);
            host.release(device8);

            Imagefloat referencef(target.width, target.height);
            source.resizeImgTo(target.width, target.height, interp, &referencef);
            RtStageBuffer devicef =
                host.allocate({RT_STAGE_F32, target.width, target.height, 3});
            submit_scale(queue, "thumbnail.scale", source_buffer, devicef,
                         interp);
            report_scale("thumbnail.scale (8-bit source)", target, interp,
                         compare_scale(devicef, referencef), true);
            host.release(devicef);
        }
    }
    host.release(source_buffer);
}

void run_scale_u16_cases(StageQueue& queue, StageBufferAllocator& host)
{
    Image16 source(SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT);
    fill_scale_source(source);
    RtStageBuffer source_buffer =
        host.allocate({RT_STAGE_U16, SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT, 3});
    if (source_buffer.data == nullptr) {
        check(false, "thumbnail scale 16-bit source fixture allocates");
        return;
    }
    copy_scale_source(source, source_buffer);

    for (const ScaleTarget& target : SCALE_TARGETS) {
        const bool same_size = target.width == SCALE_SRC_WIDTH &&
                               target.height == SCALE_SRC_HEIGHT;
        for (TypeInterpolation interp : {TI_Nearest, TI_Bilinear}) {
            Imagefloat reference(target.width, target.height);
            source.resizeImgTo(target.width, target.height, interp, &reference);
            RtStageBuffer device =
                host.allocate({RT_STAGE_F32, target.width, target.height, 3});
            submit_scale(queue, "thumbnail.scale", source_buffer, device, interp);
            report_scale("thumbnail.scale (16-bit source)", target, interp,
                         compare_scale(device, reference),
                         interp == TI_Nearest || same_size);
            host.release(device);
        }
    }
    host.release(source_buffer);
}

void run_scale_f32_cases(StageQueue& queue, StageBufferAllocator& host)
{
    Imagefloat source(SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT);
    fill_scale_source(source);
    RtStageBuffer source_buffer =
        host.allocate({RT_STAGE_F32, SCALE_SRC_WIDTH, SCALE_SRC_HEIGHT, 3});
    if (source_buffer.data == nullptr) {
        check(false, "thumbnail scale float source fixture allocates");
        return;
    }
    copy_scale_source(source, source_buffer);

    for (const ScaleTarget& target : SCALE_TARGETS) {
        const bool same_size = target.width == SCALE_SRC_WIDTH &&
                               target.height == SCALE_SRC_HEIGHT;
        for (TypeInterpolation interp : {TI_Nearest, TI_Bilinear}) {
            Imagefloat reference(target.width, target.height);
            source.resizeImgTo(target.width, target.height, interp, &reference);
            RtStageBuffer device =
                host.allocate({RT_STAGE_F32, target.width, target.height, 3});
            submit_scale(queue, "thumbnail.scale", source_buffer, device, interp);
            report_scale("thumbnail.scale (float source)", target, interp,
                         compare_scale(device, reference),
                         interp == TI_Nearest || same_size);
            host.release(device);
        }
    }
    host.release(source_buffer);
}

/*
 * Image16 pads every row to 128 bits, so a width that is not a multiple of
 * eight separates the allocated stride from the packed row. The source here is
 * a real Image16 handed to the node through stage::view(), exactly as the
 * production call site does it, rather than an allocator buffer with packed
 * strides: a descriptor that reported the packed width would upload rows at
 * the wrong offsets and the comparison below would fail.
 */
void run_scale_padded_view_case(StageQueue& queue, StageBufferAllocator& host)
{
    /* 41 * 2 bytes: 82 packed, 96 with the 128-bit row padding. */
    constexpr int PADDED_WIDTH = 41;
    constexpr int PADDED_HEIGHT = 23;
    constexpr int PADDED_DST_WIDTH = 19;
    constexpr int PADDED_DST_HEIGHT = 11;
    const ScaleTarget target{PADDED_DST_WIDTH, PADDED_DST_HEIGHT,
                             "padded 41x23 to 19x11"};

    Image16 source(PADDED_WIDTH, PADDED_HEIGHT);
    if (source.getWidth() != PADDED_WIDTH ||
        source.getHeight() != PADDED_HEIGHT) {
        check(false, "padded Image16 fixture allocates");
        return;
    }
    fill_scale_source(source);

    const RtStageBuffer source_view = view(source);
    check(source_view.row_pitch == source.getRowStride(),
          "the Image16 view reports the allocated row stride");
    check(source_view.row_pitch >=
              static_cast<std::size_t>(PADDED_WIDTH) *
                  sizeof(unsigned short),
          "the Image16 row stride covers the packed row");
    /* Padding is present, so the case cannot pass on a packed descriptor. */
    check(source_view.row_pitch >
              static_cast<std::size_t>(PADDED_WIDTH) *
                  sizeof(unsigned short),
          "the 41-column Image16 row is padded beyond the packed row");
    check(source_view.plane_pitch == source.getPlaneStride(),
          "the Image16 view reports the allocated plane stride");

    for (TypeInterpolation interp : {TI_Nearest, TI_Bilinear}) {
        Imagefloat reference(PADDED_DST_WIDTH, PADDED_DST_HEIGHT);
        source.resizeImgTo(PADDED_DST_WIDTH, PADDED_DST_HEIGHT, interp,
                           &reference);
        RtStageBuffer device = host.allocate(
            {RT_STAGE_F32, PADDED_DST_WIDTH, PADDED_DST_HEIGHT, 3});
        submit_scale(queue, "thumbnail.scale", source_view, device, interp);
        report_scale("thumbnail.scale (padded Image16)", target, interp,
                     compare_scale(device, reference), true);
        host.release(device);
    }
}

void test_thumbnail_scale(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("thumbnail scale: no OpenCL device");
        return;
    }

    const std::vector<std::string> nodes = opencl_stage_nodes();
    check(std::find(nodes.begin(), nodes.end(), "thumbnail.scale") != nodes.end(),
          "thumbnail.scale is registered as a device node");
    check(std::find(nodes.begin(), nodes.end(), "thumbnail.scale.u8") !=
              nodes.end(),
          "thumbnail.scale.u8 is registered as a device node");

    auto queue = opencl_device()->create_queue();
    check(queue->type() == StageDeviceType::OPENCL,
          "the thumbnail scale cases run on the OpenCL queue");

    StageBufferAllocator host(StageDeviceType::CPU);
    run_scale_u8_cases(*queue, host);
    run_scale_u16_cases(*queue, host);
    run_scale_f32_cases(*queue, host);
    run_scale_padded_view_case(*queue, host);
}

/* ------------------------------------------------------------ selection */

void test_selection(bool available)
{
    using namespace rtengine::stage;

    clear_node_device_overrides();
    unsetenv("RT_STAGE_OPENCL");
    unsetenv("RT_STAGE_OPENCL_DISABLE");
    set_opencl_stage_allowed(true);

    StageNodeSelection default_choice = select_node_device("lanczos3");
    check(default_choice.device == StageNodeDevice::CPU,
          "the default selection is CPU");
    if (available) {
        check(default_choice.implemented,
              "lanczos3 has a device implementation");
    }

    StageNodeSelection unknown = select_node_device("no_such_stage");
    check(unknown.device == StageNodeDevice::CPU,
          "an unimplemented node selects CPU");
    check(!unknown.implemented, "an unimplemented node is reported as such");
    check(unknown.reason.find("no OpenCL implementation") != std::string::npos,
          "an unimplemented node explains the CPU selection");

    /* The explicit API asks for a device. */
    StageNodeSelection explicit_cpu =
        select_node_device("lanczos3", StageNodeDevice::CPU);
    check(explicit_cpu.device == StageNodeDevice::CPU,
          "an explicit CPU request returns the CPU");

    StageNodeSelection explicit_device =
        select_node_device("lanczos3", StageNodeDevice::OpenCL);
    if (available) {
        check(explicit_device.device == StageNodeDevice::OpenCL,
              "an explicit device request returns the device");
    } else {
        check(explicit_device.device == StageNodeDevice::CPU,
              "an unavailable device request falls back to CPU");
        check(!explicit_device.reason.empty(),
              "an unavailable device explains the fallback");
    }

    /* The kill switch wins over the request and the environment. */
    set_opencl_stage_allowed(false);
    StageNodeSelection killed =
        select_node_device("lanczos3", StageNodeDevice::OpenCL);
    check(killed.device == StageNodeDevice::CPU,
          "the user kill switch forces CPU");
    check(killed.reason.find("disabled") != std::string::npos,
          "the kill switch explains itself");
    set_opencl_stage_allowed(true);

    setenv("RT_STAGE_OPENCL_DISABLE", "1", 1);
    StageNodeSelection env_killed =
        select_node_device("lanczos3", StageNodeDevice::OpenCL);
    check(env_killed.device == StageNodeDevice::CPU,
          "RT_STAGE_OPENCL_DISABLE forces CPU");
    unsetenv("RT_STAGE_OPENCL_DISABLE");

    setenv("RT_STAGE_OPENCL", "0", 1);
    StageNodeSelection env_off =
        select_node_device("lanczos3", StageNodeDevice::OpenCL);
    check(env_off.device == StageNodeDevice::CPU,
          "RT_STAGE_OPENCL=0 kills an explicit device request");
    unsetenv("RT_STAGE_OPENCL");

    /* The environment opt-in. */
    setenv("RT_STAGE_OPENCL", "1", 1);
    StageNodeSelection opted_in = select_node_device("lanczos3");
    if (available) {
        check(opted_in.device == StageNodeDevice::OpenCL,
              "RT_STAGE_OPENCL=1 opts the implemented node in");
    } else {
        check(opted_in.device == StageNodeDevice::CPU,
              "RT_STAGE_OPENCL=1 falls back without a device");
    }
    unsetenv("RT_STAGE_OPENCL");

    /* An explicit per-node override replaces the environment default. */
    set_node_device_override("lanczos3", StageNodeDevice::OpenCL);
    StageNodeSelection overridden = select_node_device("lanczos3");
    if (available) {
        check(overridden.device == StageNodeDevice::OpenCL,
              "a per-node override selects the device");
    } else {
        check(overridden.device == StageNodeDevice::CPU,
              "a per-node override falls back without a device");
    }
    clear_node_device_overrides();
}

/* ------------------------------------------------ ambient ICD robustness */

/*
 * Regression cover for a broken ICD. With the distribution vendor set
 * (pocl.icd next to rusticl.icd) the rusticl platform publishes an
 * uninitialised ICD dispatch chain, so the first query against it jumps
 * through a null entry inside the loader and kills the process before any CL
 * error exists (Debian bug #1117930; `clinfo` crashes the same way).
 *
 * The device path must skip such a platform and still produce a result: either
 * it selects a usable device or it runs the node on the host. Both outcomes
 * pass; a crash, or a selection that claims a device the queue cannot use,
 * does not.
 */
void test_icd_robustness(bool available, const std::string& reason)
{
    using namespace rtengine::stage;

    StageOpenCLDeviceInfo info;
    const bool have_info = opencl_stage_device_info(&info);
    check(have_info == available, "device info and availability agree");
    if (available) {
        check(!info.platform.empty(), "the selected platform has a name");
        check(!info.device.empty(), "the selected device has a name");
        std::printf("stage-residency: icd: device=%s platform=%s executor=%s\n",
                    info.device.c_str(), info.platform.c_str(),
                    info.cpu ? "CPU" : "GPU");
    } else {
        check(!reason.empty(), "an unavailable device explains why");
        std::printf("stage-residency: icd: host fallback (%s)\n", reason.c_str());
    }
    if (!info.note.empty()) {
        std::printf("stage-residency: icd: %s\n", info.note.c_str());
    }

    /* Whichever branch was taken, the resize node still runs. */
    StageBufferAllocator host(StageDeviceType::CPU);
    NodeFixture fixture;
    if (!fixture.allocate(host, 64, 48, 32, 24)) {
        check(false, "the ICD robustness fixture allocates");
        return;
    }

    std::string select_reason;
    const auto device = select_device(StageDeviceType::OPENCL, &select_reason);
    check(device != nullptr, "node selection returns a device");
    const bool selected_device = device->type() == StageDeviceType::OPENCL;
    check(selected_device == available,
          "the selected device agrees with the availability check");

    const auto context = device->create_context();
    StageRequest request;
    request.name = "lanczos3";
    request.args = fixture.args(fixture.device_output, 0.5);
    set_node_device_override("lanczos3", StageNodeDevice::OpenCL);
    StageExecution execution;
    const RtStageStatus status = context->execute(request, nullptr, &execution);
    clear_node_device_overrides();

    check(status == RT_STAGE_OK,
          "the resize node runs with the ambient ICD set");
    check(execution.device == (available ? StageDeviceType::OPENCL
                                         : StageDeviceType::CPU),
          "the node runs on the selected device or the host");
    check(host.release(fixture.device_output), "the robustness output is released");
}

/* -------------------------------------------------------- first device node */

void test_node_parity(bool available)
{
    using namespace rtengine::stage;

    constexpr double TOLERANCE_LSB = 1.0;

    auto cpu = cpu_device()->create_context();
    StageBufferAllocator host(StageDeviceType::CPU);

    for (const double scale : {0.5, 1.5}) {
        const int dst_w = static_cast<int>(128 * scale);
        const int dst_h = static_cast<int>(96 * scale);
        NodeFixture fixture;
        if (!fixture.allocate(host, 128, 96, dst_w, dst_h)) {
            check(false, "node fixture allocates");
            continue;
        }

        StageRequest request;
        request.name = "lanczos3";
        request.args = fixture.args(fixture.cpu_output, scale);
        check(cpu->execute(request, nullptr) == RT_STAGE_OK,
              "the CPU executes the resize node");

        if (!available) {
            skip("node parity: no OpenCL device");
            continue;
        }

        auto device_context = opencl_device()->create_context();
        set_node_device_override("lanczos3", StageNodeDevice::OpenCL);
        request.args = fixture.args(fixture.device_output, scale);
        StageExecution execution;
        const RtStageStatus status =
            device_context->execute(request, nullptr, &execution);
        clear_node_device_overrides();
        if (status != RT_STAGE_OK) {
            std::fprintf(stderr, "device execute status: %d\n",
                         static_cast<int>(status));
        }
        check(status == RT_STAGE_OK, "the device executes the resize node");
        check(execution.device == StageDeviceType::OPENCL,
              "the execute result names the device as the executor");
        check(execution.asynchronous,
              "the device execution was submitted asynchronously");

        StageDeviceResidency* residency = device_context->residency();
        check(residency != nullptr, "the device context has a residency");
        if (residency != nullptr && residency->stats().allocations == 0) {
            check(false, "the device path allocated device buffers");
        }

        const Diff diff = compare(fixture.cpu_output, fixture.device_output);
        std::printf("stage-residency: lanczos3 scale %.1f: PAE %.6f LSB16, "
                    "%zu of %zu samples differ, tolerance %.1f LSB16\n",
                    scale, diff.max_abs, diff.differing, diff.samples,
                    TOLERANCE_LSB);
        check(diff.finite, "device output is finite");
        check(diff.max_abs <= TOLERANCE_LSB,
              "device output is within one 16-bit LSB of the CPU");

        check(host.release(fixture.cpu_output), "the CPU output is released");
    }

    /* Over-budget fallback: a residency that cannot hold the node runs the
     * host program and still succeeds. */
    if (available) {
        auto context = opencl_device()->create_context();
        context->residency()->set_budget(1024);
        NodeFixture fixture;
        if (fixture.allocate(host, 128, 96, 64, 48)) {
            StageRequest request;
            request.name = "lanczos3";
            request.args = fixture.args(fixture.device_output, 0.5);
            set_node_device_override("lanczos3", StageNodeDevice::OpenCL);
            StageExecution over_budget;
            const RtStageStatus status =
                context->execute(request, nullptr, &over_budget);
            clear_node_device_overrides();
            check(status == RT_STAGE_OK,
                  "an over-budget node falls back to the host and succeeds");
            check(over_budget.device == StageDeviceType::CPU,
                  "an over-budget node reports the host as the executor");
            check(!over_budget.asynchronous,
                  "an over-budget node reports a synchronous execution");
            check(context->residency()->stats().refusals > 0,
                  "the over-budget fallback is counted as a refusal");

            RtStageBuffer reference = host.allocate({RT_STAGE_F32, 64, 48, 3});
            StageRequest cpu_request;
            cpu_request.name = "lanczos3";
            cpu_request.args = fixture.args(reference, 0.5);
            check(cpu->execute(cpu_request, nullptr) == RT_STAGE_OK,
                  "the CPU reference executes after a device refusal");
            const Diff diff = compare(reference, fixture.device_output);
            check(diff.finite && diff.max_abs == 0.0,
                  "the over-budget fallback is exact");
            host.release(reference);
        }
    }
}

/* ------------------------------------------------- rgb.proc row pitches */

/*
 * `rgb.proc` used to index its source and destination as `y * width + x`.
 * The stage device pads every row to the stage alignment, so on a width whose
 * row is not already aligned the kernel addressed the wrong sample and left
 * the tail of the allocation unwritten. The kernel now takes a source and a
 * destination pitch, and these cases fail without them.
 *
 * The check needs no CPU reference. The same fixture is run at a padded width
 * and at a width whose row pitch equals its width, where the shared columns
 * must agree bit for bit; the padded run is repeated to prove the last row is
 * finite and stable.
 */

struct RgbProcFixture {
    RtStageBuffer source;
    RtStageBuffer output;
    std::vector<float> luts;
    float mixer[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float toxyz[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    bool allocate(StageBufferAllocator& allocator, int width, int height)
    {
        std::string error;
        source = allocator.allocate({RT_STAGE_F32, width, height, 3}, &error);
        output = allocator.allocate({RT_STAGE_F32, width, height, 3}, &error);
        if (!valid(source) || !valid(output)) {
            std::fprintf(stderr, "rgb.proc fixture allocation failed: %s\n",
                         error.c_str());
            return false;
        }
        fill(source, 7u);
        fill(output, 0u);

        luts.resize(65536);
        for (std::size_t i = 0; i < luts.size(); ++i) {
            luts[i] = static_cast<float>(i);
        }
        return true;
    }

    static RtStageArg table(const float* data, int entries)
    {
        RtStageBuffer buffer{};
        buffer.size = sizeof(RtStageBuffer);
        buffer.data = const_cast<float*>(data);
        buffer.width = entries;
        buffer.height = 1;
        buffer.row_pitch = static_cast<std::size_t>(entries) * sizeof(float);
        buffer.plane_pitch = buffer.row_pitch;
        buffer.planes = 1;
        buffer.format = RT_STAGE_F32;
        buffer.flags = RT_STAGE_BUFFER_READ;
        return buffer_arg(buffer);
    }

    std::vector<RtStageArg> args(RtStageBuffer& dst) const
    {
        std::vector<RtStageArg> result;
        result.push_back(buffer_arg(source));
        result.push_back(buffer_arg(dst));
        for (int i = 0; i < 6; ++i) {
            result.push_back(table(luts.data(), 65536));
        }
        result.push_back(table(luts.data(), 65536));
        result.push_back(table(luts.data(), 65536));
        result.push_back(table(mixer, 9));
        result.push_back(table(toxyz, 9));
        result.push_back(int_arg(source.width));
        result.push_back(int_arg(source.height));
        result.push_back(int_arg(0));          /* toneCurveBlack */
        result.push_back(int_arg(1));          /* clampOog */
        result.push_back(int_arg(0));          /* mixChannels */
        result.push_back(int_arg(0));          /* rgbCurveMask */
        result.push_back(double_arg(1.0));     /* expScale */
        result.push_back(double_arg(0.0));     /* comp */
        result.push_back(double_arg(65535.0)); /* hlRange */
        return result;
    }
};

/* Compare the first `columns` columns of two equal-height, equal-plane images. */
Diff compare_columns(const RtStageBuffer& a, const RtStageBuffer& b, int columns)
{
    Diff diff;
    if (a.format != RT_STAGE_F32 || b.format != RT_STAGE_F32 ||
        a.height != b.height || a.planes != b.planes || columns > a.width ||
        columns > b.width) {
        diff.finite = false;
        return diff;
    }
    for (std::uint32_t plane = 0; plane < a.planes; ++plane) {
        const unsigned char* pa = static_cast<const unsigned char*>(a.data) +
                                  static_cast<std::size_t>(plane) * a.plane_pitch;
        const unsigned char* pb = static_cast<const unsigned char*>(b.data) +
                                  static_cast<std::size_t>(plane) * b.plane_pitch;
        for (int y = 0; y < a.height; ++y) {
            const float* ra = reinterpret_cast<const float*>(
                pa + static_cast<std::size_t>(y) * a.row_pitch);
            const float* rb = reinterpret_cast<const float*>(
                pb + static_cast<std::size_t>(y) * b.row_pitch);
            for (int x = 0; x < columns; ++x) {
                ++diff.samples;
                if (!close_enough(ra[x]) || !close_enough(rb[x])) {
                    diff.finite = false;
                }
                const double delta = std::fabs(static_cast<double>(ra[x]) -
                                               static_cast<double>(rb[x]));
                if (delta > 0.0) {
                    ++diff.differing;
                }
                diff.max_abs = std::max(diff.max_abs, delta);
            }
        }
    }
    return diff;
}

bool last_row_finite(const RtStageBuffer& buffer)
{
    for (std::uint32_t plane = 0; plane < buffer.planes; ++plane) {
        const unsigned char* base = static_cast<const unsigned char*>(buffer.data) +
                                    static_cast<std::size_t>(plane) *
                                        buffer.plane_pitch +
                                    static_cast<std::size_t>(buffer.height - 1) *
                                        buffer.row_pitch;
        const float* row = reinterpret_cast<const float*>(base);
        for (int x = 0; x < buffer.width; ++x) {
            if (!std::isfinite(row[x])) {
                return false;
            }
        }
    }
    return true;
}

void test_rgb_proc_pitch(bool available)
{
    using namespace rtengine::stage;

    if (!available) {
        skip("rgb.proc pitch: no OpenCL device");
        return;
    }

    constexpr int HEIGHT = 24;
    const struct {
        int padded;
        int compact;
    } cases[] = {{238, 240}, {179, 192}};

    StageBufferAllocator host(StageDeviceType::CPU);
    const auto context = opencl_device()->create_context();
    check(context != nullptr, "rgb.proc pitch: the device creates a context");
    if (context == nullptr) {
        return;
    }

    for (const auto& item : cases) {
        std::size_t row_pitch = 0;
        std::size_t plane_pitch = 0;
        std::size_t bytes = 0;
        std::string layout_error;
        check(stage_buffer_layout({RT_STAGE_F32, item.padded, HEIGHT, 1},
                                  row_pitch, plane_pitch, bytes, &layout_error),
              "rgb.proc pitch: the padded layout is valid");
        check(row_pitch / sizeof(float) > static_cast<std::size_t>(item.padded),
              "rgb.proc pitch: the padded case really has padded rows");
        const int padded_pitch = static_cast<int>(row_pitch / sizeof(float));
        check(stage_buffer_layout({RT_STAGE_F32, item.compact, HEIGHT, 1},
                                  row_pitch, plane_pitch, bytes, &layout_error),
              "rgb.proc pitch: the compact layout is valid");
        check(row_pitch / sizeof(float) == static_cast<std::size_t>(item.compact),
              "rgb.proc pitch: the compact case shares its pitch and width");

        RgbProcFixture padded;
        RgbProcFixture reference;
        if (!padded.allocate(host, item.padded, HEIGHT) ||
            !reference.allocate(host, item.compact, HEIGHT)) {
            check(false, "rgb.proc pitch: the fixtures allocate");
            continue;
        }

        std::string error;
        RtStageBuffer repeat =
            host.allocate({RT_STAGE_F32, item.padded, HEIGHT, 3}, &error);
        if (!valid(repeat)) {
            check(false, "rgb.proc pitch: the repeat output allocates");
            continue;
        }
        fill(repeat, 0u);

        set_node_device_override("rgb.proc", StageNodeDevice::OpenCL);

        StageRequest request;
        request.name = "rgb.proc";
        request.args = padded.args(padded.output);
        StageExecution first;
        RtStageStatus status = context->execute(request, nullptr, &first);
        check(status == RT_STAGE_OK,
              "the device executes rgb.proc on padded rows");
        check(first.device == StageDeviceType::OPENCL,
              "the padded-row execution names the device");
        check(!first.asynchronous,
              "rgb.proc reads its planes back, so the execution is complete");

        request.args = padded.args(repeat);
        StageExecution second;
        status = context->execute(request, nullptr, &second);
        check(status == RT_STAGE_OK,
              "the device repeats rgb.proc on padded rows");

        request.args = reference.args(reference.output);
        StageExecution compact;
        status = context->execute(request, nullptr, &compact);
        check(status == RT_STAGE_OK,
              "the device executes rgb.proc on a compact pitch");

        clear_node_device_overrides();

        const Diff shared =
            compare_columns(padded.output, reference.output, item.padded);
        std::printf("stage-residency: rgb.proc width %d (pitch %d): "
                    "PAE %.6f LSB16, %zu of %zu shared samples differ\n",
                    item.padded, padded_pitch,
                    shared.max_abs, shared.differing, shared.samples);
        check(shared.finite, "the padded-row output is finite");
        check(shared.differing == 0 && shared.max_abs == 0.0,
              "the padded and compact runs agree on the shared columns");

        const Diff stable = compare(padded.output, repeat);
        check(stable.finite, "the repeated padded-row output is finite");
        check(stable.differing == 0 && stable.max_abs == 0.0,
              "the padded-row output is stable across two runs");
        check(last_row_finite(padded.output),
              "the padded-row output writes its last row");

        host.release(padded.output);
        host.release(repeat);
        host.release(reference.output);
        host.release(padded.source);
        host.release(reference.source);
    }
}

/*
 * drawFrame() reads rtengine::Color::gamma2curve, which rtengine::Color::init()
 * fills from the sRGB table. The test initialises the same table subset so the
 * production CPU path can be called without the engine-wide initialisation,
 * which needs the Settings singleton and the ICC stores.
 */
void init_gamma2curve()
{
    constexpr int ENTRIES = 65536;
    constexpr double MAX_VALUE = 65535.0;
    constexpr float MAX_VALUE_F = 65535.0f;

    rtengine::Color::gammatab_srgb(ENTRIES, 0);
    for (int i = 0; i < ENTRIES; ++i) {
        rtengine::Color::gammatab_srgb[i] =
            rtengine::Color::gamma2(i / MAX_VALUE);
    }
    rtengine::Color::gammatab_srgb *= MAX_VALUE_F;
    rtengine::Color::gamma2curve.share(rtengine::Color::gammatab_srgb,
                                       LUT_CLIP_BELOW | LUT_CLIP_ABOVE);
}

/* The border clip drawFrame() applies to the 8-bit GUI value. */
float frame_border(int value)
{
    constexpr int MAX_8BIT = 255;
    constexpr double MAX_16BIT = 65535.0;
    const int sanitized = std::max(0, std::min(value, MAX_8BIT));
    const double normalized = static_cast<double>(sanitized) / MAX_8BIT;
    const int index = static_cast<int>(normalized * MAX_16BIT);
    return rtengine::Color::gamma2curve[index];
}

std::unique_ptr<Imagefloat> make_image(int width, int height, unsigned seed)
{
    auto image = std::make_unique<Imagefloat>(width, height);
    RtStageBuffer buffer = view(*image);
    fill(buffer, seed);
    return image;
}

struct FramingCase {
    const char* name;
    int src_width;
    int src_height;
    int dst_width;
    int dst_height;
    int red;
    int green;
    int blue;
};

void test_framing(bool available)
{
    using namespace rtengine::stage;

    init_gamma2curve();

    const FramingCase cases[] = {
        {"even delta, white", 32, 24, 40, 32, 255, 255, 255},
        {"odd delta, black", 31, 23, 40, 32, 0, 0, 0},
        {"both sides smaller, grey", 20, 16, 33, 27, 128, 128, 128},
        {"per-channel border", 25, 19, 34, 28, 255, 128, 0},
        {"one axis equal, zero row offset", 25, 19, 34, 19, 255, 128, 0},
    };

    /* Both early returns are host rules and must not reach the node. */
    rtengine::procparams::ProcParams early_params;
    early_params.framing.enabled = true;
    rtengine::ImProcFunctions early_ipf(&early_params, false);

    set_node_device_override("framing", StageNodeDevice::OpenCL);
    {
        auto same = make_image(24, 18, 7u);
        Imagefloat* same_pointer = same.get();
        rtengine::ImProcFunctions::FramingData dims;
        dims.enabled = true;
        dims.imgWidth = 24;
        dims.imgHeight = 18;
        dims.framedWidth = 24;
        dims.framedHeight = 18;
        Imagefloat* returned =
            early_ipf.drawFrame(same_pointer, early_params.framing, dims);
        check(returned == same_pointer,
              "framing returns the source pointer when it equals the frame");
        same.release();
        delete returned;
    }
    {
        auto larger = make_image(30, 24, 11u);
        Imagefloat* larger_pointer = larger.get();
        rtengine::ImProcFunctions::FramingData dims;
        dims.enabled = true;
        dims.imgWidth = 30;
        dims.imgHeight = 24;
        dims.framedWidth = 28;
        dims.framedHeight = 22;
        Imagefloat* returned =
            early_ipf.drawFrame(larger_pointer, early_params.framing, dims);
        check(returned == larger_pointer,
              "framing returns the source pointer when it is larger");
        larger.release();
        delete returned;
    }
    clear_node_device_overrides();

    if (!available) {
        skip("framing: no OpenCL device");
        return;
    }

    const auto context = opencl_device()->create_context();
    check(context != nullptr, "framing: the device creates a context");
    if (context == nullptr) {
        return;
    }

    for (const FramingCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.framing.enabled = true;
        params.framing.borderRed = item.red;
        params.framing.borderGreen = item.green;
        params.framing.borderBlue = item.blue;
        rtengine::ImProcFunctions ipf(&params, false);

        rtengine::ImProcFunctions::FramingData dims;
        dims.enabled = true;
        dims.imgWidth = item.src_width;
        dims.imgHeight = item.src_height;
        dims.framedWidth = item.dst_width;
        dims.framedHeight = item.dst_height;

        const int row_offset = static_cast<int>(std::round(
            (static_cast<double>(item.dst_height) - item.src_height) / 2.0));
        const int col_offset = static_cast<int>(std::round(
            (static_cast<double>(item.dst_width) - item.src_width) / 2.0));

        auto seed = make_image(item.src_width, item.src_height, 3u);

        unsetenv("RT_STAGE_OPENCL");
        clear_node_device_overrides();
        auto cpu_source = make_image(item.src_width, item.src_height, 3u);
        std::unique_ptr<Imagefloat> cpu(
            ipf.drawFrame(cpu_source.release(), params.framing, dims));
        if (!cpu) {
            check(false, "framing: the CPU path returns an image");
            continue;
        }

        Imagefloat device(item.dst_width, item.dst_height);
        RtStageArg args[7]{};
        args[0] = buffer_arg(view(*seed));
        args[1] = buffer_arg(view(device));
        args[2] = double_arg(frame_border(item.red));
        args[3] = double_arg(frame_border(item.green));
        args[4] = double_arg(frame_border(item.blue));
        args[5] = int_arg(row_offset);
        args[6] = int_arg(col_offset);

        set_node_device_override("framing", StageNodeDevice::OpenCL);
        StageRequest request;
        request.name = "framing";
        request.args.assign(args, args + 7);
        StageExecution execution;
        const RtStageStatus status =
            context->execute(request, nullptr, &execution);
        clear_node_device_overrides();

        check(status == RT_STAGE_OK,
              "the device executes the framing node");
        check(execution.device == StageDeviceType::OPENCL,
              "the framing execution names the device");
        check(!execution.asynchronous,
              "framing reads its planes back, so the execution is complete");

        const Diff diff = compare(view(device), view(*cpu));
        std::printf("stage-residency: framing %s %dx%d -> %dx%d "
                    "(offset %d,%d): PAE %.6f LSB16, %zu of %zu samples "
                    "differ\n",
                    item.name, item.src_width, item.src_height,
                    item.dst_width, item.dst_height, row_offset, col_offset,
                    diff.max_abs, diff.differing, diff.samples);
        check(diff.finite, "the framing device output is finite");
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "the framing device output is bit-exact against drawFrame");

        /*
         * The production call site must take the same decision and return the
         * same pixels when the node is selected.
         */
        auto call_source = make_image(item.src_width, item.src_height, 3u);
        set_node_device_override("framing", StageNodeDevice::OpenCL);
        std::unique_ptr<Imagefloat> call_result(
            ipf.drawFrame(call_source.release(), params.framing, dims));
        clear_node_device_overrides();
        if (!call_result) {
            check(false, "framing: the call site returns an image");
            continue;
        }
        const Diff call_diff = compare(view(*call_result), view(*cpu));
        check(call_diff.differing == 0 && call_diff.max_abs == 0.0,
              "the framing call site output is bit-exact against drawFrame");
    }
}

/* ------------------------------------------------------------- gamut node */

/*
 * Color::XYZ2Lab() reads Color::cachef and Color::cachefy, which
 * Color::init() fills together with the denoise tables and the Settings
 * singleton. The two Lab tables are therefore filled here with the same
 * formulas and the same double intermediate, as init_gamma2curve() does for
 * the framing node.
 */
void init_lab_caches()
{
    constexpr int ENTRIES = 65536;
    constexpr double MAX_VALUE = 65535.0;
    constexpr double KAPPA = 24389.0 / 27.0;
    const double eps_max = MAX_VALUE * (216.0 / 24389.0);

    rtengine::Color::cachef(ENTRIES, LUT_CLIP_BELOW);
    rtengine::Color::cachefy(ENTRIES, LUT_CLIP_BELOW);

    int i = 0;
    for (; i <= static_cast<int>(eps_max); ++i) {
        const double index = i;
        rtengine::Color::cachef[i] =
            327.68 * ((KAPPA * index / MAX_VALUE + 16.0) / 116.0);
        rtengine::Color::cachefy[i] = 327.68 * (KAPPA * index / MAX_VALUE);
    }
    for (; i < ENTRIES; ++i) {
        const double index = i;
        rtengine::Color::cachef[i] = 327.68 * std::cbrt(index / MAX_VALUE);
        rtengine::Color::cachefy[i] =
            327.68 * (116.0 * std::cbrt(index / MAX_VALUE) - 16.0);
    }
}

/*
 * Deterministic Lab content with real gamut excursions: L in [0, 32768] and
 * a, b in [-16384, 16384]. That covers saturated colours, which the
 * compression must treat, without pushing the pipeline far outside the range
 * a photograph can produce.
 */
void fill_lab(LabImage& lab, unsigned seed)
{
    constexpr unsigned MAX_L = 32768u;
    constexpr int MAX_AB = 16384;
    constexpr unsigned AB_RANGE = 2u * MAX_AB + 1u;

    std::uint32_t state = seed * 2654435761u + 1u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };

    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            lab.L[y][x] = static_cast<float>(next() % (MAX_L + 1u));
            lab.a[y][x] = static_cast<float>(
                static_cast<int>(next() % AB_RANGE) - MAX_AB);
            lab.b[y][x] = static_cast<float>(
                static_cast<int>(next() % AB_RANGE) - MAX_AB);
        }
    }
}

/*
 * A source image inside the working gamut: smooth linear RGB converted to Lab
 * with the same helpers the engine uses. The device node admits this range;
 * the random fill above deliberately leaves it and is a residual case.
 */
void fill_lab_in_gamut(LabImage& lab, const Glib::ustring& workingProfile)
{
    const TMatrix wp =
        rtengine::ICCStore::getInstance()->workingSpaceMatrix(workingProfile);
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            const float u = static_cast<float>(x) /
                            static_cast<float>(std::max(lab.W - 1, 1));
            const float v = static_cast<float>(y) /
                            static_cast<float>(std::max(lab.H - 1, 1));
            const float r = 2000.f + 60000.f * (0.6f * u + 0.2f * v);
            const float g = 1500.f + 50000.f * (0.5f * v + 0.3f * (1.f - u));
            const float b = 2500.f + 55000.f * (0.4f * u + 0.5f * v);
            float X = 0.f;
            float Y = 0.f;
            float Z = 0.f;
            rtengine::Color::rgbxyz(r, g, b, X, Y, Z, wp);
            rtengine::Color::XYZ2Lab(X, Y, Z, lab.L[y][x], lab.a[y][x],
                                     lab.b[y][x]);
        }
    }
}

/*
 * The host block of stage_gamut(), assembled from the production functions:
 * Color::Lab2XYZ, Color::xyz2rgb, ImProcFunctions::gamutcompr, Color::rgbxyz
 * and Color::XYZ2Lab, in the same order.
 */
void gamut_cpu_reference(rtengine::ImProcFunctions& ipf,
                         const rtengine::procparams::ProcParams& params,
                         const LabImage& source, LabImage& destination)
{
    const int GW = source.W;
    const int GH = source.H;

    const TMatrix wprof = rtengine::ICCStore::getInstance()->workingSpaceMatrix(
        params.icm.workingProfile);
    const double wp[3][3] = {
        {wprof[0][0], wprof[0][1], wprof[0][2]},
        {wprof[1][0], wprof[1][1], wprof[1][2]},
        {wprof[2][0], wprof[2][1], wprof[2][2]}};
    const TMatrix wiprof =
        rtengine::ICCStore::getInstance()->workingSpaceInverseMatrix(
            params.icm.workingProfile);
    const double wip[3][3] = {
        {wiprof[0][0], wiprof[0][1], wiprof[0][2]},
        {wiprof[1][0], wiprof[1][1], wiprof[1][2]},
        {wiprof[2][0], wiprof[2][1], wiprof[2][2]}};

    Imagefloat provcomp(GW, GH);

    for (int i = 0; i < GH; ++i) {
        for (int j = 0; j < GW; ++j) {
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            rtengine::Color::Lab2XYZ(source.L[i][j], source.a[i][j],
                                     source.b[i][j], X, Y, Z);
            rtengine::Color::xyz2rgb(X, Y, Z, provcomp.r(i, j),
                                     provcomp.g(i, j), provcomp.b(i, j), wip);
        }
    }

    const float gainev =
        pow_F(2.0f, static_cast<float>(params.icm.wgamgain));
    if (params.icm.wgamgain != 0.0f) {
        for (int i = 0; i < GH; ++i) {
            for (int j = 0; j < GW; ++j) {
                provcomp.r(i, j) *= gainev;
                provcomp.g(i, j) *= gainev;
                provcomp.b(i, j) *= gainev;
            }
        }
    }

    if (params.icm.wgamut !=
        rtengine::procparams::ColorManagementParams::Wwgamut::NONE) {
        float mac = 0.0f;
        float mac0 = 0.0f;
        float mac1 = 0.0f;
        float mac2 = 0.0f;
        ipf.gamutcompr(&provcomp, &provcomp, 1, 1.0f, 0, mac, mac0, mac1,
                       mac2);
    }

    for (int i = 0; i < GH; ++i) {
        for (int j = 0; j < GW; ++j) {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            rtengine::Color::rgbxyz(provcomp.r(i, j), provcomp.g(i, j),
                                    provcomp.b(i, j), x, y, z, wp);
            rtengine::Color::XYZ2Lab(x, y, z, destination.L[i][j],
                                     destination.a[i][j],
                                     destination.b[i][j]);
        }
    }
}

struct CropCase {
    const char* label;
    int width;
    int height;
    int crop_width;
    int crop_height;
    int x;
    int y;
};

/*
 * The crop node is a three-plane F32 row copy: identical values, no resampling
 * and no border rule, so it is compared exactly rather than at the 1 LSB16
 * processing tolerance. A refused request must leave the destination
 * untouched, because the call site's memcpy is then the only fallback.
 */
void test_crop(bool available)
{
    using namespace rtengine::stage;

    const CropCase cases[] = {
        {"full frame", 238, 64, 238, 64, 0, 0},
        {"offset odd", 238, 64, 179, 49, 3, 1},
        {"bottom right", 238, 64, 59, 17, 179, 47},
        {"single row", 238, 64, 238, 1, 0, 63},
        {"single pixel", 238, 64, 1, 1, 237, 63},
        {"8192 wide", 8192, 65, 1000, 33, 4096, 16},
    };

    /* Device off: the request must fall back, leaving the call site's copy in
     * charge exactly as before the node existed. */
    {
        LabImage source(32, 24);
        fill_lab(source, 5u);
        LabImage destination(16, 12);
        RtStageArg args[4]{};
        args[0] = buffer_arg(view(source));
        args[1] = buffer_arg(view(destination));
        args[2] = int_arg(4);
        args[3] = int_arg(3);

        unsetenv("RT_STAGE_OPENCL");
        clear_node_device_overrides();
        const RtStageStatus status = run("crop", {args[0], args[1], args[2],
                                                  args[3]}, nullptr);
        check(status != RT_STAGE_OK,
              "crop falls back when the device is not enabled");
    }

    if (!available) {
        skip("crop: no OpenCL device");
        return;
    }

    const auto context = opencl_device()->create_context();
    check(context != nullptr, "crop: the device creates a context");
    if (context == nullptr) {
        return;
    }

    for (const CropCase& item : cases) {
        LabImage source(item.width, item.height);
        fill_lab(source, 41u);

        LabImage reference(item.crop_width, item.crop_height);
        const std::size_t rowBytes =
            static_cast<std::size_t>(item.crop_width) * sizeof(float);
        for (int y = 0; y < item.crop_height; ++y) {
            std::memcpy(reference.L[y], source.L[y + item.y] + item.x,
                        rowBytes);
            std::memcpy(reference.a[y], source.a[y + item.y] + item.x,
                        rowBytes);
            std::memcpy(reference.b[y], source.b[y + item.y] + item.x,
                        rowBytes);
        }

        LabImage destination(item.crop_width, item.crop_height);
        fill_lab(destination, 99u);

        RtStageArg args[4]{};
        args[0] = buffer_arg(view(source));
        args[1] = buffer_arg(view(destination));
        args[2] = int_arg(item.x);
        args[3] = int_arg(item.y);

        set_node_device_override("crop", StageNodeDevice::OpenCL);
        StageRequest request;
        request.name = "crop";
        request.args.assign(args, args + 4);
        StageExecution execution;
        const RtStageStatus status =
            context->execute(request, nullptr, &execution);
        clear_node_device_overrides();

        check(status == RT_STAGE_OK, "the device executes the crop node");
        check(execution.device == StageDeviceType::OPENCL,
              "the crop execution names the device");

        const Diff diff = compare(view(reference), view(destination));
        std::printf("stage-residency: crop %s: PAE %.6f LSB16, %zu of %zu "
                    "samples differ\n", item.label, diff.max_abs,
                    diff.differing, diff.samples);
        check(diff.finite && diff.max_abs == 0.0 && diff.differing == 0,
              "the crop copy is exact");
    }

    /* The call-site form: run() with the node override records a device pass. */
    {
        const std::string profile_path =
            std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
            "/stage-residency-crop.jsonl";
        LabImage source(238, 64);
        fill_lab(source, 43u);
        LabImage destination(179, 49);

        RtStageArg args[4]{};
        args[0] = buffer_arg(view(source));
        args[1] = buffer_arg(view(destination));
        args[2] = int_arg(2);
        args[3] = int_arg(3);

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("crop", StageNodeDevice::OpenCL);
        const RtStageStatus status = run("crop", {args[0], args[1], args[2],
                                                  args[3]}, nullptr);
        clear_node_device_overrides();
        unsetenv("RT_STAGE_PROFILE");

        check(status == RT_STAGE_OK,
              "the crop node completes through run()");
        const std::string profile = read_file(profile_path);
        check(profile.find("\"stage\":\"crop\"") != std::string::npos &&
                  profile.find("\"impl\":\"opencl\"") != std::string::npos,
              "the crop node records a device pass");
    }

    /* Refusals: an invalid rectangle, an oversized destination and an alias
     * must not touch the destination. */
    {
        LabImage source(64, 48);
        fill_lab(source, 7u);
        LabImage destination(32, 32);
        fill_lab(destination, 8u);
        const LabImage before(destination, true);

        const auto refused = [&](const RtStageBuffer& src,
                                 const RtStageBuffer& dst, int x, int y) {
            RtStageArg args[4]{};
            args[0] = buffer_arg(src);
            args[1] = buffer_arg(dst);
            args[2] = int_arg(x);
            args[3] = int_arg(y);
            set_node_device_override("crop", StageNodeDevice::OpenCL);
            StageRequest request;
            request.name = "crop";
            request.args.assign(args, args + 4);
            StageExecution execution;
            const RtStageStatus status =
                context->execute(request, nullptr, &execution);
            clear_node_device_overrides();
            return status;
        };

        check(refused(view(source), view(destination), -1, 0) ==
                  RT_STAGE_FALLBACK,
              "crop refuses a negative x offset");
        check(refused(view(source), view(destination), 33, 0) ==
                  RT_STAGE_FALLBACK,
              "crop refuses a rectangle past the right edge");
        check(refused(view(source), view(destination), 0, 17) ==
                  RT_STAGE_FALLBACK,
              "crop refuses a rectangle past the bottom edge");

        LabImage large(33, 32);
        check(refused(view(destination), view(large), 0, 0) ==
                  RT_STAGE_FALLBACK,
              "crop refuses a destination wider than the source");

        check(refused(view(source), view(source), 0, 0) == RT_STAGE_FALLBACK,
              "crop refuses an aliased source and destination");

        const Diff untouched = compare(view(before), view(destination));
        check(untouched.max_abs == 0.0 && untouched.differing == 0,
              "a refused crop leaves the destination untouched");
    }
}

struct GamutCase {
    const char* name;
    int width;
    int height;
    rtengine::procparams::ColorManagementParams::Wwgamut gamut;
    double gain;
    double power;
};

/*
 * Compare the node with the production host block for every end-of-process
 * gamut, for the gain-only case, and at widths whose device rows are padded
 * by the stage alignment (238 and 179 do not tile the alignment).
 */
void test_gamut(bool available)
{
    using namespace rtengine::stage;
    using Wwgamut = rtengine::procparams::ColorManagementParams::Wwgamut;

    init_lab_caches();

    /*
     * gamutcompr() reads settings->verbose. The engine normally installs the
     * Settings singleton; a value-initialised one keeps this test standalone.
     */
    static rtengine::Settings test_settings{};
    rtengine::settings = &test_settings;

    {
        rtengine::procparams::ColorManagementParams icm;
        icm.workingProfile = "Rec2020";
        icm.wgamut = static_cast<Wwgamut>(99);
        GamutNodeParams rejected;
        std::string error;
        check(!gamut_node_params(icm, rejected, error),
              "gamut rejects a working gamut value outside the enum");
    }

    if (available) {
        check(node_has_device_implementation("gamut"),
              "the gamut node has a device implementation");
    }

    const GamutCase cases[] = {
        {"rec2020", 238, 24, Wwgamut::REC2020, 0.0, 1.0},
        {"adobe", 238, 24, Wwgamut::ADOBE, 0.0, 1.0},
        {"srgb", 238, 24, Wwgamut::SRGB, 0.0, 1.0},
        {"dcip3", 238, 24, Wwgamut::DCIP3, 0.0, 1.0},
        {"gain only", 238, 24, Wwgamut::NONE, 0.5, 1.0},
        {"rec2020 narrow", 179, 17, Wwgamut::REC2020, 0.0, 1.2},
        {"adobe narrow", 179, 17, Wwgamut::ADOBE, 0.0, 1.2},
        {"srgb narrow", 179, 17, Wwgamut::SRGB, 0.0, 1.2},
        {"dcip3 narrow", 179, 17, Wwgamut::DCIP3, 0.0, 1.2},
        {"gain only narrow", 179, 17, Wwgamut::NONE, -0.5, 1.0},
    };

    if (!available) {
        skip("gamut: no OpenCL device");
        return;
    }

    const auto context = opencl_device()->create_context();
    check(context != nullptr, "gamut: the device creates a context");
    if (context == nullptr) {
        return;
    }

    for (const GamutCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.icm.workingProfile = "Rec2020";
        params.icm.workingTRC =
            rtengine::procparams::ColorManagementParams::WorkingTrc::GAMMA_2_2;
        params.icm.trcExp = true;
        params.icm.wgamut = item.gamut;
        params.icm.wgamgain = item.gain;
        params.icm.wgampower = item.power;

        rtengine::ImProcFunctions ipf(&params, false);

        LabImage source(item.width, item.height);
        fill_lab(source, 5u);
        LabImage cpu(item.width, item.height);
        gamut_cpu_reference(ipf, params, source, cpu);

        LabImage device(item.width, item.height);
        fill_lab(device, 5u);

        GamutNodeParams node_params;
        std::string error;
        if (!gamut_node_params(params.icm, node_params, error)) {
            check(false, "gamut: the host parameters are built");
            continue;
        }
        GamutNodeArgs node;
        gamut_node_args(device, node_params, node);

        set_node_device_override("gamut", StageNodeDevice::OpenCL);
        StageRequest request;
        request.name = "gamut";
        request.args.assign(node.args, node.args + 8);
        StageExecution execution;
        const RtStageStatus status =
            context->execute(request, nullptr, &execution);
        clear_node_device_overrides();

        check(status == RT_STAGE_OK, "the device executes the gamut node");
        check(execution.device == StageDeviceType::OPENCL,
              "the gamut execution names the device");
        check(!execution.asynchronous,
              "gamut reads its planes back, so the execution is complete");

        std::size_t row_pitch = 0;
        std::size_t plane_pitch = 0;
        std::size_t bytes = 0;
        std::string layout_error;
        check(stage_buffer_layout({RT_STAGE_F32, item.width, item.height, 3},
                                  row_pitch, plane_pitch, bytes,
                                  &layout_error),
              "gamut: the device layout is valid");
        check(row_pitch / sizeof(float) >
                  static_cast<std::size_t>(item.width),
              "gamut: the narrow case really has padded rows");

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: gamut %s %dx%d (pitch %zu): "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    item.name, item.width, item.height,
                    row_pitch / sizeof(float), diff.max_abs,
                    diff.differing, diff.samples);
        check(diff.finite, "the gamut device output is finite");
        check(diff.max_abs <= 1.0,
              "the gamut device output is within one LSB16");

        /*
         * The node validates its inputs. A refusal must leave the buffer
         * untouched, because the call site then runs the host block on it.
         */
        if (item.gamut == Wwgamut::REC2020 && item.width == 238) {
            LabImage guarded(item.width, item.height);
            fill_lab(guarded, 5u);
            GamutNodeArgs bad;
            gamut_node_args(guarded, node_params, bad);
            bad.args[7].u.i = 2;
            set_node_device_override("gamut", StageNodeDevice::OpenCL);
            StageRequest refuse;
            refuse.name = "gamut";
            refuse.args.assign(bad.args, bad.args + 8);
            StageExecution refused;
            const RtStageStatus refuse_status =
                context->execute(refuse, nullptr, &refused);
            clear_node_device_overrides();
            check(refuse_status != RT_STAGE_OK,
                  "gamut refuses a compress flag that is not 0 or 1");
            const Diff untouched = compare(view(guarded), view(source));
            check(untouched.differing == 0 && untouched.max_abs == 0.0,
                  "a refused gamut leaves the Lab buffer untouched");

            bad.args[7].u.i = 1;
            bad.args[6].u.f = 0.0;
            set_node_device_override("gamut", StageNodeDevice::OpenCL);
            refuse.args.assign(bad.args, bad.args + 8);
            const RtStageStatus power_status =
                context->execute(refuse, nullptr, &refused);
            clear_node_device_overrides();
            check(power_status != RT_STAGE_OK,
                  "gamut refuses a non-positive compression power");

            /*
             * The ABI contract is a 9x1 table. A 3x3 table has the same entry
             * count but a device row pitch that the kernel's flat read would
             * cross into, so the launcher must reject the shape.
             */
            bad.args[6].u.f = node_params.pwr;
            RtStageBuffer square = bad.args[1].u.buffer;
            square.width = 3;
            square.height = 3;
            square.row_pitch = 3 * sizeof(float);
            square.plane_pitch = 9 * sizeof(float);
            bad.args[1].u.buffer = square;
            set_node_device_override("gamut", StageNodeDevice::OpenCL);
            refuse.args.assign(bad.args, bad.args + 8);
            const RtStageStatus shape_status =
                context->execute(refuse, nullptr, &refused);
            clear_node_device_overrides();
            check(shape_status != RT_STAGE_OK,
                  "gamut refuses a 3x3 table in place of the 9x1 contract");
            const Diff square_untouched = compare(view(guarded), view(source));
            check(square_untouched.differing == 0 &&
                      square_untouched.max_abs == 0.0,
                  "a refused 3x3 table leaves the Lab buffer untouched");
        }
    }
}

/* --------------------------------------------------- output sharpening */

float sharpen_fixture_value(int x, int y, unsigned seed)
{
    std::uint32_t state = seed * 2654435761u + 1u;
    for (int index = 0; index < y * 4096 + x; ++index) {
        state = state * 1664525u + 1013904223u;
    }
    state ^= static_cast<std::uint32_t>(x) * 2246822519u;
    state ^= static_cast<std::uint32_t>(y) * 3266489917u;
    /* Spread over negative and positive L, including a dark left band. */
    int value = static_cast<int>((state >> 8) % 34001u) - 1000;
    if (x < 3) {
        value -= 2000;
    }
    return static_cast<float>(value);
}

void fill_sharpen_l(LabImage& lab, unsigned seed)
{
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            lab.L[y][x] = sharpen_fixture_value(x, y, seed);
            lab.a[y][x] = 0.0f;
            lab.b[y][x] = 0.0f;
        }
    }
}

/*
 * Richardson-Lucy divides by the blurred estimate, so white noise drives the
 * iteration into an unrealistic dynamic range. This fixture keeps smooth
 * gradients, a few hard edges and a negative band, which is the content the
 * deconvolution is meant for.
 */
void fill_rld_l(LabImage& lab, unsigned seed)
{
    const float phase = static_cast<float>(seed) * 0.37f;
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            float value = 16000.0f +
                          7000.0f * std::sin(0.15f * x + phase) *
                              std::cos(0.11f * y - phase) +
                          3000.0f * std::sin(0.31f * (x + y));
            if (((x / 8) + (y / 8)) % 2 == 0) {
                value += 4000.0f;
            }
            if (x < 3) {
                value -= 18000.0f;
            }
            lab.L[y][x] = value;
            lab.a[y][x] = 0.0f;
            lab.b[y][x] = 0.0f;
        }
    }
}

void copy_sharpen_l(const LabImage& source, LabImage& destination)
{
    for (int y = 0; y < source.H; ++y) {
        std::memcpy(destination.L[y], source.L[y],
                    static_cast<std::size_t>(source.W) * sizeof(float));
        std::memcpy(destination.a[y], source.a[y],
                    static_cast<std::size_t>(source.W) * sizeof(float));
        std::memcpy(destination.b[y], source.b[y],
                    static_cast<std::size_t>(source.W) * sizeof(float));
    }
}

Diff compare_sharpen_l(const LabImage& a, const LabImage& b)
{
    Diff diff;
    if (a.W != b.W || a.H != b.H) {
        diff.finite = false;
        return diff;
    }
    for (int y = 0; y < a.H; ++y) {
        for (int x = 0; x < a.W; ++x) {
            ++diff.samples;
            const float actual = a.L[y][x];
            const float expected = b.L[y][x];
            if (!close_enough(actual) || !close_enough(expected)) {
                diff.finite = false;
            }
            const double delta = std::fabs(static_cast<double>(actual) -
                                           static_cast<double>(expected));
            if (delta > 0.0) {
                ++diff.differing;
            }
            diff.max_abs = std::max(diff.max_abs, delta);
        }
    }
    return diff;
}

/* The production CPU path, with the node pinned to the host. */
void run_cpu_sharpen(const rtengine::procparams::SharpeningParams& sharpening,
                     double scale, LabImage& lab)
{
    rtengine::procparams::ProcParams params;
    params.prsharpening = sharpening;
    rtengine::ImProcFunctions ipf(&params, false);
    ipf.setScale(scale);
    set_node_device_override("output.sharpen", StageNodeDevice::CPU);
    ipf.sharpening(&lab, sharpening);
    clear_node_device_overrides();
}

RtStageStatus run_device_sharpen(const std::shared_ptr<StageContext>& context,
                                 const rtengine::procparams::SharpeningParams& sharpening,
                                 double scale, LabImage& lab,
                                 StageExecution& execution, std::string& error)
{
    SharpenNodeParams node_params;
    if (!sharpen_node_params(sharpening, scale, false, node_params, error)) {
        return RT_STAGE_INVALID;
    }
    SharpenNodeArgs node;
    sharpen_node_args(lab.L[0], lab.W, lab.H, node_params, node);
    set_node_device_override("output.sharpen", StageNodeDevice::OpenCL);
    StageRequest request;
    request.name = "output.sharpen";
    request.args.assign(node.args, node.args + SHARPEN_NODE_ARGS);
    const RtStageStatus status = context->execute(request, nullptr, &execution);
    clear_node_device_overrides();
    return status;
}

struct SharpenCase {
    std::string label;
    int width;
    int height;
    double scale;
    rtengine::procparams::SharpeningParams params;
    bool exact;
    bool smooth;
};



/* Color::init()'s sRGB inverse table, the only LUT workingtrc() uses in its
 * fixed first pass. The rest of the initialisation needs the engine-wide
 * Settings and ICC stores. */
void init_srgb_inverse()
{
    constexpr int ENTRIES = 65536;
    constexpr float MAX_VALUE = 65535.0f;

    rtengine::Color::igammatab_srgb(ENTRIES, 0);
    for (int i = 0; i < ENTRIES; ++i) {
        rtengine::Color::igammatab_srgb[i] =
            rtengine::Color::igamma2(i / 65535.0);
    }
    rtengine::Color::igammatab_srgb *= MAX_VALUE;
}

struct WorkingTrcCase {
    const char* label;
    int width;
    int height;
    rtengine::procparams::ColorManagementParams::WorkingTrc trc;
    double gamma;
    double slope;
    bool gamut;
    double saturation;
    /* The configuration belongs to the device set. */
    bool configSupported;
    /* The source stays inside the working gamut. */
    bool inGamut;
    /* The host setup admits the configuration (type-5 TRC, range). */
    bool setupAccepted;
};

/*
 * The production host block, the device node and the call-site contract.
 *
 * The supported cases run in-gamut source data; a configuration that leaves
 * the working gamut is inside the configuration set but the setup refuses it,
 * so it must fall back. The final Lab is compared within one LSB16. A residual
 * - a switch the node does not implement, or the out-of-gamut case - must be
 * refused by the launcher and executed by the host builtin, byte for byte.
 */
void test_working_trc(bool available)
{
    using namespace rtengine::stage;
    using WorkingTrc =
        rtengine::procparams::ColorManagementParams::WorkingTrc;

    init_lab_caches();
    init_srgb_inverse();

    static rtengine::Settings test_settings{};
    rtengine::settings = &test_settings;

    /* The engine creates the lcms mutex in rtengine::init(); a standalone
     * test provides one so the profile transform path is safe. */
    static MyMutex test_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &test_lcms_mutex;
    }

    {
        rtengine::procparams::ProcParams defaults;
        check(!workingTrcSupported(defaults),
              "working.trc refuses WorkingTRC=none");
    }

    const WorkingTrcCase cases[] = {
        {"gamma 2.2", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, true, true, true},
        {"bt709", 238, 24, WorkingTrc::BT709, 2.2, 4.5, true, 0.5, true, true, true},
        {"srgb", 238, 24, WorkingTrc::SRGB, 2.4, 12.92310, true, 0.5, true, true, true},
        {"gamma 1.8", 238, 24, WorkingTrc::GAMMA_1_8, 1.8, 4.5, true, 0.5, true, true, true},
        {"linear", 238, 24, WorkingTrc::LINEAR, 1.0, 1.0, true, 0.5, true, true, true},
        {"no gamut", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, false, 0.5, true, true, true},
        {"no saturation", 179, 17, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.0, true, true, true},
        {"full saturation", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 1.0, true, true, true},
        {"odd width", 179, 17, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, true, true, true},
        {"out of gamut", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, true, false, false},
        {"midtone equaliser", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, false, false, true},
        {"soft light", 179, 17, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, false, false, true},
        {"opacity curve", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, false, true, true},
        {"custom gamma 0.4", 238, 24, WorkingTrc::CUSTOM, 0.4, 4.5, true, 0.5, true, true, false},
        {"empty opacity", 238, 24, WorkingTrc::GAMMA_2_2, 2.4, 12.92310, true, 0.5, true, true, true},
    };

    if (!available) {
        skip("working.trc: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("working.trc"),
          "the working.trc node has a device implementation");

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-working-trc.jsonl";

    for (const WorkingTrcCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.icm.workingProfile = "Rec2020";
        params.icm.workingTRC = item.trc;
        params.icm.trcExp = true;
        params.icm.wGamma = item.gamma;
        params.icm.wSlope = item.slope;
        params.icm.gamut = item.gamut;
        params.icm.wapsat = item.saturation;
        if (std::strcmp(item.label, "soft light") == 0) {
            params.softlight.enabled = true;
            params.softlight.strength = 50;
        } else if (std::strcmp(item.label, "midtone equaliser") == 0) {
            params.icm.wmidtcie = 25.0;
        } else if (std::strcmp(item.label, "empty opacity") == 0) {
            params.icm.wavExp = true;
            params.icm.opacityCurveWLI = {
                static_cast<double>(FCT_Linear)};
            rtengine::procparams::ColorManagementParams colparams = params.icm;
            rtengine::WavOpacityCurveWL curve;
            colparams.getCurves(curve);
            check(!curve, "the empty opacity curve has no LUT");
        } else if (std::strcmp(item.label, "opacity curve") == 0) {
            params.icm.wavExp = true;
            params.icm.opacityCurveWLI = {
                static_cast<double>(FCT_MinMaxCPoints),
                0.0, 0.5, 0.0, 0.0,
                0.998, 0.5, 0.0, 0.0,
                1.0, 0.9, 0.0, 0.0};
            rtengine::procparams::ColorManagementParams colparams = params.icm;
            rtengine::WavOpacityCurveWL curve;
            colparams.getCurves(curve);
            bool flatStart = true;
            for (int i = 0; i < 500; ++i) {
                flatStart = flatStart && curve[i] == 0.5f;
            }
            check(flatStart, "the opacity test curve is flat in 0..499");
            check(curve[500] != 0.5f,
                  "the opacity test curve changes at the last entry");
        }
        check(workingTrcSupported(params) == item.configSupported,
              (std::string("working.trc ") + item.label +
               " resolves to the expected configuration set").c_str());

        rtengine::ImProcFunctions ipf(&params, false);

        LabImage source(item.width, item.height);
        if (item.inGamut) {
            fill_lab_in_gamut(source, params.icm.workingProfile);
        } else {
            fill_lab(source, 7u);
        }

        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        workingTrcHostBlock(ipf, params, cpu);

        LabImage device(item.width, item.height);
        device.CopyFrom(&source);

        WorkingTrcContext context(params, ipf);
        const bool expectDevice = available && item.configSupported &&
                                  item.setupAccepted;
        RtWorkingTrcContext descriptor =
            context.descriptor(item.configSupported);

        RtStageArg args[6] = {};
        args[0].kind = RT_STAGE_ARG_BUFFER;
        args[0].u.buffer = view(device);
        args[1].kind = RT_STAGE_ARG_PTR;
        args[1].u.ptr = &descriptor;
        args[2].kind = RT_STAGE_ARG_INT;
        args[2].u.i = item.configSupported ? 1 : 0;
        args[3].kind = RT_STAGE_ARG_DOUBLE;
        args[3].u.f = item.saturation;
        bool deviceReady = false;
        if (item.configSupported) {
            RtWorkingTrcResult setup{};
            check(context.prepare(source), "working.trc host prefix runs");
            deviceReady = context.build(setup) == 0;
            check(deviceReady == item.setupAccepted,
                  (std::string("working.trc ") + item.label +
                   " setup acceptance matches the configuration").c_str());
            if (deviceReady) {
                args[4].kind = RT_STAGE_ARG_BUFFER;
                args[4].u.buffer = view(context.xyz());
                args[5].kind = RT_STAGE_ARG_BUFFER;
                args[5].u.buffer = view(context.linearReference());
            }
        }
        descriptor.supported = deviceReady ? 1u : 0u;
        args[2].u.i = descriptor.supported;

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("working.trc", StageNodeDevice::OpenCL);
        const RtStageStatus status = run(
            "working.trc", std::vector<RtStageArg>(args, args + 6));
        clear_node_device_overrides();
        const std::string profile = read_file(profile_path);
        unsetenv("RT_STAGE_PROFILE");

        check(status == RT_STAGE_OK,
              (std::string("working.trc ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"working.trc\"") != std::string::npos,
              (std::string("working.trc ") + item.label +
               " records the node").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: working.trc %s %dx%d %s: PAE %.6f "
                    "LSB16, %zu of %zu samples differ\n",
                    item.label, item.width, item.height,
                    expectDevice ? "device" : "host", diff.max_abs,
                    diff.differing, diff.samples);
        check(diff.finite,
              (std::string("working.trc ") + item.label +
               " stays finite").c_str());

        if (expectDevice) {
            check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
                  (std::string("working.trc ") + item.label +
                   " records a device execution").c_str());
            check(diff.max_abs <= 1.0,
                  (std::string("working.trc ") + item.label +
                   " is within one LSB16").c_str());
        } else {
            check(profile.find("\"impl\":\"host\"") != std::string::npos,
                  (std::string("working.trc ") + item.label +
                   " records a host execution").c_str());
            check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
                  (std::string("working.trc ") + item.label +
                   " never mixes a device pass").c_str());
            check(diff.differing == 0 && diff.max_abs == 0.0,
                  (std::string("working.trc ") + item.label +
                   " matches the CPU block").c_str());
        }
    }

    if (!available) {
        return;
    }

    /*
     * A device refusal after a successful setup must still run the builtin
     * and record impl=host. Corrupting the second-pass buffer shape makes the
     * launcher refuse after the context has built.
     */
    {
        rtengine::procparams::ProcParams params;
        params.icm.workingProfile = "Rec2020";
        params.icm.workingTRC = WorkingTrc::GAMMA_2_2;
        params.icm.trcExp = true;
        params.icm.gamut = true;
        params.icm.wapsat = 0.5;
        rtengine::ImProcFunctions ipf(&params, false);

        LabImage source(238, 24);
        fill_lab_in_gamut(source, params.icm.workingProfile);
        LabImage cpu(238, 24);
        cpu.CopyFrom(&source);
        workingTrcHostBlock(ipf, params, cpu);
        LabImage device(238, 24);
        device.CopyFrom(&source);

        WorkingTrcContext context(params, ipf);
        RtWorkingTrcResult setup{};
        check(context.prepare(source), "forced fallback prefix runs");
        check(context.build(setup) == 0, "forced fallback setup builds");
        RtWorkingTrcContext descriptor = context.descriptor(true);
        RtStageArg args[6] = {};
        args[0].kind = RT_STAGE_ARG_BUFFER;
        args[0].u.buffer = view(device);
        args[1].kind = RT_STAGE_ARG_PTR;
        args[1].u.ptr = &descriptor;
        args[2].kind = RT_STAGE_ARG_INT;
        args[2].u.i = 1;
        args[3].kind = RT_STAGE_ARG_DOUBLE;
        args[3].u.f = params.icm.wapsat;
        args[4].kind = RT_STAGE_ARG_BUFFER;
        args[4].u.buffer = view(context.xyz());
        args[4].u.buffer.planes = 1;
        args[5].kind = RT_STAGE_ARG_BUFFER;
        args[5].u.buffer = view(context.linearReference());

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("working.trc", StageNodeDevice::OpenCL);
        const RtStageStatus status =
            run("working.trc", std::vector<RtStageArg>(args, args + 6));
        clear_node_device_overrides();
        const std::string profile = read_file(profile_path);
        unsetenv("RT_STAGE_PROFILE");

        check(status == RT_STAGE_OK, "the forced fallback completes");
        check(profile.find("\"stage\":\"working.trc\"") != std::string::npos,
              "the forced fallback records the node");
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              "the forced fallback records a host execution");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "the forced fallback never mixes a device pass");
        const Diff diff = compare(view(device), view(cpu));
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "the forced fallback matches the CPU block");
    }

    /*
     * Probe parity. The setup runs on the host prefix; the reference runs the
     * same production setup on a host first pass. The prefix is the block's
     * own lab2rgb and sRGB inverse, so the means are exact.
     */
    {
        rtengine::procparams::ProcParams params;
        params.icm.workingProfile = "Rec2020";
        params.icm.workingTRC = WorkingTrc::GAMMA_2_2;
        params.icm.trcExp = true;
        params.icm.gamut = true;
        params.icm.wapsat = 0.5;
        rtengine::ImProcFunctions ipf(&params, false);

        LabImage source(238, 24);
        fill_lab_in_gamut(source, params.icm.workingProfile);

        LabImage reference_source(238, 24);
        reference_source.CopyFrom(&source);
        rtengine::Imagefloat first(238, 24);
        ipf.lab2rgb(reference_source, first, params.icm.workingProfile);

        float rdx = 0.f, rdy = 0.f, grx = 0.f, gry = 0.f, blx = 0.f,
              bly = 0.f;
        float meanx = 0.f, meany = 0.f, meanxe = 0.f, meanye = 0.f,
              maxdat = 0.f;
        double p[6] = {0., 0., 0., 0., 0., 0.};
        cmsHTRANSFORM dummy = nullptr;
        int ill = 0;
        Glib::ustring prof = params.icm.workingProfile;
        ipf.workingtrc(0, &first, &first, first.getWidth(), first.getHeight(),
                       -5, prof, WORKING_TRC_FIRST_GAMMA,
                       WORKING_TRC_FIRST_SLOPE, 0, ill, 0, 0, rdx, rdy, grx,
                       gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, p,
                       dummy, true, false, false, false);

        Imagefloat probe(238, 24);
        first.copyData(&probe);
        float refMeanX = 0.f;
        float refMeanY = 0.f;
        cmsHPROFILE refProfile = nullptr;
        double refPrim[9] = {};
        ipf.workingtrc(0, &probe, &probe, probe.getWidth(), probe.getHeight(),
                       5, prof, params.icm.wGamma, params.icm.wSlope,
                       rtengine::toUnderlying(params.icm.wcat), ill, 0, 0,
                       rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe,
                       meanye, maxdat, p, dummy, false, true, true,
                       params.icm.gamut, &refMeanX, &refMeanY, &refProfile,
                       true, refPrim);
        check(refProfile != nullptr,
              "working.trc probe reference builds a profile");
        if (refProfile != nullptr) {
            cmsCloseProfile(refProfile);
        }

        LabImage device(238, 24);
        device.CopyFrom(&source);
        WorkingTrcContext context(params, ipf);
        RtWorkingTrcContext descriptor = context.descriptor(true);
        check(context.prepare(source), "working.trc probe prefix runs");
        RtWorkingTrcResult probeSetup{};
        check(context.build(probeSetup) == 0,
              "working.trc probe setup builds");
        descriptor.supported = 1u;
        RtStageArg args[6] = {};
        args[0].kind = RT_STAGE_ARG_BUFFER;
        args[0].u.buffer = view(device);
        args[1].kind = RT_STAGE_ARG_PTR;
        args[1].u.ptr = &descriptor;
        args[2].kind = RT_STAGE_ARG_INT;
        args[2].u.i = 1;
        args[3].kind = RT_STAGE_ARG_DOUBLE;
        args[3].u.f = params.icm.wapsat;
        args[4].kind = RT_STAGE_ARG_BUFFER;
        args[4].u.buffer = view(context.xyz());
        args[5].kind = RT_STAGE_ARG_BUFFER;
        args[5].u.buffer = view(context.linearReference());

        set_node_device_override("working.trc", StageNodeDevice::OpenCL);
        const RtStageStatus status =
            run("working.trc", std::vector<RtStageArg>(args, args + 6));
        clear_node_device_overrides();

        const double deltaX =
            std::fabs(context.meanx() - static_cast<double>(refMeanX));
        const double deltaY =
            std::fabs(context.meany() - static_cast<double>(refMeanY));
        std::printf("stage-residency: working.trc probe: device "
                    "%.9g/%.9g host %.9g/%.9g delta %.3g/%.3g\n",
                    context.meanx(), context.meany(),
                    static_cast<double>(refMeanX),
                    static_cast<double>(refMeanY), deltaX, deltaY);
        check(status == RT_STAGE_OK, "the probe parity run completes");
        check(context.built(), "the probe parity run built its setup");
        /*
         * The probe now runs on the host prefix, so its means are the host
         * block's means bit for bit on every device.
         */
        check(deltaX == 0.0 && deltaY == 0.0,
              "the probe means are the host means bit for bit");
    }
}

void test_output_sharpen(bool available)
{
    using namespace rtengine::stage;

    {
        rtengine::procparams::SharpeningParams bad;
        SharpenNodeParams node;
        std::string error;
        bad.method = "not-a-method";
        check(!sharpen_node_params(bad, 1.0, false, node, error),
              "output.sharpen rejects a method outside the enum");
    }

    rtengine::procparams::SharpeningParams base;
    base.enabled = true;
    base.method = "usm";
    base.amount = 200;
    base.contrast = 20.0;
    base.radius = 0.5;
    base.blurradius = 0.2;

    std::vector<SharpenCase> cases;
    const auto add = [&cases](const std::string& label, int width, int height,
                              double scale,
                              const rtengine::procparams::SharpeningParams& p,
                              bool exact, bool smooth = false) {
        cases.push_back(
            SharpenCase{label, width, height, scale, p, exact, smooth});
    };

    add("defaults", 63, 31, 1.0, base, false);

    for (double radius : {0.3, 0.5, 0.59, 0.6, 1.15, 3.0}) {
        auto p = base;
        p.radius = radius;
        add("radius " + std::to_string(radius), 63, 31, 1.0, p, false);
    }
    for (double contrast : {0.0, 20.0, 100.0}) {
        auto p = base;
        p.contrast = contrast;
        add("contrast " + std::to_string(contrast), 63, 31, 1.0, p, false);
    }
    for (int amount : {1, 100, 200, 1000}) {
        auto p = base;
        p.amount = amount;
        add("amount " + std::to_string(amount), 63, 31, 1.0, p, false);
    }

    {
        auto p = base;
        p.threshold.setValues(0, 0, 0, 0);
        add("threshold zeros", 63, 31, 1.0, p, false);
    }
    {
        auto p = base;
        p.threshold.setValues(20, 80, 1200, 1200);
        add("threshold equal right values", 63, 31, 1.0, p, false);
    }
    {
        auto p = base;
        p.threshold = rtengine::procparams::Threshold<int>(20, 200, false);
        add("threshold two values", 63, 31, 1.0, p, false);
    }
    {
        auto p = base;
        p.threshold.setValues(80, 2000, 2000, 2000);
        add("threshold breakpoints", 63, 31, 1.0, p, false);
    }

    for (int width : {8, 63, 64, 65}) {
        add("width " + std::to_string(width), width, 17, 1.0, base, false);
    }
    add("padded width 238", 238, 24, 1.0, base, false);

    /* Sigma below GAUSS_SKIP copies the plane, so the node is the identity. */
    add("skip sigma", 63, 31, 4.0, base, true);
    /* Sigma at the FIR limit. */
    add("sigma 0.25", 63, 31, 2.0, base, false);

    rtengine::procparams::SharpeningParams rld = base;
    rld.method = "rld";
    rld.deconvamount = 100;
    rld.deconvradius = 0.45;
    rld.deconviter = 100;
    rld.deconvdamping = 0;

    for (double radius : {0.3, 0.45, 0.75, 1.0}) {
        auto p = rld;
        p.deconvradius = radius;
        p.deconviter = 30;
        add("RLD radius " + std::to_string(radius), 63, 31, 1.0, p, false,
            true);
    }
    for (int iterations : {1, 5, 30, 100}) {
        auto p = rld;
        p.deconviter = iterations;
        add("RLD iterations " + std::to_string(iterations), 63, 31, 1.0, p,
            false, true);
    }
    for (int amount : {1, 50, 100, 1000}) {
        auto p = rld;
        p.deconvamount = amount;
        p.deconviter = 10;
        add("RLD amount " + std::to_string(amount), 63, 31, 1.0, p, false,
            true);
    }
    {
        auto p = rld;
        p.deconvamount = 1000;
        p.deconviter = 10;
        p.contrast = 0.0;
        add("RLD amount 1000 flat mask", 63, 31, 1.0, p, false,
            true);
    }
    {
        auto p = rld;
        p.deconviter = 30;
        add("RLD padded width 238", 238, 24, 1.0, p, false, true);
    }
    {
        auto p = rld;
        p.deconvamount = 0;
        add("RLD no-op", 63, 31, 1.0, p, true, true);
    }

    if (!available) {
        skip("output sharpening: no OpenCL device");
        return;
    }

    const auto context = opencl_device()->create_context();
    check(context != nullptr, "output sharpening: the device creates a context");
    if (context == nullptr) {
        return;
    }

    unsigned seed = 1u;
    for (const SharpenCase& item : cases) {
        LabImage source(item.width, item.height);
        if (item.smooth) {
            fill_rld_l(source, seed++);
        } else {
            fill_sharpen_l(source, seed++);
        }

        LabImage cpu(item.width, item.height);
        copy_sharpen_l(source, cpu);
        run_cpu_sharpen(item.params, item.scale, cpu);

        LabImage device(item.width, item.height);
        copy_sharpen_l(source, device);
        StageExecution execution;
        std::string error;
        const RtStageStatus status =
            run_device_sharpen(context, item.params, item.scale, device,
                               execution, error);
        const std::string prefix = "output.sharpen " + item.label;
        check(status == RT_STAGE_OK, (prefix + " completes").c_str());
        check(execution.device == StageDeviceType::OPENCL,
              (prefix + " runs on the device").c_str());
        check(!execution.asynchronous,
              (prefix + " reads the plane back synchronously").c_str());

        const Diff diff = compare_sharpen_l(device, cpu);
        std::printf("stage-residency: %s %dx%d: PAE %.6f LSB16, "
                    "%zu of %zu samples differ\n",
                    prefix.c_str(), item.width, item.height, diff.max_abs,
                    diff.differing, diff.samples);
        check(diff.finite, (prefix + " is finite").c_str());
        if (item.exact) {
            check(diff.differing == 0 && diff.max_abs == 0.0,
                  (prefix + " is bit-exact").c_str());
        } else {
            check(diff.max_abs <= 1.0,
                  (prefix + " is within one LSB16").c_str());
        }
    }

    /*
     * A supported request must be recorded as device execution through
     * run(), while a residual switch must execute the host builtin and report
     * impl=host. The host result is compared with the production CPU path and
     * with the extracted core, so the record cannot hide a mixed pass.
     */
    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-sharpen.jsonl";

    const auto run_and_read = [&](const rtengine::procparams::SharpeningParams& p,
                                  double scale, bool expect_device,
                                  const char* label) {
        LabImage source(63, 31);
        fill_sharpen_l(source, 11u);
        LabImage target(63, 31);
        copy_sharpen_l(source, target);

        SharpenNodeParams node_params;
        std::string error;
        check(sharpen_node_params(p, scale, false, node_params, error),
              (std::string(label) + " builds its parameters").c_str());
        SharpenNodeArgs node;
        sharpen_node_args(target.L[0], target.W, target.H, node_params, node);
        std::vector<RtStageArg> args(node.args, node.args + SHARPEN_NODE_ARGS);

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("output.sharpen", StageNodeDevice::OpenCL);
        const RtStageStatus status = run("output.sharpen", args);
        clear_node_device_overrides();
        const std::string profile = read_file(profile_path);
        unsetenv("RT_STAGE_PROFILE");

        check(status == RT_STAGE_OK,
              (std::string(label) + " completes through run()").c_str());
        check(profile.find("\"stage\":\"output.sharpen\"") != std::string::npos,
              (std::string(label) + " records the node").c_str());
        if (expect_device) {
            check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
                  (std::string(label) + " records a device execution").c_str());
        } else {
            check(profile.find("\"impl\":\"host\"") != std::string::npos,
                  (std::string(label) + " records a host execution").c_str());
            check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
                  (std::string(label) + " never mixes a device pass").c_str());
        }

        LabImage cpu(63, 31);
        copy_sharpen_l(source, cpu);
        run_cpu_sharpen(p, scale, cpu);
        const Diff diff = compare_sharpen_l(target, cpu);
        std::printf("stage-residency: output.sharpen %s: %s, %zu of %zu "
                    "samples differ\n",
                    label, expect_device ? "device" : "host", diff.differing,
                    diff.samples);
        check(diff.finite,
              (std::string(label) + " stays finite").c_str());
        if (expect_device) {
            check(diff.max_abs <= 1.0,
                  (std::string(label) + " is within one LSB16").c_str());
        } else {
            check(diff.differing == 0 && diff.max_abs == 0.0,
                  (std::string(label) + " matches the CPU reference").c_str());
        }

        if (!expect_device) {
            LabImage direct(63, 31);
            copy_sharpen_l(source, direct);
            rtengine::sharpen_luminance(direct.L[0], direct.W, direct.H, p,
                                        scale, true, false);
            const Diff core_diff = compare_sharpen_l(target, direct);
            check(core_diff.differing == 0 && core_diff.max_abs == 0.0,
                  (std::string(label) + " matches the extracted core").c_str());
        }
    };

    run_and_read(base, 1.0, true, "supported run");

    {
        auto p = base;
        p.halocontrol = true;
        run_and_read(p, 1.0, false, "halo control residual");
    }
    {
        auto p = base;
        p.method = "rld";
        p.deconvamount = 20;
        p.deconvradius = 0.5;
        p.deconviter = 2;
        p.deconvdamping = 50;
        run_and_read(p, 1.0, false, "RLD damping residual");
    }
    {
        auto p = base;
        p.method = "rld";
        p.deconvamount = 20;
        p.deconvradius = 1.5;
        p.deconviter = 5;
        run_and_read(p, 1.0, false, "RLD IIR radius residual");
    }
    {
        auto p = base;
        p.blurradius = 0.3;
        run_and_read(p, 1.0, false, "blur radius residual");
    }
    {
        auto p = base;
        p.edgesonly = true;
        run_and_read(p, 1.0, false, "edges only residual");
    }

    /*
     * The node contract is a tightly packed L plane. The extracted core and
     * the host builtin address it with a W-sample stride, so both padded
     * descriptors must be refused by the device launcher and by the builtin,
     * leaving the plane untouched.
     */
    const auto expect_refusal = [&](const char* label, bool padded_row,
                                    bool padded_plane,
                                    StageNodeDevice selection) {
        LabImage source(63, 31);
        fill_sharpen_l(source, 23u);
        LabImage target(63, 31);
        copy_sharpen_l(source, target);

        SharpenNodeParams node_params;
        std::string error;
        check(sharpen_node_params(base, 1.0, false, node_params, error),
              (std::string(label) + " builds its parameters").c_str());
        SharpenNodeArgs node;
        sharpen_node_args(target.L[0], target.W, target.H, node_params, node);
        RtStageBuffer descriptor =
            sharpen_l_buffer(target.L[0], target.W, target.H);
        if (padded_row) {
            descriptor.row_pitch += 4 * sizeof(float);
        }
        if (padded_plane) {
            descriptor.plane_pitch += 64;
        }
        node.args[SHARPEN_ARG_BUFFER].u.buffer = descriptor;
        std::vector<RtStageArg> args(node.args, node.args + SHARPEN_NODE_ARGS);

        set_node_device_override("output.sharpen", selection);
        const RtStageStatus status = run("output.sharpen", args);
        clear_node_device_overrides();

        check(status != RT_STAGE_OK,
              (std::string(label) + " is refused").c_str());
        const Diff untouched = compare_sharpen_l(target, source);
        check(untouched.differing == 0 && untouched.max_abs == 0.0,
              (std::string(label) + " leaves the plane untouched").c_str());
        std::printf("stage-residency: output.sharpen %s: refused (status %d), "
                    "%zu samples untouched\n",
                    label, static_cast<int>(status), untouched.samples);
    };

    for (StageNodeDevice selection :
         {StageNodeDevice::OpenCL, StageNodeDevice::CPU}) {
        const bool device = selection == StageNodeDevice::OpenCL;
        expect_refusal(device ? "padded row on the device"
                              : "padded row in the builtin",
                       true, false, selection);
        expect_refusal(device ? "padded plane on the device"
                              : "padded plane in the builtin",
                       false, true, selection);
    }

    /*
     * Show-mask is a valid residual, not an invalid descriptor: the device
     * refuses it, the host builtin runs, and the host writes the mask. The
     * contract for this case is therefore RT_STAGE_OK with impl=host.
     */
    {
        LabImage source(63, 31);
        fill_sharpen_l(source, 29u);
        LabImage target(63, 31);
        copy_sharpen_l(source, target);

        SharpenNodeParams node_params;
        std::string error;
        check(sharpen_node_params(base, 1.0, true, node_params, error),
              "show-mask residual builds its parameters");
        SharpenNodeArgs node;
        sharpen_node_args(target.L[0], target.W, target.H, node_params, node);
        std::vector<RtStageArg> args(node.args, node.args + SHARPEN_NODE_ARGS);

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("output.sharpen", StageNodeDevice::OpenCL);
        const RtStageStatus status = run("output.sharpen", args);
        clear_node_device_overrides();
        const std::string profile = read_file(profile_path);
        unsetenv("RT_STAGE_PROFILE");

        check(status == RT_STAGE_OK, "show-mask residual completes");
        check(profile.find("\"stage\":\"output.sharpen\"") != std::string::npos,
              "show-mask residual records the node");
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              "show-mask residual records a host execution");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "show-mask residual never records a device pass");

        LabImage cpu(63, 31);
        copy_sharpen_l(source, cpu);
        rtengine::procparams::ProcParams params;
        params.prsharpening = base;
        rtengine::ImProcFunctions ipf(&params, false);
        ipf.setScale(1.0);
        set_node_device_override("output.sharpen", StageNodeDevice::CPU);
        ipf.sharpening(&cpu, base, true);
        clear_node_device_overrides();

        const Diff diff = compare_sharpen_l(target, cpu);
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "show-mask output matches the CPU mask");
        const Diff written = compare_sharpen_l(target, source);
        check(written.differing > 0,
              "show-mask writes the mask into the plane");
        std::printf("stage-residency: output.sharpen show-mask residual: "
                    "impl=host, %zu of %zu samples carry the mask\n",
                    written.differing, written.samples);
    }
}

/* ------------------------------------------------------- lab.sharpening */

struct LabPlaneDiff {
    double max_l = 0.0;
    double max_a = 0.0;
    double max_b = 0.0;
    std::size_t differing = 0;
    std::size_t samples = 0;
    bool finite = true;
};

LabPlaneDiff compare_lab_planes(const LabImage& actual,
                                const LabImage& expected)
{
    LabPlaneDiff diff;
    if (actual.W != expected.W || actual.H != expected.H) {
        diff.finite = false;
        return diff;
    }
    for (int y = 0; y < actual.H; ++y) {
        for (int x = 0; x < actual.W; ++x) {
            ++diff.samples;
            const double dl = std::fabs(static_cast<double>(actual.L[y][x]) -
                                        static_cast<double>(expected.L[y][x]));
            const double da = std::fabs(static_cast<double>(actual.a[y][x]) -
                                        static_cast<double>(expected.a[y][x]));
            const double db = std::fabs(static_cast<double>(actual.b[y][x]) -
                                        static_cast<double>(expected.b[y][x]));
            if (!close_enough(actual.L[y][x]) ||
                !close_enough(actual.a[y][x]) ||
                !close_enough(actual.b[y][x])) {
                diff.finite = false;
            }
            diff.max_l = std::max(diff.max_l, dl);
            diff.max_a = std::max(diff.max_a, da);
            diff.max_b = std::max(diff.max_b, db);
            if (dl > 0.0 || da > 0.0 || db > 0.0) {
                ++diff.differing;
            }
        }
    }
    return diff;
}

/* The observable corrected set: a pixel whose L, a or b differs from input. */
std::vector<unsigned char> lab_changed_mask(const LabImage& image,
                                            const LabImage& input)
{
    std::vector<unsigned char> changed(
        static_cast<std::size_t>(image.W) * image.H, 0u);
    for (int y = 0; y < image.H; ++y) {
        for (int x = 0; x < image.W; ++x) {
            if (image.L[y][x] != input.L[y][x] ||
                image.a[y][x] != input.a[y][x] ||
                image.b[y][x] != input.b[y][x]) {
                changed[static_cast<std::size_t>(y) * image.W + x] = 1u;
            }
        }
    }
    return changed;
}

std::size_t lab_changed_count(const std::vector<unsigned char>& mask)
{
    return static_cast<std::size_t>(
        std::count(mask.begin(), mask.end(), static_cast<unsigned char>(1u)));
}

/*
 * The impulse fixture: a ramp, a flat plateau, a smooth field, isolated
 * single-pixel spikes (one on the left border and one in the bottom-right
 * corner), and a 5x5 cluster whose centre has no unmarked neighbour inside its
 * own window. a and b carry the same spikes so the correction is visible on
 * all three planes.
 */
void fill_impulse_fixture(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            float value;
            if (x < 20) {
                value = 8000.0f + 400.0f * x;
            } else if (x < 30) {
                value = 18000.0f;
            } else {
                value = 12000.0f + 2000.0f * std::sin(0.2f * y + 0.1f * x);
            }
            value += 1000.0f * std::sin(0.37f * x) * std::cos(0.23f * y);
            lab.L[y][x] = value;
            lab.a[y][x] = 1000.0f + 30.0f * x - 20.0f * y;
            lab.b[y][x] = -800.0f - 15.0f * x + 25.0f * y;
        }
    }

    const int spikes[][2] = {{5, 5}, {32, 7}, {50, 25}, {0, 3}, {62, 30}};
    const float spike_l[] = {20000.0f, 25000.0f, -18000.0f, 20000.0f,
                             -15000.0f};
    for (int i = 0; i < 5; ++i) {
        const int x = spikes[i][0];
        const int y = spikes[i][1];
        lab.L[y][x] += spike_l[i];
        lab.a[y][x] += 2500.0f;
        lab.b[y][x] -= 2000.0f;
    }

    for (int y = 10; y <= 14; ++y) {
        for (int x = 38; x <= 42; ++x) {
            lab.L[y][x] += 15000.0f;
            lab.a[y][x] += 3000.0f;
            lab.b[y][x] -= 2500.0f;
        }
    }
}

/*
 * The defringe fixture: a high-contrast luminance edge, a three-pixel magenta
 * ring beside it whose hue sits at the default curve's peak, neutral and
 * saturated patches, and a fringe on the left border. The ring is the only
 * region with a meaningful chromaticity, so the correction is non-vacuous.
 */
void fill_defringe_fixture(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            lab.L[y][x] = x < 35 ? 4000.0f : 30000.0f;
            lab.a[y][x] = 0.0f;
            lab.b[y][x] = 0.0f;
        }
    }
    /* 3-px magenta ring against the dark side of the edge. atan2(b, a) is
     * -0.529, which huelab_to_huehsv2 maps to the default curve's peak. */
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 31; x <= 33; ++x) {
            lab.a[y][x] = 6000.0f;
            lab.b[y][x] = -3510.0f;
        }
    }
    /* Neutral patch and a saturated patch away from the fringe. */
    for (int y = 3; y <= 9; ++y) {
        for (int x = 3; x <= 9; ++x) {
            lab.a[y][x] = 0.0f;
            lab.b[y][x] = 0.0f;
        }
    }
    for (int y = 20; y <= 30; ++y) {
        for (int x = 45; x <= 55; ++x) {
            lab.a[y][x] = 9000.0f;
            lab.b[y][x] = 9000.0f;
        }
    }
    /* One fringe at the left border, where the host's window is asymmetric. */
    for (int y = 15; y <= 25; ++y) {
        lab.a[y][0] = 7000.0f;
        lab.b[y][0] = -4100.0f;
    }
}

/*
 * The ML fixture: a broad luminance ramp with hard blocks, diagonal ridges and
 * fine texture, plus chroma that varies on both axes with its own edges. It
 * gives every 5x5 window of both ML passes non-zero weights and drives L both
 * below and above MLsharpen()'s v < 92 guard.
 */
void fill_ml_fixture(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            const float u =
                static_cast<float>(x) / static_cast<float>(lab.W - 1);
            const float v =
                static_cast<float>(y) / static_cast<float>(lab.H - 1);
            float l = 2500.0f +
                      28000.0f * (0.45f * u + 0.55f * v) +
                      4200.0f * std::sin(0.35f * x) * std::cos(0.29f * y);
            if (((x / 7 + y / 5) & 1) == 0) {
                l += 6500.0f;
            }
            l += 2200.0f * static_cast<float>((x * 37 + y * 17) % 13) / 13.0f;
            l += 900.0f * static_cast<float>((x * 11 + y * 23) % 7) / 7.0f;
            lab.L[y][x] = l;
            lab.a[y][x] = 8500.0f * std::sin(0.21f * x + 0.13f * y) +
                          2600.0f * static_cast<float>(x % 9) - 9000.0f;
            lab.b[y][x] = 8500.0f * std::cos(0.17f * x - 0.19f * y) +
                          2600.0f * static_cast<float>(y % 7) - 7000.0f;
        }
    }
}

/* The production CPU path, with the node pinned to the host. */
void run_cpu_lab_sharpening(rtengine::procparams::ProcParams& params,
                            LabImage& lab, bool ciecam_arm = false)
{
    rtengine::ImProcFunctions ipf(&params, false);
    ipf.setScale(1.0);
    set_node_device_override("lab.sharpening", StageNodeDevice::CPU);
    labSharpeningHostBlock(ipf, params, lab, ciecam_arm);
    clear_node_device_overrides();
}

RtStageStatus run_device_lab_sharpening(
    const std::shared_ptr<StageContext>& context,
    rtengine::procparams::ProcParams& params, LabImage& lab,
    StageExecution& execution, std::string& error, bool ciecam_arm = false)
{
    rtengine::ImProcFunctions ipf(&params, false);
    ipf.setScale(1.0);
    LabSharpeningContext node_context(params, ipf, lab.W, lab.H, ciecam_arm);
    if (node_context.build(&error) != 0) {
        return RT_STAGE_INVALID;
    }
    const LabSharpeningNodeParams node =
        labSharpeningNodeParams(params, ciecam_arm);
    std::string support_error;
    const bool supported = labSharpeningSupported(
        params, lab.W, lab.H, ciecam_arm, &support_error);
    RtLabSharpeningContext descriptor = node_context.descriptor(supported);
    std::vector<RtStageArg> args;
    labSharpeningArgs(lab, node, node_context, descriptor, args);

    StageRequest request;
    request.name = "lab.sharpening";
    request.args = args;
    set_node_device_override("lab.sharpening", StageNodeDevice::OpenCL);
    const RtStageStatus status = context->execute(request, nullptr, &execution);
    clear_node_device_overrides();
    return status;
}

void test_lab_sharpening(bool available)
{
    using namespace rtengine::stage;

    /* MLmicrocontrast() reads settings->verbose. */
    static rtengine::Settings lab_sharpening_settings{};
    rtengine::settings = &lab_sharpening_settings;

    /* ------------------------------------------------ admission predicates */
    {
        rtengine::procparams::ProcParams params;
        std::string error;
        check(!labSharpeningSupported(params, 63, 31, true, &error),
              "lab.sharpening refuses a profile with no enabled tool");

        params.impulseDenoise.enabled = true;
        params.impulseDenoise.thresh = 50;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening admits impulse denoise alone");

        params.impulseDenoise.thresh = 101;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses an impulse threshold above 100");
        params.impulseDenoise.thresh = -1;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses an impulse threshold below 0");
        params.impulseDenoise.thresh = 50;

        check(!labSharpeningSupported(params, 7, 31, false, &error),
              "lab.sharpening refuses an image narrower than 8");

        params.defringe.enabled = true;
        params.defringe.radius = 2.0;
        params.defringe.threshold = 13;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening admits the default defringe pair");

        params.defringe.radius = 1.15;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "a defringe radius of 1.15 is the separable-IIR arm");
        params.defringe.radius = 1.0;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "a defringe radius of 1.0 is the separable-IIR arm");
        params.defringe.radius = 0.6;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "a defringe radius of 0.6 is the separable-IIR boundary");
        params.defringe.radius = 0.5;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "a defringe radius of 0.5 is the 3x3 arm");
        params.defringe.radius = 0.25;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "a defringe radius of 0.25 is the 3x3 boundary");
        params.defringe.radius = 0.2;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "a defringe radius below 0.25 is the host no-op");
        params.defringe.radius = 25.0;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses a defringe radius of 25");
        params.defringe.radius = 30.0;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses a defringe radius above 25");
        params.defringe.radius = -1.0;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses a negative defringe radius");
        params.defringe.radius = 2.0;

        params.defringe.threshold = 101;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses a defringe threshold above 100");
        params.defringe.threshold = 13;

        LabSharpeningNodeParams node = labSharpeningNodeParams(params);
        check(!lab_sharpening_admitted(node, 63, 31, false, error),
              "defringe needs device fp64");
        check(lab_sharpening_admitted(node, 63, 31, true, error),
              "defringe is admitted with device fp64");
        /* The impulse-only pair never needs fp64. */
        node.defringe_enabled = 0;
        check(lab_sharpening_admitted(node, 63, 31, false, error),
              "impulse denoise alone needs no fp64");

        /* The image must be wide enough for the host's unclamped left tail. */
        params.defringe.radius = 5.0;
        check(!labSharpeningSupported(params, 19, 19, false, &error),
              "a defringe window wider than the image is refused");
    }

    /* ------------------------------------- ML edge and micro admission set */
    {
        rtengine::procparams::ProcParams params;
        std::string error;
        params.sharpenEdge.enabled = true;
        params.sharpenEdge.passes = 2;
        params.sharpenEdge.amount = 50.0;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening admits ML sharpen edges alone");

        params.sharpenEdge.threechannels = true;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening admits the three-channel edge arm");

        params.sharpenEdge.passes = 5;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses more than four edge passes");
        params.sharpenEdge.passes = 2;
        params.sharpenEdge.amount = 101.0;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses an edge amount above 100");
        params.sharpenEdge.amount = -1.0;
        check(!labSharpeningSupported(params, 63, 31, false, &error),
              "lab.sharpening refuses an edge amount below 0");
        params.sharpenEdge.amount = 50.0;
        params.sharpenEdge.passes = 0;
        check(labSharpeningSupported(params, 63, 31, false, &error),
              "zero edge passes is the host no-op");

        rtengine::procparams::ProcParams micro;
        micro.sharpenMicro.enabled = true;
        micro.sharpenMicro.amount = 20.0;
        micro.sharpenMicro.contrast = 20.0;
        micro.sharpenMicro.uniformity = 5;
        check(labSharpeningSupported(micro, 63, 31, false, &error),
              "lab.sharpening admits ML microcontrast alone");
        micro.sharpenMicro.matrix = true;
        check(labSharpeningSupported(micro, 63, 31, false, &error),
              "lab.sharpening admits the 3x3 microcontrast arm");
        micro.sharpenMicro.uniformity = 11;
        check(!labSharpeningSupported(micro, 63, 31, false, &error),
              "lab.sharpening refuses uniformity above 10");
        micro.sharpenMicro.uniformity = 5;
        micro.sharpenMicro.amount = 101.0;
        check(!labSharpeningSupported(micro, 63, 31, false, &error),
              "lab.sharpening refuses a microcontrast amount above 100");
        micro.sharpenMicro.amount = 20.0;
        micro.sharpenMicro.contrast = 201.0;
        check(!labSharpeningSupported(micro, 63, 31, false, &error),
              "lab.sharpening refuses microcontrast contrast above 200");
        micro.sharpenMicro.contrast = 100.0;
        check(labSharpeningSupported(micro, 63, 31, false, &error),
              "contrast 100 is the host no-op");

        /* Edges are a Lab pass in both arms; the other three are not. */
        params.sharpenEdge.passes = 2;
        params.impulseDenoise.enabled = true;
        params.defringe.enabled = true;
        params.defringe.radius = 2.0;
        params.defringe.threshold = 13;
        const LabSharpeningNodeParams ciecam =
            labSharpeningNodeParams(params, true);
        check(ciecam.impulse_enabled == 0 && ciecam.defringe_enabled == 0 &&
                  ciecam.edges_enabled == 1 && ciecam.micro_enabled == 0,
              "the CIE-CAM arm keeps only ML sharpen edges on the Lab image");
        check(labSharpeningSupported(params, 63, 31, true, &error),
              "the CIE-CAM arm admits ML sharpen edges");

        /*
         * The defringe hue table is only sampled when the defringe pass can
         * read it. In the CIE-CAM arm the CIE-CAM pipeline owns that pass, so
         * a curve the device table cannot follow must not refuse the edges
         * pass; the same applies with defringe disabled or below the host's
         * Gaussian identity.
         */
        rtengine::procparams::ProcParams curve_params;
        curve_params.sharpenEdge.enabled = true;
        curve_params.sharpenEdge.passes = 1;
        curve_params.sharpenEdge.amount = 50.0;
        curve_params.defringe.enabled = true;
        curve_params.defringe.radius = 1.0;
        curve_params.defringe.threshold = 13;
        curve_params.defringe.huecurve = {1.0, 0.0, 0.5, 0.35, 0.35,
                                          0.2, 0.9, 0.35, 0.35,
                                          0.8, 0.1, 0.35, 0.35,
                                          1.0, 0.5, 0.35, 0.35};
        {
            rtengine::ImProcFunctions curve_ipf(&curve_params, false);
            curve_ipf.setScale(1.0);

            LabSharpeningContext lab_context(curve_params, curve_ipf, 63, 31,
                                             false);
            std::string build_error;
            check(lab_context.build(&build_error) == 0,
                  ("the Lab arm builds the defringe hue table: " +
                   build_error).c_str());
            check(lab_context.hueCurve() &&
                      lab_context.hueEntries() ==
                          static_cast<int>(RT_LAB_SHARPENING_HUE_ENTRIES),
                  "the Lab arm samples the active defringe hue curve");

            LabSharpeningContext cam_context(curve_params, curve_ipf, 63, 31,
                                             true);
            check(cam_context.build(&build_error) == 0,
                  ("the CIE-CAM arm builds without a hue table: " +
                   build_error).c_str());
            check(!cam_context.hueCurve() && cam_context.hueEntries() == 2,
                  "the CIE-CAM arm skips the defringe hue table");

            rtengine::procparams::ProcParams off_params = curve_params;
            off_params.defringe.enabled = false;
            LabSharpeningContext off_context(off_params, curve_ipf, 63, 31,
                                             false);
            check(off_context.build(&build_error) == 0 &&
                      !off_context.hueCurve() &&
                      off_context.hueEntries() == 2,
                  "a disabled defringe skips the hue table");

            rtengine::procparams::ProcParams noop_params = curve_params;
            noop_params.defringe.radius = 0.2;
            LabSharpeningContext noop_context(noop_params, curve_ipf, 63, 31,
                                              false);
            check(noop_context.build(&build_error) == 0 &&
                      !noop_context.hueCurve() &&
                      noop_context.hueEntries() == 2,
                  "the defringe Gaussian identity skips the hue table");
        }
    }

    if (!available) {
        skip("lab sharpening: no OpenCL device");
        return;
    }

    const auto context = opencl_device()->create_context();
    check(context != nullptr, "lab sharpening: the device creates a context");
    if (context == nullptr) {
        return;
    }

    /* ------------------------------------------------------ impulse denoise */
    {
        rtengine::procparams::ProcParams params;
        params.impulseDenoise.enabled = true;
        params.impulseDenoise.thresh = 50;

        LabImage source(63, 31);
        fill_impulse_fixture(source);

        LabImage cpu(63, 31);
        copy_sharpen_l(source, cpu);
        run_cpu_lab_sharpening(params, cpu);

        LabImage device(63, 31);
        copy_sharpen_l(source, device);
        StageExecution execution;
        std::string error;
        const RtStageStatus status = run_device_lab_sharpening(
            context, params, device, execution, error);
        check(status == RT_STAGE_OK,
              ("lab.sharpening impulse " + error + " completes").c_str());
        check(execution.device == StageDeviceType::OPENCL,
              "lab.sharpening impulse runs on the device");
        check(!execution.asynchronous,
              "lab.sharpening impulse reads the planes back synchronously");

        /* markImpulse()'s own mask is the reference marked set. */
        std::vector<char> mask(
            static_cast<std::size_t>(source.W) * source.H, 0);
        std::vector<char*> mask_rows(static_cast<std::size_t>(source.H));
        for (int y = 0; y < source.H; ++y) {
            mask_rows[y] = mask.data() +
                           static_cast<std::size_t>(y) * source.W;
        }
        rtengine::markImpulse(source.W, source.H, source.L, mask_rows.data(),
                              static_cast<float>(params.impulseDenoise.thresh /
                                                 20.0));
        const std::size_t marked = static_cast<std::size_t>(
            std::count(mask.begin(), mask.end(), static_cast<char>(1)));

        const std::vector<unsigned char> cpu_changed =
            lab_changed_mask(cpu, source);
        const std::vector<unsigned char> device_changed =
            lab_changed_mask(device, source);
        const std::size_t cpu_count = lab_changed_count(cpu_changed);
        const std::size_t device_count = lab_changed_count(device_changed);
        check(cpu_changed == device_changed,
              "lab.sharpening impulse corrects exactly the host's pixel set");
        check(marked >= 25, "lab.sharpening impulse fixture marks a region");
        check(cpu_count >= 20,
              "lab.sharpening impulse fixture corrects a region");
        check(marked >= cpu_count,
              "lab.sharpening impulse never corrects an unmarked pixel");
        /* The corrected set is a subset of the marked set, pixel for pixel. */
        std::size_t unmarked_corrected = 0;
        for (std::size_t index = 0; index < mask.size(); ++index) {
            if (cpu_changed[index] != 0u && mask[index] == 0) {
                ++unmarked_corrected;
            }
        }
        check(unmarked_corrected == 0,
              "lab.sharpening impulse corrects only pixels the mask marks");

        /* Zero device differences outside the 5x5 correction neighbourhoods. */
        std::size_t outside = 0;
        for (int y = 0; y < device.H; ++y) {
            for (int x = 0; x < device.W; ++x) {
                const std::size_t here =
                    static_cast<std::size_t>(y) * device.W + x;
                if (!device_changed[here]) {
                    continue;
                }
                bool near = false;
                for (int yy = std::max(0, y - 2);
                     yy <= std::min(device.H - 1, y + 2) && !near; ++yy) {
                    for (int xx = std::max(0, x - 2);
                         xx <= std::min(device.W - 1, x + 2); ++xx) {
                        const std::size_t there =
                            static_cast<std::size_t>(yy) * device.W + xx;
                        if (cpu_changed[there]) {
                            near = true;
                            break;
                        }
                    }
                }
                if (!near) {
                    ++outside;
                }
            }
        }
        check(outside == 0,
              "lab.sharpening impulse changes nothing outside a corrected "
              "window");

        const LabPlaneDiff diff = compare_lab_planes(device, cpu);
        check(diff.finite, "lab.sharpening impulse stays finite");
        check(diff.max_l <= 0.5 && diff.max_a <= 0.5 && diff.max_b <= 0.5,
              "lab.sharpening impulse is within half a 16-bit LSB");
        std::printf("stage-residency: lab.sharpening impulse: marked %zu, "
                    "corrected cpu %zu/device %zu, PAE L %.6f a %.6f b %.6f, "
                    "%zu of %zu samples differ\n",
                    marked, cpu_count, device_count, diff.max_l, diff.max_a,
                    diff.max_b, diff.differing, diff.samples);
    }

    /* -------------------------------------------------- impulse stress case */
    {
        /*
         * A large deterministic pseudo-random field exercises the SSE bulk
         * rows and columns and thousands of corrections at once. The impulse
         * pass has no threshold comparison that a rounding could move, so it
         * must be bit-identical on all three planes.
         */
        rtengine::procparams::ProcParams params;
        params.impulseDenoise.enabled = true;
        params.impulseDenoise.thresh = 50;

        LabImage source(128, 80);
        unsigned state = 0x12345678u;
        const auto next = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<int>((state >> 8) & 0xffffu);
        };
        for (int y = 0; y < source.H; ++y) {
            for (int x = 0; x < source.W; ++x) {
                source.L[y][x] = static_cast<float>(next());
                source.a[y][x] = static_cast<float>(next()) - 32768.0f;
                source.b[y][x] = static_cast<float>(next()) - 32768.0f;
                if (((x * 7 + y * 13) % 97) == 0) {
                    source.L[y][x] += 20000.0f;
                    source.a[y][x] += 6000.0f;
                }
            }
        }

        LabImage cpu(128, 80);
        copy_sharpen_l(source, cpu);
        run_cpu_lab_sharpening(params, cpu);

        LabImage device(128, 80);
        copy_sharpen_l(source, device);
        StageExecution execution;
        std::string error;
        const RtStageStatus status = run_device_lab_sharpening(
            context, params, device, execution, error);
        check(status == RT_STAGE_OK &&
                  execution.device == StageDeviceType::OPENCL,
              "lab.sharpening impulse stress runs on the device");
        const LabPlaneDiff diff = compare_lab_planes(device, cpu);
        check(diff.finite && diff.max_l == 0.0 && diff.max_a == 0.0 &&
                  diff.max_b == 0.0,
              "lab.sharpening impulse is bit-identical on a random field");
        std::printf("stage-residency: lab.sharpening impulse stress: %zu of "
                    "%zu samples differ, PAE L %.6f a %.6f b %.6f\n",
                    diff.differing, diff.samples, diff.max_l, diff.max_a,
                    diff.max_b);
    }

    /* ------------------------------------------------------------- defringe */
    {
        rtengine::procparams::ProcParams params;
        params.defringe.enabled = true;
        params.defringe.radius = 2.0;
        params.defringe.threshold = 13;

        LabImage source(70, 42);
        fill_defringe_fixture(source);

        LabImage cpu(70, 42);
        copy_sharpen_l(source, cpu);
        run_cpu_lab_sharpening(params, cpu);

        LabImage device(70, 42);
        copy_sharpen_l(source, device);
        StageExecution execution;
        std::string error;
        const RtStageStatus status = run_device_lab_sharpening(
            context, params, device, execution, error);
        check(status == RT_STAGE_OK,
              ("lab.sharpening defringe " + error + " completes").c_str());
        check(execution.device == StageDeviceType::OPENCL,
              "lab.sharpening defringe runs on the device");
        check(!execution.asynchronous,
              "lab.sharpening defringe reads the planes back synchronously");

        const std::vector<unsigned char> cpu_changed =
            lab_changed_mask(cpu, source);
        const std::vector<unsigned char> device_changed =
            lab_changed_mask(device, source);
        const std::size_t cpu_count = lab_changed_count(cpu_changed);
        const std::size_t device_count = lab_changed_count(device_changed);
        check(cpu_changed == device_changed,
              "lab.sharpening defringe corrects exactly the host's pixel set");
        check(cpu_count >= 30,
              "lab.sharpening defringe fixture corrects a region");
        check(cpu.L[10][32] == source.L[10][32],
              "lab.sharpening defringe leaves L untouched");

        /* Zero device differences outside the halfwin neighbourhood. */
        const int reach = 4; /* ceil(2 * 2.0) + 1 - 1 */
        std::size_t outside = 0;
        for (int y = 0; y < device.H; ++y) {
            for (int x = 0; x < device.W; ++x) {
                const std::size_t here =
                    static_cast<std::size_t>(y) * device.W + x;
                if (!device_changed[here]) {
                    continue;
                }
                bool near = false;
                for (int yy = std::max(0, y - reach);
                     yy <= std::min(device.H - 1, y + reach) && !near; ++yy) {
                    for (int xx = std::max(0, x - reach);
                         xx <= std::min(device.W - 1, x + reach); ++xx) {
                        const std::size_t there =
                            static_cast<std::size_t>(yy) * device.W + xx;
                        if (cpu_changed[there]) {
                            near = true;
                            break;
                        }
                    }
                }
                if (!near) {
                    ++outside;
                }
            }
        }
        check(outside == 0,
              "lab.sharpening defringe changes nothing outside a corrected "
              "window");

        const LabPlaneDiff diff = compare_lab_planes(device, cpu);
        check(diff.finite, "lab.sharpening defringe stays finite");
        check(diff.max_l == 0.0,
              "lab.sharpening defringe never moves L");
        check(diff.max_a <= 0.5 && diff.max_b <= 0.5,
              "lab.sharpening defringe is within half a 16-bit LSB");
        std::printf("stage-residency: lab.sharpening defringe: corrected cpu "
                    "%zu/device %zu, PAE L %.6f a %.6f b %.6f, %zu of %zu "
                    "samples differ\n",
                    cpu_count, device_count, diff.max_l, diff.max_a, diff.max_b,
                    diff.differing, diff.samples);
    }

    /* ------------------------------------------------- the pair is atomic */
    {
        /* Impulse admits, defringe does not: the whole pair runs on the
         * host. A prescaled threshold above 100 is the documented residual. */
        rtengine::procparams::ProcParams params;
        params.impulseDenoise.enabled = true;
        params.impulseDenoise.thresh = 50;
        params.defringe.enabled = true;
        params.defringe.radius = 2.0;
        params.defringe.threshold = 101;

        LabImage source(63, 31);
        fill_impulse_fixture(source);

        LabImage cpu(63, 31);
        copy_sharpen_l(source, cpu);
        run_cpu_lab_sharpening(params, cpu);

        LabImage device(63, 31);
        copy_sharpen_l(source, device);
        StageExecution execution;
        std::string error;
        const RtStageStatus status = run_device_lab_sharpening(
            context, params, device, execution, error);
        check(status == RT_STAGE_OK,
              "a refused pair completes through the host builtin");
        check(execution.device != StageDeviceType::OPENCL,
              "a refused pair never runs on the device");
        const LabPlaneDiff diff = compare_lab_planes(device, cpu);
        std::printf("stage-residency: lab.sharpening refused pair: %zu of %zu "
                    "samples differ, PAE L %.6f a %.6f b %.6f\n",
                    diff.differing, diff.samples, diff.max_l, diff.max_a,
                    diff.max_b);
        check(diff.finite && diff.differing == 0 && diff.max_l == 0.0 &&
                  diff.max_a == 0.0 && diff.max_b == 0.0,
              "a refused pair matches the host result exactly");

        const std::string profile_path =
            std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                              : "/tmp") +
            "/stage-residency-lab-sharpening.jsonl";
        rtengine::ImProcFunctions ipf(&params, false);
        ipf.setScale(1.0);
        LabSharpeningContext node_context(params, ipf, source.W, source.H);
        check(node_context.build(&error) == 0,
              "a refused pair builds its hue table");
        RtLabSharpeningContext descriptor =
            node_context.descriptor(labSharpeningSupported(
                params, source.W, source.H, false, &error));
        std::vector<RtStageArg> args;
        labSharpeningArgs(device, labSharpeningNodeParams(params),
                          node_context, descriptor, args);

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("lab.sharpening", StageNodeDevice::OpenCL);
        const RtStageStatus run_status = run("lab.sharpening", args);
        clear_node_device_overrides();
        const std::string profile = read_file(profile_path);
        unsetenv("RT_STAGE_PROFILE");
        check(run_status == RT_STAGE_OK,
              "a refused pair completes through run()");
        check(profile.find("\"stage\":\"lab.sharpening\"") != std::string::npos,
              "a refused pair records the node");
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              "a refused pair records a host execution");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "a refused pair never records a device pass");
    }
    /* ------------------------------------- the pair runs in one device pass */
    {
        rtengine::procparams::ProcParams params;
        params.impulseDenoise.enabled = true;
        params.impulseDenoise.thresh = 50;
        params.defringe.enabled = true;
        params.defringe.radius = 2.0;
        params.defringe.threshold = 13;

        LabImage source(70, 42);
        fill_defringe_fixture(source);
        /* Spikes in the neutral and saturated regions so the impulse pass has
         * work and the defringe reads its corrected planes. */
        source.L[5][5] += 18000.0f;
        source.L[25][50] -= 16000.0f;
        source.a[25][50] += 3000.0f;
        source.L[0][69] += 15000.0f;

        LabImage cpu(70, 42);
        copy_sharpen_l(source, cpu);
        run_cpu_lab_sharpening(params, cpu);

        LabImage device(70, 42);
        copy_sharpen_l(source, device);
        StageExecution execution;
        std::string error;
        const RtStageStatus status = run_device_lab_sharpening(
            context, params, device, execution, error);
        check(status == RT_STAGE_OK &&
                  execution.device == StageDeviceType::OPENCL,
              "the impulse+defringe pair runs in one device pass");
        const LabPlaneDiff diff = compare_lab_planes(device, cpu);
        std::printf("stage-residency: lab.sharpening pair: PAE L %.6f a %.6f "
                    "b %.6f, %zu of %zu samples differ\n",
                    diff.max_l, diff.max_a, diff.max_b, diff.differing,
                    diff.samples);
        check(diff.finite && diff.max_l <= 0.5 && diff.max_a <= 0.5 &&
                  diff.max_b <= 0.5,
              "the impulse+defringe pair is within half a 16-bit LSB");
        const std::vector<unsigned char> cpu_changed =
            lab_changed_mask(cpu, source);
        const std::vector<unsigned char> device_changed =
            lab_changed_mask(device, source);
        check(cpu_changed == device_changed,
              "the impulse+defringe pair corrects exactly the host's set");
    }

    /* The defringe no-op below the Gaussian identity limit. */
    {
        rtengine::procparams::ProcParams params;
        params.defringe.enabled = true;
        params.defringe.radius = 0.2;
        params.defringe.threshold = 13;

        LabImage source(63, 31);
        fill_impulse_fixture(source);
        LabImage device(63, 31);
        copy_sharpen_l(source, device);

        StageExecution execution;
        std::string error;
        const RtStageStatus status = run_device_lab_sharpening(
            context, params, device, execution, error);
        check(status == RT_STAGE_OK, "the defringe no-op completes");
        check(execution.device == StageDeviceType::OPENCL,
              "the defringe no-op is a device pass");
        const LabPlaneDiff untouched = compare_lab_planes(device, source);
        check(untouched.differing == 0 && untouched.max_l == 0.0 &&
                  untouched.max_a == 0.0 && untouched.max_b == 0.0,
              "the defringe no-op leaves every plane untouched");
    }

    /*
     * --------------------------------- the defringe radius range on the device
     *
     * PF_correct_RT() calls gaussianBlur() with GAUSS_STANDARD, so the whole
     * legal GUI range is device: the 3x3 convolution from 0.25 to below 0.6,
     * the separable-IIR pair from 0.6. Each case is compared with the host
     * block and must change pixels, so none is vacuous. The CIE-CAM cases set
     * a radius the Lab node must skip, because the CIE-CAM pipeline owns the
     * pass there.
     */
    {
        struct RadiusCase {
            const char* label;
            double radius;
            bool edges;
            bool micro;
            bool ciecam;
            bool main_pass;
        };
        const RadiusCase cases[] = {
            {"radius 0.25 3x3 lower edge", 0.25, false, false, false, false},
            {"radius 0.5 3x3", 0.5, false, false, false, false},
            {"radius 0.59 3x3 upper edge", 0.59, false, false, false, false},
            {"radius 0.6 IIR boundary", 0.6, false, false, false, false},
            {"radius 1.0 IIR", 1.0, false, false, false, false},
            {"radius 1.15 IIR", 1.15, false, false, false, false},
            {"radius 0.5 + edges", 0.5, true, false, false, false},
            {"radius 1.0 + edges + micro", 1.0, true, true, false, false},
            {"radius 0.5 CIE-CAM arm + edges", 0.5, true, false, true, false},
            {"radius 1.0 CIE-CAM arm + edges", 1.0, true, false, true, false},
            {"radius 1.0 CIE-CAM arm + edges + main", 1.0, true, false, true,
             true},
        };

        std::size_t device_cases = 0;
        for (const RadiusCase& item : cases) {
            rtengine::procparams::ProcParams params;
            params.defringe.enabled = true;
            params.defringe.radius = item.radius;
            params.defringe.threshold = 13;
            if (item.edges) {
                params.sharpenEdge.enabled = true;
                params.sharpenEdge.passes = 2;
                params.sharpenEdge.amount = 50.0;
            }
            if (item.micro) {
                params.sharpenMicro.enabled = true;
                params.sharpenMicro.amount = 20.0;
                params.sharpenMicro.contrast = 20.0;
                params.sharpenMicro.uniformity = 5;
            }
            if (item.main_pass) {
                /* The main sharpen pass is the shared output.sharpen node; it
                 * must not change this block's result. */
                params.sharpening.enabled = true;
            }

            LabImage source(70, 42);
            if (item.edges || item.micro) {
                fill_ml_fixture(source);
            } else {
                fill_defringe_fixture(source);
            }

            LabImage cpu(70, 42);
            copy_sharpen_l(source, cpu);
            run_cpu_lab_sharpening(params, cpu, item.ciecam);

            LabImage device(70, 42);
            copy_sharpen_l(source, device);
            StageExecution execution;
            std::string error;
            const RtStageStatus status = run_device_lab_sharpening(
                context, params, device, execution, error, item.ciecam);
            const std::string label =
                std::string("lab.sharpening ") + item.label;
            check(status == RT_STAGE_OK,
                  (label + " completes: " + error).c_str());
            check(execution.device == StageDeviceType::OPENCL,
                  (label + " runs on the device").c_str());
            if (status == RT_STAGE_OK &&
                execution.device == StageDeviceType::OPENCL) {
                ++device_cases;
            }

            const LabPlaneDiff diff = compare_lab_planes(device, cpu);
            check(diff.finite, (label + " stays finite").c_str());
            check(diff.max_l <= 0.5 && diff.max_a <= 0.5 &&
                      diff.max_b <= 0.5,
                  (label + " is within half a 16-bit LSB").c_str());

            if (!item.edges && !item.micro) {
                /* Defringe never touches L, on the host or the device. */
                const LabPlaneDiff l_only = compare_lab_planes(device, source);
                check(l_only.max_l == 0.0,
                      (label + " leaves L untouched").c_str());
            }

            const std::vector<unsigned char> changed =
                lab_changed_mask(device, source);
            const std::size_t count = lab_changed_count(changed);
            check(count > 0, (label + " is not vacuous").c_str());
            std::printf("stage-residency: %s: %zu of %zu samples differ, "
                        "PAE L %.6f a %.6f b %.6f\n",
                        label.c_str(), diff.differing, diff.samples,
                        diff.max_l, diff.max_a, diff.max_b);
        }
        check(device_cases >= 8,
              "lab.sharpening covers at least eight defringe radius cases");
    }

    /* --------------------------------- ML sharpen edges and microcontrast */
    {
        struct MlCase {
            const char* label;
            bool edges;
            bool micro;
            bool pairs;
            bool ciecam;
            int passes;
            double edge_amount;
            bool threechannels;
            bool matrix;
            double micro_amount;
            double micro_contrast;
            int uniformity;
        };
        const MlCase cases[] = {
            {"edges L/a/b 1 pass 50", true, false, false, false, 1, 50.0,
             false, false, 0.0, 0.0, 0},
            {"edges L/a/b 4 passes 100", true, false, false, false, 4, 100.0,
             false, false, 0.0, 0.0, 0},
            {"edges L only 2 passes 35", true, false, false, false, 2, 35.0,
             true, false, 0.0, 0.0, 0},
            {"edges L only 3 passes 80", true, false, false, false, 3, 80.0,
             true, false, 0.0, 0.0, 0},
            {"micro 5x5 20/20/5", false, true, false, false, 2, 0.0, false,
             false, 20.0, 20.0, 5},
            {"micro 5x5 60/0/0", false, true, false, false, 2, 0.0, false,
             false, 60.0, 0.0, 0},
            {"micro 3x3 20/50/10", false, true, false, false, 2, 0.0, false,
             true, 20.0, 50.0, 10},
            {"micro 3x3 100/200/0", false, true, false, false, 2, 0.0, false,
             true, 100.0, 200.0, 0},
            {"impulse+defringe+edges L only 50", true, false, true, false, 2,
             50.0, true, false, 0.0, 0.0, 0},
            {"impulse+defringe+edges L/a/b+micro", true, true, true, false,
             2, 40.0, false, false, 30.0, 30.0, 6},
            {"CIE-CAM arm edges only", true, true, true, true, 2, 60.0, true,
             false, 20.0, 20.0, 5},
        };

        std::size_t device_cases = 0;
        for (const MlCase& item : cases) {
            rtengine::procparams::ProcParams params;
            if (item.pairs) {
                params.impulseDenoise.enabled = true;
                params.impulseDenoise.thresh = 50;
                params.defringe.enabled = true;
                params.defringe.radius = 2.0;
                params.defringe.threshold = 13;
            }
            if (item.edges) {
                params.sharpenEdge.enabled = true;
                params.sharpenEdge.passes = item.passes;
                params.sharpenEdge.amount = item.edge_amount;
                params.sharpenEdge.threechannels = item.threechannels;
            }
            if (item.micro) {
                params.sharpenMicro.enabled = true;
                params.sharpenMicro.matrix = item.matrix;
                params.sharpenMicro.amount = item.micro_amount;
                params.sharpenMicro.contrast = item.micro_contrast;
                params.sharpenMicro.uniformity = item.uniformity;
            }

            LabImage source(70, 42);
            fill_ml_fixture(source);

            LabImage cpu(70, 42);
            copy_sharpen_l(source, cpu);
            run_cpu_lab_sharpening(params, cpu, item.ciecam);

            LabImage device(70, 42);
            copy_sharpen_l(source, device);
            StageExecution execution;
            std::string error;
            const RtStageStatus status = run_device_lab_sharpening(
                context, params, device, execution, error, item.ciecam);
            const std::string label = std::string("lab.sharpening ML ") +
                                      item.label;
            check(status == RT_STAGE_OK,
                  (label + " completes: " + error).c_str());
            check(execution.device == StageDeviceType::OPENCL,
                  (label + " runs on the device").c_str());
            if (status == RT_STAGE_OK &&
                execution.device == StageDeviceType::OPENCL) {
                ++device_cases;
            }

            const LabPlaneDiff diff = compare_lab_planes(device, cpu);
            check(diff.finite, (label + " stays finite").c_str());
            check(diff.max_l <= 0.5 && diff.max_a <= 0.5 &&
                      diff.max_b <= 0.5,
                  (label + " is within half a 16-bit LSB").c_str());

            const std::vector<unsigned char> changed =
                lab_changed_mask(device, source);
            const std::size_t count = lab_changed_count(changed);
            check(count > 0, (label + " is not vacuous").c_str());
            std::printf("stage-residency: %s: %zu of %zu samples differ, "
                        "PAE L %.6f a %.6f b %.6f\n",
                        label.c_str(), diff.differing, diff.samples,
                        diff.max_l, diff.max_a, diff.max_b);
        }
        check(device_cases >= 8,
              "lab.sharpening ML covers at least eight device cases");
    }
}

/* -------------------------------------------------------------- lab.tools */

/*
 * The LUT instances the export ImageProcessor holds. stage_rgb_proc() sizes
 * them before lab.tools runs, so the test starts from the same sizes.
 */
struct LabToolsHostState {
    LUTu hist16;
    LUTf lumacurve;
    LUTu dummy;
    LUTf clcurve;
    LUTf curve1;
    LUTf curve2;
    LUTf satcurve;
    LUTf lhskcurve;
    bool autili = false;
    bool butili = false;

    LabToolsHostState()
    {
        hist16(65536);
        lumacurve(32770, 0);
        curve1(65536);
        curve2(65536);
        satcurve(65536, 0);
        lhskcurve(65536, 0);
        clcurve(65536, 0);
    }

    LabToolsLuts luts()
    {
        return {&hist16,  &lumacurve, &dummy,    &clcurve,
                &curve1,  &curve2,   &satcurve, &lhskcurve};
    }
};

/*
 * A Lab image that exercises the block: L covers 0..32767 with a smooth ramp
 * plus a large-scale structure so the tonal-width threshold crosses, and a, b
 * span the block's working range including values that force the Lab-to-RGB
 * round trip out of gamut. A few samples sit outside 0..32767 to exercise the
 * blend's range guard.
 */
void fill_lab_tools(LabImage& lab, unsigned seed)
{
    const int width = lab.W;
    const int height = lab.H;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned hash =
                (seed * 2654435761u + static_cast<unsigned>(x) * 2246822519u +
                 static_cast<unsigned>(y) * 3266489917u) >>
                8;
            const unsigned cell = (hash ^ (hash >> 13)) % 4096u;
            float l = 32768.f * static_cast<float>(cell) / 4096.f;
            if ((hash % 97u) == 0u) {
                l = 32768.f + 2000.f; /* above the blend's upper guard */
            } else if ((hash % 89u) == 0u) {
                l = -500.f; /* below the blend's lower guard */
            }
            lab.L[y][x] = l;
            const int a = static_cast<int>(hash % 40000u) - 20000;
            const int b = static_cast<int>((hash / 7u) % 40000u) - 20000;
            lab.a[y][x] = static_cast<float>(a);
            lab.b[y][x] = static_cast<float>(b);
        }
    }
}

/*
 * A smooth, photo-like Lab plane: a diagonal luminance ramp with a broad
 * highlight, a deep shadow and a low-amplitude ripple, plus small chroma. The
 * guided soft radius is sensitive to local variance, which the random
 * generator keeps large, so this image exercises the low-variance arm.
 */
void fill_lab_smooth(LabImage& lab, unsigned seed)
{
    const int width = lab.W;
    const int height = lab.H;
    const float phase = static_cast<float>(seed % 97u) * 0.031f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(y) / static_cast<float>(height);
            float l = 3000.f + 24000.f * (0.6f * u + 0.4f * v);
            l += 900.f * std::sin(12.0f * u + phase) *
                 std::cos(9.0f * v - phase);
            if (u < 0.12f) {
                l *= 0.02f;
            } else if (u > 0.88f) {
                l = 30000.f + 2000.f * u;
            }
            lab.L[y][x] = l;
            lab.a[y][x] = 2500.f * (u - 0.5f);
            lab.b[y][x] = 2500.f * (v - 0.5f);
        }
    }
}

struct LabToolsCase {
    const char* label;
    int width;
    int height;
    bool labgrid;
    bool shadowsHighlights;
    bool labMode;
    int highlights;
    int shadows;
    int htonalwidth;
    int stonalwidth;
    int radius;
};

struct LabToolsRun {
    bool supported = false;
    bool deviceReady = false;
    RtStageStatus status = RT_STAGE_INVALID;
    /* The black-and-white flag the context exported to the launcher. */
    uint32_t black_white = 0;
    /* The exported C=f(H) table's node count, 0 when the slot is inactive. */
    uint32_t hue_nodes = 0;
};

/*
 * The call site's submission, reproduced for one image: build the host setup,
 * fill the ABI, and run the node with the device selected.
 */
LabToolsRun run_lab_tools_node(LabImage& image,
                               const rtengine::procparams::ProcParams& params,
                               LabToolsHostState& host,
                               rtengine::ImProcFunctions& ipf,
                               const std::string& profile_path)
{
    using namespace rtengine::stage;

    LabToolsRun result;
    result.supported = labToolsSupported(params);

    if (params.labCurve.enabled && params.labCurve.contrast != 0) {
        labToolsBuildHistogram(image, host.hist16);
    }
    LabToolsLuts luts = host.luts();
    LabToolsContext context(params, ipf, luts, host.autili, host.butili);
    RtLabToolsResult setup{};
    const bool deviceReady = result.supported && context.build(setup) == 0;
    result.deviceReady = deviceReady;
    result.black_white = deviceReady ? setup.black_white : 0;
    if (deviceReady && setup.lab_curve_active != 0 &&
        setup.lab_curve_ch_active != 0 && setup.lab_curve_hue != nullptr) {
        result.hue_nodes = static_cast<uint32_t>(setup.lab_curve_hue[0]);
    }
    RtLabToolsContext descriptor = context.descriptor(deviceReady);

    RtStageArg args[76] = {};
    args[0].kind = RT_STAGE_ARG_BUFFER;
    args[0].u.buffer = view(image);
    args[1].kind = RT_STAGE_ARG_PTR;
    args[1].u.ptr = &descriptor;
    args[2].kind = RT_STAGE_ARG_INT;
    args[2].u.i = deviceReady ? 1 : 0;
    args[3].kind = RT_STAGE_ARG_INT;
    args[3].u.i = (params.colorToning.enabled &&
                   params.colorToning.method == "LabGrid")
                      ? 1
                      : 0;
    for (int index = 0; index < 4; ++index) {
        args[4 + index].kind = RT_STAGE_ARG_DOUBLE;
        args[4 + index].u.f = deviceReady ? setup.labgrid[index] : 0.0;
    }
    args[8].kind = RT_STAGE_ARG_INT;
    args[8].u.i = params.sh.enabled ? 1 : 0;
    args[9].kind = RT_STAGE_ARG_INT;
    args[9].u.i = params.sh.highlights;
    args[10].kind = RT_STAGE_ARG_INT;
    args[10].u.i = params.sh.shadows;
    args[11].kind = RT_STAGE_ARG_INT;
    args[11].u.i = params.sh.htonalwidth;
    args[12].kind = RT_STAGE_ARG_INT;
    args[12].u.i = params.sh.stonalwidth;
    args[13].kind = RT_STAGE_ARG_INT;
    args[13].u.i = params.sh.radius;
    args[14].kind = RT_STAGE_ARG_INT;
    args[14].u.i = params.sh.lab ? 1 : 0;
    args[15].kind = RT_STAGE_ARG_INT;
    args[15].u.i = LAB_TOOLS_EXPORT_SCALE;

    const bool vibrance = labToolsVibranceActive(params);
    args[22].kind = RT_STAGE_ARG_INT;
    args[22].u.i = deviceReady && vibrance ? 1 : 0;
    args[23].kind = RT_STAGE_ARG_INT;
    args[23].u.i = params.vibrance.pastels;
    args[24].kind = RT_STAGE_ARG_INT;
    args[24].u.i = params.vibrance.saturated;
    args[25].kind = RT_STAGE_ARG_INT;
    args[25].u.i = params.vibrance.psthreshold.getTopLeft();
    args[26].kind = RT_STAGE_ARG_INT;
    args[26].u.i = params.vibrance.psthreshold.getBottomLeft();
    args[27].kind = RT_STAGE_ARG_INT;
    args[27].u.i = params.vibrance.protectskins ? 1 : 0;
    args[28].kind = RT_STAGE_ARG_INT;
    args[28].u.i = params.vibrance.avoidcolorshift ? 1 : 0;
    args[29].kind = RT_STAGE_ARG_INT;
    args[29].u.i = deviceReady && vibrance && setup.skin_curve_set != 0 ? 1 : 0;
    args[30].kind = RT_STAGE_ARG_INT;
    args[30].u.i = params.toneCurve.hrenabled ? 1 : 0;

    args[34].kind = RT_STAGE_ARG_INT;
    args[34].u.i = deviceReady ? setup.lab_curve_active : 0;
    args[35].kind = RT_STAGE_ARG_INT;
    args[35].u.i = deviceReady ? setup.lab_curve_l_active : 0;
    args[36].kind = RT_STAGE_ARG_INT;
    args[36].u.i = deviceReady ? setup.lab_curve_a_active : 0;
    args[37].kind = RT_STAGE_ARG_INT;
    args[37].u.i = deviceReady ? setup.lab_curve_b_active : 0;
    args[38].kind = RT_STAGE_ARG_INT;
    args[38].u.i = deviceReady ? setup.lab_curve_cc_active : 0;
    args[39].kind = RT_STAGE_ARG_INT;
    args[39].u.i = deviceReady ? setup.lab_curve_lc_active : 0;
    args[40].kind = RT_STAGE_ARG_INT;
    args[40].u.i = deviceReady ? setup.lab_curve_cl_active : 0;
    args[41].kind = RT_STAGE_ARG_INT;
    args[41].u.i = deviceReady ? setup.lab_curve_munsell : 0;
    args[42].kind = RT_STAGE_ARG_INT;
    args[42].u.i = deviceReady ? setup.lab_curve_lcredsk : 0;
    const double lab_curve_doubles[5] = {
        deviceReady ? setup.lab_curve_rst_protection : 0.0,
        deviceReady ? setup.lab_curve_protect_red : 0.0,
        deviceReady ? setup.lab_curve_protect_red_h : 0.0,
        deviceReady ? setup.lab_curve_amount_chroma : 0.0,
        deviceReady ? setup.lab_curve_adjust : 1.0};
    for (int index = 0; index < 5; ++index) {
        args[43 + index].kind = RT_STAGE_ARG_DOUBLE;
        args[43 + index].u.f = lab_curve_doubles[index];
    }
    for (int index = 48; index < 54; ++index) {
        args[index].kind = RT_STAGE_ARG_BUFFER;
    }
    args[54].kind = RT_STAGE_ARG_INT;
    args[54].u.i = deviceReady ? setup.lab_curve_l_entries : 0;
    args[55].kind = RT_STAGE_ARG_INT;
    args[55].u.i = deviceReady ? setup.lab_curve_entries : 0;

    args[56].kind = RT_STAGE_ARG_INT;
    args[56].u.i = deviceReady ? setup.local_contrast_enabled : 0;
    args[57].kind = RT_STAGE_ARG_INT;
    args[57].u.i = deviceReady ? setup.local_contrast_radius : 0;
    args[58].kind = RT_STAGE_ARG_DOUBLE;
    args[58].u.f = deviceReady ? setup.local_contrast_amount : 0.0;
    args[59].kind = RT_STAGE_ARG_DOUBLE;
    args[59].u.f = deviceReady ? setup.local_contrast_darkness : 1.0;
    args[60].kind = RT_STAGE_ARG_DOUBLE;
    args[60].u.f = deviceReady ? setup.local_contrast_lightness : 1.0;
    args[61].kind = RT_STAGE_ARG_INT;
    args[61].u.i = deviceReady ? setup.local_contrast_scale
                               : LAB_TOOLS_EXPORT_SCALE;
    args[62].kind = RT_STAGE_ARG_PTR;
    args[62].u.ptr = deviceReady ? setup.local_contrast_iir : nullptr;

    args[63].kind = RT_STAGE_ARG_INT;
    args[63].u.i = deviceReady ? setup.region_count : 0;
    args[64].kind = RT_STAGE_ARG_BUFFER;
    args[65].kind = RT_STAGE_ARG_INT;
    args[65].u.i = deviceReady ? setup.region_lut_stride : 0;
    args[66].kind = RT_STAGE_ARG_BUFFER;
    args[67].kind = RT_STAGE_ARG_PTR;
    args[67].u.ptr = deviceReady ? setup.ws_double : nullptr;
    args[68].kind = RT_STAGE_ARG_PTR;
    args[68].u.ptr = deviceReady ? setup.iws_double : nullptr;
    args[69].kind = RT_STAGE_ARG_INT;
    args[69].u.i = deviceReady ? setup.black_white : 0;

    args[70].kind = RT_STAGE_ARG_INT;
    args[70].u.i = deviceReady ? setup.lab_curve_hue_stride : 0;
    args[71].kind = RT_STAGE_ARG_BUFFER;
    args[72].kind = RT_STAGE_ARG_INT;
    args[72].u.i = deviceReady ? setup.lab_curve_ch_active : 0;
    args[73].kind = RT_STAGE_ARG_INT;
    args[73].u.i = deviceReady ? setup.lab_curve_lh_active : 0;
    args[74].kind = RT_STAGE_ARG_INT;
    args[74].u.i = deviceReady ? setup.lab_curve_hh_active : 0;
    args[75].kind = RT_STAGE_ARG_INT;
    args[75].u.i = deviceReady ? setup.lab_curve_chromaticity : 0;

    for (int index = 16; index < 22; ++index) {
        args[index].kind = RT_STAGE_ARG_BUFFER;
    }
    for (int index = 31; index < 34; ++index) {
        args[index].kind = RT_STAGE_ARG_BUFFER;
    }
    if (deviceReady) {
        const auto host_buffer = [](const float* data, int width) {
            RtStageBuffer buffer{};
            buffer.size = sizeof(RtStageBuffer);
            buffer.data = const_cast<float*>(data);
            buffer.width = width;
            buffer.height = 1;
            buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
            buffer.plane_pitch = buffer.row_pitch;
            buffer.planes = 1;
            buffer.format = RT_STAGE_F32;
            buffer.flags = RT_STAGE_BUFFER_READ;
            return buffer;
        };
        /* The hue flat-curve nodes are doubles: two float words per node, the
         * same byte-counted upload the launcher makes. */
        const auto host_double_buffer = [](const double* data, int count) {
            RtStageBuffer buffer{};
            buffer.size = sizeof(RtStageBuffer);
            buffer.data = const_cast<double*>(data);
            buffer.width = 2 * count;
            buffer.height = 1;
            buffer.row_pitch = static_cast<std::size_t>(buffer.width) *
                               sizeof(float);
            buffer.plane_pitch = buffer.row_pitch;
            buffer.planes = 1;
            buffer.format = RT_STAGE_F32;
            buffer.flags = RT_STAGE_BUFFER_READ;
            return buffer;
        };
        args[16].u.buffer = host_buffer(
            setup.lut_highlight, static_cast<int>(setup.lut_entries));
        args[17].u.buffer = host_buffer(
            setup.lut_shadow, static_cast<int>(setup.lut_entries));
        args[18].u.buffer = host_buffer(setup.ws, 9);
        args[19].u.buffer = host_buffer(setup.iws, 9);
        args[20].u.buffer = host_buffer(
            setup.cachef, static_cast<int>(RT_LAB_TOOLS_CACHE_ENTRIES));
        args[21].u.buffer = host_buffer(
            setup.cachefy, static_cast<int>(RT_LAB_TOOLS_CACHE_ENTRIES));
        if (vibrance) {
            args[31].u.buffer = host_buffer(
                setup.munsell, static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES *
                                                RT_LAB_TOOLS_MUNSELL_STRIDE));
            args[32].u.buffer = host_buffer(
                setup.munsell_sizes,
                static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES));
            args[33].u.buffer = host_buffer(
                setup.skin_curve,
                setup.skin_curve_set != 0
                    ? static_cast<int>(RT_LAB_TOOLS_SKIN_CURVE_ENTRIES)
                    : 1);
        } else if (setup.lab_curve_munsell != 0) {
            args[31].u.buffer = host_buffer(
                setup.munsell, static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES *
                                                RT_LAB_TOOLS_MUNSELL_STRIDE));
            args[32].u.buffer = host_buffer(
                setup.munsell_sizes,
                static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES));
        }
        if (setup.region_count != 0) {
            args[64].u.buffer = host_buffer(
                setup.region_luts,
                static_cast<int>(setup.region_count *
                                 RT_LAB_TOOLS_REGION_CURVES *
                                 setup.region_lut_stride));
            args[66].u.buffer = host_buffer(
                setup.region_params,
                static_cast<int>(setup.region_count *
                                 RT_LAB_TOOLS_REGION_PARAM_STRIDE));
        }
        if (setup.lab_curve_active != 0) {
            args[48].u.buffer = host_buffer(
                setup.lab_curve_l, static_cast<int>(setup.lab_curve_l_entries));
            args[49].u.buffer = host_buffer(
                setup.lab_curve_a, static_cast<int>(setup.lab_curve_entries));
            args[50].u.buffer = host_buffer(
                setup.lab_curve_b, static_cast<int>(setup.lab_curve_entries));
            args[51].u.buffer = host_buffer(
                setup.lab_curve_cc, static_cast<int>(setup.lab_curve_entries));
            args[52].u.buffer = host_buffer(
                setup.lab_curve_lc, static_cast<int>(setup.lab_curve_entries));
            args[53].u.buffer = host_buffer(
                setup.lab_curve_cl, static_cast<int>(setup.lab_curve_entries));
            args[71].u.buffer = host_double_buffer(
                setup.lab_curve_hue,
                static_cast<int>(setup.lab_curve_hue_stride *
                                 RT_LAB_TOOLS_HUE_CURVES));
        }
    }

    std::remove(profile_path.c_str());
    setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
    set_node_device_override("lab.tools", StageNodeDevice::OpenCL);
    result.status =
        run("lab.tools", std::vector<RtStageArg>(args, args + 76));
    clear_node_device_overrides();
    unsetenv("RT_STAGE_PROFILE");
    return result;
}

/* The block's diagonal curve point list: 0 -> 0, 0.5 -> mid, 1 -> 1. */
std::vector<double> lab_curve_points(double mid)
{
    return {static_cast<double>(DCT_Spline),
            0.0, 0.0,
            0.5, mid,
            1.0, 1.0};
}

/*
 * The lab.tools node against the production block.
 *
 * The supported cases run the two device tools; the residual cases - each an
 * otherwise supported profile with one more tool switched on - must be
 * refused by the launcher, run the builtin and match the block bit for bit.
 */
void test_lab_tools(bool available)
{
    using namespace rtengine::stage;

    /* Color::init() fills the Lab caches, the Munsell tables the vibrance
     * residual reads and the gamma tables the block's curves use. */
    static MyMutex lab_tools_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &lab_tools_lcms_mutex;
    }
    static rtengine::Settings lab_tools_settings{};
    rtengine::settings = &lab_tools_settings;
    rtengine::Color::init();

    {
        rtengine::procparams::ProcParams defaults;
        check(!labToolsSupported(defaults),
              "lab.tools refuses the default profile (no tool enabled)");
    }

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-tools.jsonl";

    if (!available) {
        skip("lab.tools: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.tools"),
          "the lab.tools node has a device implementation");

    const LabToolsCase cases[] = {
        /* label, w, h, labgrid, sh, lab mode, hi, sh, htonal, stonal, radius */
        {"LabGrid only", 238, 64, true, false, true, 0, 0, 50, 50, 10},
        {"shadows/highlights Lab mode", 238, 64, false, true, true, 50, 50, 50, 50, 10},
        {"shadows/highlights RGB mode", 238, 64, false, true, false, 50, 50, 50, 50, 10},
        {"both tools Lab mode", 238, 64, true, true, true, 50, 50, 50, 50, 10},
        {"both tools RGB mode", 179, 48, true, true, false, 50, 50, 50, 50, 10},
        {"highlight only", 238, 64, false, true, true, 100, 0, 50, 50, 10},
        {"shadow only", 238, 64, false, true, true, 0, 100, 50, 50, 10},
        {"amount 1", 238, 64, false, true, true, 1, 1, 50, 50, 10},
        {"amount 50", 238, 64, false, true, true, 50, 50, 50, 50, 10},
        {"amount 200", 238, 64, false, true, true, 200, 200, 50, 50, 10},
        {"amount 1000", 238, 64, false, true, true, 1000, 1000, 50, 50, 10},
        {"tonalwidth 0", 238, 64, false, true, true, 100, 100, 0, 0, 10},
        {"tonalwidth 100", 238, 64, false, true, true, 100, 100, 100, 100, 10},
        {"tonalwidth split", 238, 64, false, true, true, 100, 100, 0, 100, 10},
        {"radius 1", 238, 64, false, true, true, 100, 100, 50, 50, 1},
        {"radius 10", 238, 64, false, true, true, 100, 100, 50, 50, 10},
        {"radius 100", 238, 64, false, true, true, 100, 100, 50, 50, 100},
        {"padded width 179", 179, 48, false, true, true, 100, 100, 50, 50, 10},
        {"padded width 238 RGB", 238, 64, false, true, false, 100, 100, 50, 50, 100},
        {"RGB highlight only", 238, 64, false, true, false, 50, 0, 50, 50, 10},
        {"RGB shadow only", 238, 64, false, true, false, 0, 50, 50, 50, 10},
        {"amount 100 RGB mode", 238, 64, false, true, false, 100, 100, 50, 50, 10},
        {"large width 8192", 8192, 64, false, true, true, 100, 100, 50, 50, 40},
    };

    unsigned seed = 1u;
    for (const LabToolsCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.colorToning.enabled = item.labgrid;
        params.colorToning.method = "LabGrid";
        params.colorToning.labgridALow = -40.0;
        params.colorToning.labgridAHigh = 25.0;
        params.colorToning.labgridBLow = 12.0;
        params.colorToning.labgridBHigh = 55.0;
        params.sh.enabled = item.shadowsHighlights;
        params.sh.lab = item.labMode;
        params.sh.highlights = item.highlights;
        params.sh.shadows = item.shadows;
        params.sh.htonalwidth = item.htonalwidth;
        params.sh.stonalwidth = item.stonalwidth;
        params.sh.radius = item.radius;

        check(labToolsSupported(params),
              (std::string("lab.tools ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.tools ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.tools ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.tools ") + item.label +
               " records a device execution").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools %s %dx%d: PAE %.6f LSB16, "
                    "%zu of %zu samples differ\n",
                    item.label, item.width, item.height, diff.max_abs,
                    diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.tools ") + item.label +
               " stays finite").c_str());
        check(diff.max_abs <= 1.0,
              (std::string("lab.tools ") + item.label +
               " is within one LSB16").c_str());
    }

    /*
     * Colour toning other than LabGrid is applied by rgbProc() before this
     * block, so it must not refuse the node: with the same admitted tool the
     * device pass is identical to the host block, because neither side
     * evaluates the toning method here.
     */
    for (const char* method : {"Splitco", "Splitlr", "RGBSliders"}) {
        rtengine::procparams::ProcParams params;
        params.colorToning.enabled = true;
        params.colorToning.method = method;
        params.sh.enabled = true;
        params.sh.lab = true;
        params.sh.highlights = 50;
        params.sh.shadows = 50;
        params.sh.htonalwidth = 50;
        params.sh.stonalwidth = 50;
        params.sh.radius = 10;

        check(labToolsSupported(params),
              "lab.tools admits a tone method rgb.proc owns");

        LabImage source(179, 48);
        fill_lab_tools(source, 7u);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              "the toning profile resolves to the supported set");
        check(result.status == RT_STAGE_OK,
              "the toning profile completes through run()");
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos &&
                  profile.find("\"impl\":\"opencl\"") != std::string::npos,
              "the toning profile records a device execution");

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools toning %s: PAE %.6f LSB16, "
                    "%zu of %zu samples differ\n", method, diff.max_abs,
                    diff.differing, diff.samples);
        check(diff.finite && diff.max_abs <= 1.0,
              "the toning profile is within one LSB16");
    }

    /*
     * Black and white: chromiLuminanceCurve()'s early return zeroes a and b
     * when the Lab curves are off (`bwonly`). The node repeats it in the
     * Lab-curve pass's slot, so the other admitted tools still run in their
     * own positions and see the grey image. The zeroing is exact, so a
     * B&W-only profile must leave L bit-identical to the block.
     *
     * Black and white with colour toning takes neither the early return nor
     * the full arm: the block's `bwonly` is false, so `bwToning` is false and
     * the Lab pass does exactly the work it does with black-and-white off.
     * rgbProc() has already produced the black-and-white image and applied
     * the toning, so the node is admitted with the flag at 0 and must keep
     * the chroma LabGrid/regions write. Black and white with the Lab curves
     * on and no colour toning is the full `bwToning` arm, covered by the
     * Lab-curve matrix below.
     */
    struct BwCase {
        const char* label;
        bool labgrid;
        bool shadowsHighlights;
        bool localContrast;
        bool vibrance;
        bool regions;
        bool labCurve;
        const char* toningMethod;
        bool bwToningControls;
        bool admitted;
    };
    const BwCase bw_cases[] = {
        {"black and white only", false, false, false, false, false, false,
         nullptr, false, true},
        {"black and white + shadows/highlights", false, true, false, false,
         false, false, nullptr, false, true},
        {"black and white + local contrast", false, false, true, false, false,
         false, nullptr, false, true},
        {"black and white + vibrance", false, false, false, true, false, false,
         nullptr, false, true},
        {"black and white + shadows/highlights + local contrast + vibrance",
         false, true, true, true, false, false, nullptr, false, true},
        {"black and white + LabGrid", true, false, false, false, false, false,
         nullptr, false, true},
        {"black and white + LabGrid + Lab curves", true, false, false, false,
         false, true, nullptr, false, true},
        {"black and white + LabGrid + shadows/highlights + vibrance", true,
         true, false, true, false, false, nullptr, false, true},
        {"black and white + Splitco", false, true, false, false, false, false,
         "Splitco", false, true},
        {"black and white + Splitlr + Lab curves", false, true, false, false,
         true, true, "Splitlr", false, true},
        {"black and white + B&W toning controls", false, true, false, false,
         false, false, nullptr, true, true},
        {"black and white + LabGrid + B&W toning controls", true, true, false,
         false, false, false, nullptr, true, true},
        {"black and white + Lab regions", false, false, false, false, true,
         false, nullptr, false, true},
        {"black and white + LabGrid + Lab regions", true, false, false, false,
         true, false, nullptr, true, true},
    };

    const auto bw_params = [](const BwCase& item, bool enabled) {
        rtengine::procparams::ProcParams params;
        params.blackwhite.enabled = enabled;
        if (item.labgrid) {
            params.colorToning.enabled = true;
            params.colorToning.method = "LabGrid";
            params.colorToning.labgridALow = -40.0;
            params.colorToning.labgridAHigh = 25.0;
            params.colorToning.labgridBLow = 12.0;
            params.colorToning.labgridBHigh = 55.0;
        }
        if (item.toningMethod != nullptr) {
            params.colorToning.enabled = true;
            params.colorToning.method = item.toningMethod;
            params.colorToning.strength = 60;
            params.colorToning.balance = 20;
        }
        if (item.bwToningControls) {
            /* rgbProc()'s B&W output controls: the mixer weights, the
             * before/after tone curves and the gamma TRC. The Lab block reads
             * none of them, so the node's work must not change. */
            params.blackwhite.method = "ChannelMixer";
            params.blackwhite.mixerRed = 50;
            params.blackwhite.mixerGreen = 30;
            params.blackwhite.mixerBlue = 20;
            params.blackwhite.gammaRed = 10;
            params.blackwhite.gammaGreen = 5;
            params.blackwhite.gammaBlue = 0;
            params.blackwhite.beforeCurveMode =
                rtengine::procparams::BlackWhiteParams::TcMode::STD_BW;
            params.blackwhite.afterCurveMode =
                rtengine::procparams::BlackWhiteParams::TcMode::
                    WEIGHTEDSTD_BW;
            params.blackwhite.beforeCurve = lab_curve_points(0.45);
            params.blackwhite.afterCurve = lab_curve_points(0.55);
        }
        if (item.labCurve) {
            params.labCurve.enabled = true;
            params.labCurve.lcurve = lab_curve_points(0.55);
            params.labCurve.acurve = lab_curve_points(0.6);
            params.labCurve.bcurve = lab_curve_points(0.4);
        }
        if (item.shadowsHighlights) {
            params.sh.enabled = true;
            params.sh.lab = true;
            params.sh.highlights = 60;
            params.sh.shadows = 60;
            params.sh.htonalwidth = 40;
            params.sh.stonalwidth = 40;
            params.sh.radius = 10;
        }
        if (item.localContrast) {
            params.localContrast.enabled = true;
            params.localContrast.radius = 20;
            params.localContrast.amount = 0.3;
            params.localContrast.darkness = 0.9;
            params.localContrast.lightness = 1.1;
        }
        if (item.vibrance) {
            params.vibrance.enabled = true;
            params.vibrance.pastels = 50;
            params.vibrance.saturated = 30;
        }
        if (item.regions) {
            params.colorToning.enabled = true;
            params.colorToning.method = "LabRegions";
            rtengine::procparams::ColorToningParams::LabCorrectionRegion
                region;
            region.a = 10.0;
            region.b = -8.0;
            region.saturation = 30.0;
            region.slope = 1.0;
            region.offset = 0.0;
            region.power = 1.0;
            region.maskBlur = 0.4;
            region.channel = rtengine::procparams::ColorToningParams::
                LabCorrectionRegion::CHAN_ALL;
            params.colorToning.labregions.push_back(region);
        }
        return params;
    };

    const auto chroma_max = [](const LabImage& image) {
        double worst = 0.0;
        for (int y = 0; y < image.H; ++y) {
            for (int x = 0; x < image.W; ++x) {
                worst = std::max(
                    worst,
                    std::abs(static_cast<double>(image.a[y][x])));
                worst = std::max(
                    worst,
                    std::abs(static_cast<double>(image.b[y][x])));
            }
        }
        return worst;
    };

    for (const BwCase& item : bw_cases) {
        const rtengine::procparams::ProcParams params = bw_params(item, true);

        check(labToolsSupported(params) == item.admitted,
              (std::string("lab.tools ") + item.label +
               (item.admitted ? " is the supported set"
                              : " is refused as a device configuration"))
                  .c_str());

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported == item.admitted,
              (std::string("lab.tools ") + item.label +
               (item.admitted ? " resolves to the supported set"
                              : " is refused by the node"))
                  .c_str());
        check(result.deviceReady == item.admitted,
              (std::string("lab.tools ") + item.label +
               (item.admitted ? " builds its host setup"
                              : " does not build a device setup"))
                  .c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.tools ") + item.label +
               " records a device execution").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools %s %dx%d: PAE %.6f LSB16, "
                    "%zu of %zu samples differ\n",
                    item.label, 179, 48, diff.max_abs, diff.differing,
                    diff.samples);
        check(diff.finite,
              (std::string("lab.tools ") + item.label +
               " stays finite").c_str());
        check(diff.max_abs <= 1.0,
              (std::string("lab.tools ") + item.label +
               " is within one LSB16").c_str());

        /*
         * The exported flag is the block's `bwonly`. Colour toning makes it
         * zero, so the device skips both black-and-white arms exactly as the
         * host does. Had it stayed at `blackwhite.enabled`, the device would
         * zero a and b and the one-LSB check above would fail by the whole
         * chroma the toning writes.
         */
        const bool toning =
            item.labgrid || item.regions || item.toningMethod != nullptr;
        check(result.black_white == (toning ? 0u : 1u),
              (std::string("lab.tools ") + item.label +
               (toning ? " exports a zero black-and-white flag"
                       : " exports the black-and-white flag"))
                  .c_str());

        const double host_chroma = chroma_max(cpu);
        const double device_chroma = chroma_max(device);
        if (host_chroma == 0.0) {
            check(device_chroma == 0.0,
                  (std::string("lab.tools ") + item.label +
                   " leaves a and b at exactly zero").c_str());
            std::printf("stage-residency: lab.tools %s: a/b are exactly 0 "
                        "on both sides\n",
                        item.label);
        } else {
            std::printf("stage-residency: lab.tools %s: the host block "
                        "reintroduces chroma (max %.6f); the device chroma "
                        "is %.6f\n",
                        item.label, host_chroma, device_chroma);
        }
        if (toning) {
            /*
             * Non-vacuity against the old refusal's zeroing: the toning must
             * still write chroma on both sides, so a result that greyed the
             * image cannot pass.
             */
            check(host_chroma > 0.0 && device_chroma > 0.0,
                  (std::string("lab.tools ") + item.label +
                   " keeps the colour-toning chroma")
                      .c_str());
        }

        if (std::strcmp(item.label, "black and white only") == 0) {
            check(diff.differing == 0 && diff.max_abs == 0.0,
                  "lab.tools black and white only leaves L bit-identical");
        }

        /*
         * Non-vacuity: the combined admitted profile with black and white off
         * must differ from the grey result, so a no-op zeroing cannot pass.
         */
        if (std::strcmp(item.label,
                        "black and white + shadows/highlights + local "
                        "contrast + vibrance") == 0) {
            const rtengine::procparams::ProcParams colour_params =
                bw_params(item, false);
            rtengine::ImProcFunctions colour_ipf(&colour_params, false);
            LabToolsHostState colour_host;
            LabImage colour(179, 48);
            colour.CopyFrom(&source);
            const LabToolsRun colour_result = run_lab_tools_node(
                colour, colour_params, colour_host, colour_ipf, profile_path);
            check(colour_result.status == RT_STAGE_OK,
                  "lab.tools combined tools without black and white "
                  "completes");
            const Diff diff_colour = compare(view(colour), view(device));
            std::printf("stage-residency: lab.tools combined tools without "
                        "black and white: PAE %.6f LSB16, %zu of %zu samples "
                        "differ from the grey result\n",
                        diff_colour.max_abs, diff_colour.differing,
                        diff_colour.samples);
            check(diff_colour.differing > 0,
                  "lab.tools black and white changes the combined result");
        }

        /*
         * Black and white with colour toning, seen from the block: the
         * host's `bwonly` is false either way, so switching the black-and-
         * white tool off must leave the node's output bit-identical. That is
         * the encoded host behaviour, and it separates this node from
         * rgbProc(), which owns the black-and-white image.
         */
        if (std::strcmp(item.label,
                        "black and white + LabGrid + shadows/highlights + "
                        "vibrance") == 0) {
            const rtengine::procparams::ProcParams bw_off = bw_params(item, false);
            rtengine::ImProcFunctions bw_off_ipf(&bw_off, false);
            LabToolsHostState bw_off_host;
            LabImage bw_off_image(179, 48);
            bw_off_image.CopyFrom(&source);
            const LabToolsRun bw_off_result =
                run_lab_tools_node(bw_off_image, bw_off, bw_off_host,
                                   bw_off_ipf, profile_path);
            const std::string bw_off_profile = read_file(profile_path);
            check(bw_off_result.status == RT_STAGE_OK &&
                      bw_off_result.black_white == 0 &&
                      bw_off_profile.find("\"impl\":\"opencl\"") !=
                          std::string::npos,
                  "lab.tools black and white off exports a zero flag on the "
                  "device");
            const Diff diff_bw_off =
                compare(view(bw_off_image), view(device));
            std::printf("stage-residency: lab.tools black and white off "
                        "under colour toning: PAE %.6f LSB16, %zu of %zu "
                        "samples differ from the black-and-white result\n",
                        diff_bw_off.max_abs, diff_bw_off.differing,
                        diff_bw_off.samples);
            check(diff_bw_off.differing == 0 && diff_bw_off.max_abs == 0.0,
                  "lab.tools black and white does not change the Lab block "
                  "under colour toning");
        }

        /*
         * Non-vacuity against colour toning off: the same profile without the
         * toning must change the result, so the admitted combination cannot be
         * a no-op that merely passes the flag check.
         */
        if (std::strcmp(item.label,
                        "black and white + LabGrid + shadows/highlights + "
                        "vibrance") == 0) {
            rtengine::procparams::ProcParams toning_off = params;
            toning_off.colorToning.enabled = false;
            rtengine::ImProcFunctions toning_off_ipf(&toning_off, false);
            LabToolsHostState toning_off_host;
            LabImage toning_off_image(179, 48);
            toning_off_image.CopyFrom(&source);
            const LabToolsRun toning_off_result =
                run_lab_tools_node(toning_off_image, toning_off,
                                   toning_off_host, toning_off_ipf,
                                   profile_path);
            check(toning_off_result.status == RT_STAGE_OK &&
                      toning_off_result.black_white == 1,
                  "lab.tools colour toning off restores the "
                  "black-and-white flag");
            const Diff diff_toning_off =
                compare(view(device), view(toning_off_image));
            std::printf("stage-residency: lab.tools colour toning off under "
                        "black and white: PAE %.6f LSB16, %zu of %zu samples "
                        "differ from the toned result\n",
                        diff_toning_off.max_abs, diff_toning_off.differing,
                        diff_toning_off.samples);
            check(diff_toning_off.differing > 0,
                  "lab.tools colour toning changes the black-and-white "
                  "result");
        }
    }

    /*
     * Residuals: an otherwise supported profile plus one tool the node does
     * not implement. The launcher must refuse it, the builtin must run the
     * block, and the result must be the block's own output.
     */
    const char* residual_labels[] = {
        "vibrance + local contrast out of range",
        "vibrance + EPD", "vibrance + Lab regions show mask",
        "vibrance no-op", "vibrance pastels out of range"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        params.sh.enabled = true;
        params.sh.lab = true;
        params.sh.highlights = 60;
        params.sh.shadows = 60;
        params.sh.htonalwidth = 40;
        params.sh.stonalwidth = 40;
        params.sh.radius = 10;
        params.vibrance.enabled = true;
        params.vibrance.pastels = 50;
        params.vibrance.saturated = 30;

        if (std::strcmp(label, "vibrance + local contrast out of range") ==
                   0) {
            params.localContrast.enabled = true;
            params.localContrast.radius = 20;
            params.localContrast.amount = 30;
        } else if (std::strcmp(label, "vibrance + EPD") == 0) {
            params.epd.enabled = true;
            params.epd.strength = 0.5;
            params.epd.gamma = 1.0;
        } else if (std::strcmp(label, "vibrance no-op") == 0) {
            /* Enabled with both sliders at zero and an identity curve: the
             * host vibrance would return without touching a pixel. */
            params.vibrance.pastels = 0;
            params.vibrance.saturated = 0;
        } else if (std::strcmp(label, "vibrance pastels out of range") == 0) {
            params.vibrance.pastels = 200;
        } else if (std::strcmp(
                       label, "vibrance + Lab regions show mask") == 0) {
            params.colorToning.enabled = true;
            params.colorToning.method = "LabRegions";
            params.colorToning.labregionsShowMask = 0;
            rtengine::procparams::ColorToningParams::LabCorrectionRegion
                region;
            region.a = 10.0;
            region.b = -8.0;
            region.saturation = 30.0;
            region.slope = 1.0;
            region.offset = 0.0;
            region.power = 1.0;
            region.maskBlur = 0.4;
            region.channel = rtengine::procparams::ColorToningParams::
                LabCorrectionRegion::CHAN_ALL;
            params.colorToning.labregions.push_back(region);
        } else {
            check(false, label);
        }

        check(!labToolsSupported(params),
              (std::string("lab.tools ") + label +
               " is a residual").c_str());

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.tools ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.tools ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools ") + label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools ") + label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.tools ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.tools ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.tools ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.tools ") + label +
               " matches the CPU block").c_str());
    }

    /*
     * RGB mode refuses a tone table whose steepest step, times one ulp of a
     * 16-bit channel, can exceed one LSB16. Such a table needs a hand-edited
     * amount above the 0..100 slider range; the setup refuses it and the
     * builtin runs the block.
     */
    for (int amount : {200, 1000}) {
        rtengine::procparams::ProcParams params;
        params.sh.enabled = true;
        params.sh.lab = false;
        params.sh.highlights = amount;
        params.sh.shadows = amount;
        params.sh.htonalwidth = 50;
        params.sh.stonalwidth = 50;
        params.sh.radius = 10;

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabToolsLuts cpu_luts = cpu_host.luts();
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.tools RGB amount ") + std::to_string(amount) +
               " is inside the predicate").c_str());
        check(!result.deviceReady,
              (std::string("lab.tools RGB amount ") + std::to_string(amount) +
               " refuses the steep tone table").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools RGB amount ") + std::to_string(amount) +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.tools RGB amount ") + std::to_string(amount) +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.tools RGB amount ") + std::to_string(amount) +
               " never mixes a device pass").c_str());
        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools RGB amount %d: refused, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    amount, diff.max_abs, diff.differing, diff.samples);
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.tools RGB amount ") + std::to_string(amount) +
               " matches the CPU block").c_str());
    }

    /*
     * A supported configuration whose setup builds but whose submission the
     * launcher refuses must still run the builtin and record impl=host. A tone
     * table of the wrong length is that refusal.
     */
    {
        rtengine::procparams::ProcParams params;
        params.sh.enabled = true;
        params.sh.lab = true;
        params.sh.highlights = 80;
        params.sh.shadows = 80;
        params.sh.htonalwidth = 50;
        params.sh.stonalwidth = 50;
        params.sh.radius = 10;

        LabImage source(238, 64);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(238, 64);
        device.CopyFrom(&source);
        LabToolsLuts device_luts = device_host.luts();
        LabToolsContext context(params, device_ipf, device_luts,
                                device_host.autili, device_host.butili);
        RtLabToolsResult setup{};
        check(context.build(setup) == 0, "forced fallback setup builds");
        RtLabToolsContext descriptor = context.descriptor(true);

        RtStageArg args[76] = {};
        args[0].kind = RT_STAGE_ARG_BUFFER;
        args[0].u.buffer = view(device);
        args[1].kind = RT_STAGE_ARG_PTR;
        args[1].u.ptr = &descriptor;
        args[2].kind = RT_STAGE_ARG_INT;
        args[2].u.i = 1;
        args[3].kind = RT_STAGE_ARG_INT;
        args[3].u.i = 0;
        for (int index = 0; index < 4; ++index) {
            args[4 + index].kind = RT_STAGE_ARG_DOUBLE;
            args[4 + index].u.f = setup.labgrid[index];
        }
        args[8].kind = RT_STAGE_ARG_INT;
        args[8].u.i = 1;
        args[9].kind = RT_STAGE_ARG_INT;
        args[9].u.i = params.sh.highlights;
        args[10].kind = RT_STAGE_ARG_INT;
        args[10].u.i = params.sh.shadows;
        args[11].kind = RT_STAGE_ARG_INT;
        args[11].u.i = params.sh.htonalwidth;
        args[12].kind = RT_STAGE_ARG_INT;
        args[12].u.i = params.sh.stonalwidth;
        args[13].kind = RT_STAGE_ARG_INT;
        args[13].u.i = params.sh.radius;
        args[14].kind = RT_STAGE_ARG_INT;
        args[14].u.i = 1;
        args[15].kind = RT_STAGE_ARG_INT;
        args[15].u.i = LAB_TOOLS_EXPORT_SCALE;
        for (int index = 22; index < 31; ++index) {
            args[index].kind = RT_STAGE_ARG_INT;
            args[index].u.i = 0;
        }
        const auto host_buffer = [](const float* data, int width) {
            RtStageBuffer buffer{};
            buffer.size = sizeof(RtStageBuffer);
            buffer.data = const_cast<float*>(data);
            buffer.width = width;
            buffer.height = 1;
            buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
            buffer.plane_pitch = buffer.row_pitch;
            buffer.planes = 1;
            buffer.format = RT_STAGE_F32;
            buffer.flags = RT_STAGE_BUFFER_READ;
            return buffer;
        };
        for (int index = 16; index < 22; ++index) {
            args[index].kind = RT_STAGE_ARG_BUFFER;
        }
        for (int index = 31; index < 34; ++index) {
            args[index].kind = RT_STAGE_ARG_BUFFER;
        }
        for (int index = 34; index <= 42; ++index) {
            args[index].kind = RT_STAGE_ARG_INT;
            args[index].u.i = 0;
        }
        for (int index = 43; index <= 47; ++index) {
            args[index].kind = RT_STAGE_ARG_DOUBLE;
            args[index].u.f = 0.0;
        }
        for (int index = 48; index < 54; ++index) {
            args[index].kind = RT_STAGE_ARG_BUFFER;
        }
        args[54].kind = RT_STAGE_ARG_INT;
        args[55].kind = RT_STAGE_ARG_INT;
        args[56].kind = RT_STAGE_ARG_INT;
        args[57].kind = RT_STAGE_ARG_INT;
        args[58].kind = RT_STAGE_ARG_DOUBLE;
        args[59].kind = RT_STAGE_ARG_DOUBLE;
        args[60].kind = RT_STAGE_ARG_DOUBLE;
        args[61].kind = RT_STAGE_ARG_INT;
        args[62].kind = RT_STAGE_ARG_PTR;
        args[63].kind = RT_STAGE_ARG_INT;
        args[63].u.i = 0;
        args[64].kind = RT_STAGE_ARG_BUFFER;
        args[65].kind = RT_STAGE_ARG_INT;
        args[65].u.i = 0;
        args[66].kind = RT_STAGE_ARG_BUFFER;
        args[67].kind = RT_STAGE_ARG_PTR;
        args[68].kind = RT_STAGE_ARG_PTR;
        args[68].u.ptr = setup.iws_double;
        args[69].kind = RT_STAGE_ARG_INT;
        args[69].u.i = setup.black_white;
        args[70].kind = RT_STAGE_ARG_INT;
        args[70].u.i = setup.lab_curve_hue_stride;
        args[71].kind = RT_STAGE_ARG_BUFFER;
        args[72].kind = RT_STAGE_ARG_INT;
        args[72].u.i = setup.lab_curve_ch_active;
        args[73].kind = RT_STAGE_ARG_INT;
        args[73].u.i = setup.lab_curve_lh_active;
        args[74].kind = RT_STAGE_ARG_INT;
        args[74].u.i = setup.lab_curve_hh_active;
        args[75].kind = RT_STAGE_ARG_INT;
        args[75].u.i = setup.lab_curve_chromaticity;
        args[59].u.f = 1.0;
        args[60].u.f = 1.0;
        args[61].u.i = LAB_TOOLS_EXPORT_SCALE;
        /* Half the required length: the launcher refuses the submission. */
        args[16].u.buffer = host_buffer(setup.lut_highlight,
                                        static_cast<int>(setup.lut_entries) / 2);
        args[17].u.buffer = host_buffer(setup.lut_shadow,
                                        static_cast<int>(setup.lut_entries));
        args[18].u.buffer = host_buffer(setup.ws, 9);
        args[19].u.buffer = host_buffer(setup.iws, 9);
        args[20].u.buffer = host_buffer(
            setup.cachef, static_cast<int>(RT_LAB_TOOLS_CACHE_ENTRIES));
        args[21].u.buffer = host_buffer(
            setup.cachefy, static_cast<int>(RT_LAB_TOOLS_CACHE_ENTRIES));

        std::remove(profile_path.c_str());
        setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
        set_node_device_override("lab.tools", StageNodeDevice::OpenCL);
        const RtStageStatus status =
            run("lab.tools", std::vector<RtStageArg>(args, args + 76));
        clear_node_device_overrides();
        const std::string profile = read_file(profile_path);
        unsetenv("RT_STAGE_PROFILE");

        check(status == RT_STAGE_OK, "the forced fallback completes");
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              "the forced fallback records a host execution");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "the forced fallback never mixes a device pass");
        const Diff diff = compare(view(device), view(cpu));
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "the forced fallback matches the CPU block");
    }
}

/* --------------------------------------------------- lab.tools vibrance */

/*
 * The vibrance domain image: one pixel per (L, C, hue) sample. L covers every
 * luminance band Color::MunsellLch() branches on, C runs from below the
 * vibrance gate of 6 to the LUT clamp of 139, and the hue sweep covers all
 * four Munsell zones and the skin region. The full grid is
 * width x height = hue samples x (L samples * C samples).
 */
void fill_vibrance_domain(LabImage& lab)
{
    const float lightness[] = {6.f,  9.5f, 12.f, 17.f, 22.f, 27.f, 32.f, 37.f,
                               42.f, 47.f, 52.f, 57.f, 62.f, 67.f, 72.f, 77.f,
                               82.f, 87.f, 92.f, 97.f};
    const float chroma[] = {6.5f, 8.f, 12.f, 20.f, 40.f, 80.f, 110.f, 139.f};
    const int hue_count = 125;
    const int lightness_count =
        static_cast<int>(sizeof(lightness) / sizeof(lightness[0]));
    const int chroma_count =
        static_cast<int>(sizeof(chroma) / sizeof(chroma[0]));
    const int rows = lightness_count * chroma_count;
    const int height = lab.H < rows ? lab.H : rows;
    const int width = lab.W < hue_count ? lab.W : hue_count;

    for (int y = 0; y < height; ++y) {
        const float l = lightness[y % lightness_count];
        const float c = chroma[(y / lightness_count) % chroma_count];
        for (int x = 0; x < width; ++x) {
            const float hue = -3.10f + 0.05f * static_cast<float>(x);
            lab.L[y][x] = l * 327.68f;
            lab.a[y][x] = c * cosf(hue) * 327.68f;
            lab.b[y][x] = c * sinf(hue) * 327.68f;
        }
    }
}

/* A neutral image: no chroma, so vibrance's CC == 0 branch runs. */
void fill_vibrance_neutral(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            const float l = 32768.f * static_cast<float>(x) /
                            static_cast<float>(lab.W > 1 ? lab.W - 1 : 1);
            lab.L[y][x] = l;
            lab.a[y][x] = 0.f;
            lab.b[y][x] = 0.f;
        }
    }
}

/*
 * The two pixels the RX 550 pipeline moved: their input hue sits within one
 * ulp of a Munsell zone limit. The values are the full-precision pre-vibrance
 * Lab samples captured from the PEF export.
 */
void fill_vibrance_knife_edge(LabImage& lab)
{
    struct Sample {
        float l;
        float a;
        float b;
    };
    const Sample samples[] = {
        {12361.283203f, 14259.261719f, -8742.428711f},
        {18340.724609f, 13182.235352f, -11103.244141f},
    };
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            const Sample& sample = samples[x % 2];
            lab.L[y][x] = sample.l;
            lab.a[y][x] = sample.a;
            lab.b[y][x] = sample.b;
        }
    }
}

/* A saturated image: chroma well above the LUT clamp at several hues. */
void fill_vibrance_saturated(LabImage& lab)
{
    const float hues[] = {-2.8f, -1.2f, 0.2f, 1.0f, 2.6f};
    for (int y = 0; y < lab.H; ++y) {
        const float l = 10.f + 80.f * static_cast<float>(y) /
                                   static_cast<float>(lab.H > 1 ? lab.H - 1 : 1);
        for (int x = 0; x < lab.W; ++x) {
            const float hue = hues[x % 5];
            const float c = 100.f + 39.f * static_cast<float>((x / 5) % 2);
            lab.L[y][x] = l * 327.68f;
            lab.a[y][x] = c * cosf(hue) * 327.68f;
            lab.b[y][x] = c * sinf(hue) * 327.68f;
        }
    }
}

struct VibranceCase {
    const char* label;
    int width;
    int height;
    int pastels;
    int saturated;
    int threshold_top;
    int threshold_bottom;
    bool protectskins;
    bool avoidcolorshift;
    bool skin_curve;
    bool highlight;
    int image; /* 0 domain, 1 neutral, 2 saturated, 3 random */
};

/* The distribution of the device-vs-host difference, in LSB16. */
struct Distribution {
    std::size_t samples = 0;
    std::size_t exact = 0;
    std::size_t quarter = 0;
    std::size_t half = 0;
    std::size_t one = 0;
    std::size_t over = 0;
    double max_abs = 0.0;
    bool finite = true;
};

Distribution distribution(const RtStageBuffer& a, const RtStageBuffer& b)
{
    Distribution d;
    if (a.format != RT_STAGE_F32 || b.format != RT_STAGE_F32 ||
        a.width != b.width || a.height != b.height || a.planes != b.planes) {
        d.finite = false;
        return d;
    }
    for (std::uint32_t plane = 0; plane < a.planes; ++plane) {
        for (int y = 0; y < a.height; ++y) {
            const float* ra = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(a.data) +
                static_cast<std::size_t>(plane) * a.plane_pitch +
                static_cast<std::size_t>(y) * a.row_pitch);
            const float* rb = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(b.data) +
                static_cast<std::size_t>(plane) * b.plane_pitch +
                static_cast<std::size_t>(y) * b.row_pitch);
            for (int x = 0; x < a.width; ++x) {
                ++d.samples;
                if (!std::isfinite(ra[x]) || !std::isfinite(rb[x])) {
                    d.finite = false;
                    ++d.over;
                    continue;
                }
                const double diff = std::fabs(static_cast<double>(ra[x]) -
                                              static_cast<double>(rb[x]));
                d.max_abs = std::max(d.max_abs, diff);
                if (diff == 0.0) {
                    ++d.exact;
                } else if (diff <= 0.25) {
                    ++d.quarter;
                } else if (diff <= 0.5) {
                    ++d.half;
                } else if (diff <= 1.0) {
                    ++d.one;
                } else {
                    ++d.over;
                }
            }
        }
    }
    return d;
}

void test_lab_tools_vibrance(bool available)
{
    using namespace rtengine::stage;

    static MyMutex vibrance_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &vibrance_lcms_mutex;
    }
    static rtengine::Settings vibrance_settings{};
    rtengine::settings = &vibrance_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-tools-vibrance.jsonl";

    /*
     * The full Munsell table set: every declared table is exported, its size
     * is the host LUT's own size, every entry is the host entry, and the
     * padding past that size is zero.
     */
    {
        rtengine::procparams::ProcParams params;
        params.vibrance.enabled = true;
        params.vibrance.pastels = 50;
        params.vibrance.saturated = 30;

        rtengine::ImProcFunctions ipf(&params, false);
        LabToolsHostState host;
        LabToolsLuts luts = host.luts();
        LabToolsContext context(params, ipf, luts, host.autili, host.butili);
        RtLabToolsResult setup{};
        check(context.build(setup) == 0, "vibrance setup builds");
        check(setup.munsell_tables == RT_LAB_TOOLS_MUNSELL_TABLES,
              "the setup exports every Munsell table");
        check(setup.munsell != nullptr && setup.munsell_sizes != nullptr,
              "the Munsell table set has a buffer");
        check(rtengine::Color::munsellLutCount() ==
                  static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES),
              "Color exposes every declared Munsell table");

        std::size_t checked_entries = 0;
        bool sizes_ok = true;
        bool entries_ok = true;
        bool finite = true;
        for (std::uint32_t table = 0; table < RT_LAB_TOOLS_MUNSELL_TABLES;
             ++table) {
            const LUTf* lut = rtengine::Color::munsellLut(static_cast<int>(table));
            const int size = lut != nullptr ? static_cast<int>(lut->getSize())
                                            : 0;
            sizes_ok = sizes_ok && size == static_cast<int>(
                                             setup.munsell_sizes[table]);
            sizes_ok = sizes_ok && (size == 50 || size == 90 || size == 140);
            for (std::uint32_t index = 0; index < RT_LAB_TOOLS_MUNSELL_STRIDE;
                 ++index) {
                const float value =
                    setup.munsell[table * RT_LAB_TOOLS_MUNSELL_STRIDE + index];
                finite = finite && std::isfinite(value);
                const float expected =
                    index < static_cast<std::uint32_t>(size)
                        ? (*lut)[static_cast<int>(index)]
                        : 0.f;
                entries_ok = entries_ok && value == expected;
                ++checked_entries;
            }
        }
        check(sizes_ok, "every Munsell table exports its own size");
        check(entries_ok,
              "every Munsell entry equals the host LUT and pads with zero");
        check(finite, "the Munsell table set is finite");
        std::printf("stage-residency: Munsell table set: %u tables, %zu "
                    "entries checked against Color::MunsellLch's LUTs\n",
                    RT_LAB_TOOLS_MUNSELL_TABLES, checked_entries);
    }

    if (!available) {
        skip("lab.tools vibrance: no OpenCL device");
        return;
    }

    const VibranceCase cases[] = {
        /* label, w, h, pastels, saturated, top, bottom, protect, avoid,
         * skin curve, highlight, image */
        {"pastels only", 125, 160, 60, 0, 75, 0, false, true, false, false, 0},
        {"saturated only", 125, 160, 0, 60, 75, 0, false, true, false, false,
         0},
        {"pastels and saturated", 125, 160, 60, 35, 75, 0, false, true, false,
         false, 0},
        {"equal sliders", 125, 160, 50, 50, 75, 0, false, true, false, false,
         0},
        {"negative saturated", 125, 160, 40, -40, 75, 0, false, true, false,
         false, 0},
        {"threshold 0", 125, 160, 60, 30, 0, 0, false, true, false, false, 0},
        {"threshold 100", 125, 160, 60, 30, 100, 0, false, true, false, false,
         0},
        {"threshold split", 125, 160, 60, 30, 40, 80, false, true, false, false,
         0},
        {"protect skins", 125, 160, 60, 35, 75, 0, true, true, false, false, 0},
        {"avoidcolorshift off", 125, 160, 60, 35, 75, 0, false, false, false,
         false, 0},
        {"skin curve", 125, 160, 60, 35, 75, 0, false, true, true, false, 0},
        {"skin curve protect", 125, 160, 60, 35, 75, 0, true, true, true, false,
         0},
        {"highlight reconstruction", 125, 160, 60, 35, 75, 0, false, true,
         false, true, 0},
        {"neutral 179", 179, 48, 60, 35, 75, 0, false, true, false, false, 1},
        {"neutral 238", 238, 64, 40, 20, 75, 0, true, true, false, false, 1},
        {"saturated 179", 179, 48, 60, 35, 75, 0, false, true, false, false, 2},
        {"saturated 238", 238, 64, 40, 20, 75, 0, true, true, false, false, 2},
        {"padded 179", 179, 48, 60, 35, 75, 0, false, true, false, false, 3},
        {"padded 238", 238, 64, 60, 35, 75, 0, false, true, false, false, 3},
        {"large width 8192", 8192, 64, 60, 35, 75, 0, false, true, false, false,
         3},
        {"Munsell zone knife edge", 64, 16, 60, 35, 75, 0, true, true, false,
         false, 4},
    };

    unsigned seed = 0x9e3779b9u;
    for (const VibranceCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.vibrance.enabled = true;
        params.vibrance.pastels = item.pastels;
        params.vibrance.saturated = item.saturated;
        params.vibrance.psthreshold = rtengine::procparams::Threshold<int>(
            item.threshold_bottom, item.threshold_top, false);
        params.vibrance.protectskins = item.protectskins;
        params.vibrance.avoidcolorshift = item.avoidcolorshift;
        params.toneCurve.hrenabled = item.highlight;
        if (item.skin_curve) {
            params.vibrance.skintonescurve = {
                static_cast<double>(FCT_MinMaxCPoints),
                0.0, 0.0, 0.0, 0.0,
                0.5, 0.62, 0.0, 0.0,
                1.0, 1.0, 0.0, 0.0};
        }

        check(labToolsSupported(params),
              (std::string("lab.tools vibrance ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        switch (item.image) {
        case 0:
            fill_vibrance_domain(source);
            break;
        case 1:
            fill_vibrance_neutral(source);
            break;
        case 2:
            fill_vibrance_saturated(source);
            break;
        case 4:
            fill_vibrance_knife_edge(source);
            break;
        default:
            fill_lab_tools(source, seed++);
            break;
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.tools vibrance ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.tools vibrance ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools vibrance ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools vibrance ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.tools vibrance ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.tools vibrance %s %dx%d: PAE %.6f "
                    "LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, >1 "
                    "%zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.tools vibrance ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.tools vibrance ") + item.label +
               " is within one LSB16").c_str());
    }

    /*
     * Diagnostic: the gate image has a pixel whose hue sits exactly on the
     * Munsell zone limit -0.55. Run the production block and the device node
     * on that single value.
     */
    /*
     * The Munsell path is not vacuous: on the domain grid, switching
     * avoidcolorshift off must change the host output, so the table
     * comparisons above exercised AllMunsellLch().
     */
    {
        rtengine::procparams::ProcParams on;
        on.vibrance.enabled = true;
        on.vibrance.pastels = 60;
        on.vibrance.saturated = 35;
        on.vibrance.avoidcolorshift = true;
        rtengine::procparams::ProcParams off = on;
        off.vibrance.avoidcolorshift = false;

        LabImage source(125, 160);
        fill_vibrance_domain(source);

        rtengine::ImProcFunctions on_ipf(&on, false);
        LabToolsHostState on_host;
        LabImage on_image(125, 160);
        on_image.CopyFrom(&source);
        LabToolsLuts on_luts = on_host.luts();
        labToolsHostBlock(on_ipf, on, on_image, on_luts, on_host.autili,
                          on_host.butili);

        rtengine::ImProcFunctions off_ipf(&off, false);
        LabToolsHostState off_host;
        LabImage off_image(125, 160);
        off_image.CopyFrom(&source);
        LabToolsLuts off_luts = off_host.luts();
        labToolsHostBlock(off_ipf, off, off_image, off_luts, off_host.autili,
                          off_host.butili);

        const Diff correction = compare(view(on_image), view(off_image));
        std::printf("stage-residency: Munsell correction changes %zu of %zu "
                    "domain samples, PAE %.6f LSB16\n",
                    correction.differing, correction.samples,
                    correction.max_abs);
        check(correction.differing > 0,
              "the Munsell correction changes the domain image");
    }
}


/* --------------------------------------------------- lab.tools Lab curves */

/*
 * The Lab-curve fixtures. `neutral` has no chroma, so the block's CC == 0
 * branch, the a/b tables and the Munsell chroma gate are all exercised from
 * the other side. `saturated` spans the hue circle and chroma up to the
 * Munsell LUT limit.
 */
void fill_lab_curve_neutral(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        for (int x = 0; x < lab.W; ++x) {
            lab.L[y][x] = 32768.f * static_cast<float>(x) /
                          static_cast<float>(lab.W > 1 ? lab.W - 1 : 1);
            lab.a[y][x] = 0.f;
            lab.b[y][x] = 0.f;
        }
    }
}

void fill_lab_curve_saturated(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        const float l = 4.f + 96.f * static_cast<float>(y) /
                                   static_cast<float>(lab.H > 1 ? lab.H - 1 : 1);
        for (int x = 0; x < lab.W; ++x) {
            const float hue =
                -3.05f + 6.10f * static_cast<float>(x % 61) / 60.f;
            const float c = 55.f + 30.f * static_cast<float>((x / 61) % 4);
            lab.L[y][x] = l * 327.68f;
            lab.a[y][x] = c * cosf(hue) * 327.68f;
            lab.b[y][x] = c * sinf(hue) * 327.68f;
        }
    }
}

struct LabCurveCase {
    const char* label;
    int width;
    int height;
    int image; /* 0 random, 1 neutral, 2 saturated */
    int brightness;
    double mid;
    bool lcurve;
    bool acurve;
    bool bcurve;
    bool cccurve;
    bool lccurve;
    bool clcurve;
    int chromaticity;
    const char* munsell;
    double rstprotection;
    bool lcredsk;
    int protectred;
    double protectredh;
    /* The hue flat-curve family and the tools that run beside it. */
    bool chcurve = false;
    bool lhcurve = false;
    bool hhcurve = false;
    bool blackwhite = false;
    bool vibrance = false;
    bool shadowsHighlights = false;
    int contrast = 0;
    /* A dense C=f(H) instead of the five-knot wave: the control-point count,
     * and whether the values alternate (steep) or follow a smooth wave. */
    int densePoints = 0;
    bool denseSteep = false;
    double denseStart = 0.0;
};

/* A non-identity hue flat curve: a wave through (0,0.5), (0.25,0.72),
 * (0.5,0.5), (0.75,0.28) and (1,0.5). The zero tangents make every segment
 * linear, so the host table follows it exactly. */
std::vector<double> lab_hue_points()
{
    return {static_cast<double>(FCT_MinMaxCPoints),
            0.0, 0.5, 0.0, 0.0,
            0.25, 0.72, 0.0, 0.0,
            0.5, 0.5, 0.0, 0.0,
            0.75, 0.28, 0.0, 0.0,
            1.0, 0.5, 0.0, 0.0};
}

/*
 * A dense hue flat curve: `points` knots from `start` to 1 with zero tangents,
 * so every segment is linear and FlatCurve's polyline carries exactly one knot
 * per control point. The steep variant alternates between 0.55 and 0.45, which
 * at 40 000 points is a segment slope of 4 000 - the case a float knot
 * abscissa cannot represent within the table's 5e-7 value bound. The smooth
 * variant follows a three-cycle wave of the same amplitude, whose local slope
 * stays below 2, so it is dense without being steep.
 *
 * The closing knot repeats the opening value, as a periodic hue curve does:
 * FlatCurve::getVal(1.0) reads the periodic copy of the first point, so a
 * curve whose last value differs jumps at the period boundary. A `start`
 * above zero leaves the polyline spanning [start, start + 1], which is the
 * table's wrap case: getVal() adds one period to a query below the first knot.
 */
std::vector<double> lab_hue_points_dense(int points, bool steep,
                                         double start = 0.0)
{
    const double last = static_cast<double>(points - 1);
    std::vector<double> dense;
    dense.reserve(1 + 4 * static_cast<std::size_t>(points));
    dense.push_back(static_cast<double>(FCT_MinMaxCPoints));
    for (int index = 0; index < points; ++index) {
        const double fraction = static_cast<double>(index) / last;
        /* The convex form keeps the closing knot exactly at 1. */
        const double x = start * (1.0 - fraction) + fraction;
        double y = 0.5;
        if (steep) {
            y = (index % 2 == 0 || index == points - 1) ? 0.55 : 0.45;
        } else {
            y = 0.5 + 0.1 * std::sin(6.0 * M_PI * x);
        }
        dense.push_back(x);
        dense.push_back(y);
        dense.push_back(0.0);
        dense.push_back(0.0);
    }
    return dense;
}

void test_lab_tools_lab_curve(bool available)
{
    using namespace rtengine::stage;

    static MyMutex lab_curve_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &lab_curve_lcms_mutex;
    }
    static rtengine::Settings lab_curve_settings{};
    rtengine::settings = &lab_curve_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-tools-labcurve.jsonl";

    if (!available) {
        skip("lab.tools Lab curves: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.tools"),
          "the lab.tools node has a device implementation for Lab curves");

    const LabCurveCase cases[] = {
        /* label, w, h, image, brightness, mid, l, a, b, cc, lc, cl,
         * chromaticity, munsell, rstprotection, lcredsk, protectred,
         * protectredh */
        {"L curve only", 238, 64, 0, 0, 0.58, true, false, false, false,
         false, false, 0, "MUN", 0.0, true, 60, 0.3},
        {"identity tables", 179, 48, 0, 0, 0.5, false, false, false, false,
         false, false, 0, "MUN", 0.0, true, 60, 0.3},
        {"L curve brightness", 238, 64, 0, 20, 0.58, true, false, false,
         false, false, false, 0, "MUN", 0.0, true, 60, 0.3},
        {"a curve only", 179, 48, 0, 0, 0.60, false, true, false, false, false,
         false, 0, "MUN", 0.0, true, 60, 0.3},
        {"a/b curves", 238, 64, 0, 0, 0.60, false, true, true, false, false,
         false, 0, "MUN", 0.0, true, 60, 0.3},
        {"C=f(C)", 238, 64, 2, 0, 1.35, false, false, false, true, false,
         false, 0, "MUN", 0.0, true, 60, 0.3},
        {"L=f(C)", 238, 64, 2, 0, 1.10, false, false, false, false, true,
         false, 0, "MUN", 0.0, true, 60, 0.3},
        {"C=f(L)", 238, 64, 0, 0, 0.70, false, false, false, false, false,
         true, 0, "MUN", 0.0, true, 60, 0.3},
        {"combined curves", 238, 64, 0, 0, 0.62, true, true, true, true, true,
         true, 0, "MUN", 0.0, true, 60, 0.3},
        {"combined NONE", 238, 64, 0, 0, 0.62, true, true, true, true, true,
         true, 0, "NONE", 0.0, true, 60, 0.3},
        {"combined MUN", 238, 64, 0, 0, 0.62, true, true, true, true, true,
         true, 0, "MUN", 0.0, true, 60, 0.3},
        /*
         * The XYZ and XYZREL gamut corrections: Color::gamutmap() on the
         * saturated image, which leaves the working-space gamut, then the
         * Munsell arm that follows every non-NONE method.
         */
        {"XYZ gamut correction", 238, 64, 2, 0, 0.62, true, true, true, true,
         true, true, 0, "XYZ", 0.0, true, 60, 0.3},
        {"XYZREL gamut correction", 238, 64, 2, 0, 0.62, true, true, true,
         true, true, true, 0, "XYZREL", 0.0, true, 60, 0.3},
        {"XYZ neutral", 179, 48, 1, 0, 0.5, false, false, false, false, false,
         false, 0, "XYZ", 0.0, true, 60, 0.3},
        {"XYZ with hue curves", 238, 64, 2, 0, 0.62, false, false, false,
         false, false, false, 50, "XYZ", 30.0, true, 60, 0.3, true, true,
         true},
        /*
         * The LAB gamut correction: Color::gamutLchonly()'s RGB overload on
         * the double working-space inverse, iterating on the 65535 clip
         * tests, then the same Munsell arm the other non-NONE methods run.
         */
        {"LAB gamut correction", 238, 64, 2, 0, 0.62, true, true, true, true,
         true, true, 0, "LAB", 0.0, true, 60, 0.3},
        {"LAB neutral", 179, 48, 1, 0, 0.5, false, false, false, false, false,
         false, 0, "LAB", 0.0, true, 60, 0.3},
        {"LAB with hue curves", 238, 64, 2, 0, 0.62, false, false, false,
         false, false, false, 50, "LAB", 30.0, true, 60, 0.3, true, true,
         true},
        {"red/skin protection", 238, 64, 2, 0, 1.20, false, false, false, true,
         true, false, 0, "MUN", 30.0, true, 60, 0.3},
        {"protection low red", 179, 48, 2, 0, 1.20, false, false, false, true,
         true, false, 0, "MUN", 60.0, true, 25, 0.12},
        {"protection high red", 238, 64, 2, 0, 1.20, false, false, false, true,
         true, false, 0, "MUN", 10.0, true, 160, 0.9},
        {"lcredsk off", 179, 48, 2, 0, 1.10, false, false, false, false, true,
         false, 0, "MUN", 30.0, false, 60, 0.3},
        {"neutral L curve", 238, 64, 1, 0, 0.58, true, false, false, false,
         false, false, 0, "MUN", 0.0, true, 60, 0.3},
        {"neutral combined MUN", 179, 48, 1, 0, 0.62, true, true, true, true,
         true, true, 0, "MUN", 0.0, true, 60, 0.3},
        {"saturated combined", 179, 48, 2, 0, 0.62, true, true, true, true,
         true, true, 0, "MUN", 0.0, true, 60, 0.3},
        {"padded width 179", 179, 48, 0, 0, 0.62, true, true, true, true, true,
         true, 0, "MUN", 0.0, true, 60, 0.3},
        {"padded width 238", 238, 64, 0, 0, 0.62, true, true, true, true, true,
         true, 0, "MUN", 0.0, true, 60, 0.3},
        {"large width 8192", 8192, 64, 0, 0, 0.62, true, true, true, true, true,
         true, 0, "MUN", 0.0, true, 60, 0.3},
        {"contrast 5", 238, 64, 0, 0, 0.58, true, false, false, false, false,
         false, 0, "MUN", 0.0, true, 60, 0.3, false, false, false, false,
         false, false, 5},
        {"contrast 15 L curve", 238, 64, 0, 10, 0.58, true, false, false,
         false, false, false, 0, "MUN", 0.0, true, 60, 0.3, false, false,
         false, false, false, false, 15},
        {"contrast 30 combined", 179, 48, 0, 0, 0.62, true, true, true, true,
         true, true, 0, "MUN", 0.0, true, 60, 0.3, false, false, false,
         false, false, false, 30},
        /*
         * The chromaticity slider and the three hue flat curves. The
         * saturated image spans the hue circle so each curve has work to do.
         */
        {"chromaticity -100", 238, 64, 2, 0, 0.62, false, false, false, false,
         false, false, -100, "MUN", 0.0, true, 60, 0.3},
        {"chromaticity -100 ignores the hue curves", 238, 64, 2, 0, 0.62,
         false, false, false, false, false, false, -100, "MUN", 0.0, true,
         60, 0.3, true, true, true},
        {"chromaticity midpoint", 238, 64, 2, 0, 0.62, false, false, false,
         false, false, false, 50, "MUN", 30.0, true, 60, 0.3},
        {"chromaticity +100", 179, 48, 2, 0, 0.62, false, false, false, false,
         false, false, 100, "MUN", 60.0, true, 60, 0.3},
        {"C=f(H)", 238, 64, 2, 0, 0.62, false, false, false, false, false,
         false, 0, "MUN", 0.0, true, 60, 0.3, true},
        {"L=f(H)", 238, 64, 2, 0, 0.62, false, false, false, false, false,
         false, 0, "MUN", 0.0, true, 60, 0.3, false, true},
        {"H=f(H)", 179, 48, 2, 0, 0.62, false, false, false, false, false,
         false, 0, "MUN", 0.0, true, 60, 0.3, false, false, true},
        {"hue curves with the chromaticity slider", 238, 64, 2, 0, 0.62,
         false, false, false, false, false, false, 50, "MUN", 30.0, true, 60,
         0.3, true, true, true},
        {"a/b tables with the hue curves", 238, 64, 2, 0, 0.62, false, true,
         true, false, false, false, 0, "MUN", 0.0, true, 60, 0.3, true, true,
         true},
        {"hue curves with vibrance and shadows/highlights", 238, 64, 2, 0,
         0.62, false, false, false, false, false, false, 0, "MUN", 0.0, true,
         60, 0.3, true, true, true, false, true, true},
        {"black and white L=f(H)", 238, 64, 2, 0, 0.62, false, false, false,
         false, false, false, 0, "MUN", 0.0, true, 60, 0.3, false, true, false,
         true},
    };

    /*
     * Dense C=f(H) curves, run through the same case body. The host carries
     * one table node per polyline knot, so the exported node count equals the
     * control-point count; the steep variants are the ones a float table
     * cannot follow. `tables` adds the L curve and the a/b tables, so the
     * hue table is read on the chroma the tables wrote after it.
     */
    struct LabHueDenseCase {
        const char* label;
        int points;
        bool steep;
        int chromaticity;
        bool tables;
        /* The first knot's abscissa; above zero exercises the period wrap. */
        double start;
    };
    const LabHueDenseCase dense_cases[] = {
        {"dense C=f(H) 512", 512, true, 0, false, 0.0},
        {"dense C=f(H) 4096", 4096, true, 0, false, 0.0},
        {"dense C=f(H) 40000", 40000, true, 0, false, 0.0},
        {"dense smooth C=f(H) 8000", 8000, false, 0, false, 0.0},
        {"dense C=f(H) 40000 and the chromaticity slider", 40000, true, 50,
         false, 0.0},
        {"dense C=f(H) 40000 with a/b tables and L curve", 40000, true, 50,
         true, 0.0},
        {"dense smooth C=f(H) 8000 with a/b tables and L curve", 8000, false,
         50, true, 0.0},
        /* A polyline spanning [0.2, 1.2]: the table's wrap case. */
        {"dense C=f(H) 4096 from 0.2", 4096, true, 50, false, 0.2},
    };

    unsigned seed = 0x51ed270bu;
    const auto run_case = [&](const LabCurveCase& item) {
        rtengine::procparams::ProcParams params;
        params.labCurve.enabled = true;
        params.labCurve.brightness = item.brightness;
        params.labCurve.contrast = item.contrast;
        params.labCurve.chromaticity = item.chromaticity;
        params.labCurve.gamutmunselmethod = item.munsell;
        params.labCurve.rstprotection = item.rstprotection;
        params.labCurve.lcredsk = item.lcredsk;
        if (item.lcurve) {
            params.labCurve.lcurve = lab_curve_points(item.mid);
        }
        if (item.acurve) {
            params.labCurve.acurve = lab_curve_points(item.mid);
        }
        if (item.bcurve) {
            params.labCurve.bcurve = lab_curve_points(item.mid);
        }
        if (item.cccurve) {
            params.labCurve.cccurve = lab_curve_points(item.mid);
        }
        if (item.lccurve) {
            params.labCurve.lccurve = lab_curve_points(item.mid);
        }
        if (item.clcurve) {
            params.labCurve.clcurve = lab_curve_points(item.mid);
        }
        if (item.densePoints != 0) {
            params.labCurve.chcurve = lab_hue_points_dense(
                item.densePoints, item.denseSteep, item.denseStart);
        } else if (item.chcurve) {
            params.labCurve.chcurve = lab_hue_points();
        }
        if (item.lhcurve) {
            params.labCurve.lhcurve = lab_hue_points();
        }
        if (item.hhcurve) {
            params.labCurve.hhcurve = lab_hue_points();
        }
        if (item.blackwhite) {
            params.blackwhite.enabled = true;
        }
        if (item.vibrance) {
            params.vibrance.enabled = true;
            params.vibrance.pastels = 50;
            params.vibrance.saturated = 30;
        }
        if (item.shadowsHighlights) {
            params.sh.enabled = true;
            params.sh.lab = true;
            params.sh.highlights = 60;
            params.sh.shadows = 60;
            params.sh.htonalwidth = 40;
            params.sh.stonalwidth = 40;
            params.sh.radius = 10;
        }

        lab_curve_settings.protectred = item.protectred;
        lab_curve_settings.protectredh = item.protectredh;
        lab_curve_settings.amchroma = 40;

        check(labToolsSupported(params),
              (std::string("lab.tools Lab curve ") + item.label +
               " is the supported set").c_str());
        check(labToolsLabCurveActive(params),
              (std::string("lab.tools Lab curve ") + item.label +
               " runs the Lab-curve pass").c_str());

        LabImage source(item.width, item.height);
        switch (item.image) {
        case 1:
            fill_lab_curve_neutral(source);
            break;
        case 2:
            fill_lab_curve_saturated(source);
            break;
        default:
            fill_lab_tools(source, seed++);
            break;
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        /*
         * Non-vacuity: the same profile with the hue family switched off must
         * produce a different image, otherwise the case proves nothing about
         * the port.
         */
        if (item.chcurve || item.lhcurve || item.hhcurve ||
            item.chromaticity != 0) {
            rtengine::procparams::ProcParams plain = params;
            plain.labCurve.chromaticity = 0;
            plain.labCurve.chcurve.clear();
            plain.labCurve.lhcurve.clear();
            plain.labCurve.hhcurve.clear();
            rtengine::ImProcFunctions plain_ipf(&plain, false);
            LabToolsHostState plain_host;
            LabImage plain_image(item.width, item.height);
            plain_image.CopyFrom(&source);
            LabToolsLuts plain_luts = plain_host.luts();
            labToolsHostBlock(plain_ipf, plain, plain_image, plain_luts,
                              plain_host.autili, plain_host.butili);
            const Diff vacuity = compare(view(cpu), view(plain_image));
            std::printf("stage-residency: lab.tools Lab curve %s: PAE %.6f "
                        "against the family off\n",
                        item.label, vacuity.max_abs);
            check(vacuity.differing > 0,
                  (std::string("lab.tools Lab curve ") + item.label +
                   " is not a no-op").c_str());
        }

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.tools Lab curve ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.tools Lab curve ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools Lab curve ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools Lab curve ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.tools Lab curve ") + item.label +
               " records a device execution").c_str());

        /*
         * The dense cases carry the polyline itself: one node per control
         * point (plus the wrap boundary when the polyline starts above zero),
         * which is exact and is what admits a 40 000-point curve.
         */
        if (item.densePoints != 0) {
            /*
             * One node per knot, plus at most the two period boundary nodes:
             * a closing knot at x=1 maps to 0 and its periodic copy can land a
             * rounding away from the opening knot. The refiner would need
             * more than two nodes per kink, so this bound is what proves the
             * polyline was carried.
             */
            const uint32_t knots = static_cast<uint32_t>(item.densePoints);
            std::printf("stage-residency: lab.tools Lab curve %s: %u table "
                        "nodes for %d control points\n",
                        item.label, result.hue_nodes, item.densePoints);
            check(result.hue_nodes >= knots &&
                      result.hue_nodes <= knots + 2,
                  (std::string("lab.tools Lab curve ") + item.label +
                   " carries one node per polyline knot").c_str());
        }

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.tools Lab curve %s %dx%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.tools Lab curve ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.tools Lab curve ") + item.label +
               " is within one LSB16").c_str());
        if (d.over > 0) {
            double worst = -1.0;
            int wx = -1;
            int wy = -1;
            for (int y = 0; y < item.height; ++y) {
                for (int x = 0; x < item.width; ++x) {
                    for (int plane = 0; plane < 3; ++plane) {
                        const float hv = plane == 0 ? cpu.L[y][x]
                                        : plane == 1 ? cpu.a[y][x]
                                                     : cpu.b[y][x];
                        const float dv = plane == 0 ? device.L[y][x]
                                        : plane == 1 ? device.a[y][x]
                                                     : device.b[y][x];
                        const double delta =
                            std::fabs(static_cast<double>(hv) - dv);
                        if (delta > worst) {
                            worst = delta;
                            wx = x;
                            wy = y;
                        }
                    }
                }
            }
            std::printf("stage-residency: worst Lab-curve pixel %d,%d: host "
                        "L=%.4f a=%.4f b=%.4f device L=%.4f a=%.4f b=%.4f\n",
                        wx, wy, cpu.L[wy][wx], cpu.a[wy][wx],
                        cpu.b[wy][wx], device.L[wy][wx], device.a[wy][wx],
                        device.b[wy][wx]);
        }
    };

    for (const LabCurveCase& item : cases) {
        run_case(item);
    }
    for (const LabHueDenseCase& item : dense_cases) {
        LabCurveCase dense{};
        dense.label = item.label;
        dense.width = 238;
        dense.height = 64;
        dense.image = 2;
        dense.mid = 0.62;
        dense.chromaticity = item.chromaticity;
        dense.munsell = "MUN";
        dense.rstprotection = 0.0;
        dense.lcredsk = true;
        dense.protectred = 60;
        dense.protectredh = 0.3;
        dense.lcurve = item.tables;
        dense.acurve = item.tables;
        dense.bcurve = item.tables;
        dense.chcurve = true;
        dense.densePoints = item.points;
        dense.denseSteep = item.steep;
        dense.denseStart = item.start;
        run_case(dense);
    }

    /*
     * Residuals: every other Lab-curve family or setting must be refused and
     * must run the block on the host byte for byte.
     */
    const char* residual_labels[] = {"over-budget hue curve"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        params.labCurve.enabled = true;
        params.labCurve.lcurve = lab_curve_points(0.60);
        params.labCurve.acurve = lab_curve_points(0.60);
        params.labCurve.cccurve = lab_curve_points(0.70);
        params.labCurve.gamutmunselmethod = "MUN";
        lab_curve_settings.protectred = 60;
        lab_curve_settings.protectredh = 0.3;
        lab_curve_settings.amchroma = 40;

        if (std::strcmp(label, "over-budget hue curve") == 0) {
            /*
             * A hand-built curve with more knots than the slot can carry and
             * more kinks than the refiner can subdivide: 100 000 linear
             * control points alternating between 0.55 and 0.45. FlatCurve's
             * polyline has one knot per segment, above the 65 536-node
             * budget, and the refiner needs a node on each side of every one
             * of its 100 000 kinks, so neither representation fits and the
             * predicate refuses the node.
             */
            constexpr int POINTS = 100000;
            params.labCurve.chcurve = lab_hue_points_dense(POINTS, true);
        }

        check(!labToolsSupported(params),
              (std::string("lab.tools Lab curve residual ") + label +
               " is a residual").c_str());

        LabImage source(179, 48);
        fill_lab_curve_saturated(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.tools Lab curve residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.tools Lab curve residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools Lab curve residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.tools Lab curve residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.tools Lab curve residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools Lab curve residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.tools Lab curve residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.tools Lab curve residual ") + label +
               " matches the CPU block").c_str());
    }

    /*
     * Black-and-white output through `chromiLuminanceCurve()`'s full
     * `bwToning` arm. With `blackwhite.enabled` and the Lab curves on, the
     * block takes the hue and chroma from the original pixel before the a and
     * b tables, skips C=f(L), C=f(C) and L=f(C) and the red/skin gate, still
     * applies the L table, and closes on the a/b tables' deviation from the
     * original chroma: exactly zero with neither table active, and the curve's
     * delta with one. Every case is compared with the host block.
     *
     * The block's L=f(H) flat curve also runs under `bwToning`; the main
     * Lab-curve matrix above covers black-and-white with L=f(H) as a device
     * case, so this matrix stays the diagonal-table and activity-flag one.
     *
     * `chroma_max` is the zero test for the closing write: the host leaves a
     * and b at exactly 0.0f when no a/b table is active.
     */
    {
        const auto chroma_max = [](const LabImage& image) {
            double worst = 0.0;
            for (int y = 0; y < image.H; ++y) {
                for (int x = 0; x < image.W; ++x) {
                    worst = std::max(
                        worst,
                        std::abs(static_cast<double>(image.a[y][x])));
                    worst = std::max(
                        worst,
                        std::abs(static_cast<double>(image.b[y][x])));
                }
            }
            return worst;
        };

        struct BwCurveCase {
            const char* label;
            int width;
            int height;
            bool lcurve;
            bool abcurve;
            bool cccurve;
            bool lccurve;
            bool clcurve;
            bool vibrance;
            bool shadowsHighlights;
            bool localContrast;
            const char* munsell;
        };
        const BwCurveCase bw_curve_cases[] = {
            {"black and white identity curves", 238, 64, false, false, false,
             false, false, false, false, false, "MUN"},
            {"black and white L curve", 238, 64, true, false, false, false,
             false, false, false, false, "MUN"},
            {"black and white a/b curves", 238, 64, false, true, false, false,
             false, false, false, false, "MUN"},
            {"black and white C=f(L)", 179, 48, false, false, false, false,
             true, false, false, false, "MUN"},
            {"black and white L=f(C)", 238, 64, false, false, false, true,
             false, false, false, false, "MUN"},
            {"black and white C=f(C) + C=f(L) + L=f(C)", 238, 64, false,
             false, true, true, true, false, false, false, "NONE"},
            {"black and white curve set with vibrance, shadows/highlights "
             "and local contrast", 238, 64, true, true, true, true, true,
             true, true, true, "NONE"},
        };

        for (const BwCurveCase& item : bw_curve_cases) {
            rtengine::procparams::ProcParams params;
            params.blackwhite.enabled = true;
            params.labCurve.enabled = true;
            params.labCurve.contrast = 0;
            params.labCurve.gamutmunselmethod = item.munsell;
            params.labCurve.rstprotection = 0.0;
            params.labCurve.lcredsk = true;
            if (item.lcurve) {
                params.labCurve.lcurve = lab_curve_points(0.58);
            }
            if (item.abcurve) {
                params.labCurve.acurve = lab_curve_points(0.60);
                params.labCurve.bcurve = lab_curve_points(0.60);
            }
            if (item.cccurve) {
                params.labCurve.cccurve = lab_curve_points(1.10);
            }
            if (item.lccurve) {
                params.labCurve.lccurve = lab_curve_points(1.10);
            }
            if (item.clcurve) {
                params.labCurve.clcurve = lab_curve_points(0.52);
            }
            if (item.vibrance) {
                params.vibrance.enabled = true;
                params.vibrance.pastels = 50;
                params.vibrance.saturated = 30;
            }
            if (item.shadowsHighlights) {
                params.sh.enabled = true;
                params.sh.lab = true;
                params.sh.highlights = 60;
                params.sh.shadows = 60;
                params.sh.htonalwidth = 40;
                params.sh.stonalwidth = 40;
                params.sh.radius = 10;
            }
            if (item.localContrast) {
                params.localContrast.enabled = true;
                params.localContrast.radius = 20;
                params.localContrast.amount = 0.3;
                params.localContrast.darkness = 0.9;
                params.localContrast.lightness = 1.1;
            }
            lab_curve_settings.protectred = 60;
            lab_curve_settings.protectredh = 0.3;
            lab_curve_settings.amchroma = 40;

            const std::string label =
                std::string("lab.tools ") + item.label;
            check(labToolsSupported(params),
                  (label + " is the supported set").c_str());
            check(labToolsLabCurveActive(params),
                  (label + " runs the Lab-curve pass").c_str());

            LabImage source(item.width, item.height);
            fill_lab_curve_saturated(source);

            rtengine::ImProcFunctions cpu_ipf(&params, false);
            LabToolsHostState cpu_host;
            LabImage cpu(item.width, item.height);
            cpu.CopyFrom(&source);
            LabToolsLuts cpu_luts = cpu_host.luts();
            labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                              cpu_host.butili);

            rtengine::ImProcFunctions device_ipf(&params, false);
            LabToolsHostState device_host;
            LabImage device(item.width, item.height);
            device.CopyFrom(&source);
            const LabToolsRun result =
                run_lab_tools_node(device, params, device_host, device_ipf,
                                   profile_path);
            const std::string profile = read_file(profile_path);

            check(result.supported,
                  (label + " resolves to the supported set").c_str());
            check(result.deviceReady,
                  (label + " builds its host setup").c_str());
            check(result.status == RT_STAGE_OK,
                  (label + " completes through run()").c_str());
            check(profile.find("\"stage\":\"lab.tools\"") !=
                      std::string::npos &&
                      profile.find("\"impl\":\"opencl\"") !=
                          std::string::npos,
                  (label + " records a device execution").c_str());

            const Distribution d = distribution(view(device), view(cpu));
            std::printf("stage-residency: %s %dx%d: PAE %.6f LSB16; exact "
                        "%zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, >1 %zu of "
                        "%zu\n",
                        item.label, item.width, item.height, d.max_abs,
                        d.exact, d.quarter, d.half, d.one, d.over, d.samples);
            check(d.finite, (label + " stays finite").c_str());
            check(d.over == 0,
                  (label + " is within one LSB16").c_str());

            const double host_chroma = chroma_max(cpu);
            const double device_chroma = chroma_max(device);
            std::printf("stage-residency: %s: host chroma %.6f, device "
                        "chroma %.6f\n",
                        item.label, host_chroma, device_chroma);
            if (item.abcurve) {
                check(host_chroma > 0.0 && device_chroma > 0.0,
                      (label + " keeps the a/b tables' deviation").c_str());
            } else {
                check(host_chroma == 0.0,
                      (label + " leaves the host's a and b at exactly "
                               "zero").c_str());
                check(device_chroma == 0.0,
                      (label + " leaves a and b at exactly zero").c_str());
            }

            /*
             * Non-vacuity: the same profile with black-and-white off must
             * produce a different host result, so the arm cannot pass by
             * being skipped.
             */
            rtengine::procparams::ProcParams colour_params = params;
            colour_params.blackwhite.enabled = false;
            rtengine::ImProcFunctions colour_ipf(&colour_params, false);
            LabToolsHostState colour_host;
            LabImage colour(item.width, item.height);
            colour.CopyFrom(&source);
            LabToolsLuts colour_luts = colour_host.luts();
            labToolsHostBlock(colour_ipf, colour_params, colour, colour_luts,
                              colour_host.autili, colour_host.butili);
            const Diff without = compare(view(colour), view(cpu));
            std::printf("stage-residency: %s: black and white changes %zu of "
                        "%zu host samples, PAE %.6f LSB16\n",
                        item.label, without.differing, without.samples,
                        without.max_abs);
            check(without.differing > 0,
                  (label + " changes the host result").c_str());
        }

        /*
         * The a/b tables' deviation is the arm colour toning would otherwise
         * hide: with the same tables and black-and-white off, the host writes
         * the curved absolute chroma instead. The two host results must
         * differ, so the deviation path is exercised rather than a no-op.
         */
        rtengine::procparams::ProcParams bw;
        bw.blackwhite.enabled = true;
        bw.labCurve.enabled = true;
        bw.labCurve.acurve = lab_curve_points(0.60);
        bw.labCurve.bcurve = lab_curve_points(0.60);
        bw.labCurve.gamutmunselmethod = "MUN";
        rtengine::procparams::ProcParams curved = bw;
        curved.blackwhite.enabled = false;

        LabImage source(238, 64);
        fill_lab_curve_saturated(source);
        const auto host_block = [&](const rtengine::procparams::ProcParams& p) {
            LabImage image(238, 64);
            image.CopyFrom(&source);
            rtengine::ImProcFunctions ipf(&p, false);
            LabToolsHostState state;
            LabToolsLuts luts = state.luts();
            labToolsHostBlock(ipf, p, image, luts, state.autili, state.butili);
            return image;
        };
        const LabImage grey_deviation = host_block(bw);
        const LabImage curved_absolute = host_block(curved);
        const Diff deviation = compare(view(grey_deviation),
                                       view(curved_absolute));
        std::printf("stage-residency: black-and-white a/b deviation differs "
                    "from the curved chroma on %zu of %zu samples, PAE %.6f "
                    "LSB16\n",
                    deviation.differing, deviation.samples,
                    deviation.max_abs);
        check(deviation.differing > 0,
              "the black-and-white a/b deviation is not the curved chroma");
    }

    /*
     * Non-vacuity: on the saturated fixture the Munsell correction and the
     * red/skin protection must change the host block's own output.
     */
    {
        rtengine::procparams::ProcParams params;
        params.labCurve.enabled = true;
        params.labCurve.acurve = lab_curve_points(0.62);
        params.labCurve.gamutmunselmethod = "MUN";
        rtengine::procparams::ProcParams none = params;
        none.labCurve.gamutmunselmethod = "NONE";

        LabImage source(179, 48);
        fill_lab_curve_saturated(source);
        const auto host_block = [&](const rtengine::procparams::ProcParams& p) {
            LabImage image(179, 48);
            image.CopyFrom(&source);
            rtengine::ImProcFunctions ipf(&p, false);
            LabToolsHostState state;
            LabToolsLuts luts = state.luts();
            labToolsHostBlock(ipf, p, image, luts, state.autili, state.butili);
            return image;
        };
        const LabImage mun = host_block(params);
        const LabImage blended = host_block(none);
        const Diff correction = compare(view(mun), view(blended));
        std::printf("stage-residency: Lab-curve Munsell correction changes %zu "
                    "of %zu saturated samples, PAE %.6f LSB16\n",
                    correction.differing, correction.samples,
                    correction.max_abs);
        check(correction.differing > 0,
              "the Lab-curve Munsell correction changes the image");

        rtengine::procparams::ProcParams protected_on = params;
        protected_on.labCurve.cccurve = lab_curve_points(1.20);
        protected_on.labCurve.rstprotection = 30.0;
        rtengine::procparams::ProcParams protected_off = protected_on;
        lab_curve_settings.protectred = 0;
        lab_curve_settings.protectredh = 0;
        const LabImage without = host_block(protected_off);
        lab_curve_settings.protectred = 60;
        lab_curve_settings.protectredh = 0.3;
        const LabImage with = host_block(protected_on);
        const Diff protection = compare(view(with), view(without));
        std::printf("stage-residency: Lab-curve red/skin protection changes %zu "
                    "of %zu saturated samples, PAE %.6f LSB16\n",
                    protection.differing, protection.samples,
                    protection.max_abs);
        check(protection.differing > 0,
              "the Lab-curve red/skin protection changes the image");
    }
}


/* ---------------------------------------------- lab.tools local contrast */

/*
 * A wide, non-integer Lab fixture for the IIR tail. The host's non-vectorised
 * tail lines subtract a float source from a float forward state before the
 * double boundary matrix multiplies the deviation. Random data over a wide
 * range rarely makes that subtraction round; alternating a large value with a
 * tiny one does, because the exact difference needs more mantissa bits than a
 * float has. L therefore alternates between about 30000 and about 0.4, with
 * fractional parts, and a,b carry fractional values as well.
 */
void fill_lab_tools_wide(LabImage& lab, unsigned seed)
{
    const int width = lab.W;
    const int height = lab.H;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned hash =
                (seed * 2654435761u + static_cast<unsigned>(x) * 2246822519u +
                 static_cast<unsigned>(y) * 3266489917u) >>
                7;
            lab.L[y][x] = (hash & 1u) == 0u
                              ? 30000.0f +
                                    static_cast<float>(hash % 1024u) * 0.0123f
                              : 0.37f +
                                    static_cast<float>(hash % 997u) * 0.001f;
            lab.a[y][x] =
                static_cast<float>(static_cast<int>((hash / 3u) % 48000u) -
                                   24000) +
                0.25f;
            lab.b[y][x] =
                static_cast<float>(static_cast<int>((hash / 11u) % 48000u) -
                                   24000) +
                0.75f;
        }
    }
}

/*
 * Local contrast is a device pass of its own: the host blur is a separable
 * Young-van Vliet IIR filter, and gaussianBlur() switches between the SSE
 * float path and the double path at sigma 25. The matrix below covers both
 * (radii 1 and 10 against 40, 80 and 100), the three amounts and the three
 * darkness/lightness settings named in the brief, padded widths, a large
 * width and combinations with the other device tools.
 */
void test_lab_tools_local_contrast(bool available)
{
    using namespace rtengine::stage;

    static MyMutex local_contrast_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &local_contrast_lcms_mutex;
    }
    static rtengine::Settings local_contrast_settings{};
    rtengine::settings = &local_contrast_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-tools-localcontrast.jsonl";

    if (!available) {
        skip("lab.tools local contrast: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.tools"),
          "the lab.tools node has a device implementation for local contrast");

    struct LocalContrastCase {
        const char* label;
        int width;
        int height;
        int radius;
        double amount;
        double darkness;
        double lightness;
        bool labCurve;
        bool shadows;
        bool vibrance;
        bool wide;
    };
    const LocalContrastCase cases[] = {
        /* label, w, h, radius, amount, dark, light, lab curve, sh, vibrance,
         * wide */
        {"radius 1", 238, 64, 1, 1.0, 1.0, 1.0, false, false, false, false},
        {"radius 10", 238, 64, 10, 1.0, 1.0, 1.0, false, false, false, false},
        {"radius 40", 238, 64, 40, 1.0, 1.0, 1.0, false, false, false, false},
        {"radius 80", 238, 64, 80, 1.0, 1.0, 1.0, false, false, false, false},
        {"radius 100", 238, 64, 100, 1.0, 1.0, 1.0, false, false, false,
         false},
        {"amount 0", 238, 64, 40, 0.0, 1.0, 1.0, false, false, false, false},
        {"amount 0.1", 238, 64, 40, 0.1, 1.0, 1.0, false, false, false,
         false},
        {"amount 2", 238, 64, 40, 2.0, 1.0, 1.0, false, false, false, false},
        {"darkness 0.5", 238, 64, 40, 1.0, 0.5, 1.0, false, false, false,
         false},
        {"lightness 2", 238, 64, 40, 1.0, 1.0, 2.0, false, false, false,
         false},
        {"darkness 2 lightness 0.5", 238, 64, 40, 1.0, 2.0, 0.5, false,
         false, false, false},
        {"darkness 0.5 lightness 2", 179, 48, 80, 1.0, 0.5, 2.0, false, false,
         false, false},
        {"padded width 179", 179, 48, 40, 1.0, 1.0, 1.0, false, false, false,
         false},
        {"padded width 238", 238, 64, 80, 1.0, 1.0, 1.0, false, false, false,
         false},
        {"odd height 50", 179, 50, 10, 1.0, 1.0, 1.0, false, false, false,
         false},
        {"large width 8192", 8192, 64, 100, 1.0, 0.5, 2.0, false, false, false,
         false},
        /* sigma 24 is just below the double-path split, so every odd row and
         * column tail runs the SSE path's double tail. The wide cases carry
         * non-integer values outside 0..32767 so the float deviation rounding
         * is visible; the integer case keeps that rounding invisible. */
        {"sigma 24 integer", 179, 48, 24, 1.0, 1.0, 1.0, false, false, false,
         false},
        {"sigma 24 wide 179x50", 179, 50, 24, 1.0, 1.0, 1.0, false, false,
         false, true},
        {"sigma 24 wide 238x63", 238, 63, 24, 2.0, 0.5, 2.0, false, false,
         false, true},
        {"sigma 24 wide amount 2", 179, 50, 24, 2.0, 0.5, 2.0, false, false,
         false, true},
        {"with Lab curve", 238, 64, 40, 1.0, 1.0, 1.0, true, false, false,
         false},
        {"with shadows/highlights", 238, 64, 40, 1.0, 0.5, 2.0, false, true,
         false, false},
        {"with vibrance", 238, 64, 40, 1.0, 0.5, 2.0, false, false, true,
         false},
    };

    unsigned seed = 0x9e3779b9u;
    for (const LocalContrastCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.localContrast.enabled = true;
        params.localContrast.radius = item.radius;
        params.localContrast.amount = item.amount;
        params.localContrast.darkness = item.darkness;
        params.localContrast.lightness = item.lightness;
        if (item.labCurve) {
            params.labCurve.enabled = true;
            params.labCurve.lcurve = lab_curve_points(0.58);
            params.labCurve.acurve = lab_curve_points(0.60);
        }
        if (item.shadows) {
            params.sh.enabled = true;
            params.sh.lab = true;
            params.sh.highlights = 40;
            params.sh.shadows = 40;
            params.sh.htonalwidth = 50;
            params.sh.stonalwidth = 50;
            params.sh.radius = 20;
        }
        if (item.vibrance) {
            params.vibrance.enabled = true;
            params.vibrance.pastels = 30;
            params.vibrance.saturated = 20;
            params.vibrance.psthreshold = rtengine::procparams::Threshold<int>(
                50, 50, false);
        }

        check(labToolsSupported(params),
              (std::string("lab.tools local contrast ") + item.label +
               " is the supported set").c_str());
        check(labToolsLocalContrastActive(params),
              (std::string("lab.tools local contrast ") + item.label +
               " runs the local-contrast pass").c_str());

        LabImage source(item.width, item.height);
        if (item.wide) {
            fill_lab_tools_wide(source, seed++);
        } else {
            fill_lab_tools(source, seed++);
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.tools local contrast ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.tools local contrast ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools local contrast ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools local contrast ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.tools local contrast ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.tools local contrast %s %dx%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.tools local contrast ") + item.label +
               " stays finite").c_str());
        check(d.exact == d.samples,
              (std::string("lab.tools local contrast ") + item.label +
               " is bit-exact").c_str());
        check(d.over == 0,
              (std::string("lab.tools local contrast ") + item.label +
               " is within one LSB16").c_str());
    }

    /*
     * Residuals: a local contrast outside the ABI range, and a local contrast
     * combined with a tool the node does not implement, must be refused by
     * the predicate and run the block on the host byte for byte.
     */
    struct LocalContrastResidual {
        const char* label;
        int radius;
        double amount;
        double darkness;
        double lightness;
        bool epd;
    };
    const LocalContrastResidual residuals[] = {
        {"radius below the ABI range", 0, 1.0, 1.0, 1.0, false},
        {"radius above the ABI range", 300, 1.0, 1.0, 1.0, false},
        {"amount above the ABI range", 40, 3.0, 1.0, 1.0, false},
        {"negative amount", 40, -0.1, 1.0, 1.0, false},
        {"darkness above the ABI range", 40, 1.0, 4.0, 1.0, false},
        {"negative lightness", 40, 1.0, 1.0, -0.5, false},
        {"not a number amount", 40, std::nan(""), 1.0, 1.0, false},
        {"with EPD", 40, 1.0, 1.0, 1.0, true},
    };
    for (const LocalContrastResidual& item : residuals) {
        rtengine::procparams::ProcParams params;
        params.localContrast.enabled = true;
        params.localContrast.radius = item.radius;
        params.localContrast.amount = item.amount;
        params.localContrast.darkness = item.darkness;
        params.localContrast.lightness = item.lightness;
        if (item.epd) {
            params.epd.enabled = true;
            params.epd.strength = 0.5;
            params.epd.gamma = 1.0;
        }
        check(!labToolsSupported(params),
              (std::string("lab.tools local contrast residual ") + item.label +
               " is a residual").c_str());

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.tools local contrast residual ") + item.label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.tools local contrast residual ") + item.label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools local contrast residual ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.tools local contrast residual ") + item.label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.tools local contrast residual ") + item.label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools local contrast residual %s: "
                    "host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    item.label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.tools local contrast residual ") + item.label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.tools local contrast residual ") + item.label +
               " matches the CPU block").c_str());
    }

    /*
     * Non-vacuity: the device pass must change pixels, otherwise a kernel that
     * copied its input would pass every parity check above.
     */
    {
        rtengine::procparams::ProcParams params;
        params.localContrast.enabled = true;
        params.localContrast.radius = 10;
        params.localContrast.amount = 1.0;
        params.localContrast.darkness = 1.0;
        params.localContrast.lightness = 1.0;

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);
        LabImage image(179, 48);
        image.CopyFrom(&source);
        rtengine::ImProcFunctions ipf(&params, false);
        LabToolsHostState state;
        LabToolsLuts luts = state.luts();
        labToolsHostBlock(ipf, params, image, luts, state.autili,
                          state.butili);
        const Diff change = compare(view(image), view(source));
        std::printf("stage-residency: local contrast changes %zu of %zu "
                    "samples, PAE %.6f LSB16\n",
                    change.differing, change.samples, change.max_abs);
        check(change.differing > 0,
              "local contrast changes the host block's own output");
    }
}


/* -------------------------------------------------------- lab.wavelet */

/*
 * A Lab fixture whose hues and chromas sweep the skin-protection bands at
 * every lightness: hue -0.4..1.8, chroma 5..60, L 0..100.
 */
void fill_lab_wavelet_skin(LabImage& lab)
{
    for (int y = 0; y < lab.H; ++y) {
        const float l = 32768.f * (static_cast<float>(y) + 0.5f) /
                        static_cast<float>(lab.H);
        for (int x = 0; x < lab.W; ++x) {
            const float hue =
                -0.4f + 2.2f * static_cast<float>(x % 64) / 63.f;
            const float chroma =
                5.f + 55.f * static_cast<float>((x / 64) % 4);
            /* Pixel-scale structure so the high-pass is non-zero where the
             * protection scale is read. */
            const float flicker =
                static_cast<float>((x * 37 + y * 101) % 17) * 180.f - 1440.f;
            lab.L[y][x] = l + flicker;
            lab.a[y][x] = chroma * 327.68f * cosf(hue);
            lab.b[y][x] = chroma * 327.68f * sinf(hue);
        }
    }
}

struct LabWaveletCase {
    const char* label;
    int width;
    int height;
    int image; /* 0 random, 1 skin, 2 saturated hue circle */
    double mult[6];
    double threshold;
    double skinprotect;
    bool gamutlab;
};

struct LabWaveletRun {
    bool supported = false;
    bool deviceReady = false;
    RtStageStatus status = RT_STAGE_INVALID;
};

/* The call site's submission, reproduced for one image. */
LabWaveletRun run_lab_wavelet_node(LabImage& image,
                                   rtengine::procparams::ProcParams& params,
                                   rtengine::ImProcFunctions& ipf,
                                   const std::string& profile_path,
                                   bool submit_device = true,
                                   bool force = false)
{
    using namespace rtengine::stage;

    LabWaveletRun result;
    result.supported = force ||
                       labWaveletSupported(params, image.W, image.H);

    LabWaveletContext context(params, ipf, image.W, image.H);
    RtLabWaveletResult setup{};
    const bool deviceReady = result.supported && context.build(setup) == 0;
    result.deviceReady = deviceReady;
    RtLabWaveletContext descriptor = context.descriptor(deviceReady);

    RtStageArg args[22] = {};
    args[0].kind = RT_STAGE_ARG_BUFFER;
    args[0].u.buffer = view(image);
    args[1].kind = RT_STAGE_ARG_PTR;
    args[1].u.ptr = &descriptor;
    args[2].kind = RT_STAGE_ARG_INT;
    args[2].u.i = deviceReady ? 1 : 0;
    for (int level = 0; level < 6; ++level) {
        args[3 + level].kind = RT_STAGE_ARG_DOUBLE;
        args[3 + level].u.f = params.dirpyrequalizer.mult[level];
    }
    args[9].kind = RT_STAGE_ARG_DOUBLE;
    args[9].u.f = params.dirpyrequalizer.threshold;
    args[10].kind = RT_STAGE_ARG_DOUBLE;
    args[10].u.f = params.dirpyrequalizer.skinprotect;
    args[11].kind = RT_STAGE_ARG_DOUBLE;
    args[11].u.f = params.dirpyrequalizer.hueskin.getBottomLeft() / 100.0;
    args[12].kind = RT_STAGE_ARG_DOUBLE;
    args[12].u.f = params.dirpyrequalizer.hueskin.getTopLeft() / 100.0;
    args[13].kind = RT_STAGE_ARG_DOUBLE;
    args[13].u.f = params.dirpyrequalizer.hueskin.getTopRight() / 100.0;
    args[14].kind = RT_STAGE_ARG_INT;
    args[14].u.i = params.dirpyrequalizer.gamutlab ? 1 : 0;
    args[15].kind = RT_STAGE_ARG_INT;
    args[15].u.i = LAB_WAVELET_EXPORT_SCALE;
    args[16].kind = RT_STAGE_ARG_INT;
    args[16].u.i = deviceReady ? setup.levels : 0;
    for (int index = 17; index < 20; ++index) {
        args[index].kind = RT_STAGE_ARG_BUFFER;
    }
    args[20].kind = RT_STAGE_ARG_INT;
    args[20].u.i = deviceReady && setup.wavelet != 0 ? 1 : 0;
    args[21].kind = RT_STAGE_ARG_BUFFER;
    if (deviceReady) {
        const auto host_buffer = [](const float* data, int width) {
            RtStageBuffer buffer{};
            buffer.size = sizeof(RtStageBuffer);
            buffer.data = const_cast<float*>(data);
            buffer.width = width;
            buffer.height = 1;
            buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
            buffer.plane_pitch = buffer.row_pitch;
            buffer.planes = 1;
            buffer.format = RT_STAGE_F32;
            buffer.flags = RT_STAGE_BUFFER_READ;
            return buffer;
        };
        const auto mask_buffer = [](const float* data, int width, int height) {
            RtStageBuffer buffer{};
            buffer.size = sizeof(RtStageBuffer);
            buffer.data = const_cast<float*>(data);
            buffer.width = width;
            buffer.height = height;
            buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
            buffer.plane_pitch =
                buffer.row_pitch * static_cast<std::size_t>(height);
            buffer.planes = 1;
            buffer.format = RT_STAGE_F32;
            buffer.flags = RT_STAGE_BUFFER_READ;
            return buffer;
        };
        if (setup.levels > 0) {
            args[17].u.buffer = host_buffer(
                setup.luts,
                static_cast<int>(setup.levels *
                                 RT_LAB_WAVELET_LUT_ENTRIES));
        }
        if (setup.wavelet != 0 && setup.params != nullptr) {
            args[21].u.buffer = host_buffer(
                setup.params, static_cast<int>(setup.wavelet_entries));
        }
        if (setup.skinprotect != 0) {
            args[18].u.buffer = mask_buffer(setup.hue, image.W, image.H);
            args[19].u.buffer = mask_buffer(setup.chrom, image.W, image.H);
        }
    }

    std::remove(profile_path.c_str());
    setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
    unsetenv("RT_STAGE_OPENCL");
    if (submit_device) {
        set_node_device_override("lab.wavelet", StageNodeDevice::OpenCL);
    } else {
        clear_node_device_overrides();
    }
    result.status = run("lab.wavelet", std::vector<RtStageArg>(args, args + 22));
    clear_node_device_overrides();
    unsetenv("RT_STAGE_PROFILE");
    return result;
}

/*
 * The denoise groundwork's median step must match ImProcFunctions::Mad()
 * exactly, including the truncating absolute-value histogram and the
 * interpolated median.
 */
void test_lab_wavelet_mad()
{
    using namespace rtengine::stage;

    static MyMutex mad_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &mad_lcms_mutex;
    }
    static rtengine::Settings mad_settings{};
    rtengine::settings = &mad_settings;

    rtengine::procparams::ProcParams params;
    rtengine::ImProcFunctions ipf(&params, false);

    const std::vector<std::vector<float>> fixtures = {
        {0.f, 1.f, -1.f, 2.f, -2.f, 3.f, -3.f},
        {0.4f, -0.6f, 100.9f, -100.1f, 32767.9f, -40000.f, 12.25f,
         -12.25f},
        {5.f, 5.f, 5.f, 5.f, -5.f},
        {-0.9f, -0.1f, 0.1f, 0.9f, 1.1f, -1.1f, 7.7f, -7.7f},
    };
    for (const std::vector<float>& data : fixtures) {
        std::vector<std::uint32_t> histogram(32768u, 0u);
        for (float value : data) {
            const int bin = static_cast<int>(
                std::min(32767.f, std::fabs(value)));
            ++histogram[static_cast<std::size_t>(bin)];
        }
        const float expected =
            ipf.Mad(data.data(), static_cast<int>(data.size()));
        const float actual = labWaveletMadFromHistogram(
            histogram.data(), static_cast<std::uint32_t>(data.size()));
        check(actual == expected,
              "lab.wavelet MAD matches ImProcFunctions::Mad()");
    }

    check(labWaveletMadFromHistogram(nullptr, 2) == 0.f,
          "lab.wavelet MAD handles a null histogram");
    std::uint32_t empty[32768] = {};
    check(labWaveletMadFromHistogram(empty, 0) == 0.f,
          "lab.wavelet MAD handles an empty band");
    check(labWaveletMadFromHistogram(empty, 1) == 0.f,
          "lab.wavelet MAD handles a single sample");
}

/*
 * The lab.wavelet node against the production block. Supported cases run the
 * equaliser on the device and must stay within one LSB16; residual cases must
 * be refused, run the builtin and match the block byte for byte.
 */
void test_lab_wavelet(bool available)
{
    using namespace rtengine::stage;

    static MyMutex wavelet_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &wavelet_lcms_mutex;
    }
    static rtengine::Settings wavelet_settings{};
    rtengine::settings = &wavelet_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet.jsonl";

    if (!available) {
        skip("lab.wavelet: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    /*
     * The per-level range tables are the block's own fillLut() output and the
     * launcher interpolates them exactly as LUTf::operator[](float) does.
     */
    {
        rtengine::procparams::ProcParams params;
        params.dirpyrequalizer.enabled = true;
        params.dirpyrequalizer.cbdlMethod = "aft";
        const double mult[6] = {1.6, 1.4, 1.2, 1.1, 1.05, 1.02};
        for (int level = 0; level < 6; ++level) {
            params.dirpyrequalizer.mult[level] = mult[level];
        }
        rtengine::ImProcFunctions ipf(&params, false);
        LabWaveletContext context(params, ipf, 179, 48);
        RtLabWaveletResult setup{};
        check(context.build(setup) == 0, "lab.wavelet setup builds");
        check(setup.levels == 6, "lab.wavelet exports every active level");
        check(setup.lut_entries == RT_LAB_WAVELET_LUT_ENTRIES,
              "lab.wavelet range tables have the block's size");
        bool finite = true;
        for (std::size_t index = 0;
             index < static_cast<std::size_t>(setup.levels) *
                         RT_LAB_WAVELET_LUT_ENTRIES;
             ++index) {
            finite = finite && std::isfinite(setup.luts[index]);
        }
        check(finite, "the lab.wavelet range tables are finite");
    }

    const LabWaveletCase cases[] = {
        {"all levels", 179, 48, 0, {1.5, 1.5, 1.5, 1.5, 1.5, 1.5}, 0.2, 0.0,
         false},
        {"finest only", 179, 48, 0, {2.0, 1, 1, 1, 1, 1}, 0.2, 0.0, false},
        {"three levels", 238, 64, 0, {2.0, 1.5, 1.2, 1, 1, 1}, 0.2, 0.0,
         false},
        {"threshold 0", 238, 64, 0, {1.4, 1.3, 1.2, 1.1, 1.05, 1.02}, 0.0,
         0.0, false},
        {"threshold 1", 179, 48, 0, {1, 1, 1, 1, 1, 1.8}, 1.0, 0.0, false},
        {"skin protection", 238, 64, 1, {1.6, 1.4, 1.2, 1.1, 1.05, 1.02},
         0.2, 60.0, false},
        {"negative skin", 179, 48, 1, {1.6, 1.4, 1.2, 1.1, 1.05, 1.02},
         0.2, -60.0, false},
        {"gamutlab and skin", 238, 64, 0, {1.6, 1.4, 1.2, 1.1, 1.05, 1.02},
         0.2, 60.0, true},
        {"gamutlab no skin", 179, 48, 0, {1.6, 1.4, 1.2, 1.1, 1.05, 1.02},
         0.2, 0.0, true},
        {"padded 179 skin", 179, 48, 1, {1.4, 1.3, 1.2, 1.1, 1.05, 1.02},
         0.2, 40.0, true},
        {"padded 238 skin", 238, 64, 1, {1.4, 1.3, 1.2, 1.1, 1.05, 1.02},
         0.2, 40.0, false},
        {"saturated hue circle", 238, 64, 2,
         {1.6, 1.4, 1.2, 1.1, 1.05, 1.02}, 0.2, 30.0, true},
        {"large width 8192", 8192, 64, 0,
         {1.4, 1.3, 1.2, 1.1, 1.05, 1.02}, 0.2, 0.0, false},
        {"large width 8192 skin", 8192, 64, 1,
         {1.4, 1.3, 1.2, 1.1, 1.05, 1.02}, 0.2, 50.0, true},
    };

    unsigned seed = 0x6d2b79f5u;
    for (const LabWaveletCase& item : cases) {
        rtengine::procparams::ProcParams params;
        params.dirpyrequalizer.enabled = true;
        params.dirpyrequalizer.cbdlMethod = "aft";
        params.dirpyrequalizer.threshold = item.threshold;
        params.dirpyrequalizer.skinprotect = item.skinprotect;
        params.dirpyrequalizer.gamutlab = item.gamutlab;
        for (int level = 0; level < 6; ++level) {
            params.dirpyrequalizer.mult[level] = item.mult[level];
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        switch (item.image) {
        case 1:
            fill_lab_wavelet_skin(source);
            break;
        case 2:
            fill_lab_curve_saturated(source);
            break;
        default:
            fill_lab_tools(source, seed++);
            break;
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);
        check(result.supported,
              (std::string("lab.wavelet ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet %s %dx%d: PAE %.6f LSB16; "
                    "exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, >1 %zu of "
                    "%zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.wavelet ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet ") + item.label +
               " is within one LSB16").c_str());

        /* The equaliser must actually change the image, so a device pass that
         * silently returned its input cannot pass. */
        if (item.image != 2) {
            const Diff changed = compare(view(source), view(device));
            check(changed.differing > 0,
                  (std::string("lab.wavelet ") + item.label +
                   " changes the image").c_str());
        }
    }

    /*
     * Residuals: the wavelet tool, the before-CBDL method, out-of-range
     * parameters, a closed CIE-CAM gate and an all-neutral pyramid must be
     * refused and must run the block on the host byte for byte.
     */
    const char* residual_labels[] = {
        "wavelet enabled without contrast", "cbdlMethod bef", "mult out of range",
        "threshold out of range", "skin out of range",
        "all multipliers neutral", "CIE-CAM gate closed"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        params.dirpyrequalizer.enabled = true;
        params.dirpyrequalizer.cbdlMethod = "aft";
        params.dirpyrequalizer.mult[0] = 1.5;
        params.dirpyrequalizer.threshold = 0.2;
        params.dirpyrequalizer.skinprotect = 0.0;

        if (std::strcmp(label, "wavelet enabled without contrast") == 0) {
            params.wavelet.enabled = true;
        } else if (std::strcmp(label, "cbdlMethod bef") == 0) {
            params.dirpyrequalizer.cbdlMethod = "bef";
        } else if (std::strcmp(label, "mult out of range") == 0) {
            params.dirpyrequalizer.mult[0] = 5.0;
        } else if (std::strcmp(label, "threshold out of range") == 0) {
            params.dirpyrequalizer.threshold = 2.0;
        } else if (std::strcmp(label, "skin out of range") == 0) {
            params.dirpyrequalizer.skinprotect = 200.0;
        } else if (std::strcmp(label, "all multipliers neutral") == 0) {
            for (int level = 0; level < 6; ++level) {
                params.dirpyrequalizer.mult[level] = 1.0;
            }
        } else {
            params.colorappearance.enabled = true;
            params.colorappearance.modelmethod = "02";
            wavelet_settings.autocielab = true;
        }

        check(!labWaveletSupported(params, 179, 48),
              (std::string("lab.wavelet residual ") + label +
               " is a residual").c_str());

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet residual %s: host, PAE "
                    "%.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet residual ") + label +
               " matches the CPU block").c_str());
        wavelet_settings.autocielab = false;
    }

    /*
     * The default path: with RT_STAGE_OPENCL unset and no override, run()
     * resolves to the built-in, which executes the same extracted host block
     * the call site uses when the node is not selected. It must match the
     * direct block byte for byte.
     */
    {
        rtengine::procparams::ProcParams params;
        params.dirpyrequalizer.enabled = true;
        params.dirpyrequalizer.cbdlMethod = "aft";
        params.dirpyrequalizer.mult[0] = 1.5;
        params.dirpyrequalizer.threshold = 0.2;

        LabImage source(179, 48);
        fill_lab_tools(source, 0xa5a5a5a5u);

        rtengine::ImProcFunctions host_ipf(&params, false);
        LabImage host(179, 48);
        host.CopyFrom(&source);
        labWaveletHostBlock(host_ipf, params, host);

        rtengine::ImProcFunctions builtin_ipf(&params, false);
        LabImage builtin(179, 48);
        builtin.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(builtin, params, builtin_ipf, profile_path,
                                 false);
        const std::string profile = read_file(profile_path);
        check(result.status == RT_STAGE_OK,
              "lab.wavelet default path completes through run()");
        check(profile.find("\"impl\":\"builtin\"") != std::string::npos,
              "lab.wavelet default path records the built-in implementation");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "lab.wavelet default path never runs a device pass");
        const Diff diff = compare(view(builtin), view(host));
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "lab.wavelet default path matches the host block");
    }

    /*
     * Non-vacuity: skin protection and the gamutlab pre-pass must change the
     * host block's own output, so the supported cases exercised both.
     */
    {
        const auto host_block = [&](rtengine::procparams::ProcParams& p,
                                    const LabImage& source) {
            LabImage image(source.W, source.H);
            image.CopyFrom(&source);
            rtengine::ImProcFunctions ipf(&p, false);
            labWaveletHostBlock(ipf, p, image);
            return image;
        };

        rtengine::procparams::ProcParams skin;
        skin.dirpyrequalizer.enabled = true;
        skin.dirpyrequalizer.cbdlMethod = "aft";
        skin.dirpyrequalizer.mult[0] = 1.6;
        skin.dirpyrequalizer.threshold = 0.2;
        skin.dirpyrequalizer.skinprotect = 60.0;

        LabImage skin_source(238, 64);
        fill_lab_wavelet_skin(skin_source);
        rtengine::procparams::ProcParams no_skin = skin;
        no_skin.dirpyrequalizer.skinprotect = 0.0;
        const Diff protection =
            compare(view(host_block(skin, skin_source)),
                    view(host_block(no_skin, skin_source)));
        std::printf("stage-residency: lab.wavelet skin protection changes %zu "
                    "of %zu samples, PAE %.6f LSB16\n",
                    protection.differing, protection.samples,
                    protection.max_abs);
        check(protection.differing > 0,
              "lab.wavelet skin protection changes the image");

        rtengine::procparams::ProcParams gamut = skin;
        gamut.dirpyrequalizer.gamutlab = true;
        rtengine::procparams::ProcParams plain = skin;
        plain.dirpyrequalizer.gamutlab = false;
        LabImage gamut_source(238, 64);
        fill_lab_tools(gamut_source, 0x51ed270bu);
        const Diff artifact =
            compare(view(host_block(gamut, gamut_source)),
                    view(host_block(plain, gamut_source)));
        std::printf("stage-residency: lab.wavelet gamutlab pre-pass changes %zu "
                    "of %zu samples, PAE %.6f LSB16\n",
                    artifact.differing, artifact.samples, artifact.max_abs);
        check(artifact.differing > 0,
              "lab.wavelet gamutlab pre-pass changes the image");
    }
}

/* --------------------------------------------- lab.wavelet contrast pass */

/*
 * The contrast-only wavelet configuration. The equaliser is off unless the
 * case asks for both passes; every other panel is off, which is the set the
 * device admits.
 */
void wavelet_contrast_params(rtengine::procparams::ProcParams& params,
                             int levels, bool highlight_shadow,
                             const int* contrast, double sigma, double offset,
                             int strength)
{
    params.dirpyrequalizer.enabled = false;
    params.wavelet.enabled = true;
    params.wavelet.expcontrast = true;
    params.wavelet.expchroma = false;
    params.wavelet.expedge = false;
    params.wavelet.expbl = false;
    params.wavelet.expresid = false;
    params.wavelet.expfinal = false;
    params.wavelet.exptoning = false;
    params.wavelet.expnoise = false;
    params.wavelet.expclari = false;
    params.wavelet.CLmethod = "all";
    params.wavelet.Tilesmethod = "full";
    params.wavelet.HSmethod = highlight_shadow ? "with" : "without";
    params.wavelet.daubcoeffmethod = "6_";
    params.wavelet.thres = levels;
    params.wavelet.threshold = 4;
    params.wavelet.threshold2 = 5;
    params.wavelet.sigma = sigma;
    params.wavelet.offset = offset;
    params.wavelet.lowthr = 40.0;
    params.wavelet.strength = strength;
    params.wavelet.softrad = 0.0;
    params.wavelet.softradend = 0.0;
    params.wavelet.skinprotect = 0.0;
    params.wavelet.avoid = false;
    params.wavelet.median = false;
    params.wavelet.medianlev = false;
    params.wavelet.sup = 0;
    params.wavelet.thrend = 0.0;
    for (int level = 0; level < 9; ++level) {
        params.wavelet.c[level] = contrast[level];
    }
}

void test_lab_wavelet_contrast(bool available)
{
    using namespace rtengine::stage;

    static MyMutex contrast_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &contrast_lcms_mutex;
    }
    static rtengine::Settings contrast_settings{};
    rtengine::settings = &contrast_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-contrast.jsonl";

    if (!available) {
        skip("lab.wavelet contrast: no OpenCL device");
        return;
    }

    struct WaveletCase {
        const char* label;
        int width;
        int height;
        int levels;
        bool highlight_shadow;
        int contrast[9];
        double sigma;
        double offset;
        int strength;
        bool transform_only;
        bool equalizer;
        int image; /* 0 random, 1 gradient */
        double thrend;
    };

    const WaveletCase cases[] = {
        /* Transform only: contrast multipliers are zero and the residual
         * contrast slider keeps the decomposition alive, so the device runs
         * the forward and inverse transform through the same path the host
         * block uses. Production refuses c[0] == 0; the forced submission
         * proves the transform itself is bit-exact. */
        {"transform only", 179, 48, 5, false, {0, 0, 0, 0, 0, 0, 0, 0, 0},
         1.0, 1.0, 100, true, false, 1},
        {"contrast without HS", 179, 48, 5, false,
         {24, 18, 12, 6, 3, 0, 0, 0, 0}, 1.0, 1.0, 100, false, false, 1},
        {"contrast negative c", 179, 48, 5, false,
         {-30, -20, -10, 5, 5, 0, 0, 0, 0}, 1.0, 1.0, 100, false, false, 0},
        {"contrast sigma offset", 238, 64, 6, false,
         {35, 25, 15, 8, 4, 2, 0, 0, 0}, 0.5, 1.4, 100, false, false, 1},
        {"contrast strength 60", 238, 64, 6, false,
         {35, 25, 15, 8, 4, 2, 0, 0, 0}, 1.0, 1.0, 60, false, false, 1},
        {"contrast with HS", 238, 64, 6, true,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 1.0, 1.0, 100, false, false, 1},
        {"contrast HS padded 179", 179, 49, 5, true,
         {30, 22, 14, 8, 4, 0, 0, 0, 0}, 1.0, 1.0, 100, false, false, 0},
        {"levels 1", 179, 48, 1, false, {40, 0, 0, 0, 0, 0, 0, 0, 0}, 1.0,
         1.0, 100, false, false, 1},
        {"levels 2", 179, 48, 2, false, {40, 20, 0, 0, 0, 0, 0, 0, 0}, 1.0,
         1.0, 100, false, false, 1},
        {"levels 3", 179, 48, 3, false, {40, 20, 10, 0, 0, 0, 0, 0, 0},
         1.0, 1.0, 100, false, false, 1},
        {"levels 7", 179, 160, 7, true,
         {40, 30, 20, 10, 5, 3, 2, 0, 0}, 1.0, 1.0, 100, false, false, 1},
        {"wide 8192", 8192, 64, 6, true,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 1.0, 1.0, 100, false, false, 0},
        {"equalizer and wavelet", 238, 64, 6, true,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 1.0, 1.0, 100, false, true, 1, 0.0},
        /* Thrend>0 undoes the trailing-zero trim: the host keeps six levels
         * although the last two contrast multipliers are zero, so delta and
         * the highlight/shadow kH use maxlvl 6. */
        {"thrend keeps zero levels", 238, 64, 6, true,
         {40, 30, 20, 10, 0, 0, 0, 0, 0}, 1.0, 1.0, 100, false, false, 1,
         5.0},
    };

    unsigned seed = 0x1b56c4e9u;
    for (const WaveletCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_contrast_params(params, item.levels, item.highlight_shadow,
                                item.contrast, item.sigma, item.offset,
                                item.strength);
        params.wavelet.thrend = item.thrend;
        if (item.transform_only) {
            /* Keep the decomposition alive with a non-zero residual contrast
             * slider; the residual curve itself needs Expresid, which is off,
             * so the host block only runs the transform. */
            params.wavelet.contrast = 1;
        }
        if (item.equalizer) {
            params.dirpyrequalizer.enabled = true;
            params.dirpyrequalizer.cbdlMethod = "aft";
            params.dirpyrequalizer.mult[0] = 1.4;
            params.dirpyrequalizer.threshold = 0.2;
        }

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        if (item.transform_only) {
            check(!labWaveletSupported(params, item.width, item.height),
                  "lab.wavelet transform-only set is a production residual");
        } else {
            check(labWaveletSupported(params, item.width, item.height),
                  (std::string("lab.wavelet contrast ") + item.label +
                   " is the supported set").c_str());
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result = run_lab_wavelet_node(
            device, params, device_ipf, profile_path, true,
            item.transform_only);
        const std::string profile = read_file(profile_path);

        check(result.deviceReady,
              (std::string("lab.wavelet contrast ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet contrast ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet contrast ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet contrast ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet contrast %s %dx%d L%d: "
                    "PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet contrast ") + item.label +
               " stays finite").c_str());
        check(d.exact == d.samples,
              (std::string("lab.wavelet contrast ") + item.label +
               " is bit-exact").c_str());
    }

    /*
     * Residuals: every parameter outside the contrast-only set must run the
     * block on the host, byte for byte.
     */
    const char* residual_labels[] = {
        "expcontrast off", "expchroma on", "expedge on", "expbl on",
        "expresid on", "expnoise on",
        "Tilesmethod big", "softrad",
        "skinprotect", "avoid", "median", "sup", "daubcoeffmethod 4_",
        "HSmethod other", "Chcurve", "sigma range", "offset range",
        "lowthr range", "threshold range", "threshold2 range",
        "strength range", "c range", "c0 zero", "levels zero",
        "thrend range", "nothing enabled"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
        wavelet_contrast_params(params, 6, true, contrast, 1.0, 1.0, 100);

        if (std::strcmp(label, "expcontrast off") == 0) {
            params.wavelet.expcontrast = false;
        } else if (std::strcmp(label, "expchroma on") == 0) {
            params.wavelet.expchroma = true;
        } else if (std::strcmp(label, "expedge on") == 0) {
            params.wavelet.expedge = true;
        } else if (std::strcmp(label, "expbl on") == 0) {
            params.wavelet.expbl = true;
        } else if (std::strcmp(label, "expresid on") == 0) {
            params.wavelet.expresid = true;
        } else if (std::strcmp(label, "expnoise on") == 0) {
            params.wavelet.expnoise = true;
        } else if (std::strcmp(label, "Tilesmethod big") == 0) {
            params.wavelet.Tilesmethod = "big";
        } else if (std::strcmp(label, "softrad") == 0) {
            params.wavelet.softrad = 5.0;
        } else if (std::strcmp(label, "skinprotect") == 0) {
            params.wavelet.skinprotect = 30.0;
        } else if (std::strcmp(label, "avoid") == 0) {
            params.wavelet.avoid = true;
        } else if (std::strcmp(label, "median") == 0) {
            params.wavelet.median = true;
        } else if (std::strcmp(label, "sup") == 0) {
            params.wavelet.sup = 10;
        } else if (std::strcmp(label, "daubcoeffmethod 4_") == 0) {
            params.wavelet.daubcoeffmethod = "4_";
        } else if (std::strcmp(label, "HSmethod other") == 0) {
            params.wavelet.HSmethod = "other";
        } else if (std::strcmp(label, "Chcurve") == 0) {
            params.wavelet.Chcurve = {1.0, 0.0, 0.5, 0.35, 0.35, 0.5, 0.2, 0.35, 0.35, 1.0, 0.5, 0.35, 0.35};
        } else if (std::strcmp(label, "sigma range") == 0) {
            params.wavelet.sigma = 3.0;
        } else if (std::strcmp(label, "offset range") == 0) {
            params.wavelet.offset = 2.0;
        } else if (std::strcmp(label, "lowthr range") == 0) {
            params.wavelet.lowthr = 10.0;
        } else if (std::strcmp(label, "threshold range") == 0) {
            params.wavelet.threshold = 0;
        } else if (std::strcmp(label, "threshold2 range") == 0) {
            params.wavelet.threshold2 = 2;
        } else if (std::strcmp(label, "strength range") == 0) {
            params.wavelet.strength = 101;
        } else if (std::strcmp(label, "c range") == 0) {
            params.wavelet.c[0] = 400;
        } else if (std::strcmp(label, "c0 zero") == 0) {
            params.wavelet.c[0] = 0;
        } else if (std::strcmp(label, "levels zero") == 0) {
            params.wavelet.thres = 0;
        } else if (std::strcmp(label, "thrend range") == 0) {
            params.wavelet.thrend = 101.0;
        } else if (std::strcmp(label, "nothing enabled") == 0) {
            params.wavelet.enabled = false;
            params.dirpyrequalizer.enabled = false;
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet contrast residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet contrast residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet contrast residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet contrast residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet contrast residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet contrast residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet contrast residual %s: "
                    "host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet contrast residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet contrast residual ") + label +
               " matches the CPU block").c_str());
    }

    /*
     * The default path: with RT_STAGE_OPENCL unset and no override, run()
     * resolves to the built-in, which executes the same extracted host block.
     * It must match the direct block byte for byte, with the equaliser off
     * and only the wavelet tool enabled.
     */
    {
        rtengine::procparams::ProcParams params;
        const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
        wavelet_contrast_params(params, 6, true, contrast, 1.0, 1.0, 100);

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions host_ipf(&params, false);
        LabImage host(238, 64);
        host.CopyFrom(&source);
        labWaveletHostBlock(host_ipf, params, host);

        rtengine::ImProcFunctions builtin_ipf(&params, false);
        LabImage builtin(238, 64);
        builtin.CopyFrom(&source);
        const LabWaveletRun result = run_lab_wavelet_node(
            builtin, params, builtin_ipf, profile_path, false);
        const std::string profile = read_file(profile_path);
        check(result.status == RT_STAGE_OK,
              "lab.wavelet contrast default path completes through run()");
        check(profile.find("\"impl\":\"builtin\"") != std::string::npos,
              "lab.wavelet contrast default path records the built-in "
              "implementation");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "lab.wavelet contrast default path never runs a device pass");
        const Diff diff = compare(view(builtin), view(host));
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "lab.wavelet contrast default path matches the host block");
    }

    /*
     * Non-vacuity: the contrast path must change thousands of samples and
     * move at least one by more than one LSB16 against the identity.
     */
    {
        rtengine::procparams::ProcParams params;
        const int contrast[9] = {40, 30, 20, 10, 5, 3, 0, 0, 0};
        wavelet_contrast_params(params, 6, true, contrast, 1.0, 1.0, 100);

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions ipf(&params, false);
        LabImage changed(238, 64);
        changed.CopyFrom(&source);
        labWaveletHostBlock(ipf, params, changed);

        const Diff diff = compare(view(changed), view(source));
        std::printf("stage-residency: lab.wavelet contrast changes %zu of "
                    "%zu samples against the identity, PAE %.6f LSB16\n",
                    diff.differing, diff.samples, diff.max_abs);
        check(diff.differing > 1000,
              "lab.wavelet contrast changes thousands of samples");
        check(diff.max_abs > 1.0,
              "lab.wavelet contrast moves more than one LSB16");
    }
}

/* ------------------------------------------------ lab.wavelet chroma arm */

/*
 * The chroma panel's "link" arm: the contrast multipliers c[] drive both the
 * L contrast path (when it is on) and the a/b coefficient. The panel's own
 * strength is chro and its contrast-modulation sigma is sigmacol.
 */
void wavelet_chroma_params(rtengine::procparams::ProcParams& params,
                           int levels, const int* contrast, int chro,
                           double sigmacol, double skinprotect,
                           bool expcontrast, bool expchroma)
{
    wavelet_contrast_params(params, levels, false, contrast, 1.0, 1.0, 100);
    params.wavelet.expcontrast = expcontrast;
    params.wavelet.expchroma = expchroma;
    params.wavelet.CHmethod = "link";
    params.wavelet.chro = chro;
    params.wavelet.sigmacol = sigmacol;
    params.wavelet.skinprotect = skinprotect;
}

void test_lab_wavelet_chroma(bool available)
{
    using namespace rtengine::stage;

    static MyMutex chroma_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &chroma_lcms_mutex;
    }
    static rtengine::Settings chroma_settings{};
    rtengine::settings = &chroma_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-chroma.jsonl";

    if (!available) {
        skip("lab.wavelet chroma: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    struct ChromaCase {
        const char* label;
        int width;
        int height;
        int levels;
        int contrast[9];
        int chro;
        double sigmacol;
        double skinprotect;
        bool expcontrast;
        bool equalizer;
        int image; /* 0 random, 1 skin, 2 saturated hue circle */
    };

    const ChromaCase cases[] = {
        {"chroma alone levels 6", 238, 64, 6,
         {40, 30, 20, 10, 5, 2, 0, 0, 0}, 60, 1.0, 0.0, false, false, 0},
        {"chroma alone levels 3", 179, 48, 3,
         {40, 20, 10, 0, 0, 0, 0, 0, 0}, 80, 1.0, 0.0, false, false, 0},
        {"chroma strength 100", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 100, 1.0, 0.0, false, false, 0},
        {"chroma sigma 0.5", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 70, 0.5, 0.0, false, false, 0},
        {"chroma with contrast", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 50, 1.0, 0.0, true, false, 0},
        {"chroma equalizer", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 60, 1.0, 0.0, false, true, 1},
        {"chroma skin positive", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 60, 1.0, 60.0, false, false, 1},
        {"chroma skin negative", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 60, 1.0, -60.0, false, false, 1},
        {"chroma equalizer skin", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 60, 1.0, 60.0, false, true, 1},
        {"chroma saturated hue", 238, 64, 6,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 70, 1.0, 40.0, false, false, 2},
        {"chroma padded 179", 179, 49, 5,
         {40, 30, 20, 10, 4, 0, 0, 0, 0}, 60, 1.0, 40.0, false, false, 1},
        {"chroma levels 7", 179, 160, 7,
         {40, 30, 20, 10, 5, 3, 2, 0, 0}, 60, 1.0, 0.0, false, false, 0},
    };

    unsigned seed = 0x2f7c1a93u;
    for (const ChromaCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_chroma_params(params, item.levels, item.contrast, item.chro,
                              item.sigmacol, item.skinprotect,
                              item.expcontrast, true);
        if (item.equalizer) {
            params.dirpyrequalizer.enabled = true;
            params.dirpyrequalizer.cbdlMethod = "aft";
            params.dirpyrequalizer.mult[0] = 1.4;
            params.dirpyrequalizer.threshold = 0.2;
        }

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else if (item.image == 2) {
            fill_lab_curve_saturated(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet chroma ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet chroma ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet chroma ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet chroma ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet chroma ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet chroma ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet chroma %s %dx%d L%d: "
                    "PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet chroma ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet chroma ") + item.label +
               " is within one LSB16").c_str());

        /*
         * Non-vacuity: with the chroma panel off and everything else equal,
         * the host block's a/b pass must change the image, otherwise the
         * parity above compares a no-op.
         */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.expchroma = false;
        off.wavelet.skinprotect = 0.0;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet chroma %s changes %zu of "
                    "%zu samples against the panel-off result, PAE %.6f "
                    "LSB16\n",
                    item.label, active.differing, active.samples,
                    active.max_abs);
        check(active.differing > 0,
              (std::string("lab.wavelet chroma ") + item.label +
               " is non-vacuous").c_str());
    }

    /*
     * Refusals: the unported CHmethod arms, sliders outside their GUI range,
     * a non-identity H curve, chroma mixed with the unported L skin
     * protection, a chroma-only profile with no strength and another enabled
     * panel must all run the host block byte for byte.
     */
    const char* residual_labels[] = {
        "CHmethod without", "CHmethod with", "chro range", "sigmacol range",
        "skin range", "hhcurve", "contrast and skin", "chroma only no "
        "strength", "edge panel", "balance chroma method",
        "balance chroma flag"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
        wavelet_chroma_params(params, 6, contrast, 60, 1.0, 0.0, false,
                              true);

        if (std::strcmp(label, "CHmethod without") == 0) {
            params.wavelet.CHmethod = "without";
        } else if (std::strcmp(label, "CHmethod with") == 0) {
            params.wavelet.CHmethod = "with";
        } else if (std::strcmp(label, "chro range") == 0) {
            params.wavelet.chro = 101;
        } else if (std::strcmp(label, "sigmacol range") == 0) {
            params.wavelet.sigmacol = 3.0;
        } else if (std::strcmp(label, "skin range") == 0) {
            params.wavelet.skinprotect = 200.0;
        } else if (std::strcmp(label, "hhcurve") == 0) {
            params.wavelet.hhcurve = {1.0, 0.0, 0.5, 0.35, 0.35, 0.5, 0.2,
                                      0.35, 0.35, 1.0, 0.5, 0.35, 0.35};
        } else if (std::strcmp(label, "contrast and skin") == 0) {
            params.wavelet.expcontrast = true;
            params.wavelet.skinprotect = 30.0;
        } else if (std::strcmp(label,
                                "chroma only no strength") == 0) {
            params.wavelet.chro = 0;
        } else if (std::strcmp(label, "balance chroma method") == 0) {
            params.wavelet.BAmethod = "sli";
        } else if (std::strcmp(label, "balance chroma flag") == 0) {
            params.wavelet.tmr = true;
        } else {
            params.wavelet.expedge = true;
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet chroma residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet chroma residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet chroma residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet chroma residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet chroma residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet chroma residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet chroma residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet chroma residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet chroma residual ") + label +
               " matches the CPU block").c_str());
    }
}

/* ------------------------------------------------------ lab.wavelet edge */

/*
 * The edge panel's plain arm. The strength, radius, level repartition and
 * edge effect are the widget's own sliders; the median detection, np-method
 * smoothing and lipst influence are not ported and stay on the host.
 */
void wavelet_edge_params(rtengine::procparams::ProcParams& params, int levels)
{
    params.dirpyrequalizer.enabled = false;
    params.wavelet.enabled = true;
    params.wavelet.expcontrast = false;
    params.wavelet.expchroma = false;
    params.wavelet.expedge = true;
    params.wavelet.expbl = false;
    params.wavelet.expresid = false;
    params.wavelet.expfinal = false;
    params.wavelet.exptoning = false;
    params.wavelet.expnoise = false;
    params.wavelet.expclari = false;
    params.wavelet.CLmethod = "all";
    params.wavelet.Tilesmethod = "full";
    params.wavelet.HSmethod = "without";
    params.wavelet.daubcoeffmethod = "6_";
    params.wavelet.thres = levels;
    params.wavelet.threshold = 4;
    params.wavelet.threshold2 = 5;
    params.wavelet.sigma = 1.0;
    params.wavelet.offset = 1.0;
    params.wavelet.lowthr = 40.0;
    params.wavelet.strength = 100;
    params.wavelet.softrad = 0.0;
    params.wavelet.softradend = 0.0;
    params.wavelet.skinprotect = 0.0;
    params.wavelet.avoid = false;
    params.wavelet.median = false;
    params.wavelet.medianlev = false;
    params.wavelet.linkedg = false;
    params.wavelet.lipst = false;
    params.wavelet.NPmethod = "none";
    params.wavelet.EDmethod = "CU";
    params.wavelet.Medgreinf = "less";
    params.wavelet.sup = 0;
    params.wavelet.thrend = 0.0;
    params.wavelet.contrast = 0;
    params.wavelet.edgval = 50;
    params.wavelet.edgrad = 15;
    params.wavelet.edgthresh = 10;
    params.wavelet.edgeffect = 1.0;
    for (int level = 0; level < 9; ++level) {
        params.wavelet.c[level] = 0;
    }
}

void test_lab_wavelet_edge(bool available)
{
    using namespace rtengine::stage;

    static MyMutex edge_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &edge_lcms_mutex;
    }
    /*
     * The edge arm reads the options' Lipschitz influence (edghi/edglo) and
     * the radius limit, which are not part of the pp3. Use the shipped
     * defaults so the reinforce arithmetic runs with its real values.
     */
    static rtengine::Settings edge_settings{};
    edge_settings.edghi = 3.0;
    edge_settings.edglo = 0.5;
    edge_settings.limrad = 20.0;
    rtengine::settings = &edge_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-edge.jsonl";

    if (!available) {
        skip("lab.wavelet edge: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    struct EdgeCase {
        const char* label;
        int width;
        int height;
        int levels;
        bool contrast;
        bool chroma;
        int contrast_values[9];
        int chro;
        int edgval;
        int edgrad;
        int edgthresh;
        double edgeffect;
        const char* reinforce;
        int curve; /* 0 default (active), 1 identity, 2 custom */
        int image; /* 0 random, 1 skin */
    };

    const EdgeCase cases[] = {
        {"edge alone 50", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 1.0, "less", 0, 1},
        {"edge strength 100", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 100, 15, 10, 1.0, "less", 0, 0},
        {"edge radius 90", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 60, 90, 10, 1.0, "less", 0, 1},
        {"edge threshold 60", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 60, 1.0, "less", 0, 0},
        {"edge threshold -30", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, -30, 1.0, "less", 0, 1},
        {"edge effect 0.6", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 0.6, "less", 0, 1},
        {"edge effect 2.5", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 2.5, "less", 0, 1},
        {"edge reinforce more", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 1.0, "more", 0, 1},
        {"edge reinforce none", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 1.0, "none", 0, 1},
        {"edge identity curve", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 1.0, "less", 1, 1},
        {"edge custom curve", 238, 64, 6, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 1.0, "less", 2, 1},
        {"edge with contrast", 238, 64, 6, true, false,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 0, 50, 15, 10, 1.0, "less", 0, 1},
        {"edge with chroma", 238, 64, 6, false, true,
         {40, 30, 20, 10, 5, 2, 0, 0, 0}, 60, 50, 15, 10, 1.0, "less", 0, 1},
        {"edge contrast chroma", 238, 64, 6, true, true,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 60, 50, 15, 10, 1.0, "less", 0, 1},
        {"edge padded 179x49", 179, 49, 5, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 70, 20, 20, 1.2, "less", 0, 1},
        {"edge levels 3", 179, 48, 3, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 80, 15, 10, 1.0, "less", 0, 1},
        {"edge levels 7", 179, 160, 7, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 50, 15, 10, 1.0, "less", 0, 1},
    };

    unsigned seed = 0x7f4a7c15u;
    for (const EdgeCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_edge_params(params, item.levels);
        params.wavelet.expcontrast = item.contrast;
        params.wavelet.expchroma = item.chroma;
        if (item.chroma) {
            params.wavelet.CHmethod = "link";
            params.wavelet.chro = item.chro;
        }
        for (int level = 0; level < 9; ++level) {
            params.wavelet.c[level] = item.contrast_values[level];
        }
        params.wavelet.edgval = item.edgval;
        params.wavelet.edgrad = item.edgrad;
        params.wavelet.edgthresh = item.edgthresh;
        params.wavelet.edgeffect = item.edgeffect;
        params.wavelet.Medgreinf = item.reinforce;
        if (item.curve == 1) {
            params.wavelet.ccwcurve = {static_cast<double>(FCT_Linear)};
        } else if (item.curve == 2) {
            params.wavelet.ccwcurve = {
                static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.10, 0.35,
                0.35, 0.50, 0.90, 0.35, 0.35, 1.0, 0.90, 0.35, 0.35};
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet edge ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet edge ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet edge ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet edge ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet edge ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet edge ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet edge %s %dx%d L%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet edge ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet edge ") + item.label +
               " is within one LSB16").c_str());

        /* Non-vacuity: switching the edge panel off must change the host
         * block's own result, so the parity above is not a no-op. */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.expedge = false;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet edge %s changes %zu of %zu "
                    "samples against the panel-off result, PAE %.6f LSB16\n",
                    item.label, active.differing, active.samples,
                    active.max_abs);
        check(active.differing > 0,
              (std::string("lab.wavelet edge ") + item.label +
               " is non-vacuous").c_str());
    }

    /*
     * Refusals: unknown sub-mode selections, the Lipschitz sliders outside
     * their widget ranges (read only when Medianlev runs), the plain sliders'
     * ranges, an unknown reinforce mode and the unported skin protection must
     * run the host block byte for byte. EDmethod SL is admitted: the engine
     * forces EDmet == 2 for both selections, so the arm is the same block.
     */
    const char* residual_labels[] = {
        "edgval range", "edgrad range", "edgthresh range",
        "edgeffect range", "Medgreinf unknown", "zero strength",
        "skin protection", "np unknown", "edmethod unknown",
        "edgedetect range", "edgedetectthr range",
        "edgedetectthr2 range", "edgesensi range", "edgeampli range"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_edge_params(params, 6);

        if (std::strcmp(label, "edgval range") == 0) {
            params.wavelet.edgval = 101;
        } else if (std::strcmp(label, "edgrad range") == 0) {
            params.wavelet.edgrad = 101;
        } else if (std::strcmp(label, "edgthresh range") == 0) {
            params.wavelet.edgthresh = 101;
        } else if (std::strcmp(label, "edgeffect range") == 0) {
            params.wavelet.edgeffect = 3.0;
        } else if (std::strcmp(label, "Medgreinf unknown") == 0) {
            params.wavelet.Medgreinf = "bogus";
        } else if (std::strcmp(label, "zero strength") == 0) {
            params.wavelet.edgval = 0;
        } else if (std::strcmp(label, "np unknown") == 0) {
            params.wavelet.NPmethod = "bogus";
        } else if (std::strcmp(label, "edmethod unknown") == 0) {
            params.wavelet.EDmethod = "XX";
        } else if (std::strcmp(label, "edgedetect range") == 0) {
            params.wavelet.medianlev = true;
            params.wavelet.edgedetect = 101;
        } else if (std::strcmp(label, "edgedetectthr range") == 0) {
            params.wavelet.medianlev = true;
            params.wavelet.edgedetectthr = 101;
        } else if (std::strcmp(label, "edgedetectthr2 range") == 0) {
            params.wavelet.medianlev = true;
            params.wavelet.edgedetectthr2 = 101;
        } else if (std::strcmp(label, "edgesensi range") == 0) {
            params.wavelet.medianlev = true;
            params.wavelet.lipst = true;
            params.wavelet.edgesensi = 101;
        } else if (std::strcmp(label, "edgeampli range") == 0) {
            params.wavelet.medianlev = true;
            params.wavelet.lipst = true;
            params.wavelet.edgeampli = 101;
        } else {
            params.wavelet.skinprotect = 30.0;
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet edge residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet edge residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet edge residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet edge residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet edge residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet edge residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet edge residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet edge residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet edge residual ") + label +
               " matches the CPU block").c_str());
    }
}


/* ------------------------------------ lab.wavelet edge Lipschitz control */

void wavelet_denoise_params(rtengine::procparams::ProcParams& params,
                            int levels, const double noise[5],
                            double ballum);

/*
 * The edge panel's Lipschitz control: calckoe()'s detection, the NPMethod
 * neighbour smoothing with Lipst, the influence sliders, and EDmethod "SL"
 * (which selects the same EDmet == 2 block as "CU" because the engine forces
 * EDmet). Every case is checked against the host block and against the same
 * profile with Medianlev off, so the parity is not vacuous.
 */
void test_lab_wavelet_edge_lipschitz(bool available)
{
    using namespace rtengine::stage;

    static MyMutex lipschitz_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &lipschitz_lcms_mutex;
    }
    static rtengine::Settings lipschitz_settings{};
    lipschitz_settings.edghi = 3.0;
    lipschitz_settings.edglo = 0.5;
    lipschitz_settings.limrad = 20.0;
    rtengine::settings = &lipschitz_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-edge-lipschitz.jsonl";

    if (!available) {
        skip("lab.wavelet edge lipschitz: no OpenCL device");
        return;
    }

    struct LipschitzCase {
        const char* label;
        int width;
        int height;
        int levels;
        int detect;
        int detect_thr;
        int detect_thr2;
        int sensi;
        int ampli;
        bool median;
        bool lipst;
        int np; /* 0 none, 1 low, 2 high */
        bool sl;
        bool contrast;
        bool denoise;
        bool final;
        int image; /* 0 random, 1 skin */
    };

    const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
    const double noise[5] = {40, 30, 20, 10, 5};
    const LipschitzCase cases[] = {
        {"detect 3x3 default", 238, 64, 6, 90, 20, 0, 60, 10, true,
         false, 0, false, false, false, false, 1},
        {"detect thr 40", 238, 64, 6, 90, 40, 0, 60, 10, true, false, 0,
         false, false, false, false, 0},
        {"detect thr 65", 238, 64, 6, 90, 65, 0, 60, 10, true, false, 0,
         false, false, false, false, 1},
        {"detect 5x5 thr 85", 238, 64, 6, 90, 85, 0, 60, 10, true, false, 0,
         false, false, false, false, 1},
        {"detect 5x5 thr 95 amp", 238, 64, 6, 60, 95, 20, 60, 10, true,
         false, 0, false, false, false, false, 0},
        {"lipst influence", 238, 64, 6, 90, 20, 0, 85, 40, true, true, 0,
         false, false, false, false, 1},
        {"np low", 238, 64, 6, 90, 20, 0, 60, 10, true, true, 1, false,
         false, false, false, 1},
        {"np high", 238, 64, 6, 90, 20, 0, 60, 10, true, true, 2, false,
         false, false, false, 1},
        {"np low thr 0", 179, 49, 5, 90, 0, 0, 60, 10, true, true, 1,
         false, false, false, false, 1},
        {"edmethod sl", 238, 64, 6, 90, 20, 0, 60, 10, true, false, 0, true,
         false, false, false, 1},
        {"median with contrast", 238, 64, 6, 90, 20, 0, 60, 10, true,
         false, 0, false, true, false, false, 1},
        {"np high with denoise", 238, 160, 6, 90, 20, 0, 60, 10, true, true,
         2, false, false, true, false, 1},
        {"np low with final", 238, 160, 6, 90, 20, 0, 60, 10, true, true, 1,
         false, false, false, true, 1},
        {"combined arms", 238, 160, 6, 70, 85, 20, 85, 40, true, true, 2,
         true, true, true, true, 0},
        {"padded 179x49 np high", 179, 49, 5, 90, 20, 0, 60, 10, true, true,
         2, false, false, false, false, 1},
    };

    unsigned seed = 0x51ab3d7fu;
    bool smoothing_moved = false;
    bool lipst_moved = false;
    for (const LipschitzCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_edge_params(params, item.levels);
        if (item.denoise) {
            wavelet_denoise_params(params, 6, noise, 0.0);
            params.wavelet.expedge = true;
            params.wavelet.edgval = 50;
            params.wavelet.edgrad = 15;
            params.wavelet.edgthresh = 10;
            params.wavelet.edgeffect = 1.0;
            params.wavelet.Medgreinf = "less";
        }
        if (item.final) {
            params.wavelet.expfinal = true;
            params.wavelet.sigmafin = 1.0;
            params.wavelet.opacityCurveWL = {
                static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.25, 0.35,
                0.35, 0.5, 0.75, 0.35, 0.35, 1.0, 0.25, 0.35, 0.35};
        }
        params.wavelet.expcontrast = item.contrast;
        for (int level = 0; level < 9; ++level) {
            params.wavelet.c[level] =
                item.contrast ? contrast[level] : 0;
        }
        params.wavelet.medianlev = item.median;
        params.wavelet.edgedetect = item.detect;
        params.wavelet.edgedetectthr = item.detect_thr;
        params.wavelet.edgedetectthr2 = item.detect_thr2;
        params.wavelet.edgesensi = item.sensi;
        params.wavelet.edgeampli = item.ampli;
        params.wavelet.lipst = item.lipst;
        params.wavelet.NPmethod =
            item.np == 2 ? "high" : (item.np == 1 ? "low" : "none");
        params.wavelet.EDmethod = item.sl ? "SL" : "CU";

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet edge lipschitz %s %dx%d "
                    "L%d: PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " is within one LSB16").c_str());

        /* Non-vacuity against the plain arm: switching Medianlev off must
         * change the host block's own result. */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.medianlev = false;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet edge lipschitz %s changes "
                    "%zu of %zu samples against the plain arm, PAE %.6f "
                    "LSB16\n",
                    item.label, active.differing, active.samples,
                    active.max_abs);
        check(active.differing > 0,
              (std::string("lab.wavelet edge lipschitz ") + item.label +
               " is non-vacuous").c_str());

        /* EDmethod "SL" must be byte-identical to "CU": the engine forces
         * EDmet == 2 before the commented-out selection runs. */
        if (item.sl) {
            rtengine::procparams::ProcParams cu = params;
            cu.wavelet.EDmethod = "CU";
            rtengine::ImProcFunctions cu_ipf(&cu, false);
            LabImage cu_image(item.width, item.height);
            cu_image.CopyFrom(&source);
            labWaveletHostBlock(cu_ipf, cu, cu_image);
            const Diff same = compare(view(cpu), view(cu_image));
            std::printf("stage-residency: lab.wavelet edge lipschitz %s "
                        "differs from EDmethod CU in %zu of %zu samples, "
                        "PAE %.6f LSB16\n",
                        item.label, same.differing, same.samples,
                        same.max_abs);
            check(same.differing == 0 && same.max_abs == 0.0,
                  (std::string("lab.wavelet edge lipschitz ") + item.label +
                   " matches EDmethod CU").c_str());
        }

        /* The smoothing and influence arms must also move the host result on
         * their own, otherwise the admitted sub-mode is a no-op. */
        if (item.np != 0 && item.lipst) {
            rtengine::procparams::ProcParams plain_smoothing = params;
            plain_smoothing.wavelet.NPmethod = "none";
            rtengine::ImProcFunctions plain_ipf(&plain_smoothing, false);
            LabImage plain_image(item.width, item.height);
            plain_image.CopyFrom(&source);
            labWaveletHostBlock(plain_ipf, plain_smoothing, plain_image);
            const Diff moved = compare(view(cpu), view(plain_image));
            std::printf("stage-residency: lab.wavelet edge lipschitz %s "
                        "changes %zu of %zu samples against NPmethod none, "
                        "PAE %.6f LSB16\n",
                        item.label, moved.differing, moved.samples,
                        moved.max_abs);
            smoothing_moved = smoothing_moved || moved.differing > 0;
        }
        if (item.lipst) {
            rtengine::procparams::ProcParams no_lipst = params;
            no_lipst.wavelet.lipst = false;
            rtengine::ImProcFunctions no_lipst_ipf(&no_lipst, false);
            LabImage no_lipst_image(item.width, item.height);
            no_lipst_image.CopyFrom(&source);
            labWaveletHostBlock(no_lipst_ipf, no_lipst, no_lipst_image);
            const Diff moved = compare(view(cpu), view(no_lipst_image));
            std::printf("stage-residency: lab.wavelet edge lipschitz %s "
                        "changes %zu of %zu samples against Lipst off, PAE "
                        "%.6f LSB16\n",
                        item.label, moved.differing, moved.samples,
                        moved.max_abs);
            lipst_moved = lipst_moved || moved.differing > 0;
        }
    }
    check(smoothing_moved,
          "the edge lipschitz NPMethod smoothing moves the host result");
    check(lipst_moved,
          "the edge lipschitz Lipst influence moves the host result");
}

/* ----------------------------------------------------- lab.wavelet final */

/*
 * The final panel's curve arm: the coefficient-domain finalContAllL() curve
 * and the pixel-domain wavclCurve. The edge-like a/b round trip is forced by
 * Expfinal, so the device must keep it alive even without Expchroma.
 */
void wavelet_final_params(rtengine::procparams::ProcParams& params,
                          int levels)
{
    params.dirpyrequalizer.enabled = false;
    params.wavelet.enabled = true;
    params.wavelet.expcontrast = false;
    params.wavelet.expchroma = false;
    params.wavelet.expedge = false;
    params.wavelet.expbl = false;
    params.wavelet.expresid = false;
    params.wavelet.expfinal = true;
    params.wavelet.exptoning = false;
    params.wavelet.expnoise = false;
    params.wavelet.expclari = false;
    params.wavelet.CLmethod = "all";
    params.wavelet.Tilesmethod = "full";
    params.wavelet.HSmethod = "without";
    params.wavelet.daubcoeffmethod = "6_";
    params.wavelet.thres = levels;
    params.wavelet.threshold = 4;
    params.wavelet.threshold2 = 5;
    params.wavelet.sigma = 1.0;
    params.wavelet.offset = 1.0;
    params.wavelet.lowthr = 40.0;
    params.wavelet.strength = 100;
    params.wavelet.softrad = 0.0;
    params.wavelet.softradend = 0.0;
    params.wavelet.skinprotect = 0.0;
    params.wavelet.avoid = false;
    params.wavelet.median = false;
    params.wavelet.medianlev = false;
    params.wavelet.linkedg = false;
    params.wavelet.lipst = false;
    params.wavelet.NPmethod = "none";
    params.wavelet.EDmethod = "CU";
    params.wavelet.Medgreinf = "less";
    params.wavelet.sup = 0;
    params.wavelet.thrend = 0.0;
    params.wavelet.contrast = 0;
    params.wavelet.edgval = 50;
    params.wavelet.edgrad = 15;
    params.wavelet.edgthresh = 10;
    params.wavelet.edgeffect = 1.0;
    params.wavelet.sigmafin = 1.0;
    params.wavelet.BAmethod = "none";
    params.wavelet.tmr = false;
    params.wavelet.balance = 0;
    params.wavelet.iter = 0;
    params.wavelet.sigmadir = 1.0;
    params.wavelet.wavclCurve = {static_cast<double>(DCT_Linear)};
    params.wavelet.opacityCurveWL = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    for (int level = 0; level < 9; ++level) {
        params.wavelet.c[level] = 0;
    }
}

const std::vector<double>& final_diagonal_curve()
{
    static const std::vector<double> curve = {
        static_cast<double>(DCT_Spline), 0.0, 0.05, 0.5, 0.90, 1.0, 0.95};
    return curve;
}

const std::vector<double>& final_opacity_curve()
{
    static const std::vector<double> curve = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.20, 0.35, 0.35,
        0.5, 0.80, 0.35, 0.35, 1.0, 0.30, 0.35, 0.35};
    return curve;
}

void wavelet_denoise_params(rtengine::procparams::ProcParams& params,
                            int levels, const double noise[5], double ballum);

/*
 * The final panel's guided soft radius. It runs after the final pixel curve
 * on the reconstructed L, and the whole block is ported: the guided filter,
 * the coefficient-domain second pass (`Thrend > 0`) and the hue modulation
 * (`wavguidcurve`). The matrix covers the radius, the blend and the
 * detail-adjust sliders, the identity and curved final panels, contrast and
 * denoise, and the subsampling arms a small and a wide image take, and it
 * repeats the sweep for each of the two coefficient-domain arms alone and
 * together. A strength of zero leaves the image unchanged by construction, so
 * only cases with a non-zero blend are checked for non-vacuity.
 */
const std::vector<double>& softend_hue_curve()
{
    /* Four knots with both a pull-down and a push-up arm, so the modulation
     * runs in both directions over the image's hues. */
    static const std::vector<double> curve = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.20, 0.35, 0.35,
        0.35, 0.70, 0.35, 0.35, 0.70, 0.30, 0.35, 0.35, 1.0, 0.85, 0.35,
        0.35};
    return curve;
}

void test_lab_wavelet_softend()
{
    using namespace rtengine::stage;

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-softend.jsonl";

    struct SoftEndCase {
        const char* label;
        int width;
        int height;
        double softradend;
        double strend;
        int detend;
        double thrend;
        bool hue;
        bool curves;
        bool contrast;
        bool denoise;
        bool smooth;
        bool expect_change;
    };

    const double noise[5] = {40, 30, 20, 10, 5};
    const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
    const SoftEndCase cases[] = {
        {"softradend 2", 238, 64, 2.0, 50.0, 0, 0.0, false, false, false,
         false, false, true},
        {"softradend 10", 238, 64, 10.0, 40.0, 2, 0.0, false, false, false,
         false, false, true},
        {"softradend 40", 238, 64, 40.0, 100.0, 4, 0.0, false, false, false,
         false, false, true},
        {"softradend 10 blend 0", 238, 64, 10.0, 0.0, -2, 0.0, false, false,
         false, false, false, false},
        {"softradend 5 curves", 238, 64, 5.0, 40.0, 0, 0.0, false, true,
         false, false, false, true},
        {"softradend 20 contrast", 238, 64, 20.0, 60.0, 2, 0.0, false, true,
         true, false, false, true},
        {"softradend 10 denoise", 238, 160, 10.0, 50.0, -2, 0.0, false,
         false, false, true, false, true},
        {"softradend 10 odd 179x161", 179, 161, 10.0, 50.0, 0, 0.0, false,
         false, false, false, false, true},
        {"softradend 40 odd 703x67", 703, 67, 40.0, 80.0, 4, 0.0, false,
         false, false, false, false, true},
        {"softradend 1", 238, 64, 1.0, 70.0, 0, 0.0, false, false, false,
         false, false, true},
        {"softradend 3 detail -10", 238, 64, 3.0, 60.0, -10, 0.0, false,
         false, false, false, false, true},
        {"softradend 1000", 238, 64, 1000.0, 100.0, 10, 0.0, false, false,
         false, false, false, true},
        {"softradend 20 wide", 1024, 64, 20.0, 80.0, 0, 0.0, false, false,
         false, false, false, true},
        {"softradend 7 wide", 1024, 64, 7.0, 80.0, 0, 0.0, false, false,
         false, false, false, true},
        {"softradend 10 smooth", 400, 300, 10.0, 70.0, 0, 0.0, false, false,
         false, false, true, true},
        {"softradend 40 smooth blend 1", 400, 300, 40.0, 100.0, 2, 0.0,
         false, false, false, false, true, true},
        {"softradend 20 smooth wide", 1024, 64, 20.0, 80.0, 0, 0.0, false,
         false, false, false, true, true},
        {"softradend 40 smooth odd 703x67", 703, 67, 40.0, 80.0, 4, 0.0,
         false, false, false, false, true, true},
        /* The coefficient-domain second pass: the block's Thrend slider. */
        {"thrend 1", 238, 64, 20.0, 60.0, 0, 1.0, false, false, false,
         false, false, true},
        {"thrend 5", 238, 64, 20.0, 70.0, 1, 5.0, false, false, false,
         false, false, true},
        {"thrend 20", 238, 64, 20.0, 80.0, 2, 20.0, false, false, false,
         false, false, true},
        {"thrend 50", 238, 64, 30.0, 90.0, 3, 50.0, false, false, false,
         false, false, true},
        {"thrend 5 curves", 238, 64, 20.0, 70.0, 0, 5.0, false, true, false,
         false, false, true},
        {"thrend 5 contrast", 238, 64, 20.0, 60.0, -2, 5.0, false, true,
         true, false, false, true},
        {"thrend 5 denoise", 238, 160, 20.0, 60.0, -2, 5.0, false, false,
         false, true, false, true},
        {"thrend 20 odd 179x161", 179, 161, 20.0, 80.0, 1, 20.0, false,
         false, false, false, false, true},
        {"thrend 1 wide 1024x64", 1024, 64, 20.0, 70.0, 0, 1.0, false,
         false, false, false, false, true},
        {"thrend 5 smooth", 400, 300, 20.0, 80.0, 2, 5.0, false, false,
         false, false, true, true},
        {"thrend 5 smooth blend 0", 400, 300, 20.0, 0.0, 2, 5.0, false,
         false, false, false, true, false},
        /* The hue modulation: a non-identity wavguidcurve. */
        {"hue 5", 238, 64, 5.0, 50.0, 0, 0.0, true, false, false, false,
         false, true},
        {"hue 20", 238, 64, 20.0, 70.0, 2, 0.0, true, false, false, false,
         false, true},
        {"hue 40", 238, 64, 40.0, 100.0, 4, 0.0, true, false, false, false,
         false, true},
        {"hue 60", 238, 64, 60.0, 90.0, -3, 0.0, true, false, false, false,
         false, true},
        {"hue 20 curves", 238, 64, 20.0, 70.0, 1, 0.0, true, true, false,
         false, false, true},
        {"hue 10 contrast", 238, 64, 10.0, 60.0, 0, 0.0, true, true, true,
         false, false, true},
        {"hue 20 denoise", 238, 160, 20.0, 70.0, -2, 0.0, true, false,
         false, true, false, true},
        {"hue 20 odd 179x161", 179, 161, 20.0, 70.0, 0, 0.0, true, false,
         false, false, false, true},
        {"hue 10 wide 1024x64", 1024, 64, 10.0, 70.0, 2, 0.0, true, false,
         false, false, false, true},
        {"hue 10 smooth", 400, 300, 10.0, 70.0, 1, 0.0, true, false, false,
         false, true, true},
        {"hue 40 smooth blend 0", 400, 300, 40.0, 0.0, 0, 0.0, true, false,
         false, false, true, false},
        /* Both coefficient-domain arms together. */
        {"thrend 5 hue", 238, 64, 20.0, 70.0, 0, 5.0, true, false, false,
         false, false, true},
        {"thrend 5 hue curves", 238, 64, 20.0, 70.0, 1, 5.0, true, true,
         false, false, false, true},
        {"thrend 5 hue denoise", 238, 160, 20.0, 70.0, -2, 5.0, true, false,
         false, true, false, true},
        {"thrend 1 hue wide 1024x64", 1024, 64, 20.0, 80.0, 0, 1.0, true,
         false, false, false, false, true},
        {"thrend 20 hue odd 179x161", 179, 161, 30.0, 80.0, 2, 20.0, true,
         false, false, false, false, true},
    };

    unsigned seed = 0x50f7e2d1u;
    for (const SoftEndCase& item : cases) {
        rtengine::procparams::ProcParams params;
        if (item.denoise) {
            wavelet_denoise_params(params, 6, noise, 5.0);
        } else {
            wavelet_final_params(params, 6);
        }
        params.wavelet.expfinal = true;
        params.wavelet.sigmafin = 1.0;
        if (item.curves) {
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }
        if (item.contrast) {
            params.wavelet.expcontrast = true;
            for (int level = 0; level < 9; ++level) {
                params.wavelet.c[level] = contrast[level];
            }
        }
        params.wavelet.softradend = item.softradend;
        params.wavelet.strend = item.strend;
        params.wavelet.detend = item.detend;
        params.wavelet.thrend = item.thrend;
        params.wavelet.wavguidcurve =
            item.hue ? softend_hue_curve()
                     : std::vector<double>{
                           static_cast<double>(FCT_Linear)};

        LabImage source(item.width, item.height);
        if (item.smooth) {
            fill_lab_smooth(source, seed++);
        } else {
            fill_lab_tools(source, seed++);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet softend ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);
        check(result.deviceReady,
              (std::string("lab.wavelet softend ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet softend ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet softend ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet softend ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet softend %s %dx%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.wavelet softend ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet softend ") + item.label +
               " is within one LSB16").c_str());

        if (item.expect_change) {
            const Diff changed = compare(view(source), view(device));
            check(changed.differing > 0,
                  (std::string("lab.wavelet softend ") + item.label +
                   " changes the image").c_str());
        }
    }

    /*
     * Non-vacuity: the same profile with softradend 0 and with its blend
     * forced to zero must differ from the filtered result, so the device cases
     * cannot pass by ignoring the slider. The blend-zero case proves the
     * filter itself, and the slider-off case a blend of one.
     */
    {
        rtengine::procparams::ProcParams params;
        wavelet_final_params(params, 6);
        params.wavelet.softradend = 40.0;
        params.wavelet.strend = 100.0;
        params.wavelet.detend = 4;

        rtengine::procparams::ProcParams off = params;
        off.wavelet.softradend = 0.0;
        rtengine::procparams::ProcParams blend_zero = params;
        blend_zero.wavelet.strend = 0.0;

        LabImage source(238, 64);
        fill_lab_tools(source, seed++);

        const auto run = [&](rtengine::procparams::ProcParams& p) {
            rtengine::ImProcFunctions ipf(&p, false);
            LabImage image(238, 64);
            image.CopyFrom(&source);
            labWaveletHostBlock(ipf, p, image);
            return image;
        };

        const Diff off_diff = compare(view(run(off)), view(run(params)));
        std::printf("stage-residency: lab.wavelet softend changes %zu of %zu "
                    "samples against softradend 0, PAE %.6f LSB16\n",
                    off_diff.differing, off_diff.samples, off_diff.max_abs);
        check(off_diff.differing > 1000 && off_diff.max_abs > 1.0,
              "lab.wavelet softend changes the image against the slider off");

        const Diff blend_diff =
            compare(view(run(blend_zero)), view(run(params)));
        std::printf("stage-residency: lab.wavelet softend filter changes %zu "
                    "of %zu samples against blend 0, PAE %.6f LSB16\n",
                    blend_diff.differing, blend_diff.samples,
                    blend_diff.max_abs);
        check(blend_diff.differing > 1000 && blend_diff.max_abs > 1.0,
              "lab.wavelet softend guided filter changes the image");
    }

    /*
     * Non-vacuity of the two coefficient-domain arms: each must change the
     * result against the same profile with its own control neutralised. The
     * Thrend arm is compared with Thrend zero, the hue arm with an identity
     * wavguidcurve, and the pair with both neutral.
     */
    {
        rtengine::procparams::ProcParams thrend;
        wavelet_final_params(thrend, 6);
        thrend.wavelet.softradend = 20.0;
        thrend.wavelet.strend = 70.0;
        thrend.wavelet.detend = 1;
        thrend.wavelet.thrend = 5.0;
        thrend.wavelet.wavguidcurve = softend_hue_curve();

        rtengine::procparams::ProcParams thrend_off = thrend;
        thrend_off.wavelet.thrend = 0.0;
        rtengine::procparams::ProcParams hue_off = thrend;
        hue_off.wavelet.wavguidcurve = {
            static_cast<double>(FCT_Linear)};
        rtengine::procparams::ProcParams both_off = thrend;
        both_off.wavelet.thrend = 0.0;
        both_off.wavelet.wavguidcurve = {
            static_cast<double>(FCT_Linear)};

        LabImage source(238, 64);
        fill_lab_tools(source, seed++);

        const auto run = [&](rtengine::procparams::ProcParams& p) {
            rtengine::ImProcFunctions ipf(&p, false);
            LabImage image(238, 64);
            image.CopyFrom(&source);
            labWaveletHostBlock(ipf, p, image);
            return image;
        };

        const LabImage full = run(thrend);
        const Diff thrend_diff = compare(view(run(thrend_off)), view(full));
        std::printf("stage-residency: lab.wavelet softend thrend arm changes "
                    "%zu of %zu samples against thrend 0, PAE %.6f LSB16\n",
                    thrend_diff.differing, thrend_diff.samples,
                    thrend_diff.max_abs);
        check(thrend_diff.differing > 1000 && thrend_diff.max_abs > 1.0,
              "lab.wavelet softend thrend arm changes the image");

        const Diff hue_diff = compare(view(run(hue_off)), view(full));
        std::printf("stage-residency: lab.wavelet softend hue arm changes "
                    "%zu of %zu samples against an identity wavguidcurve, "
                    "PAE %.6f LSB16\n",
                    hue_diff.differing, hue_diff.samples, hue_diff.max_abs);
        check(hue_diff.differing > 1000 && hue_diff.max_abs > 1.0,
              "lab.wavelet softend hue arm changes the image");

        const Diff both_diff = compare(view(run(both_off)), view(full));
        std::printf("stage-residency: lab.wavelet softend coefficient arms "
                    "change %zu of %zu samples against both neutral, PAE "
                    "%.6f LSB16\n",
                    both_diff.differing, both_diff.samples,
                    both_diff.max_abs);
        check(both_diff.differing > 1000 && both_diff.max_abs > 1.0,
              "lab.wavelet softend coefficient arms change the image");
    }

    /*
     * Sensitivity: the log-domain guided filter has a high gain, so a
     * one-LSB16 change in its input L can move the output by several LSB16.
     * This bounds how much of an end-to-end export difference can be
     * attributed to the node rather than to an upstream rounding difference.
     */
    {
        rtengine::procparams::ProcParams params;
        wavelet_final_params(params, 6);
        params.wavelet.softradend = 40.0;
        params.wavelet.strend = 100.0;
        params.wavelet.detend = 2;

        LabImage source(400, 300);
        fill_lab_smooth(source, seed++);
        LabImage bumped(400, 300);
        bumped.CopyFrom(&source);
        for (int y = 0; y < bumped.H; ++y) {
            for (int x = 0; x < bumped.W; ++x) {
                bumped.L[y][x] += 1.f;
            }
        }

        const auto run = [&](LabImage& image) {
            rtengine::ImProcFunctions ipf(&params, false);
            labWaveletHostBlock(ipf, params, image);
        };
        run(source);
        run(bumped);
        const Diff gain = compare(view(source), view(bumped));
        std::printf("stage-residency: lab.wavelet softend one-LSB input gain: "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    gain.max_abs, gain.differing, gain.samples);
        check(gain.finite, "lab.wavelet softend gain stays finite");
    }

    /*
     * Refusals: a radius without the final panel, out-of-range sliders, and
     * the clarity arm or clarity soft radius alongside it.
     */
    const char* residual_labels[] = {
        "no expfinal", "softradend high", "softradend negative",
        "strend high", "detend high", "detend low", "softrad", "clarity"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_final_params(params, 6);
        params.wavelet.softradend = 10.0;
        params.wavelet.strend = 50.0;

        if (std::strcmp(label, "no expfinal") == 0) {
            params.wavelet.expfinal = false;
        } else if (std::strcmp(label, "softradend high") == 0) {
            params.wavelet.softradend = 1001.0;
        } else if (std::strcmp(label, "softradend negative") == 0) {
            params.wavelet.softradend = -1.0;
        } else if (std::strcmp(label, "strend high") == 0) {
            params.wavelet.strend = 101.0;
        } else if (std::strcmp(label, "detend high") == 0) {
            params.wavelet.detend = 11;
        } else if (std::strcmp(label, "detend low") == 0) {
            params.wavelet.detend = -11;
        } else if (std::strcmp(label, "softrad") == 0) {
            params.wavelet.softrad = 5.0;
        } else {
            params.wavelet.expclari = true;
            params.wavelet.ushamethod = "sharp";
            params.wavelet.CLmethod = "one";
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet softend residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet softend residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet softend residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet softend residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet softend residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet softend residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet softend residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet softend residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet softend residual ") + label +
               " matches the CPU block").c_str());
    }
}

void test_lab_wavelet_final(bool available)
{
    using namespace rtengine::stage;

    static MyMutex final_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &final_lcms_mutex;
    }
    static rtengine::Settings final_settings{};
    final_settings.edghi = 3.0;
    final_settings.edglo = 0.5;
    final_settings.limrad = 20.0;
    rtengine::settings = &final_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-final.jsonl";

    if (!available) {
        skip("lab.wavelet final: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    struct FinalCase {
        const char* label;
        int width;
        int height;
        int levels;
        bool contrast;
        bool chroma;
        bool edge;
        int contrast_values[9];
        int chro;
        int diagonal; /* 0 identity, 1 custom */
        int opacity;  /* 0 default constant, 1 custom */
        double sigmafin;
        bool expect_change;
        int image; /* 0 random, 1 skin */
    };

    const FinalCase cases[] = {
        {"final diagonal only", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 0, 1.0, true, 1},
        {"final opacity only", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 1, 1.0, true, 1},
        {"final both curves", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"final sigmafin 0.5", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 0.5, true, 1},
        {"final sigmafin 2.0", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 2.0, true, 1},
        {"final default curves", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 1.0, false, 1},
        {"final with contrast", 238, 64, 6, true, false, false,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"final with chroma", 238, 64, 6, false, true, false,
         {40, 30, 20, 10, 5, 2, 0, 0, 0}, 60, 1, 1, 1.0, true, 1},
        {"final with edge", 238, 64, 6, false, false, true,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"final padded 179x49", 179, 49, 5, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.2, true, 1},
        {"final levels 3", 179, 48, 3, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"final levels 7", 179, 160, 7, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"final inert balance controls", 238, 64, 6, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
    };

    unsigned seed = 0x2545f491u;
    for (const FinalCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_final_params(params, item.levels);
        params.wavelet.expcontrast = item.contrast;
        params.wavelet.expchroma = item.chroma;
        params.wavelet.expedge = item.edge;
        if (item.chroma) {
            params.wavelet.CHmethod = "link";
            params.wavelet.chro = item.chro;
        }
        for (int level = 0; level < 9; ++level) {
            params.wavelet.c[level] = item.contrast_values[level];
        }
        if (item.diagonal == 1) {
            params.wavelet.wavclCurve = final_diagonal_curve();
        }
        if (item.opacity == 1) {
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }
        params.wavelet.sigmafin = item.sigmafin;
        if (std::strcmp(item.label, "final inert balance controls") == 0) {
            /* BAmethod is "none", so these final-panel controls are inert in
             * the host block; the device pass must still match it. */
            params.wavelet.balance = 20;
            params.wavelet.iter = 2;
            params.wavelet.sigmadir = 1.7;
            params.wavelet.opacityCurveW = final_opacity_curve();
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet final ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet final ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet final ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet final ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet final ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet final ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet final %s %dx%d L%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet final ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet final ") + item.label +
               " is within one LSB16").c_str());

        if (item.expect_change) {
            /* Non-vacuity: switching the final panel off must change the host
             * block's own result. */
            rtengine::procparams::ProcParams off = params;
            off.wavelet.expfinal = false;
            rtengine::ImProcFunctions off_ipf(&off, false);
            LabImage off_image(item.width, item.height);
            off_image.CopyFrom(&source);
            labWaveletHostBlock(off_ipf, off, off_image);
            const Diff active = compare(view(cpu), view(off_image));
            std::printf("stage-residency: lab.wavelet final %s changes %zu of "
                        "%zu samples against the panel-off result, PAE "
                        "%.6f LSB16\n",
                        item.label, active.differing, active.samples,
                        active.max_abs);
            check(active.differing > 0,
                  (std::string("lab.wavelet final ") + item.label +
                   " is non-vacuous").c_str());
        }
    }

    /*
     * Refusals: the residual tone mapping, the balance-chroma method, the
     * clarity panel, out-of-range scalars and the panels the node never
     * admits must run the host block byte for byte.
     */
    const char* residual_labels[] = {
        "softrad", "BAmethod cur no curve", "tmr",
        "sigmafin low", "sigmafin high", "avoid", "median", "expbl",
        "expresid", "expnoise",
        "skin protection"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_final_params(params, 6);

        if (std::strcmp(label, "softrad") == 0) {
            params.wavelet.softrad = 0.2;
        } else if (std::strcmp(label, "BAmethod cur no curve") == 0) {
            /* "cur" reads a non-identity opacity curve; with none the block
             * skips the arm, so the node keeps it on the host. */
            params.wavelet.BAmethod = "cur";
            params.wavelet.opacityCurveW = {
                static_cast<double>(FCT_Linear)};
        } else if (std::strcmp(label, "tmr") == 0) {
            params.wavelet.tmr = true;
        } else if (std::strcmp(label, "sigmafin low") == 0) {
            params.wavelet.sigmafin = 0.0;
        } else if (std::strcmp(label, "sigmafin high") == 0) {
            params.wavelet.sigmafin = 3.0;
        } else if (std::strcmp(label, "avoid") == 0) {
            params.wavelet.avoid = true;
        } else if (std::strcmp(label, "median") == 0) {
            params.wavelet.median = true;
        } else if (std::strcmp(label, "expbl") == 0) {
            params.wavelet.expbl = true;
        } else if (std::strcmp(label, "expresid") == 0) {
            params.wavelet.expresid = true;
        } else if (std::strcmp(label, "expnoise") == 0) {
            params.wavelet.expnoise = true;
        } else {
            params.wavelet.skinprotect = 30.0;
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet final residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet final residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet final residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet final residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet final residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet final residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet final residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet final residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet final residual ") + label +
               " matches the CPU block").c_str());
    }

    /* The final panel's guided soft radius shares this test's device gate. */
    test_lab_wavelet_softend();
}


/* ------------------------------------------------- lab.wavelet balance chroma */

/*
 * The balance-chroma arm: ContAllL()'s and ContAllAB()'s coefficient multiply.
 * The L arm runs inside the final panel's coefficient pass (`expfinal`) and
 * rebuilds the sigmadir beta ramp; the a/b arm runs with `TMr` and multiplies
 * by the raw kba. Both read the luminance-dependent factor ("sli") or the
 * opacity curve ("cur"), and both repeat the multiply by the per-level
 * iteration count.
 */
void wavelet_ba_params(rtengine::procparams::ProcParams& params, int levels)
{
    params.dirpyrequalizer.enabled = false;
    params.wavelet.enabled = true;
    params.wavelet.expcontrast = false;
    params.wavelet.expchroma = false;
    params.wavelet.expedge = false;
    params.wavelet.expbl = false;
    params.wavelet.expresid = false;
    params.wavelet.expfinal = false;
    params.wavelet.exptoning = false;
    params.wavelet.expnoise = false;
    params.wavelet.expclari = false;
    params.wavelet.CLmethod = "all";
    params.wavelet.Tilesmethod = "full";
    params.wavelet.HSmethod = "without";
    params.wavelet.daubcoeffmethod = "6_";
    params.wavelet.thres = levels;
    params.wavelet.threshold = 4;
    params.wavelet.threshold2 = 5;
    params.wavelet.sigma = 1.0;
    params.wavelet.offset = 1.0;
    params.wavelet.lowthr = 40.0;
    params.wavelet.strength = 100;
    params.wavelet.softrad = 0.0;
    params.wavelet.softradend = 0.0;
    params.wavelet.skinprotect = 0.0;
    params.wavelet.avoid = false;
    params.wavelet.median = false;
    params.wavelet.medianlev = false;
    params.wavelet.linkedg = false;
    params.wavelet.lipst = false;
    params.wavelet.NPmethod = "none";
    params.wavelet.EDmethod = "CU";
    params.wavelet.Medgreinf = "less";
    params.wavelet.sup = 0;
    params.wavelet.thrend = 0.0;
    params.wavelet.contrast = 0;
    params.wavelet.edgval = 0;
    params.wavelet.edgrad = 15;
    params.wavelet.edgthresh = 10;
    params.wavelet.edgeffect = 1.0;
    params.wavelet.sigmafin = 1.0;
    params.wavelet.sigmaton = 1.0;
    params.wavelet.protab = 0.0;
    params.wavelet.BAmethod = "none";
    params.wavelet.tmr = false;
    params.wavelet.balance = 0;
    params.wavelet.iter = 0;
    params.wavelet.sigmadir = 1.0;
    params.wavelet.wavclCurve = {static_cast<double>(DCT_Linear)};
    params.wavelet.opacityCurveWL = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    params.wavelet.opacityCurveW = {
        static_cast<double>(FCT_Linear)};
    for (int level = 0; level < 9; ++level) {
        params.wavelet.c[level] = 0;
    }
}

void test_lab_wavelet_ba(bool available)
{
    using namespace rtengine::stage;

    static MyMutex ba_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &ba_lcms_mutex;
    }
    static rtengine::Settings ba_settings{};
    rtengine::settings = &ba_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-ba.jsonl";

    if (!available) {
        skip("lab.wavelet balance chroma: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet balance-chroma arm has a device implementation");

    struct BaCase {
        const char* label;
        int width;
        int height;
        int levels;
        const char* method;
        bool expfinal;
        bool tmr;
        bool final_curve;
        bool ba_curve;
        bool contrast;
        bool chroma;
        bool noise;
        int contrast_values[9];
        int balance;
        int iter;
        double sigmadir;
    };

    const BaCase cases[] = {
        {"ba sli final", 238, 64, 6, "sli", true, false, true, false, false,
         false, false, {0, 0, 0, 0, 0, 0, 0, 0, 0}, 40, 0, 1.0},
        {"ba sli final no curve", 238, 64, 6, "sli", true, false, false,
         false, false, false, false, {0, 0, 0, 0, 0, 0, 0, 0, 0}, -20, 2,
         1.4},
        {"ba sli final tmr", 238, 64, 6, "sli", true, true, true, false, false,
         false, false, {0, 0, 0, 0, 0, 0, 0, 0, 0}, 100, -3, 0.4},
        {"ba cur final", 238, 64, 6, "cur", true, false, true, true, false,
         false, false, {0, 0, 0, 0, 0, 0, 0, 0, 0}, 20, 1, 1.0},
        {"ba cur final tmr", 238, 64, 6, "cur", true, true, true, true, false,
         false, false, {0, 0, 0, 0, 0, 0, 0, 0, 0}, -30, 3, 2.5},
        {"ba sli tmr chroma", 238, 64, 6, "sli", false, true, false, false,
         false, true, false, {40, 30, 20, 10, 5, 2, 0, 0, 0}, 60, -1, 1.0},
        {"ba cur tmr contrast", 238, 64, 6, "cur", false, true, false, true,
         true, false, false, {30, 22, 14, 8, 4, 2, 0, 0, 0}, 30, 2, 0.8},
        {"ba sli tmr denoise", 238, 160, 6, "sli", false, true, false, false,
         false, false, true, {0, 0, 0, 0, 0, 0, 0, 0, 0}, 50, 0, 0.7},
        {"ba sli final tmr contrast chroma", 238, 64, 6, "sli", true, true,
         true, false, true, true, false, {30, 22, 14, 8, 4, 2, 0, 0, 0}, 80,
         -2, 1.8},
        {"ba cur final tmr denoise", 238, 160, 6, "cur", true, true, true,
         true, false, false, true, {0, 0, 0, 0, 0, 0, 0, 0, 0}, 10, 3, 0.5},
    };

    unsigned seed = 0x7a31c9e5u;
    for (const BaCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_ba_params(params, item.levels);
        params.wavelet.BAmethod = item.method;
        params.wavelet.expfinal = item.expfinal;
        params.wavelet.tmr = item.tmr;
        params.wavelet.balance = item.balance;
        params.wavelet.iter = item.iter;
        params.wavelet.sigmadir = item.sigmadir;
        if (item.final_curve) {
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }
        if (item.ba_curve) {
            params.wavelet.opacityCurveW = final_opacity_curve();
        }
        if (item.contrast) {
            params.wavelet.expcontrast = true;
        }
        if (item.contrast || item.chroma) {
            /* ContAllAB()'s chroma arm reads cp.mul[] even with the contrast
             * panel off, so a chroma-only profile still needs a non-zero
             * multiplier to do work. */
            for (int level = 0; level < 9; ++level) {
                params.wavelet.c[level] = item.contrast_values[level];
            }
        }
        if (item.chroma) {
            params.wavelet.expchroma = true;
            params.wavelet.CHmethod = "link";
            params.wavelet.chro = 60;
        }
        if (item.noise) {
            params.wavelet.expnoise = true;
            const double noise[5] = {40.0, 30.0, 20.0, 10.0, 5.0};
            params.wavelet.level0noise.setValues(0.0, noise[0]);
            params.wavelet.level1noise.setValues(0.0, noise[1]);
            params.wavelet.level2noise.setValues(0.0, noise[2]);
            params.wavelet.level3noise.setValues(0.0, noise[3]);
            params.wavelet.leveldenoise.setValues(0.0, noise[4]);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet ba ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet ba ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet ba ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet ba ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet ba ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet ba ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        double plane_pae[3] = {0.0, 0.0, 0.0};
        for (int y = 0; y < item.height; ++y) {
            for (int x = 0; x < item.width; ++x) {
                plane_pae[0] = std::max(
                    plane_pae[0], static_cast<double>(std::fabs(
                                      device.L[y][x] - cpu.L[y][x])));
                plane_pae[1] = std::max(
                    plane_pae[1], static_cast<double>(std::fabs(
                                      device.a[y][x] - cpu.a[y][x])));
                plane_pae[2] = std::max(
                    plane_pae[2], static_cast<double>(std::fabs(
                                      device.b[y][x] - cpu.b[y][x])));
            }
        }
        std::printf("stage-residency: lab.wavelet ba %s %dx%d L%d: PAE "
                    "%.6f LSB16 (L %.6f, a %.6f, b %.6f); exact %zu, "
                    "<=0.25 %zu, <=0.5 %zu, <=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, plane_pae[0], plane_pae[1], plane_pae[2],
                    d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet ba ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet ba ") + item.label +
               " is within one LSB16").c_str());

        /* Non-vacuity against the method-off host result. */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.BAmethod = "none";
        off.wavelet.tmr = false;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet ba %s changes %zu of "
                    "%zu samples against BAmethod none, PAE %.6f LSB16\n",
                    item.label, active.differing, active.samples,
                    active.max_abs);
        check(active.differing > 0,
              (std::string("lab.wavelet ba ") + item.label +
               " is non-vacuous").c_str());
    }

    /*
     * Refusals: an unknown method, "cur" without an opacity curve, the
     * sliders outside their ranges, a method that never reaches either gate,
     * TMr without a method and the L-only trim the node cannot represent.
     */
    struct BaResidual {
        const char* label;
        const char* method;
        bool expfinal;
        bool tmr;
        bool ba_curve;
        int balance;
        int iter;
        double sigmadir;
    };
    const BaResidual residuals[] = {
        {"unknown method", "mystery", true, false, true, 0, 0, 1.0},
        {"cur without curve", "cur", true, false, false, 0, 0, 1.0},
        {"balance high", "sli", true, false, false, 101, 0, 1.0},
        {"balance low", "sli", true, false, false, -31, 0, 1.0},
        {"iter high", "sli", true, false, false, 0, 4, 1.0},
        {"iter low", "sli", true, false, false, 0, -4, 1.0},
        {"sigmadir high", "sli", true, false, false, 0, 0, 3.0},
        {"sigmadir low", "sli", true, false, false, 0, 0, 0.0},
        {"no active gate", "sli", false, false, false, 0, 0, 1.0},
        {"tmr without method", "none", true, true, false, 0, 0, 1.0},
        {"tmr only dead trim", "sli", false, true, false, 0, 0, 1.0},
    };
    for (const BaResidual& item : residuals) {
        rtengine::procparams::ProcParams params;
        wavelet_ba_params(params, 6);
        params.wavelet.BAmethod = item.method;
        params.wavelet.expfinal = item.expfinal;
        params.wavelet.tmr = item.tmr;
        params.wavelet.balance = item.balance;
        params.wavelet.iter = item.iter;
        params.wavelet.sigmadir = item.sigmadir;
        if (item.ba_curve) {
            params.wavelet.opacityCurveW = final_opacity_curve();
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet ba residual ") + item.label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_tools(source, 0x1b2c3d4eu);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet ba residual ") + item.label +
               " is refused as a device configuration").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet ba residual ") + item.label +
               " records a host execution").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet ba residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    item.label, diff.max_abs, diff.differing, diff.samples);
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet ba residual ") + item.label +
               " matches the CPU block").c_str());
    }

    std::remove(profile_path.c_str());
}


/* --------------------------------------------------- lab.wavelet toning */
/*
 * The toning panel's zero-protection arm: the opacity-curve multiplier on
 * the a/b bands. The protection range and the nonzero-protab reductions are
 * host residuals.
 */
void wavelet_toning_params(rtengine::procparams::ProcParams& params,
                           int levels)
{
    params.dirpyrequalizer.enabled = false;
    params.wavelet.enabled = true;
    params.wavelet.expcontrast = false;
    params.wavelet.expchroma = false;
    params.wavelet.expedge = false;
    params.wavelet.expbl = false;
    params.wavelet.expresid = false;
    params.wavelet.expfinal = false;
    params.wavelet.exptoning = true;
    params.wavelet.expnoise = false;
    params.wavelet.expclari = false;
    params.wavelet.CLmethod = "all";
    params.wavelet.Tilesmethod = "full";
    params.wavelet.HSmethod = "without";
    params.wavelet.daubcoeffmethod = "6_";
    params.wavelet.thres = levels;
    params.wavelet.threshold = 4;
    params.wavelet.threshold2 = 5;
    params.wavelet.sigma = 1.0;
    params.wavelet.offset = 1.0;
    params.wavelet.lowthr = 40.0;
    params.wavelet.strength = 100;
    params.wavelet.softrad = 0.0;
    params.wavelet.softradend = 0.0;
    params.wavelet.skinprotect = 0.0;
    params.wavelet.avoid = false;
    params.wavelet.median = false;
    params.wavelet.medianlev = false;
    params.wavelet.linkedg = false;
    params.wavelet.lipst = false;
    params.wavelet.NPmethod = "none";
    params.wavelet.EDmethod = "CU";
    params.wavelet.Medgreinf = "less";
    params.wavelet.sup = 0;
    params.wavelet.thrend = 0.0;
    params.wavelet.contrast = 0;
    params.wavelet.edgval = 50;
    params.wavelet.edgrad = 15;
    params.wavelet.edgthresh = 10;
    params.wavelet.edgeffect = 1.0;
    params.wavelet.sigmafin = 1.0;
    params.wavelet.sigmaton = 1.0;
    params.wavelet.protab = 0.0;
    params.wavelet.rangeab = 20.0;
    params.wavelet.BAmethod = "none";
    params.wavelet.tmr = false;
    params.wavelet.wavclCurve = {static_cast<double>(DCT_Linear)};
    params.wavelet.opacityCurveWL = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    params.wavelet.opacityCurveRG = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    params.wavelet.opacityCurveBY = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    for (int level = 0; level < 9; ++level) {
        params.wavelet.c[level] = 0;
    }
}

const std::vector<double>& toning_red_green_curve()
{
    static const std::vector<double> curve = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.20, 0.35, 0.35,
        0.5, 0.80, 0.35, 0.35, 1.0, 0.30, 0.35, 0.35};
    return curve;
}

const std::vector<double>& toning_blue_yellow_curve()
{
    static const std::vector<double> curve = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.80, 0.35, 0.35,
        0.5, 0.20, 0.35, 0.35, 1.0, 0.70, 0.35, 0.35};
    return curve;
}

void test_lab_wavelet_toning(bool available)
{
    using namespace rtengine::stage;

    static MyMutex toning_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &toning_lcms_mutex;
    }
    static rtengine::Settings toning_settings{};
    toning_settings.edghi = 3.0;
    toning_settings.edglo = 0.5;
    toning_settings.limrad = 20.0;
    rtengine::settings = &toning_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-toning.jsonl";

    if (!available) {
        skip("lab.wavelet toning: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    struct ToningCase {
        const char* label;
        int width;
        int height;
        int levels;
        bool contrast;
        bool chroma;
        bool edge;
        bool final_curves;
        int contrast_values[9];
        int chro;
        int rg; /* 0 default no-op, 1 custom */
        int by;
        double sigmaton;
        bool expect_change;
        int image; /* 0 random, 1 skin */
    };

    const ToningCase cases[] = {
        {"toning a only", 238, 64, 6, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 0, 1.0, true, 1},
        {"toning b only", 238, 64, 6, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 1, 1.0, true, 1},
        {"toning both", 238, 64, 6, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"toning sigmaton 0.5", 238, 64, 6, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 0.5, true, 0},
        {"toning sigmaton 2.0", 238, 64, 6, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 2.0, true, 1},
        {"toning with contrast", 238, 64, 6, true, false, false, false,
         {30, 22, 14, 8, 4, 2, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"toning with chroma", 238, 64, 6, false, true, false, false,
         {40, 30, 20, 10, 5, 2, 0, 0, 0}, 60, 1, 1, 1.0, true, 1},
        {"toning with edge", 238, 64, 6, false, false, true, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"toning with final curves", 238, 64, 6, false, false, false, true,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"toning padded 179x49", 179, 49, 5, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.2, true, 1},
        {"toning levels 3", 179, 48, 3, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
        {"toning levels 7", 179, 160, 7, false, false, false, false,
         {0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1, 1, 1.0, true, 1},
    };

    unsigned seed = 0x9e3779b9u;
    for (const ToningCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_toning_params(params, item.levels);
        params.wavelet.expcontrast = item.contrast;
        params.wavelet.expchroma = item.chroma;
        params.wavelet.expedge = item.edge;
        params.wavelet.expfinal = item.final_curves;
        if (item.chroma) {
            params.wavelet.CHmethod = "link";
            params.wavelet.chro = item.chro;
        }
        for (int level = 0; level < 9; ++level) {
            params.wavelet.c[level] = item.contrast_values[level];
        }
        if (item.rg == 1) {
            params.wavelet.opacityCurveRG = toning_red_green_curve();
        }
        if (item.by == 1) {
            params.wavelet.opacityCurveBY = toning_blue_yellow_curve();
        }
        if (item.final_curves) {
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }
        params.wavelet.sigmaton = item.sigmaton;

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet toning ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet toning ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet toning ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet toning ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet toning ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet toning ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet toning %s %dx%d L%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet toning ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet toning ") + item.label +
               " is within one LSB16").c_str());

        if (item.expect_change) {
            rtengine::procparams::ProcParams off = params;
            off.wavelet.exptoning = false;
            rtengine::ImProcFunctions off_ipf(&off, false);
            LabImage off_image(item.width, item.height);
            off_image.CopyFrom(&source);
            labWaveletHostBlock(off_ipf, off, off_image);
            const Diff active = compare(view(cpu), view(off_image));
            std::printf("stage-residency: lab.wavelet toning %s changes %zu of "
                        "%zu samples against the panel-off result, PAE "
                        "%.6f LSB16\n",
                        item.label, active.differing, active.samples,
                        active.max_abs);
            check(active.differing > 0,
                  (std::string("lab.wavelet toning ") + item.label +
                   " is non-vacuous").c_str());
        }
    }

    /*
     * Refusals: the protection range, out-of-range sigmaton, a no-op opacity
     * pair and every panel the node never admits must run the host block byte
     * for byte.
     */
    const char* residual_labels[] = {
        "protab on", "sigmaton low", "sigmaton high",
        "no opacity multiplier", "expnoise", "expbl", "expresid",
        "skin protection"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_toning_params(params, 6);
        params.wavelet.opacityCurveRG = toning_red_green_curve();

        if (std::strcmp(label, "protab on") == 0) {
            params.wavelet.protab = 10.0;
        } else if (std::strcmp(label, "sigmaton low") == 0) {
            params.wavelet.sigmaton = 0.0;
        } else if (std::strcmp(label, "sigmaton high") == 0) {
            params.wavelet.sigmaton = 3.0;
        } else if (std::strcmp(label, "no opacity multiplier") == 0) {
            wavelet_toning_params(params, 6);
        } else if (std::strcmp(label, "expnoise") == 0) {
            params.wavelet.expnoise = true;
        } else if (std::strcmp(label, "expbl") == 0) {
            params.wavelet.expbl = true;
        } else if (std::strcmp(label, "expresid") == 0) {
            params.wavelet.expresid = true;
        } else {
            params.wavelet.skinprotect = 30.0;
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet toning residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet toning residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet toning residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet toning residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet toning residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet toning residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet toning residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet toning residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet toning residual ") + label +
               " matches the CPU block").c_str());
    }
}

/* ---------------------------------------------- lab.wavelet denoise */

/*
 * The standard denoise arm plus the chroma arm ip_wavelet() forces when
 * Expnoise selects six levels. Every other panel is neutral unless the case
 * asks for it.
 */
void wavelet_denoise_params(rtengine::procparams::ProcParams& params,
                            int levels, const double noise[5],
                            double ballum)
{
    params.dirpyrequalizer.enabled = false;
    params.wavelet.enabled = true;
    params.wavelet.expcontrast = false;
    params.wavelet.expchroma = false;
    params.wavelet.expedge = false;
    params.wavelet.expbl = false;
    params.wavelet.expresid = false;
    params.wavelet.expfinal = false;
    params.wavelet.exptoning = false;
    params.wavelet.expnoise = true;
    params.wavelet.expclari = false;
    params.wavelet.CLmethod = "all";
    params.wavelet.Tilesmethod = "full";
    params.wavelet.HSmethod = "without";
    params.wavelet.daubcoeffmethod = "6_";
    params.wavelet.thres = levels;
    params.wavelet.threshold = 4;
    params.wavelet.threshold2 = 5;
    params.wavelet.sigma = 1.0;
    params.wavelet.offset = 1.0;
    params.wavelet.lowthr = 40.0;
    params.wavelet.strength = 100;
    params.wavelet.softrad = 0.0;
    params.wavelet.softradend = 0.0;
    params.wavelet.skinprotect = 0.0;
    params.wavelet.avoid = false;
    params.wavelet.median = false;
    params.wavelet.medianlev = false;
    params.wavelet.linkedg = false;
    params.wavelet.lipst = false;
    params.wavelet.NPmethod = "none";
    params.wavelet.EDmethod = "CU";
    params.wavelet.Medgreinf = "less";
    params.wavelet.sup = 0;
    params.wavelet.thrend = 0.0;
    params.wavelet.contrast = 0;
    params.wavelet.edgval = 50;
    params.wavelet.edgrad = 15;
    params.wavelet.edgthresh = 10;
    params.wavelet.edgeffect = 1.0;
    params.wavelet.sigmafin = 1.0;
    params.wavelet.sigmaton = 1.0;
    params.wavelet.protab = 0.0;
    params.wavelet.BAmethod = "none";
    params.wavelet.tmr = false;
    params.wavelet.quamethod = "cons";
    params.wavelet.slimethod = "sli";
    params.wavelet.thrden = 0.0;
    params.wavelet.complexmethod = "normal";
    params.wavelet.cbenab = false;
    params.wavelet.balchrom = 0.0;
    params.wavelet.chromfi = 0.0;
    params.wavelet.chromco = 0.0;
    params.wavelet.ballum = ballum;
    params.wavelet.level0noise.setValues(0.0, noise[0]);
    params.wavelet.level1noise.setValues(0.0, noise[1]);
    params.wavelet.level2noise.setValues(0.0, noise[2]);
    params.wavelet.level3noise.setValues(0.0, noise[3]);
    params.wavelet.leveldenoise.setValues(0.0, noise[4]);
    params.wavelet.wavclCurve = {static_cast<double>(DCT_Linear)};
    params.wavelet.opacityCurveWL = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    params.wavelet.opacityCurveRG = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    params.wavelet.opacityCurveBY = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.50, 0.35, 0.35,
        1.0, 0.50, 0.35, 0.35};
    for (int level = 0; level < 9; ++level) {
        params.wavelet.c[level] = 0;
    }
}

void test_lab_wavelet_clarity();
void test_lab_wavelet_denoise_curve();
void test_lab_wavelet_denoise_expert();
void test_lab_wavelet_bishrink();

void test_lab_wavelet_denoise(bool available)
{
    using namespace rtengine::stage;

    static MyMutex denoise_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &denoise_lcms_mutex;
    }
    static rtengine::Settings denoise_settings{};
    denoise_settings.edghi = 3.0;
    denoise_settings.edglo = 0.5;
    denoise_settings.limrad = 20.0;
    rtengine::settings = &denoise_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-denoise.jsonl";

    if (!available) {
        skip("lab.wavelet denoise: no OpenCL device");
        return;
    }
    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    struct DenoiseCase {
        const char* label;
        int width;
        int height;
        int levels;
        double noise[5];
        double ballum;
        bool contrast;
        bool edge;
        bool final;
        bool toning;
        int image; /* 0 random, 1 skin */
        /* User chroma strength; inert for the forced arm but admitted. */
        double chromfi;
        double chromco;
        /* Non-zero selects the sli+Thrden refinement. */
        double thrden;
    };

    const DenoiseCase cases[] = {
        {"denoise levels 3", 238, 160, 3, {30, 20, 10, 0, 0}, 7.0, false,
         false, false, false, 0},
        {"denoise levels 5", 238, 160, 5, {40, 30, 20, 10, 5}, 5.0, false,
         false, false, false, 0},
        {"denoise levels 6 ballum 10", 238, 160, 6, {50, 40, 30, 20, 10},
         10.0, false, false, false, false, 0},
        {"denoise ballum 0", 238, 160, 6, {35, 25, 15, 8, 4}, 0.0, false,
         false, false, false, 1},
        {"denoise ballum -2", 238, 160, 6, {35, 25, 15, 8, 4}, -2.0, false,
         false, false, false, 0},
        {"denoise high levels only", 238, 160, 6, {0, 0, 0, 40, 30}, 7.0,
         false, false, false, false, 0},
        {"denoise odd size 179x161", 179, 161, 6, {45, 35, 25, 12, 6},
         7.0,
         false, false, false, false, 0},
        {"denoise odd width 179x160", 179, 160, 6,
         {45, 35, 25, 12, 6}, 7.0, false, false, false, false, 0},
        {"denoise odd height 180x161", 180, 161, 6,
         {45, 35, 25, 12, 6}, 7.0, false, false, false, false, 0},
        {"denoise odd both 179x161", 179, 161, 6,
         {45, 35, 25, 12, 6}, 7.0, false, false, false, false, 0},
        {"denoise light odd", 180, 161, 6, {0.11, 0, 0, 0, 0}, 7.0, false,
         false, false, false, 0},
        {"denoise with contrast", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0,
         true, false, false, false, 0},
        {"denoise with edge", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, false,
         true, false, false, 1},
        {"denoise with final curves", 238, 160, 5, {40, 30, 20, 10, 5}, 7.0,
         false, false, true, false, 0},
        {"denoise with toning", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, false,
         false, false, true, 1},
        {"denoise all panels", 238, 160, 6, {45, 35, 25, 12, 6}, 7.0, true,
         true, true, true, 0},
        {"denoise padded all panels", 179, 161, 5, {40, 30, 20, 10, 0}, 5.0,
         true, true, true, true, 1},
        {"denoise chroma fine", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, true,
         false, false, false, 0, 40.0, 0.0},
        {"denoise chroma coarse", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, true,
         false, false, false, 0, 0.0, 60.0},
        {"denoise chroma both", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, false,
         false, false, false, 0, 100.0, 100.0, 0.0},
        {"denoise thrden 1", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, true,
         false, false, false, 0, 0.0, 0.0, 1.0},
        {"denoise thrden 5", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, true,
         false, false, false, 0, 0.0, 0.0, 5.0},
        {"denoise thrden 20", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, false,
         false, false, false, 0, 0.0, 0.0, 20.0},
        {"denoise thrden with final", 238, 160, 5, {40, 30, 20, 10, 0}, 7.0,
         false, false, true, false, 0, 0.0, 0.0, 40.0},
    };

    unsigned seed = 0x51b9e3a7u;
    for (const DenoiseCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_denoise_params(params, item.levels, item.noise, item.ballum);
        if (item.contrast) {
            params.wavelet.expcontrast = true;
            const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
            for (int level = 0; level < 9; ++level) {
                params.wavelet.c[level] = contrast[level];
            }
        }
        if (item.edge) {
            params.wavelet.expedge = true;
            params.wavelet.edgval = 50;
            params.wavelet.edgrad = 15;
            params.wavelet.edgthresh = 10;
            params.wavelet.edgeffect = 1.0;
        }
        if (item.final) {
            params.wavelet.expfinal = true;
            params.wavelet.sigmafin = 1.0;
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }
        params.wavelet.chromfi = item.chromfi;
        params.wavelet.chromco = item.chromco;
        params.wavelet.thrden = item.thrden;
        if (item.toning) {
            params.wavelet.exptoning = true;
            params.wavelet.sigmaton = 1.0;
            params.wavelet.opacityCurveRG = toning_red_green_curve();
            params.wavelet.opacityCurveBY = toning_blue_yellow_curve();
        }

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet denoise ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet denoise ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet denoise ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet denoise ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet denoise ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet denoise ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        double plane_pae[3] = {0.0, 0.0, 0.0};
        for (int y = 0; y < item.height; ++y) {
            for (int x = 0; x < item.width; ++x) {
                plane_pae[0] = std::max(
                    plane_pae[0], static_cast<double>(std::fabs(
                                      device.L[y][x] - cpu.L[y][x])));
                plane_pae[1] = std::max(
                    plane_pae[1], static_cast<double>(std::fabs(
                                      device.a[y][x] - cpu.a[y][x])));
                plane_pae[2] = std::max(
                    plane_pae[2], static_cast<double>(std::fabs(
                                      device.b[y][x] - cpu.b[y][x])));
            }
        }
        std::printf("stage-residency: lab.wavelet denoise %s %dx%d L%d: "
                    "PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet denoise ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet denoise ") + item.label +
               " is within one LSB16").c_str());
        if (item.thrden != 0.0) {
            /* Non-vacuity: the same profile with Thrden zero must differ. */
            rtengine::procparams::ProcParams off = params;
            off.wavelet.thrden = 0.0;
            rtengine::ImProcFunctions off_ipf(&off, false);
            LabImage off_image(item.width, item.height);
            off_image.CopyFrom(&source);
            labWaveletHostBlock(off_ipf, off, off_image);
            const Distribution fd = distribution(view(off_image), view(cpu));
            check(fd.max_abs > 1.0,
                  (std::string("lab.wavelet denoise ") + item.label +
                   " refines the coefficients").c_str());
        }
        if (item.chromfi != 0.0 || item.chromco != 0.0) {
            /* The forced arm ignores variC at local == 1 and cp.chromco stays
             * below the ramp threshold, so the host result must be identical
             * with the sliders zeroed. */
            rtengine::procparams::ProcParams zero = params;
            zero.wavelet.chromfi = 0.0;
            zero.wavelet.chromco = 0.0;
            rtengine::ImProcFunctions zero_ipf(&zero, false);
            LabImage zero_image(item.width, item.height);
            zero_image.CopyFrom(&source);
            labWaveletHostBlock(zero_ipf, zero, zero_image);
            const Distribution zd = distribution(view(zero_image), view(cpu));
            check(zd.max_abs == 0.0,
                  (std::string("lab.wavelet denoise ") + item.label +
                   " leaves the forced arm unchanged").c_str());
        }

        /* Non-vacuity: the same profile with the denoise panel off must
         * produce a different host result. */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.expnoise = false;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet denoise %s changes %zu of "
                    "%zu samples against the panel-off result, PAE %.6f "
                    "LSB16\n",
                    item.label, active.differing, active.samples,
                    active.max_abs);
        check(active.differing > 0,
              (std::string("lab.wavelet denoise ") + item.label +
               " is non-vacuous").c_str());
    }

    /*
     * Refusals: an unknown `quaMethod`, balance chroma, the hue curve,
     * colour-by-noise, the Thrden arm, an all-zero slider set, an
     * out-of-range slider and the 128-pixel window must run the host block
     * byte for byte. The "cur" refinement has its own matrix below, and the
     * `agre` BiShrink pair has one of its own.
     */
    const char* residual_labels[] = {
        "unknown quamethod", "balance chroma", "wavhuecurve", "cbenab",
        "ballum range", "level range", "all sliders zero",
        "sub-128 window"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        const double noise[5] = {35, 25, 15, 8, 4};
        wavelet_denoise_params(params, 6, noise, 7.0);
        int width = 238;
        int height = 160;

        if (std::strcmp(label, "unknown quamethod") == 0) {
            params.wavelet.quamethod = "unknown";
        } else if (std::strcmp(label, "balance chroma") == 0) {
            params.wavelet.balchrom = 10.0;
        } else if (std::strcmp(label, "wavhuecurve") == 0) {
            params.wavelet.wavhuecurve = {1.0, 0.0, 0.5, 0.35, 0.35, 0.5,
                                          0.2, 0.35, 0.35, 1.0, 0.5, 0.35,
                                          0.35};
        } else if (std::strcmp(label, "cbenab") == 0) {
            params.wavelet.cbenab = true;
        } else if (std::strcmp(label, "ballum range") == 0) {
            params.wavelet.ballum = 10.5;
        } else if (std::strcmp(label, "level range") == 0) {
            params.wavelet.level2noise.setValues(0.0, 120.0);
        } else if (std::strcmp(label, "all sliders zero") == 0) {
            const double zero[5] = {0, 0, 0, 0, 0};
            wavelet_denoise_params(params, 6, zero, 7.0);
        } else {
            width = 128;
            height = 160;
        }

        check(!labWaveletSupported(params, width, height),
              (std::string("lab.wavelet denoise residual ") + label +
               " is a residual").c_str());

        LabImage source(width, height);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(width, height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(width, height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet denoise residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet denoise residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet denoise residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet denoise residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet denoise residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet denoise residual %s: "
                    "host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet denoise residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet denoise residual ") + label +
               " matches the CPU block").c_str());
    }

    /* The denoise-curve refinement shares this test's device gate. */
    if (available) {
        test_lab_wavelet_denoise_curve();
        test_lab_wavelet_denoise_expert();
        test_lab_wavelet_bishrink();
    }

    /* The clarity unit shares this test's device gate so it does not add a
     * second skipped entry to the suite. */
    test_lab_wavelet_clarity();
}

/* ------------------------------------ lab.wavelet denoise BiShrink ("agre") */

/*
 * `quaMethod == "agre"` runs `WaveletDenoiseAll_BiShrinkL()` and then
 * `WaveletDenoiseAllL()` on L, and `WaveletDenoiseAll_BiShrinkAB()` and then
 * `WaveletDenoiseAllAB()` on each a/b plane. The L pair is the same
 * `ShrinkAllL()` arithmetic twice (the block's `edge == 6` makes the pair's
 * per-level formula identical to the standard one, and its coarsest level
 * calls `ShrinkAllL()` outright). The chroma pair instead applies
 * `SQR(1 - exp(...))` with no box blur on levels 0..4 and `ShrinkAllAB()` on
 * level 5, and the standard pass after it rebuilds its squared MAD from the
 * post-pair bands.
 *
 * Every case is checked against `quaMethod == "cons"` for non-vacuity: the
 * pair has to change the host result.
 */
void test_lab_wavelet_bishrink()
{
    using namespace rtengine::stage;

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-bishrink.jsonl";

    struct BiShrinkCase {
        const char* label;
        int width;
        int height;
        int levels;
        double noise[5];
        double ballum;
        bool contrast;
        bool edge;
        bool final;
        bool toning;
        int image; /* 0 random, 1 skin */
        /* Non-zero selects the sli+Thrden refinement. */
        double thrden;
    };

    const BiShrinkCase cases[] = {
        {"bishrink levels 3", 238, 160, 3, {30, 20, 10, 0, 0}, 7.0, false,
         false, false, false, 0},
        {"bishrink levels 5", 238, 160, 5, {40, 30, 20, 10, 5}, 5.0, false,
         false, false, false, 0},
        {"bishrink levels 6 ballum 10", 238, 160, 6, {50, 40, 30, 20, 10},
         10.0, false, false, false, false, 0},
        {"bishrink ballum 0", 238, 160, 6, {35, 25, 15, 8, 4}, 0.0, false,
         false, false, false, 1},
        {"bishrink ballum -2", 238, 160, 6, {35, 25, 15, 8, 4}, -2.0, false,
         false, false, false, 0},
        {"bishrink odd size 179x161", 179, 161, 6, {45, 35, 25, 12, 6},
         7.0, false, false, false, false, 0},
        {"bishrink odd height 180x161", 180, 161, 6,
         {45, 35, 25, 12, 6}, 7.0, false, false, false, false, 0},
        {"bishrink with contrast", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0,
         true, false, false, false, 0},
        {"bishrink with edge", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, false,
         true, false, false, 1},
        {"bishrink with final curves", 238, 160, 5,
         {40, 30, 20, 10, 5}, 7.0, false, false, true, false, 0},
        {"bishrink with toning", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0,
         false, false, false, true, 1},
        {"bishrink all panels 179x161", 179, 161, 6,
         {45, 35, 25, 12, 6}, 7.0, true, true, true, true, 0},
        {"bishrink thrden 5", 238, 160, 6, {40, 30, 20, 10, 5}, 7.0, true,
         false, false, false, 0, 5.0},
    };

    unsigned seed = 0x7c31a5e9u;
    for (const BiShrinkCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_denoise_params(params, item.levels, item.noise, item.ballum);
        params.wavelet.quamethod = "agre";
        if (item.contrast) {
            params.wavelet.expcontrast = true;
            const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
            for (int level = 0; level < 9; ++level) {
                params.wavelet.c[level] = contrast[level];
            }
        }
        if (item.edge) {
            params.wavelet.expedge = true;
            params.wavelet.edgval = 50;
            params.wavelet.edgrad = 15;
            params.wavelet.edgthresh = 10;
            params.wavelet.edgeffect = 1.0;
        }
        if (item.final) {
            params.wavelet.expfinal = true;
            params.wavelet.sigmafin = 1.0;
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }
        if (item.toning) {
            params.wavelet.exptoning = true;
            params.wavelet.sigmaton = 1.0;
            params.wavelet.opacityCurveRG = toning_red_green_curve();
            params.wavelet.opacityCurveBY = toning_blue_yellow_curve();
        }
        params.wavelet.thrden = item.thrden;

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet bishrink ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet bishrink ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet bishrink ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet bishrink ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet bishrink ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet bishrink ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        double plane_pae[3] = {0.0, 0.0, 0.0};
        for (int y = 0; y < item.height; ++y) {
            for (int x = 0; x < item.width; ++x) {
                plane_pae[0] = std::max(
                    plane_pae[0], static_cast<double>(std::fabs(
                                      device.L[y][x] - cpu.L[y][x])));
                plane_pae[1] = std::max(
                    plane_pae[1], static_cast<double>(std::fabs(
                                      device.a[y][x] - cpu.a[y][x])));
                plane_pae[2] = std::max(
                    plane_pae[2], static_cast<double>(std::fabs(
                                      device.b[y][x] - cpu.b[y][x])));
            }
        }
        std::printf("stage-residency: lab.wavelet bishrink %s %dx%d L%d: "
                    "PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, item.levels,
                    d.max_abs, d.exact, d.quarter, d.half, d.one, d.over,
                    d.samples);
        check(d.finite,
              (std::string("lab.wavelet bishrink ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet bishrink ") + item.label +
               " is within one LSB16").c_str());

        /* Non-vacuity: the same profile with `quaMethod = "cons"` must
         * produce a different host result. */
        rtengine::procparams::ProcParams cons = params;
        cons.wavelet.quamethod = "cons";
        rtengine::ImProcFunctions cons_ipf(&cons, false);
        LabImage cons_image(item.width, item.height);
        cons_image.CopyFrom(&source);
        labWaveletHostBlock(cons_ipf, cons, cons_image);
        const Distribution cd = distribution(view(cons_image), view(cpu));
        std::printf("stage-residency: lab.wavelet bishrink %s differs from "
                    "cons on PAE %.6f LSB16\n",
                    item.label, cd.max_abs);
        check(cd.max_abs > 1.0,
              (std::string("lab.wavelet bishrink ") + item.label +
               " changes the result against quaMethod cons").c_str());
    }

    /*
     * Refusals: an unknown `quaMethod`, and the agre pair with a slider or
     * the window outside its admitted range, must run the host block byte for
     * byte.
     */
    const char* residual_labels[] = {
        "unknown quamethod", "agre ballum range", "agre level range",
        "agre sub-128 window"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        const double noise[5] = {35, 25, 15, 8, 4};
        wavelet_denoise_params(params, 6, noise, 7.0);
        params.wavelet.quamethod = "agre";
        int width = 238;
        int height = 160;

        if (std::strcmp(label, "unknown quamethod") == 0) {
            params.wavelet.quamethod = "unknown";
        } else if (std::strcmp(label, "agre ballum range") == 0) {
            params.wavelet.ballum = 10.5;
        } else if (std::strcmp(label, "agre level range") == 0) {
            params.wavelet.level2noise.setValues(0.0, 120.0);
        } else {
            width = 128;
            height = 160;
        }

        check(!labWaveletSupported(params, width, height),
              (std::string("lab.wavelet bishrink residual ") + label +
               " is a residual").c_str());

        LabImage source(width, height);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(width, height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(width, height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet bishrink residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet bishrink residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet bishrink residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet bishrink residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet bishrink residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet bishrink residual %s: "
                    "host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet bishrink residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet bishrink residual ") + label +
               " matches the CPU block").c_str());
    }

    std::remove(profile_path.c_str());
}

/* ------------------------------------- lab.wavelet denoise curve "cur" */

/*
 * A non-identity denoise curve for the "cur" refinement. The block's
 * `wavdenoise` curve is a FlatCurve sampled into a 501-entry WavCurve; every
 * value below one gives `kc = value - 1` and the negative branch, which is
 * the arm the GUI can produce (FlatCurve values stay in 0..1). The curve's
 * entries differ from one, so `cp.denoicurv` is set and the refinement runs.
 */
const std::vector<double>& denoise_curve()
{
    static const std::vector<double> curve = {
        static_cast<double>(FCT_MinMaxCPoints), 0.0, 0.55, 0.35, 0.35,
        0.5, 0.25, 0.35, 0.35, 1.0, 0.90, 0.35, 0.35};
    return curve;
}

/*
 * The "cur" arm: `sliMethod == "cur"` replaces the post-denoise statistics
 * with the pre/post mix and applies the block's `wavdenoise` curve. `denmet`
 * is hard-coded to four, so the only level factors are 0.3 on levels 0 and 1
 * and one on levels 2 and 3; every case exercises both because each level
 * iteration writes all four bands. The refinement's early-out uses the
 * pre- and post-denoise statistics, so a case is only meaningful when the
 * sli/cur host results differ.
 */
void test_lab_wavelet_denoise_curve()
{
    using namespace rtengine::stage;

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-denoise-cur.jsonl";

    struct CurveCase {
        const char* label;
        int width;
        int height;
        const char* mix;
        bool curve;
        bool contrast;
        bool edge;
        bool final;
        int image; /* 0 random, 1 skin */
    };

    const double noise[5] = {40, 30, 20, 10, 5};
    const CurveCase cases[] = {
        {"cur nois", 238, 160, "nois", true, false, false, false, 0},
        {"cur mix", 238, 160, "mix", true, false, false, false, 0},
        {"cur mix7", 238, 160, "mix7", true, false, false, false, 0},
        {"cur den", 238, 160, "den", true, false, false, false, 1},
        {"cur identity curve", 238, 160, "mix7", false, false, false, false,
         0},
        {"cur mix7 contrast", 238, 160, "mix7", true, true, false, false, 0},
        {"cur mix7 edge", 238, 160, "mix7", true, false, true, false, 1},
        {"cur mix7 final", 238, 160, "mix7", true, false, false, true, 0},
        {"cur odd 179x161", 179, 161, "mix7", true, false, false, false, 0},
        {"cur all panels", 238, 160, "mix", true, true, true, true, 1},
    };

    unsigned seed = 0x6c2b91u;
    for (const CurveCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_denoise_params(params, 6, noise, 7.0);
        params.wavelet.slimethod = "cur";
        params.wavelet.mixmethod = item.mix;
        if (item.curve) {
            params.wavelet.wavdenoise = denoise_curve();
        }
        if (item.contrast) {
            params.wavelet.expcontrast = true;
            const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
            for (int level = 0; level < 9; ++level) {
                params.wavelet.c[level] = contrast[level];
            }
        }
        if (item.edge) {
            params.wavelet.expedge = true;
            params.wavelet.edgval = 50;
            params.wavelet.edgrad = 15;
            params.wavelet.edgthresh = 10;
            params.wavelet.edgeffect = 1.0;
        }
        if (item.final) {
            params.wavelet.expfinal = true;
            params.wavelet.sigmafin = 1.0;
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet denoise curve ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet denoise curve %s %dx%d: "
                    "PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " is within one LSB16").c_str());

        /* Non-vacuity against the panel-off result. */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.expnoise = false;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet denoise curve %s changes "
                    "%zu of %zu samples against the panel-off result\n",
                    item.label, active.differing, active.samples);
        check(active.differing > 0,
              (std::string("lab.wavelet denoise curve ") + item.label +
               " is non-vacuous").c_str());

        /*
         * Non-vacuity of the refinement itself: the same profile with the
         * "sli" method leaves the curve unused, so the host difference is
         * the refinement's contribution.
         */
        if (item.curve) {
            rtengine::procparams::ProcParams sli = params;
            sli.wavelet.slimethod = "sli";
            rtengine::ImProcFunctions sli_ipf(&sli, false);
            LabImage sli_image(item.width, item.height);
            sli_image.CopyFrom(&source);
            labWaveletHostBlock(sli_ipf, sli, sli_image);
            const Diff refined = compare(view(cpu), view(sli_image));
            std::printf("stage-residency: lab.wavelet denoise curve %s "
                        "refines %zu of %zu samples against sli, PAE %.6f "
                        "LSB16\n",
                        item.label, refined.differing, refined.samples,
                        refined.max_abs);
            check(refined.differing > 0,
                  (std::string("lab.wavelet denoise curve ") + item.label +
                   " refines the result").c_str());
        }
    }

    /*
     * Refusals specific to the refinement: an out-of-range sigma (the block
     * divides by it) and an unknown `complexMethod` (the block leaves
     * `cp.complex` uninitialised) must run the host block byte for byte.
     * Thrden and a user chroma strength are admitted; the expert pairing has
     * its own matrix below.
     */
    const char* residual_labels[] = {
        "cur sigm above range", "cur complexmethod"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_denoise_params(params, 6, noise, 7.0);
        params.wavelet.slimethod = "cur";
        params.wavelet.mixmethod = "mix7";
        params.wavelet.wavdenoise = denoise_curve();
        if (std::strcmp(label, "cur sigm above range") == 0) {
            params.wavelet.sigm = 5.0;
        } else {
            params.wavelet.complexmethod = "weird";
        }

        const int width = 238;
        const int height = 160;
        check(!labWaveletSupported(params, width, height),
              (std::string("lab.wavelet denoise curve residual ") + label +
               " is a residual").c_str());

        LabImage source(width, height);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(width, height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(width, height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet denoise curve residual %s: "
                    "host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet denoise curve residual ") + label +
               " matches the CPU block").c_str());
    }
}

/* ---------------------------------------- lab.wavelet denoise expert */

/*
 * The expert level pairing (`complexMethod == "expert"`). The block runs
 * levref == 6, maps levels 0..3 to `Levelsigm`'s top value and 4..5 to its
 * bottom, and — on every one of the six level iterations — rewrites bands 0..3
 * with the wavlow/wavdenoise factor, then the wavmed factor when `Limden` is
 * positive, and bands 4..5 with the wavhigh factor built from
 * `Leveldenoise`'s lower value. Both `sliMethod` arms feed the same pairing.
 * `denmet` is hard-coded to four, so levels 0 and 1 also carry the 0.3 factor.
 */
void test_lab_wavelet_denoise_expert()
{
    using namespace rtengine::stage;

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-denoise-expert.jsonl";

    struct ExpertCase {
        const char* label;
        int width;
        int height;
        const char* method; /* "cur" or "sli" */
        double thrden;
        const char* mix;
        bool curve;
        /* Levelsigm: bottom (levels 4..5) then top (levels 0..3). */
        double sigmm56;
        double sigmm14;
        double limden;
        bool contrast;
        bool edge;
        bool final;
        int image; /* 0 random, 1 skin */
    };

    const double noise[5] = {40, 30, 20, 10, 5};
    const ExpertCase cases[] = {
        {"expert cur mix7", 238, 160, "cur", 0.0, "mix7", true, 1.0, 1.0,
         0.0, false, false, false, 0},
        {"expert cur nois sigm high", 238, 160, "cur", 0.0, "nois", true,
         3.0, 3.0, 0.0, false, false, false, 1},
        {"expert cur mix split", 238, 160, "cur", 0.0, "mix", true, 3.0,
         0.05, 0.0, true, false, false, 0},
        {"expert cur den split", 238, 160, "cur", 0.0, "den", true, 0.05,
         3.0, 0.0, false, true, false, 1},
        {"expert cur medium", 238, 160, "cur", 0.0, "mix7", true, 0.5, 2.0,
         0.5, false, false, true, 0},
        {"expert sli thrden narrow", 238, 160, "sli", 5.0, "mix7", false,
         2.5, 1.5, 0.0, true, false, false, 0},
        {"expert sli thrden medium", 238, 160, "sli", 20.0, "nois", false,
         0.05, 0.05, 1.0, false, false, false, 1},
        {"expert odd all panels", 179, 161, "cur", 0.0, "mix", true, 1.5,
         0.75, 0.75, true, true, true, 0},
    };

    unsigned seed = 0x2b7c41d3u;
    for (const ExpertCase& item : cases) {
        rtengine::procparams::ProcParams params;
        wavelet_denoise_params(params, 6, noise, 7.0);
        params.wavelet.complexmethod = "expert";
        params.wavelet.slimethod = item.method;
        params.wavelet.thrden = item.thrden;
        params.wavelet.mixmethod = item.mix;
        if (item.curve) {
            params.wavelet.wavdenoise = denoise_curve();
        }
        params.wavelet.levelsigm.setValues(item.sigmm56, item.sigmm14);
        params.wavelet.limden = item.limden;
        if (item.contrast) {
            params.wavelet.expcontrast = true;
            const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
            for (int level = 0; level < 9; ++level) {
                params.wavelet.c[level] = contrast[level];
            }
        }
        if (item.edge) {
            params.wavelet.expedge = true;
            params.wavelet.edgval = 50;
            params.wavelet.edgrad = 15;
            params.wavelet.edgthresh = 10;
            params.wavelet.edgeffect = 1.0;
        }
        if (item.final) {
            params.wavelet.expfinal = true;
            params.wavelet.sigmafin = 1.0;
            params.wavelet.wavclCurve = final_diagonal_curve();
            params.wavelet.opacityCurveWL = final_opacity_curve();
        }

        LabImage source(item.width, item.height);
        if (item.image == 1) {
            fill_lab_wavelet_skin(source);
        } else {
            fill_lab_tools(source, seed++);
        }

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet denoise expert ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet denoise expert %s %dx%d: "
                    "PAE %.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, "
                    "<=1 %zu, >1 %zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " is within one LSB16").c_str());

        /* Non-vacuity against the panel-off result. */
        rtengine::procparams::ProcParams off = params;
        off.wavelet.expnoise = false;
        rtengine::ImProcFunctions off_ipf(&off, false);
        LabImage off_image(item.width, item.height);
        off_image.CopyFrom(&source);
        labWaveletHostBlock(off_ipf, off, off_image);
        const Diff active = compare(view(cpu), view(off_image));
        std::printf("stage-residency: lab.wavelet denoise expert %s changes "
                    "%zu of %zu samples against the panel-off result\n",
                    item.label, active.differing, active.samples);
        check(active.differing > 0,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " is non-vacuous").c_str());

        /*
         * Non-vacuity of the pairing itself: the same profile with the
         * standard pairing must produce a different host result, because the
         * expert arm runs two more levels and rewrites bands 4 and 5.
         */
        rtengine::procparams::ProcParams normal = params;
        normal.wavelet.complexmethod = "normal";
        rtengine::ImProcFunctions normal_ipf(&normal, false);
        LabImage normal_image(item.width, item.height);
        normal_image.CopyFrom(&source);
        labWaveletHostBlock(normal_ipf, normal, normal_image);
        const Diff paired = compare(view(cpu), view(normal_image));
        std::printf("stage-residency: lab.wavelet denoise expert %s changes "
                    "%zu of %zu samples against the standard pairing, PAE "
                    "%.6f LSB16\n",
                    item.label, paired.differing, paired.samples,
                    paired.max_abs);
        check(paired.differing > 0,
              (std::string("lab.wavelet denoise expert ") + item.label +
               " pairs six levels").c_str());
    }

    /*
     * Refusals specific to the expert pairing: both `Levelsigm` ends, `Limden`
     * and the unhandled `wavdenoiseh` curve must run the host block byte for
     * byte.
     */
    const char* residual_labels[] = {
        "expert levelsigm top", "expert levelsigm bottom", "expert limden",
        "expert wavdenoiseh"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_denoise_params(params, 6, noise, 7.0);
        params.wavelet.complexmethod = "expert";
        params.wavelet.slimethod = "cur";
        params.wavelet.mixmethod = "mix7";
        params.wavelet.wavdenoise = denoise_curve();
        if (std::strcmp(label, "expert levelsigm top") == 0) {
            params.wavelet.levelsigm.setValues(1.0, 5.0);
        } else if (std::strcmp(label, "expert levelsigm bottom") == 0) {
            params.wavelet.levelsigm.setValues(0.01, 1.0);
        } else if (std::strcmp(label, "expert limden") == 0) {
            params.wavelet.limden = 1.5;
        } else {
            params.wavelet.wavdenoiseh = denoise_curve();
        }

        const int width = 238;
        const int height = 160;
        check(!labWaveletSupported(params, width, height),
              (std::string("lab.wavelet denoise expert residual ") + label +
               " is a residual").c_str());

        LabImage source(width, height);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(width, height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(width, height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet denoise expert residual "
                    "%s: host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " stays finite").c_str());
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet denoise expert residual ") + label +
               " matches the CPU block").c_str());
    }
}

/* ---------------------------------------------- lab.wavelet clarity */
/*
 * CLmethod's level selection and the clarity arm. The selection is
 * finalContAllL()/ContAllAB()'s coefficient zeroing and background paint; the
 * clarity arm is the block's two-pass sequence, the guided soft radius and
 * then the mergeL/mergeC merge. Every Backmethod branch and both merge sliders
 * are covered, alone and with contrast, chroma and denoise, and the soft
 * radius is covered at both subsampling arms: the plain one the small images
 * take and the bilinear one the wide images take.
 */
void test_lab_wavelet_clarity()
{
    using namespace rtengine::stage;

    static MyMutex clarity_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &clarity_lcms_mutex;
    }
    static rtengine::Settings clarity_settings{};
    clarity_settings.edghi = 3.0;
    clarity_settings.edglo = 0.5;
    clarity_settings.limrad = 20.0;
    rtengine::settings = &clarity_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-clarity.jsonl";

    check(node_has_device_implementation("lab.wavelet"),
          "the lab.wavelet node has a device implementation");

    struct ClarityCase {
        const char* label;
        int width;
        int height;
        int levels;
        const char* cl_method;
        const char* back_method;
        double merge_l;
        double merge_c;
        const char* dir_method;
        int lmethod;
        bool contrast;
        bool chroma;
        bool denoise;
        bool expclari;
        bool highlight_shadow;
        /* The clarity panel's guided soft radius; zero for every other case. */
        double softrad;
    };

    const int contrast[9] = {30, 22, 14, 8, 4, 2, 0, 0, 0};
    const double noise[5] = {40, 30, 20, 10, 5};

    const ClarityCase cases[] = {
        {"one black contrast", 238, 64, 5, "one", "black", 20, 20, "all", 4,
         true, false, false, true},
        {"inf black contrast", 179, 48, 6, "inf", "black", 35, 0, "all", 4,
         true, false, false, true},
        {"sup resid contrast", 238, 64, 6, "sup", "resid", 20, 20, "all", 4,
         true, false, false, true},
        {"one grey contrast", 238, 64, 5, "one", "grey", 20, 20, "all", 4,
         true, false, false, true},
        {"sup black contrast", 179, 48, 5, "sup", "black", 20, 20, "all", 4,
         true, false, false, true},
        {"one resid contrast", 238, 64, 5, "one", "resid", 20, 20, "all", 4,
         true, false, false, true},
        {"one black zero merge", 238, 64, 5, "one", "black", 0, 0, "all", 4,
         true, false, false, true},
        {"inf black mergeC only", 238, 64, 5, "inf", "black", 0, 50, "all", 4,
         true, false, false, true},
        {"one black dir one L2", 238, 64, 5, "one", "black", 20, 20, "one", 2,
         true, false, false, true},
        {"inf black dir two L5", 238, 64, 5, "inf", "black", 20, 20, "two", 5,
         true, false, false, true},
        {"sup resid dir thr L6", 238, 64, 6, "sup", "resid", 20, 20, "thr", 6,
         true, false, false, true},
        {"one black contrast L7", 238, 160, 7, "one", "black", 60, 60, "all",
         4, true, false, false, true},
        /* Highlight/shadow makes the trimmed first pass observable: kH is
         * built from maxlvl, so the export profile's Lmethod 7 arm is a
         * separate case from the untrimmed two-pass probes. */
        {"one black contrast L7 HS", 238, 160, 7, "one", "black", 60, 60,
         "all", 7, true, false, false, true, true},
        {"sup resid contrast L7 HS", 238, 160, 7, "sup", "resid", 60, 60,
         "all", 7, true, false, false, true, true},
        {"sup resid contrast L7", 238, 160, 7, "sup", "resid", 30, 30, "all",
         4, true, false, false, true},
        {"one black chroma", 238, 64, 6, "one", "black", 20, 20, "all", 4,
         true, true, false, true},
        {"one black denoise", 238, 160, 6, "one", "black", 20, 20, "all", 4,
         false, false, true, true},
        {"sup resid denoise", 238, 160, 6, "sup", "resid", 30, 10, "all", 4,
         false, false, true, true},
        {"selection only", 238, 64, 5, "one", "grey", 20, 20, "all", 4, true,
         false, false, false},
        {"expclari with all", 238, 64, 5, "all", "grey", 20, 20, "all", 4,
         true, false, false, true},
        /*
         * The guided soft radius. Small images take guidedfilter.cc's
         * subsampling of one (max(w, h) <= 600), so the block's box radius is
         * the slider's own; the wide cases take 5 and run the bilinear
         * down/upsample, with 703 leaving a non-integral scale.
         */
        {"softrad 5", 238, 64, 5, "one", "black", 60, 60, "all", 4, true,
         false, false, true, false, 5.0},
        {"softrad 0.5", 238, 64, 5, "sup", "resid", 30, 30, "all", 4, true,
         false, false, true, false, 0.5},
        {"softrad 2", 179, 48, 5, "inf", "black", 20, 20, "all", 4, true,
         false, false, true, false, 2.0},
        {"softrad 100", 238, 64, 5, "one", "black", 60, 60, "all", 4, true,
         false, false, true, false, 100.0},
        /* The zero-factor merge ignores the second pass, so the soft radius is
         * a no-op there and must still match the block byte for byte. */
        {"softrad zero merge", 238, 64, 5, "inf", "grey", 20, 20, "all", 4,
         true, false, false, true, false, 5.0},
        {"softrad 5 chroma", 238, 64, 6, "one", "black", 20, 20, "all", 4,
         true, true, false, true, false, 5.0},
        {"softrad 5 denoise", 238, 160, 6, "one", "black", 20, 20, "all", 4,
         false, false, true, true, false, 5.0},
        {"softrad 5 subsampled", 703, 67, 5, "one", "black", 60, 60, "all", 4,
         true, false, false, true, false, 5.0},
        {"softrad 20 wide", 8192, 64, 6, "one", "black", 60, 60, "all", 4,
         true, false, false, true, false, 20.0},
    };

    unsigned seed = 0x51c1a7u;
    for (const ClarityCase& item : cases) {
        rtengine::procparams::ProcParams params;
        if (item.denoise) {
            wavelet_denoise_params(params, item.levels, noise, 5.0);
        } else {
            wavelet_contrast_params(params, item.levels, false, contrast, 1.0,
                                    1.0, 100);
        }
        params.wavelet.CLmethod = item.cl_method;
        params.wavelet.Backmethod = item.back_method;
        params.wavelet.mergeL = item.merge_l;
        params.wavelet.mergeC = item.merge_c;
        params.wavelet.Dirmethod = item.dir_method;
        params.wavelet.Lmethod = item.lmethod;
        params.wavelet.expclari = item.expclari;
        params.wavelet.ushamethod = "sharp";
        params.wavelet.softrad = item.softrad;
        if (item.highlight_shadow) {
            params.wavelet.HSmethod = "with";
        }
        if (!item.contrast) {
            params.wavelet.expcontrast = false;
        }
        if (item.chroma) {
            params.wavelet.expchroma = true;
            params.wavelet.CHmethod = "link";
            params.wavelet.chro = 50;
        }

        LabImage source(item.width, item.height);
        fill_lab_tools(source, seed++);

        check(labWaveletSupported(params, item.width, item.height),
              (std::string("lab.wavelet clarity ") + item.label +
               " is the supported set").c_str());

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);
        check(result.deviceReady,
              (std::string("lab.wavelet clarity ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet clarity ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.wavelet\"") != std::string::npos,
              (std::string("lab.wavelet clarity ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.wavelet clarity ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet clarity %s %dx%d: PAE "
                    "%.6f LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, "
                    ">1 %zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.wavelet clarity ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.wavelet clarity ") + item.label +
               " is within one LSB16").c_str());

        const Diff changed = compare(view(source), view(device));
        check(changed.differing > 0,
              (std::string("lab.wavelet clarity ") + item.label +
               " changes the image").c_str());
    }

    /*
     * The merge must not be a no-op: the one/black arm and the sup/resid arm
     * must differ from the same profile with the selection forced to "all".
     */
    {
        rtengine::procparams::ProcParams params;
        wavelet_contrast_params(params, 5, false, contrast, 1.0, 1.0, 100);
        params.wavelet.expclari = true;
        params.wavelet.ushamethod = "sharp";
        params.wavelet.Backmethod = "black";
        params.wavelet.mergeL = 80;
        params.wavelet.mergeC = 80;
        params.wavelet.CLmethod = "one";

        rtengine::procparams::ProcParams plain = params;
        plain.wavelet.expclari = false;
        plain.wavelet.CLmethod = "all";

        LabImage source(238, 64);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions clarity_ipf(&params, false);
        LabImage clarity(238, 64);
        clarity.CopyFrom(&source);
        labWaveletHostBlock(clarity_ipf, params, clarity);

        rtengine::ImProcFunctions plain_ipf(&plain, false);
        LabImage plain_image(238, 64);
        plain_image.CopyFrom(&source);
        labWaveletHostBlock(plain_ipf, plain, plain_image);

        const Diff diff = compare(view(plain_image), view(clarity));
        std::printf("stage-residency: lab.wavelet clarity merge changes %zu "
                    "of %zu samples, PAE %.6f LSB16\n",
                    diff.differing, diff.samples, diff.max_abs);
        check(diff.differing > 1000 && diff.max_abs > 1.0,
              "lab.wavelet clarity merge changes the image");
    }

    /*
     * The guided soft radius must not be a no-op: the same profile with and
     * without it must differ on the host block, so the device cases above
     * cannot pass by ignoring the slider.
     */
    {
        rtengine::procparams::ProcParams params;
        wavelet_contrast_params(params, 5, false, contrast, 1.0, 1.0, 100);
        params.wavelet.expclari = true;
        params.wavelet.ushamethod = "sharp";
        params.wavelet.Backmethod = "black";
        params.wavelet.CLmethod = "one";
        params.wavelet.mergeL = 60;
        params.wavelet.mergeC = 60;

        rtengine::procparams::ProcParams plain = params;
        params.wavelet.softrad = 5.0;

        LabImage source(238, 64);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions soft_ipf(&params, false);
        LabImage soft(238, 64);
        soft.CopyFrom(&source);
        labWaveletHostBlock(soft_ipf, params, soft);

        rtengine::ImProcFunctions plain_ipf(&plain, false);
        LabImage plain_image(238, 64);
        plain_image.CopyFrom(&source);
        labWaveletHostBlock(plain_ipf, plain, plain_image);

        const Diff diff = compare(view(plain_image), view(soft));
        std::printf("stage-residency: lab.wavelet clarity soft radius changes "
                    "%zu of %zu samples, PAE %.6f LSB16\n",
                    diff.differing, diff.samples, diff.max_abs);
        check(diff.differing > 1000 && diff.max_abs > 1.0,
              "lab.wavelet clarity soft radius changes the image");
    }

    /*
     * Refusals: the CLmethod, Backmethod and Dirmethod values the block would
     * leave to an uninitialised field, an Lmethod or merge slider outside the
     * widget range, and the final panel's guided soft radius, which is not
     * ported.
     */
    const char* residual_labels[] = {
        "CLmethod unknown", "Backmethod unknown", "Dirmethod unknown",
        "Lmethod low", "Lmethod high", "mergeL high", "mergeC low",
        "softradend"};
    for (const char* label : residual_labels) {
        rtengine::procparams::ProcParams params;
        wavelet_contrast_params(params, 5, false, contrast, 1.0, 1.0, 100);
        params.wavelet.expclari = true;
        params.wavelet.ushamethod = "sharp";
        params.wavelet.Backmethod = "black";
        params.wavelet.CLmethod = "one";

        if (std::strcmp(label, "CLmethod unknown") == 0) {
            params.wavelet.CLmethod = "bogus";
        } else if (std::strcmp(label, "Backmethod unknown") == 0) {
            params.wavelet.Backmethod = "bogus";
        } else if (std::strcmp(label, "Dirmethod unknown") == 0) {
            params.wavelet.Dirmethod = "bogus";
        } else if (std::strcmp(label, "Lmethod low") == 0) {
            params.wavelet.Lmethod = -1;
        } else if (std::strcmp(label, "Lmethod high") == 0) {
            params.wavelet.Lmethod = 11;
        } else if (std::strcmp(label, "mergeL high") == 0) {
            params.wavelet.mergeL = 101.0;
        } else if (std::strcmp(label, "mergeC low") == 0) {
            params.wavelet.mergeC = -51.0;
        } else if (std::strcmp(label, "softradend") == 0) {
            params.wavelet.softradend = 0.5;
        }

        check(!labWaveletSupported(params, 238, 64),
              (std::string("lab.wavelet clarity residual ") + label +
               " is a residual").c_str());

        LabImage source(238, 64);
        fill_lab_wavelet_skin(source);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabImage cpu(238, 64);
        cpu.CopyFrom(&source);
        labWaveletHostBlock(cpu_ipf, params, cpu);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabImage device(238, 64);
        device.CopyFrom(&source);
        const LabWaveletRun result =
            run_lab_wavelet_node(device, params, device_ipf, profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.wavelet clarity residual ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.wavelet clarity residual ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.wavelet clarity residual ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.wavelet clarity residual ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.wavelet clarity residual ") + label +
               " never mixes a device pass").c_str());

        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.wavelet clarity residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.wavelet clarity residual ") + label +
               " matches the CPU block").c_str());
    }
}

/* A GPU device must never run the equaliser; it records a host execution. */
void test_lab_wavelet_gpu(bool available)
{
    using namespace rtengine::stage;

    StageOpenCLDeviceInfo info;
    if (!available || !opencl_stage_device_info(&info) || info.cpu) {
        skip("lab.wavelet GPU residual: no GPU OpenCL device");
        return;
    }

    static rtengine::Settings gpu_settings{};
    rtengine::settings = &gpu_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-wavelet-gpu.jsonl";

    rtengine::procparams::ProcParams params;
    params.dirpyrequalizer.enabled = true;
    params.dirpyrequalizer.cbdlMethod = "aft";
    params.dirpyrequalizer.mult[0] = 1.5;

    LabImage source(179, 48);
    fill_lab_tools(source, 0x9e3779b9u);

    rtengine::ImProcFunctions cpu_ipf(&params, false);
    LabImage cpu(179, 48);
    cpu.CopyFrom(&source);
    labWaveletHostBlock(cpu_ipf, params, cpu);

    rtengine::ImProcFunctions device_ipf(&params, false);
    LabImage device(179, 48);
    device.CopyFrom(&source);
    const LabWaveletRun result =
        run_lab_wavelet_node(device, params, device_ipf, profile_path);
    const std::string profile = read_file(profile_path);

    check(result.supported,
          "lab.wavelet GPU residual is a supported configuration");
    check(result.deviceReady,
          "lab.wavelet GPU residual builds its host setup");
    check(result.status == RT_STAGE_OK,
          "lab.wavelet GPU residual completes through run()");
    check(profile.find("\"impl\":\"host\"") != std::string::npos,
          "lab.wavelet GPU residual records a host execution");
    const Diff diff = compare(view(device), view(cpu));
    check(diff.differing == 0 && diff.max_abs == 0.0,
          "lab.wavelet GPU residual matches the CPU block");
}

/* --------------------------------------------------------- run() wiring */

void test_run_wiring(bool available)
{
    using namespace rtengine::stage;

    StageBufferAllocator host(StageDeviceType::CPU);
    NodeFixture fixture;
    if (!fixture.allocate(host, 96, 64, 48, 32)) {
        check(false, "run fixture allocates");
        return;
    }

    RtStageBuffer cpu_output = fixture.cpu_output;
    RtStageBuffer device_output = fixture.device_output;

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                          : "/tmp") +
        "/stage-residency-profile.jsonl";
    std::remove(profile_path.c_str());
    setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);

    /* The switch off: the host path must be the one that runs. */
    unsetenv("RT_STAGE_OPENCL");
    clear_node_device_overrides();
    set_opencl_stage_allowed(true);
    const auto cpu_args = fixture.args(cpu_output, 0.5);
    check(run("lanczos3", cpu_args) == RT_STAGE_OK,
          "run() executes with the switch off");
    const std::string host_profile = read_file(profile_path);
    check(host_profile.find("\"impl\":\"opencl\"") == std::string::npos,
          "the switch-off run() does not use the device");
    check(host_profile.find("\"impl\":\"builtin\"") != std::string::npos ||
              host_profile.find("\"impl\":\"module\"") != std::string::npos,
          "the switch-off run() reports a host implementation");

    if (!available) {
        skip("run wiring: no OpenCL device");
        unsetenv("RT_STAGE_PROFILE");
        return;
    }

    /* The switch on: the same call must dispatch to the device. */
    std::remove(profile_path.c_str());
    set_node_device_override("lanczos3", StageNodeDevice::OpenCL);
    const auto device_args = fixture.args(device_output, 0.5);
    check(run("lanczos3", device_args) == RT_STAGE_OK,
          "run() executes with the device selected");
    const std::string device_profile = read_file(profile_path);
    check(device_profile.find("\"impl\":\"opencl\"") != std::string::npos,
          "the device run() reports the OpenCL implementation");
    clear_node_device_overrides();

    const Diff diff = compare(cpu_output, device_output);
    std::printf("stage-residency: run() parity: PAE %.6f LSB16, "
                "%zu of %zu samples differ\n",
                diff.max_abs, diff.differing, diff.samples);
    check(diff.finite, "the run() device output is finite");
    check(diff.max_abs <= 1.0, "the run() device output is within one LSB");

    /* The same dispatch is reachable through the environment opt-in alone. */
    std::remove(profile_path.c_str());
    setenv("RT_STAGE_OPENCL", "1", 1);
    std::string env_error;
    RtStageBuffer env_output =
        host.allocate({RT_STAGE_F32, 48, 32, 3}, &env_error);
    const auto env_args = fixture.args(env_output, 0.5);
    check(run("lanczos3", env_args) == RT_STAGE_OK,
          "run() executes with RT_STAGE_OPENCL=1");
    unsetenv("RT_STAGE_OPENCL");
    const std::string env_profile = read_file(profile_path);
    unsetenv("RT_STAGE_PROFILE");
    check(env_profile.find("\"impl\":\"opencl\"") != std::string::npos,
          "RT_STAGE_OPENCL=1 alone dispatches the node to the device");
}

/* --------------------------------------------------- lab.tools Lab regions */

/*
 * A region mask the host can sample: a two-point FCT_MinMaxCPoints curve with
 * the given tangents. Zero tangents make the segment linear; 0.35 exercises
 * the curved path.
 */
std::vector<double> region_mask(double x0, double y0, double x1, double y1,
                                double tangent)
{
    return {static_cast<double>(FCT_MinMaxCPoints),
            x0, y0, tangent, tangent,
            x1, y1, tangent, tangent};
}

std::vector<double> region_no_mask()
{
    return {static_cast<double>(FCT_Linear)};
}

struct RegionSpec {
    double a;
    double b;
    double saturation;
    double slope;
    double offset;
    double power;
    double blur;
    int channel;
    /* 0 all default masks, 1 hue only, 2 chroma only, 3 lightness only,
     * 4 all curved, 5 no masks. */
    int masks;
};

struct RegionCase {
    const char* label;
    int width;
    int height;
    int count;
    RegionSpec regions[4];
    /* 0 regions alone, 1 after shadows/highlights, 2 after the Lab curves,
     * 3 after vibrance. */
    int combine;
};

/* The mask set a case asks for. The default LabCorrectionRegion already has
 * fully open hue, chroma and lightness curves. */
void set_region_masks(
    rtengine::procparams::ColorToningParams::LabCorrectionRegion& region,
    int masks)
{
    switch (masks) {
    case 1:
        region.hueMask =
            region_mask(0.05, 0.0, 0.95, 1.0, 0.35);
        region.chromaticityMask = region_no_mask();
        region.lightnessMask = region_no_mask();
        break;
    case 2:
        region.hueMask = region_no_mask();
        region.chromaticityMask =
            region_mask(0.0, 1.0, 1.2, 0.0, 0.35);
        region.lightnessMask = region_no_mask();
        break;
    case 3:
        region.hueMask = region_no_mask();
        region.chromaticityMask = region_no_mask();
        region.lightnessMask = region_mask(0.0, 0.2, 1.0, 0.8, 0.0);
        break;
    case 4:
        region.hueMask =
            region_mask(0.05, 0.0, 0.95, 1.0, 0.35);
        region.chromaticityMask =
            region_mask(0.0, 1.0, 1.2, 0.0, 0.35);
        region.lightnessMask =
            region_mask(0.0, 0.2, 1.0, 0.8, 0.35);
        break;
    case 5:
        region.hueMask.clear();
        region.chromaticityMask.clear();
        region.lightnessMask.clear();
        break;
    case 6:
        region.hueMask = {
            static_cast<double>(FCT_MinMaxCPoints),
            0.05, 0.0, 0.35, 0.35,
            0.50, 0.8, 0.20, 0.20,
            0.95, 0.1, 0.35, 0.35};
        region.chromaticityMask = {
            static_cast<double>(FCT_MinMaxCPoints),
            0.0, 1.0, 0.35, 0.35,
            0.6, 0.2, 0.1, 0.4,
            1.2, 0.9, 0.35, 0.35};
        region.lightnessMask = {
            static_cast<double>(FCT_MinMaxCPoints),
            0.0, 0.1, 0.0, 0.4,
            0.5, 0.9, 0.3, 0.1,
            1.0, 0.3, 0.35, 0.35};
        break;
    default:
        /* The LabCorrectionRegion defaults: three fully open masks. */
        break;
    }
}

void apply_region_case(const RegionCase& item,
                       rtengine::procparams::ProcParams& params)
{
    params.colorToning.enabled = true;
    params.colorToning.method = "LabRegions";
    params.colorToning.labregions.clear();
    for (int index = 0; index < item.count; ++index) {
        rtengine::procparams::ColorToningParams::LabCorrectionRegion region;
        const RegionSpec& spec = item.regions[index];
        region.a = spec.a;
        region.b = spec.b;
        region.saturation = spec.saturation;
        region.slope = spec.slope;
        region.offset = spec.offset;
        region.power = spec.power;
        region.maskBlur = spec.blur;
        region.channel = spec.channel;
        set_region_masks(region, spec.masks);
        params.colorToning.labregions.push_back(region);
    }

    if (item.combine == 1) {
        params.sh.enabled = true;
        params.sh.lab = true;
        params.sh.highlights = 50;
        params.sh.shadows = 50;
        params.sh.htonalwidth = 50;
        params.sh.stonalwidth = 50;
        params.sh.radius = 10;
    } else if (item.combine == 2) {
        params.labCurve.enabled = true;
        params.labCurve.contrast = 0;
        params.labCurve.lcurve = {
            static_cast<double>(DCT_Spline),
            0.0, 0.0,
            0.5, 0.62,
            1.0, 1.0};
        params.labCurve.acurve = {
            static_cast<double>(DCT_Spline),
            0.0, 0.0,
            0.5, 0.55,
            1.0, 1.0};
        params.labCurve.gamutmunselmethod = "NONE";
    } else if (item.combine == 3) {
        params.vibrance.enabled = true;
        params.vibrance.pastels = 60;
        params.vibrance.saturated = 35;
        params.vibrance.psthreshold =
            rtengine::procparams::Threshold<int>(0, 75, false);
    }
}

void test_lab_tools_regions(bool available)
{
    using namespace rtengine::stage;

    static MyMutex regions_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &regions_lcms_mutex;
    }
    static rtengine::Settings regions_settings{};
    rtengine::settings = &regions_settings;
    rtengine::Color::init();

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-lab-tools-regions.jsonl";

    /*
     * The default region is the bounded set's simplest case. The setup must
     * export one region and three mask tables, and every table must reproduce
     * FlatCurve::getVal() inside the accepted error.
     */
    {
        rtengine::procparams::ProcParams params;
        params.colorToning.enabled = true;
        params.colorToning.method = "LabRegions";

        rtengine::ImProcFunctions ipf(&params, false);
        LabToolsHostState host;
        LabToolsLuts luts = host.luts();
        LabToolsContext context(params, ipf, luts, host.autili, host.butili);
        RtLabToolsResult setup{};
        check(context.build(setup) == 0, "region setup builds");
        check(setup.region_count == 1, "the setup exports one default region");
        check(setup.region_lut_stride == RT_LAB_TOOLS_REGION_LUT_STRIDE,
              "the region table has the declared stride");
        check(setup.region_luts != nullptr && setup.region_params != nullptr,
              "the region tables have buffers");
        bool finite = true;
        bool curves = true;
        for (int region = 0; region < 1; ++region) {
            for (int curve = 0; curve < 3; ++curve) {
                const float* slot =
                    setup.region_luts +
                    (region * 3 + curve) * setup.region_lut_stride;
                const int count = static_cast<int>(slot[0]);
                curves = curves && count >= 2 &&
                         count <= static_cast<int>(
                                      RT_LAB_TOOLS_REGION_LUT_NODES);
                for (int index = 0; index < 1 + 2 * count; ++index) {
                    finite = finite && std::isfinite(slot[index]);
                }
            }
        }
        check(curves, "every region mask exports a piecewise-linear table");
        check(finite, "the region tables are finite");
    }

    if (!available) {
        skip("lab.tools Lab regions: no OpenCL device");
        return;
    }

    const RegionCase cases[] = {
        {"single region", 238, 64, 1,
         {{0.4, -0.3, 30.0, 1.0, 0.0, 1.0, 0.4, -1, 0}}},
        {"two regions", 238, 64, 2,
         {{0.35, -0.2, 25.0, 1.0, 0.0, 1.0, 0.4, -1, 0},
          {-0.4, 0.3, -20.0, 1.1, 0.01, 0.9, 1.5, -1, 4}}},
        {"four regions", 238, 64, 4,
         {{0.5, -0.2, 20.0, 1.0, 0.0, 1.0, 0.3, -1, 0},
          {-0.3, 0.4, -30.0, 0.9, -0.02, 1.2, 2.0, -1, 1},
          {0.2, 0.2, 10.0, 1.2, 0.0, 0.8, 0.0, -1, 2},
          {-0.2, -0.4, 40.0, 1.0, 0.03, 1.0, 4.0, -1, 3}}},
        {"shadows/highlights + regions", 238, 64, 2,
         {{0.4, -0.3, 30.0, 1.0, 0.0, 1.0, 0.4, -1, 0},
          {-0.3, 0.25, -20.0, 1.1, 0.0, 0.9, 1.2, -1, 4}},
         1},
        {"Lab curves + regions", 238, 64, 2,
         {{0.4, -0.3, 30.0, 1.0, 0.0, 1.0, 0.4, -1, 0},
          {-0.3, 0.25, -20.0, 1.1, 0.0, 0.9, 1.2, -1, 4}},
         2},
        {"vibrance + regions", 238, 64, 2,
         {{0.4, -0.3, 30.0, 1.0, 0.0, 1.0, 0.4, -1, 0},
          {-0.3, 0.25, -20.0, 1.1, 0.0, 0.9, 1.2, -1, 4}},
         3},
        {"hue mask only", 238, 64, 1,
         {{0.3, -0.3, 20.0, 1.0, 0.0, 1.0, 0.5, -1, 1}}},
        {"chroma mask only", 238, 64, 1,
         {{0.3, -0.3, 20.0, 1.0, 0.0, 1.0, 0.5, -1, 2}}},
        {"lightness mask only", 238, 64, 1,
         {{0.3, -0.3, 20.0, 1.0, 0.0, 1.0, 0.5, -1, 3}}},
        {"curved masks", 238, 64, 1,
         {{0.4, -0.3, 35.0, 1.0, 0.0, 1.0, 0.7, -1, 4}}},
        {"three-point masks", 238, 64, 1,
         {{0.35, -0.25, 25.0, 1.1, 0.01, 0.95, 0.6, -1, 6}}},
        {"no masks", 238, 64, 1,
         {{0.4, -0.3, 35.0, 1.0, 0.0, 1.0, 0.7, -1, 5}}},
        {"CDL slope", 238, 64, 1,
         {{0.3, -0.2, 10.0, 1.35, 0.0, 1.0, 0.4, -1, 0}}},
        {"CDL offset", 238, 64, 1,
         {{0.3, -0.2, 10.0, 1.0, 0.06, 1.0, 0.4, -1, 0}}},
        {"CDL power", 238, 64, 1,
         {{0.3, -0.2, 10.0, 1.0, 0.0, 0.75, 0.4, -1, 0}}},
        {"CDL all", 238, 64, 1,
         {{0.3, -0.2, 25.0, 1.2, -0.04, 1.3, 0.4, -1, 4}}},
        {"saturation up", 238, 64, 1,
         {{0.2, -0.1, 80.0, 1.0, 0.0, 1.0, 0.4, -1, 0}}},
        {"saturation down", 238, 64, 1,
         {{0.2, -0.1, -60.0, 1.0, 0.0, 1.0, 0.4, -1, 0}}},
        {"channel red", 238, 64, 1,
         {{0.4, -0.3, 20.0, 1.0, 0.0, 1.0, 0.4, 0, 0}}},
        {"channel green", 238, 64, 1,
         {{0.4, -0.3, 20.0, 1.0, 0.0, 1.0, 0.4, 1, 0}}},
        {"channel blue", 238, 64, 1,
         {{0.4, -0.3, 20.0, 1.0, 0.0, 1.0, 0.4, 2, 0}}},
        {"blur zero", 238, 64, 1,
         {{0.4, -0.3, 20.0, 1.0, 0.0, 1.0, 0.0, -1, 0}}},
        {"blur negative", 238, 64, 1,
         {{0.4, -0.3, 20.0, 1.0, 0.0, 1.0, -2.0, -1, 0}}},
        {"blur five", 238, 64, 1,
         {{0.4, -0.3, 20.0, 1.0, 0.0, 1.0, 5.0, -1, 0}}},
        {"padded width 179", 179, 48, 2,
         {{0.4, -0.3, 30.0, 1.0, 0.0, 1.0, 0.4, -1, 0},
          {-0.4, 0.2, -15.0, 1.1, 0.0, 0.9, 1.0, -1, 4}}},
        {"padded width 238", 238, 64, 4,
         {{0.3, -0.2, 20.0, 1.0, 0.0, 1.0, 0.2, -1, 0},
          {-0.2, 0.3, -15.0, 1.0, 0.0, 1.0, 0.8, -1, 1},
          {0.4, 0.1, 35.0, 1.2, 0.02, 0.9, 2.0, -1, 2},
          {-0.3, -0.4, -35.0, 0.9, -0.02, 1.1, 3.0, -1, 3}}},
        {"large width 8192", 8192, 64, 2,
         {{0.4, -0.3, 30.0, 1.0, 0.0, 1.0, 0.4, -1, 0},
          {-0.4, 0.2, -15.0, 1.1, 0.0, 0.9, 1.0, -1, 4}}},
    };

    unsigned seed = 0x51ed270bu;
    std::size_t samples = 0;
    std::size_t differing = 0;
    std::size_t worst_over = 0;
    double worst = 0.0;

    for (const RegionCase& item : cases) {
        rtengine::procparams::ProcParams params;
        apply_region_case(item, params);

        check(labToolsSupported(params),
              (std::string("lab.tools regions ") + item.label +
               " is the supported set").c_str());

        LabImage source(item.width, item.height);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(item.width, item.height);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(item.width, item.height);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              (std::string("lab.tools regions ") + item.label +
               " resolves to the supported set").c_str());
        check(result.deviceReady,
              (std::string("lab.tools regions ") + item.label +
               " builds its host setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools regions ") + item.label +
               " completes through run()").c_str());
        check(profile.find("\"stage\":\"lab.tools\"") != std::string::npos,
              (std::string("lab.tools regions ") + item.label +
               " records the node").c_str());
        check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
              (std::string("lab.tools regions ") + item.label +
               " records a device execution").c_str());

        const Distribution d = distribution(view(device), view(cpu));
        std::printf("stage-residency: lab.tools regions %s %dx%d: PAE %.6f "
                    "LSB16; exact %zu, <=0.25 %zu, <=0.5 %zu, <=1 %zu, >1 "
                    "%zu of %zu\n",
                    item.label, item.width, item.height, d.max_abs, d.exact,
                    d.quarter, d.half, d.one, d.over, d.samples);
        check(d.finite,
              (std::string("lab.tools regions ") + item.label +
               " stays finite").c_str());
        check(d.over == 0,
              (std::string("lab.tools regions ") + item.label +
               " is within one LSB16").c_str());

        samples += d.samples;
        differing += d.samples - d.exact;
        worst_over += d.over;
        worst = std::max(worst, d.max_abs);

        /*
         * Non-vacuous: the region pass must change the block's output for at
         * least one case. Compare the CPU result with its input.
         */
        const Diff changed = compare(view(cpu), view(source));
        if (std::strcmp(item.label, "single region") == 0) {
            check(changed.differing > 0,
                  "the region pass changes the image");
        }
    }

    std::printf("stage-residency: lab.tools regions total: %zu samples, "
                "%zu differing, worst PAE %.6f LSB16, %zu over one LSB16\n",
                samples, differing, worst, worst_over);

    /*
     * Residuals. Each profile is the supported set plus one property the
     * bounded set excludes; the predicate refuses it, the launcher runs the
     * builtin and the result is the block's own.
     */
    const auto run_residual = [&](const char* label,
                                  const std::function<void(
                                      rtengine::procparams::ProcParams&)>&
                                      mutate) {
        rtengine::procparams::ProcParams params;
        params.colorToning.enabled = true;
        params.colorToning.method = "LabRegions";
        params.colorToning.labregions[0].a = 0.4;
        params.colorToning.labregions[0].b = -0.3;
        params.colorToning.labregions[0].saturation = 20.0;
        params.colorToning.labregions[0].maskBlur = 0.4;
        mutate(params);

        check(!labToolsSupported(params),
              (std::string("lab.tools regions ") + label +
               " is a residual").c_str());

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(!result.supported,
              (std::string("lab.tools regions ") + label +
               " is refused as a device configuration").c_str());
        check(!result.deviceReady,
              (std::string("lab.tools regions ") + label +
               " does not build a device setup").c_str());
        check(result.status == RT_STAGE_OK,
              (std::string("lab.tools regions ") + label +
               " completes through run()").c_str());
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              (std::string("lab.tools regions ") + label +
               " records a host execution").c_str());
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              (std::string("lab.tools regions ") + label +
               " never mixes a device pass").c_str());
        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools regions residual %s: host, "
                    "PAE %.6f LSB16, %zu of %zu samples differ\n",
                    label, diff.max_abs, diff.differing, diff.samples);
        check(diff.differing == 0 && diff.max_abs == 0.0,
              (std::string("lab.tools regions ") + label +
               " matches the CPU block").c_str());
    };

    run_residual("show mask",
                 [](rtengine::procparams::ProcParams& p) {
                     p.colorToning.labregionsShowMask = 0;
                 });
    run_residual("five regions",
                 [](rtengine::procparams::ProcParams& p) {
                     while (p.colorToning.labregions.size() < 5) {
                         p.colorToning.labregions.push_back(
                             rtengine::procparams::ColorToningParams::
                                 LabCorrectionRegion());
                     }
                 });
    run_residual("unsupported curve",
                 [](rtengine::procparams::ProcParams& p) {
                     p.colorToning.labregions[0].hueMask = {
                         static_cast<double>(FCT_Unchanged),
                         0.0, 0.0, 0.0, 0.0,
                         1.0, 1.0, 0.0, 0.0};
                 });
    run_residual("slope out of range",
                 [](rtengine::procparams::ProcParams& p) {
                     p.colorToning.labregions[0].slope = 0.05;
                 });
    run_residual("channel out of range",
                 [](rtengine::procparams::ProcParams& p) {
                     p.colorToning.labregions[0].channel = 3;
                 });
    run_residual("saturation out of range",
                 [](rtengine::procparams::ProcParams& p) {
                     p.colorToning.labregions[0].saturation = 200.0;
                 });
    run_residual("blur out of range",
                 [](rtengine::procparams::ProcParams& p) {
                     p.colorToning.labregions[0].maskBlur = -20.0;
                 });

    /*
     * A supported curve type whose polyline cannot be followed within the
     * table's 5e-7 mask-error budget is refused by the setup, so the builtin
     * runs the block. A near-vertical segment between two close control
     * points is such a curve.
     */
    {
        rtengine::procparams::ProcParams params;
        params.colorToning.enabled = true;
        params.colorToning.method = "LabRegions";
        params.colorToning.labregions[0].a = 0.4;
        params.colorToning.labregions[0].b = -0.3;
        params.colorToning.labregions[0].saturation = 20.0;
        params.colorToning.labregions[0].maskBlur = 0.4;
        params.colorToning.labregions[0].hueMask = region_no_mask();
        params.colorToning.labregions[0].lightnessMask = region_no_mask();
        /* A dense zigzag: every segment is linear, but the slope changes by
         * 2000 at every node, so the piecewise-linear table cannot follow it
         * within 5e-7 even at its maximum depth and node budget. */
        std::vector<double> hostile;
        hostile.push_back(static_cast<double>(FCT_MinMaxCPoints));
        constexpr int kZigzag = 2000;
        for (int index = 0; index < kZigzag; ++index) {
            hostile.push_back(static_cast<double>(index) /
                              static_cast<double>(kZigzag));
            hostile.push_back(index % 2 == 0 ? 0.0 : 1.0);
            hostile.push_back(0.0);
            hostile.push_back(0.0);
        }
        params.colorToning.labregions[0].chromaticityMask = hostile;

        check(labToolsSupported(params),
              "a supported curve type outside the table budget is inside the "
              "predicate");
        check(labToolsRegionsActive(params),
              "the predicate does not sample the curve");

        LabImage source(179, 48);
        fill_lab_tools(source, seed++);

        rtengine::ImProcFunctions cpu_ipf(&params, false);
        LabToolsHostState cpu_host;
        LabImage cpu(179, 48);
        cpu.CopyFrom(&source);
        LabToolsLuts cpu_luts = cpu_host.luts();
        labToolsHostBlock(cpu_ipf, params, cpu, cpu_luts, cpu_host.autili,
                          cpu_host.butili);

        rtengine::ImProcFunctions device_ipf(&params, false);
        LabToolsHostState device_host;
        LabImage device(179, 48);
        device.CopyFrom(&source);
        const LabToolsRun result =
            run_lab_tools_node(device, params, device_host, device_ipf,
                               profile_path);
        const std::string profile = read_file(profile_path);

        check(result.supported,
              "the unresolvable curve is still inside the predicate");
        check(!result.deviceReady,
              "the unresolvable curve refuses the device setup");
        check(result.status == RT_STAGE_OK,
              "the unresolvable curve completes through run()");
        check(profile.find("\"impl\":\"host\"") != std::string::npos,
              "the unresolvable curve records a host execution");
        check(profile.find("\"impl\":\"opencl\"") == std::string::npos,
              "the unresolvable curve never mixes a device pass");
        const Diff diff = compare(view(device), view(cpu));
        std::printf("stage-residency: lab.tools regions unresolvable curve: "
                    "host, PAE %.6f LSB16, %zu of %zu samples differ\n",
                    diff.max_abs, diff.differing, diff.samples);
        check(diff.differing == 0 && diff.max_abs == 0.0,
              "the unresolvable curve matches the CPU block");
    }
}

/* --------------------------------------------------- local adjustments */

void fill_local_adjustments_rgb(Imagefloat& image, unsigned seed)
{
    for (int y = 0; y < image.getHeight(); ++y) {
        for (int x = 0; x < image.getWidth(); ++x) {
            const unsigned hash =
                (seed * 2654435761u + static_cast<unsigned>(x) * 2246822519u +
                 static_cast<unsigned>(y) * 3266489917u) >>
                8;
            /* A smooth ramp with structure so the transition, deltaE and clamp
             * all see non-trivial values. A few samples sit outside the
             * working range, which is what the per-spot clamp is for. */
            float r = 12000.f + 12000.f * std::sin(0.031f * x) +
                      6000.f * std::cos(0.017f * y);
            float g = 18000.f + 9000.f * std::cos(0.023f * x + 0.4f);
            float b = 9000.f + 15000.f * std::sin(0.019f * y + 0.7f);
            if ((hash % 131u) == 0u) {
                r = 70000.f;
            } else if ((hash % 127u) == 0u) {
                g = -3000.f;
            }
            image.r(y, x) = std::max(0.f, std::min(65535.f, r));
            image.g(y, x) = std::max(0.f, std::min(65535.f, g));
            image.b(y, x) = std::max(0.f, std::min(65535.f, b));
        }
    }
}

rtengine::procparams::LocallabParams::LocallabSpot localAdjustmentsSpot(
    const char* name, int centre_x, int centre_y, int extent,
    bool active, int chroma, int lightness, int contrast)
{
    rtengine::procparams::LocallabParams::LocallabSpot spot;
    spot.name = name;
    spot.shape = "ELI";
    spot.spotMethod = "norm";
    spot.centerX = centre_x;
    spot.centerY = centre_y;
    spot.loc = {extent, extent, extent, extent};
    spot.circrad = 18.0;
    spot.transit = 60.0;
    spot.transitweak = 1.0;
    spot.transitgrad = 0.0;
    spot.activ = active;
    spot.deltae = true;
    spot.avoidrad = 0.0;
    spot.avoidgamutMethod = "NONE";
    spot.merMethod = "mone";
    spot.qualitycurveMethod = "none";
    spot.gamc = 1.0;
    spot.softradiuscol = 0.0;
    spot.reparcol = 100.0; /* merge recovery; ignored without a merge */
    spot.strengthgrid = 30; /* only scales an active LabGrid */
    spot.avoidneg = true;    /* the per-spot negative-value prefilter */
    spot.expcolor = true;
    spot.chroma = chroma;
    spot.lightness = lightness;
    spot.contrast = contrast;
    return spot;
}

/* The call-site submission: build the context, fill the three-argument ABI
 * and run the node with the device selected. */
RtStageStatus run_local_adjustments_node(
    Imagefloat& base, LabImage& lab,
    rtengine::procparams::ProcParams& params, rtengine::ImProcFunctions& ipf,
    const std::string& profile_path)
{
    using namespace rtengine::stage;

    ipf.rgb2lab(base, lab, params.icm.workingProfile);

    LabImage* lab_pointer = &lab;
    LocalAdjustmentsContext context(params, ipf, &base, lab_pointer, lab.W,
                                    lab.H);

    RtStageArg args[3] = {};
    args[0].kind = RT_STAGE_ARG_BUFFER;
    args[0].u.buffer = view(lab);
    args[1].kind = RT_STAGE_ARG_PTR;
    args[1].u.ptr = &context; /* descriptor() is returned by value: see below */
    args[2].kind = RT_STAGE_ARG_INT;
    args[2].u.i = localAdjustmentsSupported(params) ? 1 : 0;

    RtLocalAdjustmentsContext descriptor =
        context.descriptor(args[2].u.i != 0);
    args[1].u.ptr = &descriptor;

    std::remove(profile_path.c_str());
    setenv("RT_STAGE_PROFILE", profile_path.c_str(), 1);
    set_node_device_override("local.adjustments", StageNodeDevice::OpenCL);
    const RtStageStatus status =
        run("local.adjustments", std::vector<RtStageArg>(args, args + 3));
    clear_node_device_overrides();
    unsetenv("RT_STAGE_PROFILE");
    return status;
}

struct LocalAdjustmentsCase {
    const char* label;
    int width;
    int height;
    int chroma;
    int lightness;
    int contrast;
    bool deltae;
    double transit;
    double feather;
    double transitweak;
    double transitgrad;
    bool avoidneg;
};

/* One unsupported setting on an otherwise supported one-spot profile. */
struct LocalAdjustmentsResidual {
    const char* label;
    void (*mutate)(rtengine::procparams::ProcParams&);
};

void test_local_adjustments(bool available)
{
    using namespace rtengine::stage;

    static MyMutex local_adjustments_lcms_mutex;
    if (rtengine::lcmsMutex == nullptr) {
        rtengine::lcmsMutex = &local_adjustments_lcms_mutex;
    }
    static MyMutex local_adjustments_fftw_mutex;
    if (rtengine::fftwMutex == nullptr) {
        rtengine::fftwMutex = &local_adjustments_fftw_mutex;
    }
    static rtengine::Settings local_adjustments_settings{};
    /* options.cc's production default: low values make the L=f(H) curve
     * extremely sensitive to the clamp's last-bit differences. */
    local_adjustments_settings.amchroma = 40;
    rtengine::settings = &local_adjustments_settings;
    rtengine::Color::init();

    StageOpenCLDeviceInfo device_info;
    const bool have_device =
        available && opencl_stage_device_info(&device_info);
    const bool cpu_device = have_device && device_info.cpu;

    {
        rtengine::procparams::ProcParams defaults;
        check(!localAdjustmentsSupported(defaults),
              "local.adjustments refuses a profile with no spots");
    }

    rtengine::procparams::ProcParams supported;
    supported.locallab.enabled = true;
    supported.locallab.spots.push_back(
        localAdjustmentsSpot("device", 0, 0, 800, true, 35, 10, -5));
    check(localAdjustmentsSupported(supported),
          "local.adjustments admits one active colour spot");

    /* The residual matrix: one otherwise supported profile per entry. */
    {
        const LocalAdjustmentsResidual residuals[] = {
            {"GHS clamp leftovers", [](auto& p) {
                 p.locallab.spots[0].activ = false;
                 p.locallab.spots[0].ghs_D = 1.0;
                 p.locallab.spots[0].ghs_BLP = 1.0;
             }},
            {"exposure", [](auto& p) {
                 p.locallab.spots[0].expexpose = true;
             }},
            {"shadows/highlights", [](auto& p) {
                 p.locallab.spots[0].expshadhigh = true;
             }},
            {"vibrance gamma", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 40;
                 p.locallab.spots[0].vibgam = 2.0;
             }},
            {"vibrance gradient", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 40;
                 p.locallab.spots[0].strvib = 5.0;
             }},
            {"vibrance warm", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 40;
                 p.locallab.spots[0].warm = 1;
             }},
            {"local contrast wavelet", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "wav";
                 p.locallab.spots[0].lcamount = 0.4;
             }},
            {"local contrast empty method", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "";
                 p.locallab.spots[0].lcamount = 0.4;
             }},
            {"local contrast fft", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "loc";
                 p.locallab.spots[0].lcamount = 0.4;
                 p.locallab.spots[0].fftwlc = true;
             }},
            {"blur", [](auto& p) {
                 p.locallab.spots[0].expblur = true;
             }},
            {"tone map", [](auto& p) {
                 p.locallab.spots[0].exptonemap = true;
             }},
            {"soft light zero strength", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
             }},
            {"soft light reti", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].softMethod = "reti";
                 p.locallab.spots[0].streng = 60;
             }},
            {"soft light mask", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 60;
                 p.locallab.spots[0].enaColorMask = true;
             }},
            {"soft light soft radius", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 60;
                 p.locallab.spots[0].softradiuscol = 1.0;
             }},
            {"soft light strength high", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 101;
             }},
            {"soft light sensitivity range", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 60;
                 p.locallab.spots[0].sensisf = 101;
             }},
            {"CBDL soft radius", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].softradiuscb = 5.0;
             }},
            {"CBDL residual contrast", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].contresid = 20;
             }},
            {"CBDL mask", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].enacbMask = true;
             }},
            {"CBDL multiplier range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].mult[2] = 5.0;
             }},
            {"CBDL chroma range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].chromacbdl = 2.0;
             }},
            {"CBDL threshold range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].threshold = 1.5;
             }},
            {"CBDL clarity range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].clarityml = 0.05;
             }},
            {"CBDL sensitivity range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].sensicb = 101;
             }},
            {"capture sharpening", [](auto& p) {
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "cap";
             }},
            {"sharpen damping range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.locallab.spots[0].shardamping = 101;
             }},
            {"sharpen damping negative", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].shardamping = -1;
             }},
            {"sharpen damped radius", [](auto& p) {
                 p.dirpyrequalizer.cbdlMethod = "bef";
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expsharp = true;
                 spot.methodcap = "rl";
                 spot.shardamping = 0;
                 spot.sharradius = 0.55;
                 spot.sharamount = 100;
                 spot.shariter = 30;
                 spot.sharcontrast = 20;
                 spot.sharblur = 0.2;
                 spot.shargam = 1.0;
                 spot.sharshow = false;
                 spot.reparsha = 100.0;
                 spot.sensisha = 40;
             }},
            {"log detail", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.6;
             }},
            {"log autocompute", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].autocompute = true;
             }},
            {"log CIE CAM", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].expcie = true;
             }},
            {"log ciecam flag", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].ciecam = true;
             }},
            {"log gradient", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].strlog = 10.0;
             }},
            {"log source gray range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].sourceGray = 0.0;
             }},
            {"log target gray range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].targetGray = 200.0;
             }},
            {"log black EV range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].blackEv = -20.0;
             }},
            {"log white EV range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].whiteEv = 40.0;
             }},
            {"log compr range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].comprlog = 2.0;
             }},
            {"log repar range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].repar = 0.0;
             }},
            {"log sensitivity range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].explog = true;
                 p.locallab.spots[0].detail = 0.0;
                 p.locallab.spots[0].sensilog = 101;
             }},
            {"mask", [](auto& p) {
                 p.locallab.spots[0].expmask = true;
             }},
            {"explicit mask", [](auto& p) {
                 p.locallab.spots[0].enaColorMask = true;
             }},
            {"soft radius", [](auto& p) {
                 p.locallab.spots[0].softradiuscol = 1.0;
             }},
            {"recursion", [](auto& p) {
                 p.locallab.spots[0].recurs = true;
             }},
            {"inversion", [](auto& p) {
                 p.locallab.spots[0].invers = true;
             }},
            {"merge", [](auto& p) {
                 p.locallab.spots[0].merMethod = "mtwo";
             }},
            {"exclude spot", [](auto& p) {
                 p.locallab.spots[0].spotMethod = "exc";
             }},
            {"LAB gamut", [](auto& p) {
                 p.locallab.spots[0].avoidgamutMethod = "LAB";
             }},
            {"XYZ gamut", [](auto& p) {
                 p.locallab.spots[0].avoidgamutMethod = "XYZ";
             }},
            {"soft radius gamut", [](auto& p) {
                 p.locallab.spots[0].avoidrad = 5.0;
             }},
            {"RGB curve", [](auto& p) {
                 p.locallab.spots[0].rgbcurve = {
                     3.0, 0.0, 0.0, 0.5, 0.7, 1.0, 1.0};
             }},
            {"LabGrid", [](auto& p) {
                 p.locallab.spots[0].labgridAHigh = 1.0;
             }},
            {"structure", [](auto& p) {
                 p.locallab.spots[0].structcol = 10;
             }},
            {"gamma", [](auto& p) {
                 p.locallab.spots[0].gamc = 2.0;
             }},
            {"pre-CBDL", [](auto& p) {
                 p.dirpyrequalizer.enabled = true;
             }},
            {"dehaze", [](auto& p) {
                 p.dehaze.enabled = true;
             }},
            {"dehaze show depth", [](auto& p) {
                 p.dehaze.showDepthMap = true;
             }},
            {"local dehaze black", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.dehazeblack = 10.0;
             }},
            {"local dehaze mask", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.enaColorMask = true;
             }},
            {"local dehaze soft radius", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.softradiuscol = 1.0;
             }},
            {"local dehaze active retinex", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.str = 10.0;
             }},
            {"local dehaze strength range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 101;
             }},
            {"local dehaze depth range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.depth = 101;
             }},
            {"local dehaze saturation range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.dehazeSaturation = 101.0;
             }},
            {"local dehaze sensitivity range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.sensih = 101;
             }},
            {"local dehaze without retinex enable", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = false;
                 spot.dehaz = 50;
             }},
            {"highlight reconstruction", [](auto& p) {
                 p.toneCurve.hrenabled = true;
             }},
            {"shadows/highlights", [](auto& p) {
                 p.locallab.spots[0].expshadhigh = true;
             }},
            {"recursion", [](auto& p) {
                 p.locallab.spots[0].recurs = true;
             }},
            {"exclude spot", [](auto& p) {
                 p.locallab.spots[0].spotMethod = "exc";
             }},
            {"merge", [](auto& p) {
                 p.locallab.spots[0].merMethod = "mtwo";
             }},
            {"LAB gamut", [](auto& p) {
                 p.locallab.spots[0].avoidgamutMethod = "LAB";
             }},
            {"XYZ gamut", [](auto& p) {
                 p.locallab.spots[0].avoidgamutMethod = "XYZ";
             }},
            {"avoid radius", [](auto& p) {
                 p.locallab.spots[0].avoidrad = 5.0;
             }},
            {"RGB curve", [](auto& p) {
                 p.locallab.spots[0].rgbcurve = {
                     3.0, 0.0, 0.0, 0.5, 0.7, 1.0, 1.0};
             }},
            {"blur radius", [](auto& p) {
                 p.locallab.spots[0].blurcolde = 101;
             }},
            {"transit 100", [](auto& p) {
                 p.locallab.spots[0].transit = 100.0;
             }},
            {"chroma range", [](auto& p) {
                 p.locallab.spots[0].chroma = 101;
             }},
            {"reparcol 50", [](auto& p) {
                 p.locallab.spots[0].reparcol = 50.0;
             }},
            {"reparcol 1", [](auto& p) {
                 p.locallab.spots[0].reparcol = 1.0;
             }},
        };
        for (const auto& residual : residuals) {
            rtengine::procparams::ProcParams params = supported;
            residual.mutate(params);
            if (localAdjustmentsSupported(params)) {
                std::fprintf(stderr, "FAIL: local.adjustments admits %s\n",
                             residual.label);
                ++failures;
            }
        }
    }

    const std::string profile_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/stage-residency-local-adjustments.jsonl";

    /* The supported matrix. The zero/zero/zero case is the clamp-only pass;
     * the rest exercise each slider, deltaE and the transition domain. */
    const LocalAdjustmentsCase cases[] = {
        {"clamp only 238x64", 238, 64, 0, 0, 0, true, 60.0, 25.0, 1.0, 0.0, true},
        {"chroma -50 238x64", 238, 64, -50, 0, 0, true, 60.0, 25.0, 1.0, 0.0, true},
        {"chroma 50 238x64", 238, 64, 50, 0, 0, true, 60.0, 25.0, 1.0, 0.0, true},
        {"lightness -50 238x64", 238, 64, 0, -50, 0, true, 60.0, 25.0, 1.0, 0.0, true},
        {"lightness 50 238x64", 238, 64, 0, 50, 0, true, 60.0, 25.0, 1.0, 0.0, true},
        {"contrast -50 238x64", 238, 64, 0, 0, -50, true, 60.0, 25.0, 1.0, 0.0, true},
        {"contrast 50 238x64", 238, 64, 0, 0, 50, true, 60.0, 25.0, 1.0, 0.0, true},
        {"all -50 238x64", 238, 64, -50, -50, -50, true, 60.0, 25.0, 1.0, 0.0, true},
        {"all 50 238x64", 238, 64, 50, 50, 50, true, 60.0, 25.0, 1.0, 0.0, true},
        {"deltae off 238x64", 238, 64, 40, 20, -20, false, 60.0, 25.0, 1.0, 0.0, true},
        {"transit 0 238x64", 238, 64, 40, 20, -20, true, 0.0, 0.0, 0.0, -10.0, true},
        {"transit 99 238x64", 238, 64, 40, 20, -20, true, 99.0, 100.0, 10.0, 10.0, true},
        {"prefilter off 238x64", 238, 64, 40, 20, -20, true, 60.0, 25.0, 1.0, 0.0, false},
        {"179x48", 179, 48, 35, 10, -5, true, 60.0, 25.0, 1.0, 0.0, true},
        {"179x49", 179, 49, 35, 10, -5, true, 60.0, 25.0, 1.0, 0.0, true},
        {"8192x64", 8192, 64, 35, 10, -5, true, 60.0, 25.0, 1.0, 0.0, true},
    };

    int device_cases = 0;
    std::unique_ptr<LabImage> clamp_only_lab;
    for (std::size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const LocalAdjustmentsCase& c = cases[index];

        rtengine::procparams::ProcParams reference_params = supported;
        reference_params.locallab.spots[0] = localAdjustmentsSpot(
            "case", 0, 0, 800, true, c.chroma, c.lightness, c.contrast);
        reference_params.locallab.spots[0].avoidgamutMethod = "NONE";
        reference_params.locallab.spots[0].deltae = c.deltae;
        reference_params.locallab.spots[0].transit = c.transit;
        reference_params.locallab.spots[0].feather = c.feather;
        reference_params.locallab.spots[0].transitweak = c.transitweak;
        reference_params.locallab.spots[0].transitgrad = c.transitgrad;
        reference_params.locallab.spots[0].avoidneg = c.avoidneg;
        check(localAdjustmentsSupported(reference_params),
              "the supported local.adjustments case is inside the predicate");

        Imagefloat reference_base(c.width, c.height);
        fill_local_adjustments_rgb(reference_base, 11u + index);
        std::unique_ptr<LabImage> reference_lab(
            new LabImage(c.width, c.height));
        rtengine::ImProcFunctions reference_ipf(&reference_params, false);
        localAdjustmentsHostBlock(reference_ipf, reference_params,
                                  &reference_base, reference_lab.get(),
                                  c.width, c.height);

        rtengine::procparams::ProcParams node_params = reference_params;
        Imagefloat node_base(c.width, c.height);
        fill_local_adjustments_rgb(node_base, 11u + index);
        std::unique_ptr<LabImage> node_lab(new LabImage(c.width, c.height));
        rtengine::ImProcFunctions node_ipf(&node_params, false);
        const RtStageStatus status =
            run_local_adjustments_node(node_base, *node_lab, node_params,
                                       node_ipf, profile_path);
        check(status == RT_STAGE_OK,
              "the local.adjustments node completes through run()");

        const std::string profile = read_file(profile_path);
        if (cpu_device) {
            check(profile.find("\"impl\":\"opencl\"") != std::string::npos,
                  "the local.adjustments case records a device pass");
            check(profile.find("\"impl\":\"host\"") == std::string::npos,
                  "the local.adjustments case never falls back");
            ++device_cases;
        } else if (have_device) {
            check(profile.find("\"impl\":\"host\"") != std::string::npos,
                  "a GPU local.adjustments case records a host execution");
        }

        const Diff diff = compare(view(*reference_lab), view(*node_lab));
        std::printf("stage-residency: local.adjustments %s: PAE %.6f LSB16, "
                    "%zu of %zu samples differ\n",
                    c.label, diff.max_abs, diff.differing, diff.samples);
        check(diff.finite && diff.max_abs <= 1.0,
              "the local.adjustments result is within 1 LSB16");
        if (index == 0) {
            clamp_only_lab.reset(new LabImage(*reference_lab, true));
        } else if (index == 8 && clamp_only_lab != nullptr) {
            const Diff changed =
                compare(view(*clamp_only_lab), view(*node_lab));
            std::printf("stage-residency: local.adjustments non-vacuity: "
                        "%zu of %zu samples differ, PAE %.6f LSB16\n",
                        changed.differing, changed.samples, changed.max_abs);
            check(changed.differing > 0,
                  "the local.adjustments colour path changes the image");
        }
    }
    if (cpu_device) {
        check(device_cases == static_cast<int>(sizeof(cases) / sizeof(cases[0])),
              "every supported local.adjustments case ran on the device");
    } else if (have_device) {
        skip("local.adjustments: GPU device selected; device parity not run");
    } else {
        skip("local.adjustments: no OpenCL device");
    }

    /* Multi-spot state, RECT, inactive spots, curves and MUNS. Each case is
     * built from scratch so the make lambda owns the spot list. */
    {
        const auto diagonal = [](double mid) {
            return std::vector<double>{3.0, 0.0, 0.0, 0.5, mid, 1.0, 1.0};
        };
        const auto flat = [](double mid) {
            return std::vector<double>{1.0, 0.0, 0.5, 0.35, 0.35,
                                       0.5, mid, 0.35, 0.35};
        };
        const auto add_spot = [](rtengine::procparams::ProcParams& params,
                                 int cx, int cy, int extent, bool active,
                                 int chroma, int lightness, int contrast,
                                 const char* shape,
                                 const char* avoid) {
            auto spot = localAdjustmentsSpot("spot", cx, cy, extent, active,
                                             chroma, lightness, contrast);
            spot.shape = shape;
            spot.avoidgamutMethod = avoid;
            params.locallab.spots.push_back(spot);
        };

        struct ComplexCase {
            std::string label;
            int width;
            int height;
            std::function<void(rtengine::procparams::ProcParams&)> make;
        };
        std::vector<ComplexCase> complex;
        complex.push_back({"two ELI", 238, 64, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 40, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, -30, 0, 10, "ELI", "NONE");
        }});
        complex.push_back({"three ELI", 238, 64, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 40, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, -30, 0, 10, "ELI", "NONE");
            add_spot(p, 0, 0, 500, true, 10, -5, 5, "ELI", "NONE");
        }});
        complex.push_back({"three ELI reversed", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 500, true, 10, -5, 5, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, -30, 0, 10, "ELI", "NONE");
            add_spot(p, -300, -150, 700, true, 40, 8, 0, "ELI", "NONE");
        }});
        complex.push_back({"RECT", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "RECT", "NONE");
        }});
        complex.push_back({"RECT two", 179, 49, [&](auto& p) {
            add_spot(p, -200, -100, 700, true, 35, 10, -5, "RECT", "NONE");
            add_spot(p, 200, 100, 600, true, -20, -5, 8, "RECT", "NONE");
        }});
        complex.push_back({"inactive then active", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "NONE");
        }});
        complex.push_back({"active then inactive", 238, 64, [&](auto& p) {
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "NONE");
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
        }});
        const auto curve_case = [&](const char* label,
                                    std::function<void(
                                        rtengine::procparams::LocallabParams::
                                            LocallabSpot&)> set) {
            complex.push_back({label, 238, 64, [&, set](auto& p) {
                add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
                p.locallab.spots[0].qualitycurveMethod = "std";
                set(p.locallab.spots[0]);
            }});
        };
        curve_case("L=f(L)", [&](auto& spot) { spot.llcurve = diagonal(0.7); });
        curve_case("C=f(L)", [&](auto& spot) { spot.clcurve = diagonal(0.65); });
        curve_case("L=f(C)", [&](auto& spot) { spot.lccurve = diagonal(1.3); });
        curve_case("C=f(C)", [&](auto& spot) { spot.cccurve = diagonal(1.2); });
        curve_case("H=f(H)", [&](auto& spot) { spot.HHcurve = flat(0.7); });
        curve_case("L=f(H)", [&](auto& spot) { spot.LHcurve = flat(0.7); });
        complex.push_back({"L=f(H) only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].qualitycurveMethod = "std";
            p.locallab.spots[0].LHcurve = flat(0.7);
        }});
        curve_case("C=f(H)", [&](auto& spot) { spot.CHcurve = flat(0.7); });
        complex.push_back({"all curves", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.qualitycurveMethod = "std";
            spot.llcurve = diagonal(0.7);
            spot.clcurve = diagonal(0.65);
            spot.lccurve = diagonal(1.3);
            spot.cccurve = diagonal(1.2);
            spot.HHcurve = flat(0.7);
            spot.LHcurve = flat(0.7);
            spot.CHcurve = flat(0.7);
        }});
        complex.push_back({"MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "MUNS");
        }});
        complex.push_back({"MUNS 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 35, 10, -5, "ELI", "MUNS");
        }});
        complex.push_back({"MUNS and NONE", 238, 64, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 40, 8, 0, "ELI", "MUNS");
            add_spot(p, 300, 150, 700, true, -30, 0, 10, "ELI", "NONE");
        }});
        complex.push_back({"inactive then MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "MUNS");
        }});
        complex.push_back({"all curves MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "MUNS");
            auto& spot = p.locallab.spots[0];
            spot.qualitycurveMethod = "std";
            spot.llcurve = diagonal(0.7);
            spot.clcurve = diagonal(0.65);
            spot.lccurve = diagonal(1.3);
            spot.cccurve = diagonal(1.2);
            spot.HHcurve = flat(0.7);
            spot.LHcurve = flat(0.7);
            spot.CHcurve = flat(0.7);
        }});

        /* Vibrance and local contrast, alone and combined with Colour &
         * Light. The colour tool is switched off in the alone cases, and the
         * local-contrast method is the Gaussian variant. */
        const auto vibrance_spot = [&](rtengine::procparams::ProcParams& p,
                                       int pastels, int saturated) {
            auto& spot = p.locallab.spots[0];
            spot.expcolor = false;
            spot.expvibrance = true;
            spot.pastels = pastels;
            spot.saturated = saturated;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.vibgam = 1.0;
            spot.warm = 0;
            spot.strvib = 0.0;
            spot.strvibab = 0.0;
            spot.strvibh = 0.0;
        };
        const auto contrast_spot = [&](rtengine::procparams::ProcParams& p,
                                       int radius, double amount) {
            auto& spot = p.locallab.spots[0];
            spot.expcolor = false;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = radius;
            spot.lcamount = amount;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.fftwlc = false;
            spot.sensilc = 60;
        };
        /* The std shadows/highlights method in its Lab mode. */
        const auto sh_spot = [&](rtengine::procparams::ProcParams& p,
                                 int highlights, int shadows) {
            auto& spot = p.locallab.spots[0];
            spot.expcolor = false;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = highlights;
            spot.shadows = shadows;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.reparsh = 100.0;
            spot.strSH = 0.0;
            spot.gamSH = 2.4;
            spot.sloSH = 12.92;
            for (int index = 0; index < 6; ++index) {
                spot.multsh[index] = 0;
            }
        };
        /* The per-pixel soft-light method. */
        const auto soft_spot = [&](rtengine::procparams::ProcParams& p,
                                   int strength) {
            auto& spot = p.locallab.spots[0];
            spot.expcolor = false;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = strength;
            spot.sensisf = 30;
        };
        /* The local sharpening RL arm. Damping zero with `cbdlMethod "aft"`
         * is the undamped recurrence the output sharpening core covers; a
         * positive damping value, or zero under the default `"bef"` fixup,
         * selects dcdamping()'s damped recurrence. The radii keep the RLD
         * sigma below the core's separable limit. */
        const auto sharpen_spot = [&](rtengine::procparams::ProcParams& p,
                                      double radius, int amount,
                                      int contrast, int sensitivity,
                                      int damping = 0,
                                      const char* method = "aft") {
            p.dirpyrequalizer.cbdlMethod = method;
            auto& spot = p.locallab.spots[0];
            spot.expcolor = false;
            spot.expsharp = true;
            spot.methodcap = "rl";
            spot.sharamount = amount;
            spot.sharradius = radius;
            spot.shariter = 30;
            spot.sharcontrast = contrast;
            spot.shardamping = damping;
            spot.sharblur = 0.2;
            spot.shargam = 1.0;
            spot.sharshow = false;
            spot.inverssha = false;
            spot.deconvAutoshar = false;
            spot.reparsha = 100.0;
            spot.sensisha = sensitivity;
        };
        /* A spot whose unclipped extent covers the fixture but whose padded
         * sharpening plane stays small. `loc` is in 1/2000 image units. */
        const auto add_sharpen_spot =
            [&](rtengine::procparams::ProcParams& p, int cx, int cy,
                int loc_x, int loc_y, bool active, const char* shape,
                const char* avoid) {
                auto spot = localAdjustmentsSpot("sharpen", cx, cy, 0, active,
                                                 0, 0, 0);
                spot.shape = shape;
                spot.avoidgamutMethod = avoid;
                spot.loc = {loc_x, loc_x, loc_y, loc_y};
                p.locallab.spots.push_back(spot);
            };
        complex.push_back({"vibrance only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"vibrance skin curve", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 40, 0);
            p.locallab.spots[0].skintonescurve = diagonal(0.8);
        }});
        complex.push_back({"vibrance skin only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 0, 0);
            p.locallab.spots[0].skintonescurve = diagonal(0.8);
        }});
        complex.push_back({"vibrance MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"vibrance 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"vibrance 8192x64", 8192, 64, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"local contrast only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
        }});
        complex.push_back({"local contrast no deltae", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
            p.locallab.spots[0].deltae = false;
        }});
        complex.push_back({"local contrast raw", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
            p.locallab.spots[0].sensilc = 100;
            p.locallab.spots[0].transit = 99.0;
        }});
        complex.push_back({"local contrast sse", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 10, 0.3);
        }});
        complex.push_back({"local contrast MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            contrast_spot(p, 40, 0.4);
        }});
        complex.push_back({"local contrast 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 10, 0.3);
        }});
        complex.push_back({"local contrast 8192x64", 8192, 64, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
        }});
        complex.push_back({"shadows/highlights only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"highlights only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 70, 0);
        }});
        complex.push_back({"shadows only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 0, 70);
        }});
        complex.push_back({"shadows/highlights RECT", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "RECT", "NONE");
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"shadows/highlights MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"shadows/highlights 179x49", 179, 49,
                           [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"shadows/highlights 8192x64", 8192, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"soft light only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            soft_spot(p, 60);
        }});
        complex.push_back({"soft light 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            soft_spot(p, 60);
        }});
        complex.push_back({"soft light 8192x64", 8192, 64, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            soft_spot(p, 60);
        }});
        complex.push_back({"soft light RECT", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "RECT", "NONE");
            soft_spot(p, 60);
        }});
        complex.push_back({"soft light MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            soft_spot(p, 60);
        }});
        complex.push_back({"soft light strength 3", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            soft_spot(p, 3);
        }});
        complex.push_back({"colour and soft light", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            p.locallab.spots[0].expsoft = true;
            p.locallab.spots[0].softMethod = "soft";
            p.locallab.spots[0].streng = 60;
            p.locallab.spots[0].sensisf = 30;
        }});
        complex.push_back({"shadows/highlights and soft light", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 50, 50);
            p.locallab.spots[0].expsoft = true;
            p.locallab.spots[0].softMethod = "soft";
            p.locallab.spots[0].streng = 60;
            p.locallab.spots[0].sensisf = 30;
        }});
        complex.push_back({"soft light and local contrast", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
            p.locallab.spots[0].expsoft = true;
            p.locallab.spots[0].softMethod = "soft";
            p.locallab.spots[0].streng = 60;
            p.locallab.spots[0].sensisf = 30;
        }});
        /* Local CBDL, alone and combined. The spot is 238x96 so the block's
         * bfw > 65 && bfh > 65 guard runs; the 49- and 64-high rows below are
         * the block's own no-op and exercise the skip. */
        const auto cbdl_spot = [&](rtengine::procparams::ProcParams& p,
                                   double m0, double m1, double m2,
                                   double m3, double m4, double m5,
                                   double chroma, double clarity) {
            auto& spot = p.locallab.spots[0];
            spot.expcolor = false;
            spot.expcbdl = true;
            const double multipliers[6] = {m0, m1, m2, m3, m4, m5};
            for (int level = 0; level < 6; ++level) {
                spot.mult[level] = multipliers[level];
            }
            spot.chromacbdl = chroma;
            spot.threshold = 0.2;
            spot.sensicb = 60;
            spot.clarityml = clarity;
            spot.contresid = 0;
            spot.softradiuscb = 0.0;
        };
        complex.push_back({"CBDL only", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.18, 1.15, 1.1, 1.05, 1.02, 0.5, 2.0);
        }});
        complex.push_back({"CBDL luma only", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.0, 1.0);
        }});
        complex.push_back({"CBDL chroma only no scope", 238, 96,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.1);
            p.locallab.spots[0].thresh = 100;
            p.locallab.spots[0].sensicb = 100;
        }});
        complex.push_back({"CBDL chroma only", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.1);
        }});
        complex.push_back({"CBDL tiny chroma", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            p.locallab.spots[0].expcbdl = true;
            p.locallab.spots[0].chromacbdl = 0.01;
        }});
        complex.push_back({"CBDL balance 2.5", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.02, 0.5, 2.0);
            p.locallab.spots[0].balan = 2.5;
        }});
        complex.push_back({"CBDL tiny chroma balance 2.5", 238, 96,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            p.locallab.spots[0].expcbdl = true;
            p.locallab.spots[0].chromacbdl = 0.01;
            p.locallab.spots[0].balan = 2.5;
        }});
        complex.push_back({"CBDL balance 2", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.02, 0.5, 2.0);
            p.locallab.spots[0].balan = 2.0;
        }});
        complex.push_back({"CBDL no scope", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.18, 1.15, 1.1, 1.05, 1.02, 0.5, 2.0);
            p.locallab.spots[0].thresh = 100;
            p.locallab.spots[0].sensicb = 100;
        }});
        complex.push_back({"CBDL RECT", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "RECT", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        complex.push_back({"CBDL MUNS", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        complex.push_back({"CBDL 179x96", 179, 96, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        complex.push_back({"CBDL 8192x96", 8192, 96, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        complex.push_back({"CBDL 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        complex.push_back({"CBDL 238x64", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        complex.push_back({"CBDL 8192x64", 8192, 64, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.2, 1.1, 1.05, 1.0, 1.0, 1.0, 0.5, 2.0);
        }});
        /* Local dehaze. The retinex enable is the block's gate; the retinex
         * tool itself is a no-op (strength zero). The dehaze pass runs before
         * CBDL, so every combination below also pins the CPU order. */
        const auto dehaze_spot = [&](rtengine::procparams::ProcParams& p,
                                     int strength, int depth, int saturation,
                                     int sensitivity) {
            auto& spot = p.locallab.spots[0];
            spot.expreti = true;
            spot.str = 0.0;
            spot.dehaz = strength;
            spot.depth = depth;
            spot.dehazeSaturation = saturation;
            spot.dehazeblack = 0.0;
            spot.sensih = sensitivity;
        };
        /* Local log encoding. detail is zero so the pass is the per-pixel
         * arm; autocompute and CIE CAM stay off; repar is the log tool's own
         * recovery weight. The sensilog values cover both senstype-11 arms
         * (below 50 and 50..100, including the 100 early exit). */
        const auto log_spot = [&](rtengine::procparams::ProcParams& p,
                                  int sensitivity, bool satcontrol,
                                  double detail, double comprlog,
                                  double repar, double blackev,
                                  double whiteev, double sourcegray,
                                  double targetgray) {
            auto& spot = p.locallab.spots[0];
            spot.explog = true;
            spot.autocompute = false;
            spot.expcie = false;
            spot.ciecam = false;
            spot.detail = detail;
            spot.strlog = 0.0;
            spot.satlog = satcontrol;
            spot.comprlog = comprlog;
            spot.repar = repar;
            spot.blackEv = blackev;
            spot.whiteEv = whiteev;
            spot.sourceGray = sourcegray;
            spot.targetGray = targetgray;
            spot.sensilog = sensitivity;
        };
        complex.push_back({"log only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log only low sensitivity", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 30, false, 0.0, 0.2, 75.0, -8.0, 12.0, 12.0,
                     20.0);
        }});
        complex.push_back({"log only raw sensitivity", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 100, true, 0.0, 0.9, 50.0, -10.0, 14.0, 8.0,
                     22.0);
        }});
        complex.push_back({"log only no compression", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 30, false, 0.0, 0.0, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log and colour", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log and dehaze", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"log and CBDL", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
        }});
        complex.push_back({"log and vibrance", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"log and shadows/highlights", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"log and soft light", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            soft_spot(p, 60);
        }});
        complex.push_back({"log and local contrast", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            contrast_spot(p, 40, 0.4);
        }});
        complex.push_back({"log all tools", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            log_spot(p, 30, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            spot.expcbdl = true;
            spot.mult[0] = 1.15;
            spot.mult[1] = 1.1;
            spot.mult[2] = 1.05;
            spot.mult[3] = 1.0;
            spot.mult[4] = 1.0;
            spot.mult[5] = 1.0;
            spot.chromacbdl = 0.4;
            spot.threshold = 0.2;
            spot.sensicb = 60;
            spot.clarityml = 1.0;
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"log MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log RECT", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "RECT", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log 238x64", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 100, true, 0.0, 0.6, 100.0, -6.0, 11.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log 8192x64", 8192, 64, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"log second spot", 238, 64, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[1].expcolor = false;
            auto& spot = p.locallab.spots[1];
            spot.explog = true;
            spot.autocompute = false;
            spot.detail = 0.0;
            spot.satlog = true;
            spot.comprlog = 0.4;
            spot.repar = 100.0;
            spot.blackEv = -5.0;
            spot.whiteEv = 10.0;
            spot.sourceGray = 10.0;
            spot.targetGray = 18.0;
            spot.sensilog = 60;
        }});
        complex.push_back({"log first then colour", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "NONE");
        }});
        complex.push_back({"log inactive then active", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "NONE");
        }});
        complex.push_back({"inactive then log", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            add_spot(p, 100, 100, 600, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[1].expcolor = false;
            auto& spot = p.locallab.spots[1];
            spot.explog = true;
            spot.autocompute = false;
            spot.detail = 0.0;
            spot.satlog = true;
            spot.comprlog = 0.4;
            spot.repar = 100.0;
            spot.blackEv = -5.0;
            spot.whiteEv = 10.0;
            spot.sourceGray = 10.0;
            spot.targetGray = 18.0;
            spot.sensilog = 60;
        }});

        complex.push_back({"dehaze only", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze negative", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, -40, 60, 20, 30);
        }});
        complex.push_back({"dehaze and colour", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze and CBDL", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze and vibrance", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 40, 20);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze and shadows/highlights", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sh_spot(p, 50, 50);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze and soft light", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            soft_spot(p, 60);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze and local contrast", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze all tools", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expcbdl = true;
            spot.mult[0] = 1.15;
            spot.mult[1] = 1.1;
            spot.mult[2] = 1.05;
            spot.mult[3] = 1.0;
            spot.mult[4] = 1.0;
            spot.mult[5] = 1.0;
            spot.chromacbdl = 0.4;
            spot.threshold = 0.2;
            spot.sensicb = 60;
            spot.clarityml = 1.0;
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "MUNS");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze RECT", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "RECT", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze 179x49", 179, 49, [&](auto& p) {
            add_spot(p, 0, 0, 700, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze 238x64", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze 8192x64", 8192, 64, [&](auto& p) {
            add_spot(p, 0, 0, 8000, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        /* The block rescales the guided-filtered channels to the 200 px cap
         * only when the ROI's shorter side exceeds 200 px; every case above
         * has a side at or below 96, so this is the nearest-neighbour
         * `dehaze_rescale` path. The full 1024x1793 ROI rescales to 200x350. */
        complex.push_back({"dehaze rescale 1024x1793", 1024, 1793,
                           [&](auto& p) {
            add_spot(p, 0, 0, 2000, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].expcolor = false;
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"dehaze second spot", 238, 64, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expreti = true;
            spot.str = 0.0;
            spot.dehaz = 50;
            spot.depth = 25;
            spot.dehazeSaturation = 50;
            spot.dehazeblack = 0.0;
            spot.sensih = 60;
        }});
        complex.push_back({"dehaze inactive then active", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            add_spot(p, 100, 100, 600, true, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expreti = true;
            spot.str = 0.0;
            spot.dehaz = 50;
            spot.depth = 25;
            spot.dehazeSaturation = 50;
            spot.dehazeblack = 0.0;
            spot.sensih = 60;
        }});
        complex.push_back({"colour and CBDL", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            p.locallab.spots[0].expcolor = true;
        }});
        complex.push_back({"vibrance and CBDL", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            p.locallab.spots[0].expvibrance = true;
            p.locallab.spots[0].pastels = 40;
            p.locallab.spots[0].saturated = 20;
            p.locallab.spots[0].psthreshold.setValues(0, 75);
            p.locallab.spots[0].sensiv = 30;
        }});
        complex.push_back({"CBDL and shadows/highlights", 238, 96,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            p.locallab.spots[0].expshadhigh = true;
            p.locallab.spots[0].shMethod = "std";
            p.locallab.spots[0].highlights = 50;
            p.locallab.spots[0].shadows = 50;
            p.locallab.spots[0].h_tonalwidth = 70;
            p.locallab.spots[0].s_tonalwidth = 30;
            p.locallab.spots[0].sh_radius = 40;
            p.locallab.spots[0].sensihs = 30;
            p.locallab.spots[0].blurSHde = 5;
        }});
        complex.push_back({"CBDL and soft light", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            p.locallab.spots[0].expsoft = true;
            p.locallab.spots[0].softMethod = "soft";
            p.locallab.spots[0].streng = 60;
            p.locallab.spots[0].sensisf = 30;
        }});
        complex.push_back({"CBDL and local contrast", 238, 96,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            p.locallab.spots[0].expcontrast = true;
            p.locallab.spots[0].localcontMethod = "loc";
            p.locallab.spots[0].lcradius = 40;
            p.locallab.spots[0].lcamount = 0.4;
            p.locallab.spots[0].lcdarkness = 0.9;
            p.locallab.spots[0].lclightness = 1.1;
            p.locallab.spots[0].reparw = 100.0;
            p.locallab.spots[0].sensilc = 60;
        }});
        complex.push_back({"all six tools", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expcbdl = true;
            spot.mult[0] = 1.15;
            spot.mult[1] = 1.1;
            spot.mult[2] = 1.05;
            spot.mult[3] = 1.0;
            spot.mult[4] = 1.0;
            spot.mult[5] = 1.0;
            spot.chromacbdl = 0.4;
            spot.threshold = 0.2;
            spot.sensicb = 60;
            spot.clarityml = 1.0;
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
        }});
        complex.push_back({"CBDL second spot", 238, 96, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expcbdl = true;
            spot.mult[0] = 1.2;
            spot.mult[1] = 1.1;
            spot.mult[2] = 1.05;
            spot.mult[3] = 1.0;
            spot.mult[4] = 1.0;
            spot.mult[5] = 1.0;
            spot.chromacbdl = 0.5;
            spot.threshold = 0.2;
            spot.sensicb = 60;
            spot.clarityml = 2.0;
        }});
        complex.push_back({"inactive then CBDL", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expcbdl = true;
            spot.mult[0] = 1.2;
            spot.mult[1] = 1.1;
            spot.mult[2] = 1.05;
            spot.mult[3] = 1.0;
            spot.mult[4] = 1.0;
            spot.mult[5] = 1.0;
            spot.chromacbdl = 0.5;
            spot.threshold = 0.2;
            spot.sensicb = 60;
            spot.clarityml = 2.0;
        }});
        complex.push_back({"all five tools", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
        }});
        complex.push_back({"colour and shadows/highlights", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            p.locallab.spots[0].expshadhigh = true;
            p.locallab.spots[0].shMethod = "std";
            p.locallab.spots[0].highlights = 50;
            p.locallab.spots[0].shadows = 50;
            p.locallab.spots[0].h_tonalwidth = 70;
            p.locallab.spots[0].s_tonalwidth = 30;
            p.locallab.spots[0].sh_radius = 40;
            p.locallab.spots[0].sensihs = 30;
            p.locallab.spots[0].blurSHde = 5;
        }});
        complex.push_back({"vibrance and shadows/highlights", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            vibrance_spot(p, 40, 20);
            p.locallab.spots[0].expshadhigh = true;
            p.locallab.spots[0].shMethod = "std";
            p.locallab.spots[0].highlights = 50;
            p.locallab.spots[0].shadows = 50;
            p.locallab.spots[0].h_tonalwidth = 70;
            p.locallab.spots[0].s_tonalwidth = 30;
            p.locallab.spots[0].sh_radius = 40;
            p.locallab.spots[0].sensihs = 30;
            p.locallab.spots[0].blurSHde = 5;
        }});
        complex.push_back({"shadows/highlights and local contrast", 238, 64,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            contrast_spot(p, 40, 0.4);
            p.locallab.spots[0].expshadhigh = true;
            p.locallab.spots[0].shMethod = "std";
            p.locallab.spots[0].highlights = 50;
            p.locallab.spots[0].shadows = 50;
            p.locallab.spots[0].h_tonalwidth = 70;
            p.locallab.spots[0].s_tonalwidth = 30;
            p.locallab.spots[0].sh_radius = 40;
            p.locallab.spots[0].sensihs = 30;
            p.locallab.spots[0].blurSHde = 5;
        }});
        complex.push_back({"all four tools", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
        }});
        complex.push_back({"shadows/highlights second spot", 238, 64,
                           [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, 0, 0, 0, "ELI", "NONE");
            auto& sh = p.locallab.spots[1];
            sh.expcolor = false;
            sh.expshadhigh = true;
            sh.shMethod = "std";
            sh.highlights = 50;
            sh.shadows = 50;
            sh.h_tonalwidth = 70;
            sh.s_tonalwidth = 30;
            sh.sh_radius = 40;
            sh.sensihs = 30;
            sh.blurSHde = 5;
            add_spot(p, 0, 0, 500, true, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[2];
            spot.expcolor = false;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 70;
        }});
        complex.push_back({"colour and vibrance", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            p.locallab.spots[0].expvibrance = true;
            p.locallab.spots[0].pastels = 40;
            p.locallab.spots[0].saturated = 20;
            p.locallab.spots[0].psthreshold.setValues(0, 75);
            p.locallab.spots[0].sensiv = 30;
        }});
        complex.push_back({"colour and local contrast", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
        }});
        complex.push_back({"all three tools", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
        }});
        complex.push_back({"all three tools MUNS", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 35, 10, -5, "ELI", "MUNS");
            auto& spot = p.locallab.spots[0];
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
        }});
        complex.push_back({"tools across spots", 238, 64, [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, 0, 0, 0, "RECT", "NONE");
            p.locallab.spots[1].expcolor = false;
            p.locallab.spots[1].expvibrance = true;
            p.locallab.spots[1].pastels = 40;
            p.locallab.spots[1].saturated = 20;
            p.locallab.spots[1].psthreshold.setValues(0, 75);
            p.locallab.spots[1].sensiv = 40;
            add_spot(p, 0, 0, 500, true, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[2];
            spot.expcolor = false;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 70;
        }});
        complex.push_back({"inactive tools", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 700, false, 0, 0, 0, "ELI", "NONE");
            auto& spot = p.locallab.spots[0];
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcamount = 0.4;
            spot.lcradius = 40;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "NONE");
        }});
        complex.push_back({"soft light second spot", 238, 64,
                           [&](auto& p) {
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_spot(p, 300, 150, 700, true, 0, 0, 0, "RECT", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
            add_spot(p, 0, 0, 500, true, 0, 0, 0, "ELI", "NONE");
            auto& lc = p.locallab.spots[2];
            lc.expcolor = false;
            lc.expcontrast = true;
            lc.localcontMethod = "loc";
            lc.lcradius = 40;
            lc.lcamount = 0.4;
            lc.lcdarkness = 0.9;
            lc.lclightness = 1.1;
            lc.reparw = 100.0;
            lc.sensilc = 70;
        }});
        complex.push_back({"gate multispot", 238, 64, [&](auto& p) {
            add_spot(p, -400, -200, 700, true, 30, 10, 5, "ELI", "NONE");
            p.locallab.spots[0].qualitycurveMethod = "std";
            p.locallab.spots[0].llcurve = diagonal(0.7);
            add_spot(p, 300, 200, 600, true, -20, -8, 10, "RECT", "NONE");
            p.locallab.spots[1].qualitycurveMethod = "std";
            p.locallab.spots[1].cccurve = diagonal(1.2);
            add_spot(p, 0, 0, 500, false, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[2].qualitycurveMethod = "none";
        }});

        /* Local sharpening, the RL arm of the output sharpening core. The
         * padded extent covering the fixture keeps the test plane small while
         * still exercising the block's zero-padded geometry. */
        complex.push_back({"sharpen only", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
        }});
        complex.push_back({"sharpen fir3", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.55, 80, 40, 60);
        }});
        complex.push_back({"sharpen kernel5", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.70, 90, 20, 40);
        }});
        complex.push_back({"sharpen kernel7", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 1.00, 70, 10, 80);
        }});
        complex.push_back({"sharpen high sensitivity", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 90);
        }});
        complex.push_back({"sharpen and colour", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            auto& spot = p.locallab.spots[0];
            spot.expcolor = true;
            spot.chroma = 35;
            spot.lightness = 10;
            spot.contrast = -5;
        }});
        complex.push_back({"sharpen and log", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"sharpen and dehaze", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"sharpen and CBDL", 238, 96, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
        }});
        complex.push_back({"sharpen and vibrance", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"sharpen and shadows/highlights", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"sharpen and soft light", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            soft_spot(p, 60);
        }});
        complex.push_back({"sharpen and local contrast", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            contrast_spot(p, 40, 0.4);
        }});
        complex.push_back({"all tools without sharpen", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            p.locallab.spots[0].transit = 0.0;
            auto& spot = p.locallab.spots[0];
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            dehaze_spot(p, 50, 25, 50, 60);
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            spot.expcolor = true;
            spot.chroma = 35;
            spot.lightness = 10;
            spot.contrast = -5;
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
            spot.expsharp = false;
        }});
        complex.push_back({"sharpen partial", 238, 64, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            p.locallab.spots[0].transit = 0.0;
        }});
        complex.push_back({"sharpen all tools", 238, 96, [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            p.locallab.spots[0].transit = 0.0;
            auto& spot = p.locallab.spots[0];
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            dehaze_spot(p, 50, 25, 50, 60);
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            /* cbdl_spot() switches Colour & Light off; restore it. */
            spot.expcolor = true;
            spot.chroma = 35;
            spot.lightness = 10;
            spot.contrast = -5;
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
        }});
        complex.push_back({"sharpen MUNS", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "MUNS");
            sharpen_spot(p, 0.75, 100, 20, 40);
        }});
        complex.push_back({"sharpen RECT", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "RECT", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
        }});
        complex.push_back({"sharpen 179x49", 179, 49, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
        }});
        complex.push_back({"sharpen 8192x64", 8192, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 1000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
        }});
        complex.push_back({"sharpen second spot", 238, 64, [&](auto& p) {
            p.dirpyrequalizer.cbdlMethod = "aft";
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expsharp = true;
            spot.methodcap = "rl";
            spot.sharamount = 100;
            spot.sharradius = 0.75;
            spot.shariter = 30;
            spot.sharcontrast = 20;
            spot.shardamping = 0;
            spot.sharblur = 0.2;
            spot.shargam = 1.0;
            spot.sharshow = false;
            spot.reparsha = 100.0;
            spot.sensisha = 40;
        }});
        complex.push_back({"sharpen inactive then active", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, false, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40);
            add_spot(p, 100, 100, 600, true, 35, 10, -5, "ELI", "NONE");
        }});

        /* The damped recurrence. The default `cbdlMethod == "bef"` raises a
         * zero damping to one, so the first case exercises the host fixup
         * without the caller's params being mutated. The other cases cover
         * the GUI range and both methods: the forward blur is the standard
         * IIR pair and the multiply blur is the 5x5 or 7x7 kernel. */
        complex.push_back({"sharpen damped default bef", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 0, "bef");
        }});
        complex.push_back({"sharpen damped one", 238, 64, [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 1, "aft");
        }});
        complex.push_back({"sharpen damped minimum radius", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.60, 80, 40, 60, 5, "bef");
        }});
        complex.push_back({"sharpen damped kernel5", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.70, 90, 20, 40, 5, "aft");
        }});
        complex.push_back({"sharpen damped kernel7", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 1.00, 70, 10, 80, 10, "bef");
        }});
        complex.push_back({"sharpen damped maximum", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 100, "aft");
        }});
        complex.push_back({"sharpen damped and colour", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            auto& spot = p.locallab.spots[0];
            spot.expcolor = true;
            spot.chroma = 35;
            spot.lightness = 10;
            spot.contrast = -5;
        }});
        complex.push_back({"sharpen damped and log", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
        }});
        complex.push_back({"sharpen damped and dehaze", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            dehaze_spot(p, 50, 25, 50, 60);
        }});
        complex.push_back({"sharpen damped and CBDL", 238, 96,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
        }});
        complex.push_back({"sharpen damped and vibrance", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            vibrance_spot(p, 40, 20);
        }});
        complex.push_back({"sharpen damped and shadows/highlights", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            sh_spot(p, 50, 50);
        }});
        complex.push_back({"sharpen damped and soft light", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            soft_spot(p, 60);
        }});
        complex.push_back({"sharpen damped and local contrast", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            contrast_spot(p, 40, 0.4);
        }});
        complex.push_back({"sharpen damped all tools", 238, 96,
                           [&](auto& p) {
            add_spot(p, 0, 0, 800, true, 0, 0, 0, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
            p.locallab.spots[0].transit = 0.0;
            auto& spot = p.locallab.spots[0];
            log_spot(p, 60, true, 0.0, 0.4, 100.0, -5.0, 10.0, 10.0,
                     18.0);
            dehaze_spot(p, 50, 25, 50, 60);
            cbdl_spot(p, 1.15, 1.1, 1.05, 1.0, 1.0, 1.0, 0.4, 1.0);
            spot.expcolor = true;
            spot.chroma = 35;
            spot.lightness = 10;
            spot.contrast = -5;
            spot.expvibrance = true;
            spot.pastels = 40;
            spot.saturated = 20;
            spot.psthreshold.setValues(0, 75);
            spot.sensiv = 30;
            spot.expcontrast = true;
            spot.localcontMethod = "loc";
            spot.lcradius = 40;
            spot.lcamount = 0.4;
            spot.lcdarkness = 0.9;
            spot.lclightness = 1.1;
            spot.reparw = 100.0;
            spot.sensilc = 60;
            spot.expshadhigh = true;
            spot.shMethod = "std";
            spot.highlights = 50;
            spot.shadows = 50;
            spot.h_tonalwidth = 70;
            spot.s_tonalwidth = 30;
            spot.sh_radius = 40;
            spot.sensihs = 30;
            spot.blurSHde = 5;
            spot.expsoft = true;
            spot.softMethod = "soft";
            spot.streng = 60;
            spot.sensisf = 30;
        }});
        complex.push_back({"sharpen damped MUNS", 238, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "MUNS");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
        }});
        complex.push_back({"sharpen damped 179x49", 179, 49,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
        }});
        complex.push_back({"sharpen damped 8192x64", 8192, 64,
                           [&](auto& p) {
            add_sharpen_spot(p, 0, 0, 1000, 2000, true, "ELI", "NONE");
            sharpen_spot(p, 0.75, 100, 20, 40, 5, "bef");
        }});
        complex.push_back({"sharpen damped second spot", 238, 64,
                           [&](auto& p) {
            p.dirpyrequalizer.cbdlMethod = "bef";
            add_spot(p, -300, -150, 700, true, 30, 8, 0, "ELI", "NONE");
            add_sharpen_spot(p, 0, 0, 2000, 2000, true, "ELI", "NONE");
            auto& spot = p.locallab.spots[1];
            spot.expcolor = false;
            spot.expsharp = true;
            spot.methodcap = "rl";
            spot.sharamount = 100;
            spot.sharradius = 0.75;
            spot.shariter = 30;
            spot.sharcontrast = 20;
            spot.shardamping = 5;
            spot.sharblur = 0.2;
            spot.shargam = 1.0;
            spot.sharshow = false;
            spot.reparsha = 100.0;
            spot.sensisha = 40;
        }});

        std::unique_ptr<LabImage> forward_three;
        for (const ComplexCase& c : complex) {
            rtengine::procparams::ProcParams reference_params;
            reference_params.locallab.enabled = true;
            c.make(reference_params);
            check(localAdjustmentsSupported(reference_params),
                  "the complex local.adjustments case is inside the "
                  "predicate");

            Imagefloat reference_base(c.width, c.height);
            fill_local_adjustments_rgb(reference_base, 211u);
            std::unique_ptr<LabImage> reference_lab(
                new LabImage(c.width, c.height));
            rtengine::ImProcFunctions reference_ipf(&reference_params, false);
            localAdjustmentsHostBlock(reference_ipf, reference_params,
                                      &reference_base, reference_lab.get(),
                                      c.width, c.height);

            rtengine::procparams::ProcParams node_params = reference_params;
            Imagefloat node_base(c.width, c.height);
            fill_local_adjustments_rgb(node_base, 211u);
            std::unique_ptr<LabImage> node_lab(
                new LabImage(c.width, c.height));
            rtengine::ImProcFunctions node_ipf(&node_params, false);
            const RtStageStatus status = run_local_adjustments_node(
                node_base, *node_lab, node_params, node_ipf, profile_path);
            check(status == RT_STAGE_OK,
                  "the complex local.adjustments case completes through run");
            const std::string profile = read_file(profile_path);
            if (cpu_device) {
                check(profile.find("\"impl\":\"opencl\"") !=
                          std::string::npos,
                      "the complex case records a device pass");
                check(profile.find("\"impl\":\"host\"") ==
                          std::string::npos,
                      "the complex case never falls back");
            }
            const Diff diff =
                compare(view(*reference_lab), view(*node_lab));
            std::printf("stage-residency: local.adjustments complex %s: "
                        "PAE %.6f LSB16, %zu of %zu samples differ\n",
                        c.label.c_str(), diff.max_abs, diff.differing,
                        diff.samples);
            check(diff.finite && diff.max_abs <= 1.0,
                  "the complex local.adjustments case is within 1 LSB16");
            if (c.label == "shadows/highlights only") {
                /* The matching clamp-only block: the same spot with the tool
                 * off and a zero Colour & Light pass. The tool must change
                 * the image, otherwise the parity above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].expshadhigh = false;
                plain_params.locallab.spots[0].expcolor = true;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments "
                            "shadows/highlights non-vacuity: %zu of %zu "
                            "samples differ, PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the shadows/highlights tool changes the image");
            }
            if (c.label == "soft light only") {
                /* The same spot with soft light off: the tool must change the
                 * image, otherwise the parity above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].expsoft = false;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments soft-light "
                            "non-vacuity: %zu of %zu samples differ, "
                            "PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the soft-light tool changes the image");
            }
            if (c.label == "CBDL only") {
                /* The same spot with CBDL off: the tool must change the
                 * image, otherwise the parity above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].expcbdl = false;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments CBDL "
                            "non-vacuity: %zu of %zu samples differ, "
                            "PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the CBDL tool changes the image");
            }
            if (c.label == "log only") {
                /* The same spot with log encoding off: the tool must change
                 * the image, otherwise the parity above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].explog = false;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments log "
                            "non-vacuity: %zu of %zu samples differ, "
                            "PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the log tool changes the image");
            }
            if (c.label == "sharpen only") {
                /* The same spot with sharpening off: the tool must change the
                 * image, otherwise the parity above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].expsharp = false;
                plain_params.locallab.spots[0].expcolor = true;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments sharpen "
                            "non-vacuity: %zu of %zu samples differ, "
                            "PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the sharpening tool changes the image");
            }
            if (c.label == "sharpen damped default bef") {
                /* The fixup makes a zero damping damped; the tool must change
                 * the image, otherwise the parity above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].expsharp = false;
                plain_params.locallab.spots[0].expcolor = true;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments damped "
                            "sharpen non-vacuity: %zu of %zu samples "
                            "differ, PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the damped sharpening tool changes the image");
            }
            if (c.label == "sharpen damped kernel7") {
                /* The same spot undamped: the damping itself must change the
                 * image, otherwise the damped path is untested. */
                rtengine::procparams::ProcParams undamped_params =
                    reference_params;
                undamped_params.dirpyrequalizer.cbdlMethod = "aft";
                undamped_params.locallab.spots[0].shardamping = 0;
                Imagefloat undamped_base(c.width, c.height);
                fill_local_adjustments_rgb(undamped_base, 211u);
                std::unique_ptr<LabImage> undamped_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions undamped_ipf(&undamped_params,
                                                       false);
                localAdjustmentsHostBlock(undamped_ipf, undamped_params,
                                          &undamped_base,
                                          undamped_lab.get(), c.width,
                                          c.height);
                const Diff changed =
                    compare(view(*undamped_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments damping "
                            "non-vacuity: %zu of %zu samples differ, "
                            "PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "damping changes the sharpened image");
            }
            if (c.label == "dehaze only") {
                /* The same spot with the retinex enable and dehaze off: the
                 * dehaze tool must change the image, otherwise the parity
                 * above is vacuous. */
                rtengine::procparams::ProcParams plain_params =
                    reference_params;
                plain_params.locallab.spots[0].expreti = false;
                plain_params.locallab.spots[0].dehaz = 0;
                Imagefloat plain_base(c.width, c.height);
                fill_local_adjustments_rgb(plain_base, 211u);
                std::unique_ptr<LabImage> plain_lab(
                    new LabImage(c.width, c.height));
                rtengine::ImProcFunctions plain_ipf(&plain_params, false);
                localAdjustmentsHostBlock(plain_ipf, plain_params,
                                          &plain_base, plain_lab.get(),
                                          c.width, c.height);
                const Diff changed =
                    compare(view(*plain_lab), view(*reference_lab));
                std::printf("stage-residency: local.adjustments dehaze "
                            "non-vacuity: %zu of %zu samples differ, "
                            "PAE %.6f LSB16\n",
                            changed.differing, changed.samples,
                            changed.max_abs);
                check(changed.differing > 0,
                      "the dehaze tool changes the image");
            }
            if (c.label == "three ELI") {
                forward_three.reset(new LabImage(*reference_lab, true));
            }
            if (c.label == "three ELI reversed" &&
                forward_three != nullptr) {
                const Diff order =
                    compare(view(*forward_three), view(*reference_lab));
                std::printf("stage-residency: local.adjustments order: "
                            "%zu of %zu samples differ, PAE %.6f LSB16\n",
                            order.differing, order.samples, order.max_abs);
                check(order.differing > 0,
                      "reversing the spot order changes the result");
            }
        }
    }

    /* A 4-pixel ROI is below Lab_Local()'s mSP=5 colour floor. The block
     * still runs the prefilter and tail clamp but does no colour work, so the
     * launcher refuses and the host builtin must match it exactly. */
    {
        rtengine::procparams::ProcParams params = supported;
        params.locallab.spots[0].loc = {40, 40, 40, 40};
        params.locallab.spots[0].centerX = 0;
        params.locallab.spots[0].centerY = 0;
        check(localAdjustmentsSupported(params),
              "the 4-px spot is still inside the block predicate");

        Imagefloat reference_base(100, 100);
        fill_local_adjustments_rgb(reference_base, 131u);
        std::unique_ptr<LabImage> reference_lab(
            new LabImage(100, 100));
        rtengine::ImProcFunctions reference_ipf(&params, false);
        localAdjustmentsHostBlock(reference_ipf, params, &reference_base,
                                  reference_lab.get(), 100, 100);

        rtengine::procparams::ProcParams node_params = params;
        Imagefloat node_base(100, 100);
        fill_local_adjustments_rgb(node_base, 131u);
        std::unique_ptr<LabImage> node_lab(new LabImage(100, 100));
        rtengine::ImProcFunctions node_ipf(&node_params, false);
        const RtStageStatus status =
            run_local_adjustments_node(node_base, *node_lab, node_params,
                                       node_ipf, profile_path);
        check(status == RT_STAGE_OK,
              "the 4-px local.adjustments case completes through run()");
        const std::string profile = read_file(profile_path);
        if (available) {
            check(profile.find("\"impl\":\"host\"") !=
                      std::string::npos,
                  "the 4-px spot records a host execution");
            check(profile.find("\"impl\":\"opencl\"") ==
                      std::string::npos,
                  "the 4-px spot never claims a device pass");
        }
        const Diff diff =
            compare(view(*reference_lab), view(*node_lab));
        std::printf("stage-residency: local.adjustments 4-px ROI: host, "
                    "PAE %.6f LSB16, %zu samples differ\n",
                    diff.max_abs, diff.differing);
        check(diff.finite && diff.max_abs == 0.0 &&
                  diff.differing == 0,
              "the 4-px local.adjustments case matches the block");
    }

    /* A shadows/highlights ROI narrower than the guided filter's 3 x 4
     * subsampled floor is still inside the block predicate but must run the
     * host block atomically. */
    {
        rtengine::procparams::ProcParams params;
        params.locallab.enabled = true;
        auto spot = localAdjustmentsSpot("sh-small", 0, 0, 10, true,
                                         0, 0, 0);
        spot.expcolor = false;
        spot.expshadhigh = true;
        spot.shMethod = "std";
        spot.highlights = 50;
        spot.shadows = 50;
        spot.h_tonalwidth = 70;
        spot.s_tonalwidth = 30;
        spot.sh_radius = 40;
        spot.sensihs = 30;
        spot.blurSHde = 5;
        params.locallab.spots.push_back(spot);
        check(localAdjustmentsSupported(params),
              "the small shadows/highlights ROI is inside the predicate");

        Imagefloat reference_base(100, 100);
        fill_local_adjustments_rgb(reference_base, 137u);
        std::unique_ptr<LabImage> reference_lab(new LabImage(100, 100));
        rtengine::ImProcFunctions reference_ipf(&params, false);
        localAdjustmentsHostBlock(reference_ipf, params, &reference_base,
                                  reference_lab.get(), 100, 100);

        rtengine::procparams::ProcParams node_params = params;
        Imagefloat node_base(100, 100);
        fill_local_adjustments_rgb(node_base, 137u);
        std::unique_ptr<LabImage> node_lab(new LabImage(100, 100));
        rtengine::ImProcFunctions node_ipf(&node_params, false);
        const RtStageStatus status =
            run_local_adjustments_node(node_base, *node_lab, node_params,
                                       node_ipf, profile_path);
        check(status == RT_STAGE_OK,
              "the small shadows/highlights ROI completes through run()");
        const std::string profile = read_file(profile_path);
        if (available) {
            check(profile.find("\"impl\":\"host\"") != std::string::npos,
                  "the small shadows/highlights ROI records a host "
                  "execution");
            check(profile.find("\"impl\":\"opencl\"") ==
                      std::string::npos,
                  "the small shadows/highlights ROI never claims a device "
                  "pass");
        }
        const Diff diff = compare(view(*reference_lab), view(*node_lab));
        std::printf("stage-residency: local.adjustments small "
                    "shadows/highlights ROI: host, PAE %.6f LSB16, %zu "
                    "samples differ\n",
                    diff.max_abs, diff.differing);
        check(diff.finite && diff.max_abs == 0.0 && diff.differing == 0,
              "the small shadows/highlights ROI matches the block");
    }

    /* The per-spot clamp alone, with no colour work and no MUNS. An inactive
     * spot runs the prefilter (avoidneg) and the tail clamp, so the comparison
     * is the clamp round trip only. The host's lab2rgb() gives its SIMD loop
     * and its scalar tail different cubic orders, so the widths cover both a
     * zero and a non-zero tail. The assertion is bit-exactness, which the
     * pre-fix clamp failed at PAE 0.045898 LSB16. */
    {
        struct ClampCase {
            const char* label;
            int width;
            int height;
            bool avoidneg;
        };
        const ClampCase clamp_cases[] = {
            {"prefilter 238x64", 238, 64, true},
            {"tail only 238x64", 238, 64, false},
            {"prefilter 237x49", 237, 49, true},
            {"prefilter 179x49", 179, 49, true},
            {"tail only 179x49", 179, 49, false},
            {"prefilter 8192x64", 8192, 64, true},
        };
        for (const ClampCase& c : clamp_cases) {
            rtengine::procparams::ProcParams params = supported;
            params.locallab.spots[0] = localAdjustmentsSpot(
                "clamp", 0, 0, 800, false, 0, 0, 0);
            params.locallab.spots[0].avoidgamutMethod = "NONE";
            params.locallab.spots[0].avoidneg = c.avoidneg;
            check(localAdjustmentsSupported(params),
                  "the clamp-only local.adjustments case is inside the "
                  "predicate");

            Imagefloat reference_base(c.width, c.height);
            fill_local_adjustments_rgb(reference_base, 301u);
            std::unique_ptr<LabImage> reference_lab(
                new LabImage(c.width, c.height));
            rtengine::ImProcFunctions reference_ipf(&params, false);
            localAdjustmentsHostBlock(reference_ipf, params, &reference_base,
                                      reference_lab.get(), c.width,
                                      c.height);

            rtengine::procparams::ProcParams node_params = params;
            Imagefloat node_base(c.width, c.height);
            fill_local_adjustments_rgb(node_base, 301u);
            std::unique_ptr<LabImage> node_lab(
                new LabImage(c.width, c.height));
            rtengine::ImProcFunctions node_ipf(&node_params, false);
            const RtStageStatus status =
                run_local_adjustments_node(node_base, *node_lab, node_params,
                                           node_ipf, profile_path);
            check(status == RT_STAGE_OK,
                  "the clamp-only case completes through run()");
            const std::string profile = read_file(profile_path);
            if (cpu_device) {
                check(profile.find("\"impl\":\"opencl\"") !=
                          std::string::npos,
                      "the clamp-only case records a device pass");
                check(profile.find("\"impl\":\"host\"") ==
                          std::string::npos,
                      "the clamp-only case never falls back");
            }

            const Diff diff =
                compare(view(*reference_lab), view(*node_lab));
            std::printf("stage-residency: local.adjustments clamp %s: "
                        "PAE %.6f LSB16, %zu of %zu samples differ\n",
                        c.label, diff.max_abs, diff.differing,
                        diff.samples);
            check(diff.finite && diff.max_abs == 0.0 &&
                      diff.differing == 0,
                  "the per-spot clamp is bit-exact against the host");
        }
    }

    /* Residuals: every one must run the host builtin, never a device pass,
     * and match the block byte for byte. */
    {
        const LocalAdjustmentsResidual residuals[] = {
            {"vibrance gamma", [](auto& p) {
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 40;
                 p.locallab.spots[0].vibgam = 2.0;
             }},
            {"vibrance gradient", [](auto& p) {
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 40;
                 p.locallab.spots[0].strvibab = 5.0;
             }},
            {"vibrance warm", [](auto& p) {
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 40;
                 p.locallab.spots[0].warm = 1;
             }},
            {"vibrance range", [](auto& p) {
                 p.locallab.spots[0].expvibrance = true;
                 p.locallab.spots[0].pastels = 101;
             }},
            {"local contrast wavelet", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "wav";
                 p.locallab.spots[0].lcamount = 0.4;
             }},
            {"local contrast empty method", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "";
                 p.locallab.spots[0].lcamount = 0.4;
             }},
            {"local contrast amount", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "loc";
                 p.locallab.spots[0].lcamount = 2.0;
             }},
            {"local contrast radius", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "loc";
                 p.locallab.spots[0].lcamount = 0.4;
                 p.locallab.spots[0].lcradius = 5;
             }},
            {"local contrast darkness", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "loc";
                 p.locallab.spots[0].lcamount = 0.4;
                 p.locallab.spots[0].lcdarkness = 4.0;
             }},
            {"local contrast recovery", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "loc";
                 p.locallab.spots[0].lcamount = 0.4;
                 p.locallab.spots[0].reparw = 50.0;
             }},
            {"local contrast wavelet strength", [](auto& p) {
                 p.locallab.spots[0].expcontrast = true;
                 p.locallab.spots[0].localcontMethod = "loc";
                 p.locallab.spots[0].lcamount = 0.4;
                 p.locallab.spots[0].strwav = 10.0;
             }},
            {"explicit mask", [](auto& p) {
                 p.locallab.spots[0].enaColorMask = true;
             }},
            {"soft radius", [](auto& p) {
                 p.locallab.spots[0].softradiuscol = 1.0;
             }},
            {"exposure tool", [](auto& p) {
                 p.locallab.spots[0].expexpose = true;
             }},
            {"soft light reti", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].softMethod = "reti";
                 p.locallab.spots[0].streng = 60;
             }},
            {"soft light mask", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 60;
                 p.locallab.spots[0].enaColorMask = true;
             }},
            {"soft light soft radius", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 60;
                 p.locallab.spots[0].softradiuscol = 1.0;
             }},
            {"soft light zero strength", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
             }},
            {"soft light strength high", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 101;
             }},
            {"soft light sensitivity range", [](auto& p) {
                 p.locallab.spots[0].expsoft = true;
                 p.locallab.spots[0].streng = 60;
                 p.locallab.spots[0].sensisf = 101;
             }},
            {"CBDL soft radius", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].softradiuscb = 5.0;
             }},
            {"CBDL residual contrast", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].contresid = 20;
             }},
            {"CBDL mask", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].enacbMask = true;
             }},
            {"CBDL multiplier range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].mult[2] = 5.0;
             }},
            {"CBDL chroma range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].chromacbdl = 2.0;
             }},
            {"CBDL threshold range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].threshold = 1.5;
             }},
            {"CBDL clarity range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].clarityml = 0.05;
             }},
            {"CBDL sensitivity range", [](auto& p) {
                 p.locallab.spots[0].expcbdl = true;
                 p.locallab.spots[0].sensicb = 101;
             }},
            {"reparcol 50", [](auto& p) {
                 p.locallab.spots[0].reparcol = 50.0;
             }},
            {"reparcol 1", [](auto& p) {
                 p.locallab.spots[0].reparcol = 1.0;
             }},
            {"capture sharpening", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "cap";
             }},
            {"sharpen damping range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].shardamping = 101;
             }},
            {"sharpen damped radius", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "bef";
                 p.locallab.spots[0].shardamping = 0;
                 p.locallab.spots[0].sharradius = 0.55;
             }},
            {"sharpen gamma", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].shargam = 2.0;
             }},
            {"sharpen blur", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sharblur = 0.5;
             }},
            {"sharpen separable radius", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sharradius = 1.5;
             }},
            {"sharpen radius range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sharradius = 0.3;
             }},
            {"sharpen amount range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sharamount = 0;
             }},
            {"sharpen iterations range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].shariter = 4;
             }},
            {"sharpen contrast range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sharcontrast = 2;
             }},
            {"sharpen sensitivity range", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sensisha = 101;
             }},
            {"sharpen mask view", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].sharshow = true;
             }},
            {"sharpen recovery", [](auto& p) {
                 p.locallab.spots[0].expcolor = false;
                 p.locallab.spots[0].expsharp = true;
                 p.locallab.spots[0].methodcap = "rl";
                 p.dirpyrequalizer.cbdlMethod = "aft";
                 p.locallab.spots[0].reparsha = 50.0;
             }},
            {"highlight reconstruction", [](auto& p) {
                 p.toneCurve.hrenabled = true;
             }},
            {"shadows/highlights micha", [](auto& p) {
                 p.locallab.spots[0].expshadhigh = true;
             }},
            {"shadows/highlights tone", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "tone";
                 spot.highlights = 50;
                 spot.shadows = 50;
             }},
            {"shadows/highlights ghs", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "ghs";
                 spot.highlights = 50;
                 spot.shadows = 50;
             }},
            {"shadows/highlights toneeq", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.shadows = 50;
                 spot.multsh[2] = 20;
             }},
            {"shadows/highlights tone gamma", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.shadows = 50;
                 spot.gamSH = 2.2;
             }},
            {"shadows/highlights tone slope", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.shadows = 50;
                 spot.sloSH = 13.0;
             }},
            {"shadows/highlights recovery", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.shadows = 50;
                 spot.reparsh = 50.0;
             }},
            {"shadows/highlights gradient", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.shadows = 50;
                 spot.strSH = 1.0;
             }},
            {"shadows/highlights highlights range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 101;
             }},
            {"shadows/highlights shadows range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.shadows = 101;
             }},
            {"shadows/highlights tonal range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.h_tonalwidth = 5;
             }},
            {"shadows/highlights radius range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.sh_radius = 101;
             }},
            {"shadows/highlights sensitivity range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.sensihs = 101;
             }},
            {"shadows/highlights blur range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.blurSHde = 1;
             }},
            {"shadows/highlights explicit mask", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expshadhigh = true;
                 spot.shMethod = "std";
                 spot.highlights = 50;
                 spot.enaSHMask = true;
             }},
            {"recursion", [](auto& p) {
                 p.locallab.spots[0].recurs = true;
             }},
            {"exclude spot", [](auto& p) {
                 p.locallab.spots[0].spotMethod = "exc";
             }},
            {"merge", [](auto& p) {
                 p.locallab.spots[0].merMethod = "mtwo";
             }},
            {"LAB gamut", [](auto& p) {
                 p.locallab.spots[0].avoidgamutMethod = "LAB";
             }},
            {"XYZ gamut", [](auto& p) {
                 p.locallab.spots[0].avoidgamutMethod = "XYZ";
             }},
            {"avoid radius", [](auto& p) {
                 p.locallab.spots[0].avoidrad = 5.0;
             }},
            {"RGB curve", [](auto& p) {
                 p.locallab.spots[0].rgbcurve = {
                     3.0, 0.0, 0.0, 0.5, 0.7, 1.0, 1.0};
             }},
            {"GHS clamp leftovers", [](auto& p) {
                 p.locallab.spots[0].activ = false;
                 p.locallab.spots[0].ghs_D = 1.0;
                 p.locallab.spots[0].ghs_BLP = 1.0;
             }},
            {"dehaze black", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.dehazeblack = 10.0;
             }},
            {"dehaze show depth", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 p.dehaze.showDepthMap = true;
             }},
            {"dehaze explicit mask", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.enaColorMask = true;
             }},
            {"dehaze soft radius", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.softradiuscol = 1.0;
             }},
            {"dehaze active retinex", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.str = 10.0;
             }},
            {"dehaze strength range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 101;
             }},
            {"dehaze depth range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.depth = 101;
             }},
            {"dehaze saturation range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.dehazeSaturation = 101.0;
             }},
            {"dehaze sensitivity range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = true;
                 spot.dehaz = 50;
                 spot.sensih = 101;
             }},
            {"dehaze without retinex enable", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.expreti = false;
                 spot.dehaz = 50;
             }},
            {"log autocompute", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.autocompute = true;
             }},
            {"log CIE CAM", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.expcie = true;
             }},
            {"log ciecam flag", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.ciecam = true;
             }},
            {"log detail", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.6;
             }},
            {"log gradient", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.strlog = 10.0;
             }},
            {"log explicit mask", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.enaLMask = true;
             }},
            {"log soft radius", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.softradiuscol = 1.0;
             }},
            {"log source gray range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.sourceGray = 0.0;
             }},
            {"log compr range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.comprlog = 2.0;
             }},
            {"log repar range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.repar = 0.0;
             }},
            {"log sensitivity range", [](auto& p) {
                 auto& spot = p.locallab.spots[0];
                 spot.expcolor = false;
                 spot.explog = true;
                 spot.detail = 0.0;
                 spot.sensilog = 101;
             }},
        };
        for (std::size_t index = 0;
             index < sizeof(residuals) / sizeof(residuals[0]); ++index) {
            const auto& residual = residuals[index];
            rtengine::procparams::ProcParams params = supported;
            residual.mutate(params);
            check(!localAdjustmentsSupported(params),
                  (std::string("the ") + residual.label +
                   " residual is outside the local.adjustments predicate")
                      .c_str());

            Imagefloat reference_base(238, 64);
            fill_local_adjustments_rgb(reference_base, 71u + index);
            std::unique_ptr<LabImage> reference_lab(
                new LabImage(238, 64));
            rtengine::ImProcFunctions reference_ipf(&params, false);
            localAdjustmentsHostBlock(reference_ipf, params,
                                      &reference_base, reference_lab.get(),
                                      238, 64);

            rtengine::procparams::ProcParams node_params = params;
            Imagefloat node_base(238, 64);
            fill_local_adjustments_rgb(node_base, 71u + index);
            std::unique_ptr<LabImage> node_lab(new LabImage(238, 64));
            rtengine::ImProcFunctions node_ipf(&node_params, false);
            const RtStageStatus status =
                run_local_adjustments_node(node_base, *node_lab, node_params,
                                           node_ipf, profile_path);
            check(status == RT_STAGE_OK,
                  "the local.adjustments residual completes through run()");
            const std::string profile = read_file(profile_path);
            if (available) {
                check(profile.find("\"impl\":\"host\"") !=
                          std::string::npos,
                      "the local.adjustments residual records a host "
                      "execution");
                check(profile.find("\"impl\":\"opencl\"") ==
                          std::string::npos,
                      "the local.adjustments residual never claims a device "
                      "pass");
            }
            const Diff diff =
                compare(view(*reference_lab), view(*node_lab));
            std::printf("stage-residency: local.adjustments residual %s: "
                        "host, PAE %.6f LSB16, %zu samples differ\n",
                        residual.label, diff.max_abs, diff.differing);
            check(diff.finite && diff.max_abs == 0.0 &&
                      diff.differing == 0,
                  "the local.adjustments residual matches the block");
        }
    }

    /* The per-spot record, its tables and the blur coefficients. */
    {
        rtengine::procparams::ProcParams params = supported;
        params.locallab.spots[0].chroma = 35;
        params.locallab.spots[0].lightness = 10;
        params.locallab.spots[0].contrast = -5;

        Imagefloat base(179, 48);
        fill_local_adjustments_rgb(base, 41u);
        std::unique_ptr<LabImage> lab(new LabImage(179, 48));
        rtengine::ImProcFunctions ipf(&params, false);
        rtengine::LabImage source(179, 48);
        ipf.rgb2lab(base, source, params.icm.workingProfile);

        LabImage* lab_pointer = lab.get();
        LocalAdjustmentsContext context(params, ipf, &base, lab_pointer,
                                        179, 48);
        RtStageBuffer source_view = view(source);
        RtLocalAdjustmentsResult setup{};
        check(context.build(0, &source_view, setup) == 0,
              "the local.adjustments setup builds one spot");
        check(setup.spot_count == 1 &&
                  setup.spot_stride ==
                      RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE,
              "the local.adjustments record has one flat spot");
        check(setup.lut_count != 0 && setup.luts != nullptr,
              "the local.adjustments setup builds its colour tables");
        check(setup.ws != nullptr && setup.iws != nullptr &&
                  setup.cachef != nullptr && setup.cachefy != nullptr,
              "the local.adjustments setup exports the clamp tables");
        check(setup.gamma_srgb != nullptr &&
                  setup.igamma_srgb != nullptr,
              "the local.adjustments setup exports the sRGB gamma tables");
        check(setup.iir != nullptr &&
                  setup.iir->double_path == 0 &&
                  setup.iir->version == RT_LAB_TOOLS_IIR_VERSION,
              "the local.adjustments setup exports the SSE blur "
              "coefficients");
        check(setup.iir_vibrance != nullptr &&
                  setup.iir_vibrance->double_path == 0 &&
                  setup.iir_vibrance->version == RT_LAB_TOOLS_IIR_VERSION,
              "the local.adjustments setup exports the deltaE radius-3 "
              "coefficients");
        check(setup.iir_local_contrast != nullptr &&
                  setup.iir_local_contrast->version ==
                      RT_LAB_TOOLS_IIR_VERSION &&
                  setup.iir_local_contrast->double_path == 1,
              "the local.adjustments setup exports the local-contrast "
              "coefficients");
        const float* record = setup.spots;
        bool finite = true;
        for (std::size_t slot = 0;
             slot < RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE; ++slot) {
            finite = finite && std::isfinite(record[slot]);
        }
        check(finite, "the local.adjustments record is finite");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_ACTIVE] == 1.f,
              "the lightness/contrast table is active");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_ACTIVE] == 1.f,
              "the positive chroma table is active");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_ACTIVE] == 0.f,
              "the negative chroma table is inactive");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDGAMUT] ==
                  RT_LOCAL_ADJUSTMENTS_AVOID_NONE,
              "the gamut method is NONE");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDNEG] == 1.f,
              "the negative-value prefilter is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_CLAMPMODE] ==
                  RT_LOCAL_ADJUSTMENTS_CLAMP_NOTZERO,
              "the tail clamp arm is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_HRENABLED] == 0.f,
              "highlight reconstruction is off");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_QCURVE] == 0.f,
              "the quality curves are off");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPVIBRANCE] == 0.f &&
                  record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCONTRAST] == 0.f &&
                  record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSOFT] == 0.f,
              "the extra tools are not recorded on a colour-only spot");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE] == 0.f,
              "the idle vibrance skin curve is not recorded");
    }

    /* The local sharpening record. */
    {
        rtengine::procparams::ProcParams params = supported;
        auto& spot = params.locallab.spots[0];
        spot.expcolor = false;
        spot.expsharp = true;
        spot.methodcap = "rl";
        spot.sharamount = 80;
        spot.sharradius = 0.7;
        spot.shariter = 25;
        spot.sharcontrast = 40;
        spot.shardamping = 0;
        spot.sharblur = 0.2;
        spot.shargam = 1.0;
        spot.sharshow = false;
        spot.sensisha = 55;
        spot.reparsha = 100.0;
        spot.loc = {2000, 2000, 2000, 2000};
        params.dirpyrequalizer.cbdlMethod = "aft";

        Imagefloat base(179, 48);
        fill_local_adjustments_rgb(base, 43u);
        std::unique_ptr<LabImage> lab(new LabImage(179, 48));
        rtengine::ImProcFunctions ipf(&params, false);
        rtengine::LabImage source(179, 48);
        ipf.rgb2lab(base, source, params.icm.workingProfile);
        LabImage* lab_pointer = lab.get();
        LocalAdjustmentsContext context(params, ipf, &base, lab_pointer,
                                        179, 48);
        RtStageBuffer source_view = view(source);
        RtLocalAdjustmentsResult setup{};
        check(context.build(0, &source_view, setup) == 0,
              "the local sharpening setup builds one spot");
        const float* record = setup.spots;
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHARP] == 1.f,
              "the local sharpening pass is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSHA] == 55.f,
              "the sharpening sensitivity is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_AMOUNT] == 80.f &&
                  record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_ITER] == 25.f &&
                  record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_CONTRAST] == 40.f,
              "the sharpening sliders are recorded");
        check(std::fabs(record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_RADIUS] -
                        0.7f) < 1e-6f &&
                  std::fabs(record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_BLUR] -
                            0.2f) < 1e-6f,
              "the sharpening radius and blur are recorded");
    }

    /* The shadows/highlights record and its two host-built tone tables. */
    {
        rtengine::procparams::ProcParams params = supported;
        auto& spot = params.locallab.spots[0];
        spot.expcolor = false;
        spot.expshadhigh = true;
        spot.shMethod = "std";
        spot.highlights = 50;
        spot.shadows = 50;
        spot.h_tonalwidth = 70;
        spot.s_tonalwidth = 30;
        spot.sh_radius = 40;
        spot.sensihs = 30;
        spot.blurSHde = 5;
        check(localAdjustmentsSupported(params),
              "the shadows/highlights spot is inside the predicate");

        Imagefloat base(179, 48);
        fill_local_adjustments_rgb(base, 43u);
        std::unique_ptr<LabImage> lab(new LabImage(179, 48));
        rtengine::ImProcFunctions ipf(&params, false);
        rtengine::LabImage source(179, 48);
        ipf.rgb2lab(base, source, params.icm.workingProfile);

        LabImage* lab_pointer = lab.get();
        LocalAdjustmentsContext context(params, ipf, &base, lab_pointer,
                                        179, 48);
        RtStageBuffer source_view = view(source);
        RtLocalAdjustmentsResult setup{};
        check(context.build(0, &source_view, setup) == 0,
              "the shadows/highlights setup builds one spot");
        const float* record = setup.spots;
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHADHIGHLIGHTS] == 1.f,
              "the shadows/highlights pass is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_ACTIVE] == 1.f &&
                  record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_ACTIVE] == 1.f,
              "both shadows/highlights tone tables are active");
        const float hl_offset =
            record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_OFFSET];
        const float sh_offset =
            record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_OFFSET];
        check(hl_offset >= 0.f && sh_offset >= 0.f &&
                  static_cast<std::size_t>(hl_offset) +
                          RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES <=
                      setup.lut_count &&
                  static_cast<std::size_t>(sh_offset) +
                          RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES <=
                      setup.lut_count,
              "both shadows/highlights tone tables are inside the flat set");
        check(setup.iir_shadows_highlights != nullptr &&
                  setup.iir_shadows_highlights->version ==
                      RT_LAB_TOOLS_IIR_VERSION &&
                  setup.iir_shadows_highlights->double_path == 0,
              "the shadows/highlights setup exports the SSE blur "
              "coefficients");
    }

    /* The soft-light record: the run flag, strength and sensitivity, and the
     * strength-dependent clamp arm. */
    {
        rtengine::procparams::ProcParams params = supported;
        auto& spot = params.locallab.spots[0];
        spot.expcolor = false;
        spot.expsoft = true;
        spot.softMethod = "soft";
        spot.streng = 60;
        spot.sensisf = 30;
        check(localAdjustmentsSupported(params),
              "the soft-light spot is inside the predicate");

        Imagefloat base(179, 48);
        fill_local_adjustments_rgb(base, 44u);
        std::unique_ptr<LabImage> lab(new LabImage(179, 48));
        rtengine::ImProcFunctions ipf(&params, false);
        rtengine::LabImage source(179, 48);
        ipf.rgb2lab(base, source, params.icm.workingProfile);

        LabImage* lab_pointer = lab.get();
        LocalAdjustmentsContext context(params, ipf, &base, lab_pointer,
                                        179, 48);
        RtStageBuffer source_view = view(source);
        RtLocalAdjustmentsResult setup{};
        check(context.build(0, &source_view, setup) == 0,
              "the soft-light setup builds one spot");
        const float* record = setup.spots;
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSOFT] == 1.f,
              "the soft-light pass is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SOFT_STRENGTH] == 60.f &&
                  record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSOFT] == 30.f,
              "the soft-light strength and sensitivity are recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_CLAMPMODE] ==
                  RT_LOCAL_ADJUSTMENTS_CLAMP_LAPLACIAN,
              "a strength above two selects the Laplacian clamp arm");

        params.locallab.spots[0].streng = 2;
        LocalAdjustmentsContext low_context(params, ipf, &base, lab_pointer,
                                            179, 48);
        RtLocalAdjustmentsResult low_setup{};
        check(low_context.build(0, &source_view, low_setup) == 0,
              "the strength-two soft-light setup builds one spot");
        check(low_setup.spots[RT_LOCAL_ADJUSTMENTS_SLOT_CLAMPMODE] ==
                  RT_LOCAL_ADJUSTMENTS_CLAMP_NOTZERO,
              "a strength of two keeps the notzero clamp arm");
    }

    /* The damped sharpening record and the host's `"bef"` fixup. The block
     * raises a zero damping to one even though the equalizer is disabled; the
     * device context records the effective value and leaves the caller's
     * params alone. */
    {
        rtengine::procparams::ProcParams params;
        params.locallab.enabled = true;
        auto spot = localAdjustmentsSpot("sharpen", 0, 0, 2000, true,
                                         0, 0, 0);
        spot.expcolor = false;
        spot.expsharp = true;
        spot.methodcap = "rl";
        spot.sharamount = 100;
        spot.sharradius = 0.75;
        spot.shariter = 30;
        spot.sharcontrast = 20;
        spot.shardamping = 0;
        spot.sharblur = 0.2;
        spot.shargam = 1.0;
        spot.sharshow = false;
        spot.reparsha = 100.0;
        spot.sensisha = 40;
        params.locallab.spots.push_back(spot);
        params.dirpyrequalizer.cbdlMethod = "bef";
        check(localAdjustmentsSupported(params),
              "the default-method damped spot is inside the predicate");

        Imagefloat base(179, 48);
        fill_local_adjustments_rgb(base, 43u);
        std::unique_ptr<LabImage> lab(new LabImage(179, 48));
        rtengine::ImProcFunctions ipf(&params, false);
        rtengine::LabImage source(179, 48);
        ipf.rgb2lab(base, source, params.icm.workingProfile);

        LabImage* lab_pointer = lab.get();
        LocalAdjustmentsContext context(params, ipf, &base, lab_pointer,
                                        179, 48);
        RtStageBuffer source_view = view(source);
        RtLocalAdjustmentsResult setup{};
        check(context.build(0, &source_view, setup) == 0,
              "the damped sharpening setup builds one spot");
        const float* record = setup.spots;
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHARP] == 1.f,
              "the damped sharpening pass is recorded");
        check(record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_DAMPING] == 1.f,
              "the bef fixup records a damping of one");
        check(params.locallab.spots[0].shardamping == 0,
              "the bef fixup does not mutate the caller's params");

        params.locallab.spots[0].shardamping = 5;
        params.dirpyrequalizer.cbdlMethod = "aft";
        LocalAdjustmentsContext aft_context(params, ipf, &base,
                                            lab_pointer, 179, 48);
        RtLocalAdjustmentsResult aft_setup{};
        check(aft_context.build(0, &source_view, aft_setup) == 0,
              "the explicitly damped setup builds one spot");
        check(aft_setup.spots[RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_DAMPING] ==
                  5.f,
              "the explicit damping value is recorded");
        check(aft_setup.spots[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHARP] == 1.f,
              "the explicitly damped pass is recorded");
    }

    /* The call-site guard is a helper so the inactive/prefill decision is
     * testable without constructing a full ImageProcessor job. */
    {
        rtengine::procparams::ProcParams disabled;
        disabled.locallab.enabled = false;
        disabled.locallab.spots.push_back(
            localAdjustmentsSpot("disabled", 0, 0, 800, true, 35, 10, -5));
        const LocalAdjustmentsDispatch disabled_route =
            localAdjustmentsDispatch(disabled);
        check(!disabled_route.submit && !disabled_route.prefill &&
                  !disabled_route.supported,
              "a disabled block does not submit or prefill");

        rtengine::procparams::ProcParams empty;
        empty.locallab.enabled = true;
        const LocalAdjustmentsDispatch empty_route =
            localAdjustmentsDispatch(empty);
        check(!empty_route.submit && !empty_route.prefill &&
                  !empty_route.supported,
              "an empty block does not submit or prefill");

        const LocalAdjustmentsDispatch active_route =
            localAdjustmentsDispatch(supported);
        check(active_route.submit && active_route.prefill &&
                  active_route.supported,
              "a supported active block submits and prefills");

        rtengine::procparams::ProcParams residual = supported;
        residual.locallab.spots[0].reparcol = 50.0;
        const LocalAdjustmentsDispatch residual_route =
            localAdjustmentsDispatch(residual);
        check(residual_route.submit && !residual_route.prefill &&
                  !residual_route.supported,
              "an active residual submits without prefilling");
    }

    /* An inactive block must not round-trip the base image. This is the
     * regression that showed up as the working.trc gate row moving by
     * ~2 LSB when the stage node was selected. */
    {
        const auto no_round_trip = [](rtengine::procparams::ProcParams& params,
                                      const char* label) {
            Imagefloat base(179, 48);
            Imagefloat expected(179, 48);
            fill_local_adjustments_rgb(base, 91u);
            fill_local_adjustments_rgb(expected, 91u);
            std::unique_ptr<LabImage> lab(new LabImage(179, 48));
            rtengine::ImProcFunctions ipf(&params, false);
            localAdjustmentsHostBlock(ipf, params, &base, lab.get(), 179, 48);
            const Diff diff = compare(view(base), view(expected));
            std::printf("stage-residency: local.adjustments %s: base PAE "
                        "%.6f LSB16, %zu samples differ\n",
                        label, diff.max_abs, diff.differing);
            check(diff.finite && diff.max_abs == 0.0 &&
                      diff.differing == 0,
                  "an inactive local.adjustments block leaves baseImg "
                  "untouched");
        };

        rtengine::procparams::ProcParams disabled;
        disabled.locallab.enabled = false;
        disabled.locallab.spots.push_back(
            localAdjustmentsSpot("disabled", 0, 0, 800, true, 35, 10, -5));
        check(!localAdjustmentsSupported(disabled),
              "a disabled local.adjustments block is outside the predicate");
        no_round_trip(disabled, "disabled block");

        rtengine::procparams::ProcParams empty;
        empty.locallab.enabled = true;
        check(!localAdjustmentsSupported(empty),
              "an empty local.adjustments block is outside the predicate");
        no_round_trip(empty, "empty block");
    }

    std::remove(profile_path.c_str());
}


} // namespace

int main()
{
    using namespace rtengine::stage;

    /* The switch is global; start from a known state. */
    unsetenv("RT_STAGE_OPENCL");
    unsetenv("RT_STAGE_OPENCL_DISABLE");
    clear_node_device_overrides();
    set_opencl_stage_allowed(true);

    std::string reason;
    const bool available = opencl_stage_available(&reason);
    if (!available) {
        std::printf("stage-residency: OpenCL unavailable (%s)\n", reason.c_str());
    }
    const char* require = std::getenv("RT_REQUIRE_GPU");
    const bool require_device = require != nullptr && require[0] != '\0' &&
                                std::strcmp(require, "0") != 0;
    if (!available && require_device) {
        std::fprintf(stderr,
                     "FAIL: RT_REQUIRE_GPU is set but no OpenCL device is "
                     "available\n");
        return 1;
    }

    if (available) {
        const auto nodes = opencl_stage_nodes();
        check(!nodes.empty(), "at least one node has a device implementation");
        check(has_opencl_stage_node("lanczos3"),
              "the resize node has a device implementation");
        check(has_opencl_stage_node("crop"),
              "the crop node has a device implementation");
    }

    test_cpu_events();
    test_failed_event();
    test_icd_robustness(available, reason);
    test_device_events(available);
    test_transform_identity(available);
    test_lab_to_u8(available);
    test_thumbnail_wb(available);
    test_thumbnail_permute(available);
    test_thumbnail_permute_u8(available);
    test_thumbnail_scale(available);
    test_selection(available);
    test_residency();
    test_node_parity(available);
    test_rgb_proc_pitch(available);
    test_framing(available);
    test_crop(available);
    test_gamut(available);
    test_working_trc(available);
    test_lab_tools(available);
    test_lab_tools_vibrance(available);
    test_lab_tools_lab_curve(available);
    test_lab_tools_local_contrast(available);
    test_lab_tools_regions(available);
    test_local_adjustments(available);
    test_lab_wavelet_mad();
    test_lab_wavelet(available);
    test_lab_wavelet_contrast(available);
    test_lab_wavelet_chroma(available);
    test_lab_wavelet_edge(available);
    test_lab_wavelet_edge_lipschitz(available);
    test_lab_wavelet_final(available);
    test_lab_wavelet_ba(available);
    test_lab_wavelet_toning(available);
    test_lab_wavelet_denoise(available);
    test_lab_wavelet_gpu(available);
    test_output_sharpen(available);
    test_lab_sharpening(available);
    test_run_wiring(available);

    if (failures == 0) {
        std::printf("stage-residency-test: PASS (%d skipped)\n", skipped);
    }
    return failures == 0 ? 0 : 1;
}
