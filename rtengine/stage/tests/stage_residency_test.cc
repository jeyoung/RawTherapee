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
#include "iccstore.h"
#include "improcfun.h"
#include "procparams.h"
#include "settings.h"
#include "stage/algorithms/gamut_cl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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
        }
    }
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
    test_gamut(available);
    test_run_wiring(available);

    if (failures == 0) {
        std::printf("stage-residency-test: PASS (%d skipped)\n", skipped);
    }
    return failures == 0 ? 0 : 1;
}
