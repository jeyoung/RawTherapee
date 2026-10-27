#include "stage_buffer.h"
#include "stage_buffer_state.h"
#include "stage_device.h"
#include "stage_parity.h"
#include "stage_runtime.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
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

void fill(const RtStageBuffer& buffer)
{
    for (std::uint32_t plane = 0; plane < buffer.planes; ++plane) {
        unsigned char* plane_data = static_cast<unsigned char*>(buffer.data) +
                                    static_cast<std::size_t>(plane) * buffer.plane_pitch;
        for (int y = 0; y < buffer.height; ++y) {
            float* row = reinterpret_cast<float*>(
                plane_data + static_cast<std::size_t>(y) * buffer.row_pitch);
            for (int x = 0; x < buffer.width; ++x) {
                row[x] = static_cast<float>(
                    (x * 17 + y * 31 + static_cast<int>(plane) * 101) % 65536);
            }
        }
    }
}

} // namespace

int main()
{
    using namespace rtengine::stage;

    /* CPU device and its guarantees. */
    const auto cpu = cpu_device();
    check(cpu != nullptr, "CPU device exists");
    check(cpu->type() == StageDeviceType::CPU, "CPU device reports its type");
    check(cpu->available(), "CPU device is available");
    check(std::string(cpu->name()) == "cpu", "CPU device name is stable");
    const StageCapabilities cpu_capabilities = cpu->capabilities();
    check(cpu_capabilities.deterministic, "CPU device is deterministic");
    check(cpu_capabilities.full_precision, "CPU device is full precision");
    check(!cpu_capabilities.device_buffers, "CPU device has no device buffers");

    /* Default device selection is CPU. */
    std::string reason = "unset";
    const auto selected = select_device(StageDeviceType::CPU, &reason);
    check(selected->type() == StageDeviceType::CPU,
          "explicit CPU selection returns the CPU device");
    check(reason.empty(), "CPU selection clears the fallback reason");

    StageContext default_context;
    check(default_context.device_type() == StageDeviceType::CPU,
          "the default context selects CPU");

    const auto context = cpu->create_context();
    check(context != nullptr, "CPU device creates a context");
    if (context == nullptr) {
        return 1;
    }
    check(context->device_type() == StageDeviceType::CPU,
          "the device context stays on CPU");

    /* Buffers come from the context allocator. */
    StageBufferAllocator& allocator = context->allocator();
    check(!allocator.device_local(), "CPU allocator is host local");

    std::string error;
    RtStageBuffer source =
        allocator.allocate({RT_STAGE_F32, 64, 48, 3}, &error);
    RtStageBuffer context_output =
        allocator.allocate({RT_STAGE_F32, 32, 24, 3}, &error);
    RtStageBuffer direct_output =
        allocator.allocate({RT_STAGE_F32, 32, 24, 3}, &error);
    check(valid(source), "allocator returns a valid source buffer");
    check(valid(context_output), "allocator returns a valid context output");
    check(valid(direct_output), "allocator returns a valid direct output");
    check(allocator.owns(source), "allocator owns its block");
    if (!valid(source) || !valid(context_output) || !valid(direct_output)) {
        return 1;
    }
    fill(source);

    StageRequest request;
    request.name = "lanczos3";
    request.impl = "builtin";
    request.args = {buffer_arg(source), buffer_arg(context_output), double_arg(0.5),
                    int_arg(2)};

    /* StageContext compiles through StageCompiler and caches the program. */
    const auto program = context->compile(request, &error);
    check(program != nullptr, "CPU context compiles Lanczos");
    if (program == nullptr) {
        std::fprintf(stderr, "compile error: %s\n", error.c_str());
        return 1;
    }
    check(context->program_cache_size() == 1, "program cache holds one entry");
    check(context->compile(request, &error) == program,
          "a repeated compile reuses the cached program");
    context->clear_program_cache();
    check(context->program_cache_size() == 0, "program cache can be cleared");
    check(context->compile(request, &error) != nullptr,
          "Lanczos recompiles after a cache clear");

    /* Execution through the context queue. */
    RtStageProfiling profiling{};
    check(context->execute(request, &profiling) == RT_STAGE_OK,
          "CPU context executes Lanczos");
    check(profiling.elapsed_ns > 0, "context execution reports elapsed time");

    /* The same request executed directly must produce identical output. */
    StageRequest direct_request = request;
    direct_request.args = {buffer_arg(source), buffer_arg(direct_output),
                           double_arg(0.5), int_arg(2)};
    const auto direct_program =
        StageCompiler::instance().compile(direct_request, &error);
    check(direct_program != nullptr, "direct Lanczos compiles");
    if (direct_program != nullptr) {
        RtStageProfiling direct_profiling{};
        check(direct_program->execute(direct_request.args, direct_profiling) ==
                  RT_STAGE_OK,
              "direct Lanczos executes");
    }
    const ParityResult parity = compare(context_output, direct_output);
    check(parity.same_geometry, "context and direct geometry match");
    check(parity.differing == 0, "context and direct pixels match exactly");
    check(parity.max_abs == 0, "context and direct maximum difference is zero");

    /* The CPU queue executes inline and reports a completed event. */
    const auto cpu_queue = cpu->create_queue();
    check(cpu_queue != nullptr, "CPU device creates a queue");
    if (cpu_queue != nullptr) {
        check(cpu_queue->type() == StageDeviceType::CPU,
              "CPU queue reports its type");
        StageEvent event =
            cpu_queue->submit(*program, request.args, nullptr);
        check(event.status == StageEventStatus::Complete,
              "CPU queue completes inline");
        check(event.succeeded(), "CPU queue event succeeded");
        check(event.done(), "CPU queue event is done");
        check(event.elapsed_ns > 0, "CPU queue event carries timing");
        check(event.device == StageDeviceType::CPU,
              "CPU queue event names the CPU");
        check(!event.asynchronous, "CPU queue event is synchronous");
        check(cpu_queue->wait(event) == RT_STAGE_OK, "CPU queue wait reports OK");
    }

    /*
     * The OpenCL device owns real buffers and queues. A compiled host program
     * cannot run on it, so that entry point reports a failure; node requests
     * are the device dispatch path and are covered by stage-residency-test.
     */
    const auto ocl = opencl_device();
    check(ocl->type() == StageDeviceType::OPENCL,
          "OpenCL device reports its type");
    const auto ocl_queue = ocl->create_queue();
    check(ocl_queue != nullptr, "OpenCL device creates a queue");
    if (ocl_queue != nullptr) {
        check(ocl_queue->type() == StageDeviceType::OPENCL,
              "OpenCL queue reports its type");
        StageEvent event = ocl_queue->submit(*program, request.args, nullptr);
        check(event.status == StageEventStatus::Failed,
              "OpenCL queue refuses a compiled host program");
        check(event.result == RT_STAGE_FALLBACK,
              "OpenCL queue reports fallback for a host program");
        check(ocl_queue->wait(event) == RT_STAGE_FALLBACK,
              "OpenCL wait reports fallback");
    }

    /* Device selection falls back to CPU when OpenCL is unavailable. */
    std::string ocl_reason;
    const auto ocl_selection = select_device(StageDeviceType::OPENCL, &ocl_reason);
    std::printf("opencl-stage-device: %s\n",
                ocl->available() ? "available" : "unavailable (CPU fallback)");
    if (ocl->available()) {
        check(ocl_selection->type() == StageDeviceType::OPENCL,
              "available OpenCL is selected");
        check(ocl_reason.empty(), "available OpenCL has no fallback reason");
        const StageCapabilities ocl_capabilities = ocl->capabilities();
        check(ocl_capabilities.device_buffers,
              "OpenCL device advertises device buffers");
        check(ocl_capabilities.async, "OpenCL device advertises asynchrony");
        const auto ocl_context = ocl_selection->create_context();
        check(ocl_context != nullptr, "OpenCL device creates a context");
        if (ocl_context != nullptr) {
            check(ocl_context->residency() != nullptr,
                  "OpenCL context exposes its residency");
            /* The switch is unset, so the context runs the host program. */
            check(ocl_context->execute(request, nullptr) == RT_STAGE_OK,
                  "an OpenCL context runs the host program by default");
        }
    } else {
        check(ocl_selection->type() == StageDeviceType::CPU,
              "unavailable OpenCL falls back to CPU");
        check(!ocl_reason.empty(), "OpenCL fallback records a reason");
    }

    /* Buffer state transitions. */
    StageBufferState state(source);
    check(state.location() == StageBufferLocation::HostValid,
          "a new buffer is host valid");
    check(state.generation() == 0, "a new buffer starts at generation zero");
    check(state.ref_count() == 1, "a new buffer holds one reference");
    check(state.ensure_host() == RT_STAGE_OK,
          "CPU ensure_host succeeds without a copy");
    check(state.location() == StageBufferLocation::HostValid,
          "CPU ensure_host leaves the location unchanged");
    check(state.ensure_device(1) == RT_STAGE_FALLBACK,
          "CPU ensure_device reports unsupported");
    check(state.location() == StageBufferLocation::HostValid,
          "failed ensure_device leaves the location unchanged");

    state.add_ref();
    check(state.ref_count() == 2, "add_ref increments the reference count");
    check(!state.release(), "releasing a shared reference keeps the buffer");
    check(state.ref_count() == 1, "release decrements the reference count");

    state.mark_device_write(7);
    check(state.location() == StageBufferLocation::DeviceValid,
          "device write marks the device copy valid");
    check(state.generation() == 7, "device write records the generation");
    check(state.ensure_host() == RT_STAGE_FALLBACK,
          "a stale host copy cannot be read back yet");
    check(state.ensure_device(8) == RT_STAGE_FALLBACK,
          "device transfer still reports unsupported");

    state.mark_both_valid(9);
    check(state.location() == StageBufferLocation::Both,
          "both copies can be marked valid");
    check(state.generation() == 9, "both-valid records the generation");
    check(state.ensure_host() == RT_STAGE_OK,
          "both-valid allows host access");

    state.mark_host_write();
    check(state.location() == StageBufferLocation::HostValid,
          "host write invalidates the device copy");

    state.mark_dirty();
    check(state.location() == StageBufferLocation::Dirty,
          "dirty contents are recorded");
    check(state.ensure_host() == RT_STAGE_INVALID,
          "dirty contents cannot be read");

    check(state.release(), "the last release reports the buffer dropped");
    check(state.ref_count() == 0, "the reference count reaches zero");

    check(std::string(to_string(StageBufferLocation::HostValid)) == "host-valid",
          "buffer locations have stable names");
    check(std::string(to_string(StageDeviceType::OPENCL)) == "opencl",
          "device types have stable names");
    check(std::string(to_string(StageEventStatus::Failed)) == "failed",
          "event statuses have stable names");

    /* Allocator guards. */
    error.clear();
    const RtStageBuffer bad =
        allocator.allocate({RT_STAGE_F32, 0, 4, 1}, &error);
    check(bad.data == nullptr, "an invalid shape is refused");
    check(!error.empty(), "an invalid shape reports an error");

    StageBufferAllocator ocl_allocator(StageDeviceType::OPENCL);
    error.clear();
    const RtStageBuffer refused =
        ocl_allocator.allocate({RT_STAGE_F32, 4, 4, 1}, &error);
    check(refused.data == nullptr,
          "the host allocator refuses device-local allocation");
    check(!error.empty(), "the device refusal reports why");
    check(!ocl_allocator.owns(refused), "a refused buffer is not owned");

    check(allocator.release(source), "allocator releases the source");
    check(allocator.release(context_output), "allocator releases the context output");
    check(allocator.release(direct_output), "allocator releases the direct output");
    check(!allocator.owns(source), "a released buffer is no longer owned");
    check(!allocator.release(source), "a second release is refused");

    if (failures == 0) {
        std::puts("stage-device-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
