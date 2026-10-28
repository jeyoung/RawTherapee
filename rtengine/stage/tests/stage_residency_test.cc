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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

using namespace rtengine::stage;

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
    test_selection(available);
    test_residency();
    test_node_parity(available);
    test_run_wiring(available);

    if (failures == 0) {
        std::printf("stage-residency-test: PASS (%d skipped)\n", skipped);
    }
    return failures == 0 ? 0 : 1;
}
