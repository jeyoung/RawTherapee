#pragma once

#include "stage_abi.h"
#include "stage_runtime.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtengine::stage
{

class StageContext;
class StageDevice;
class StageDeviceResidency;

enum class StageDeviceType {
    CPU,
    OPENCL,
    /* No execution happened: a refusal, a resolution failure or an event that
     * never left Pending. Devices and queues are always CPU or OPENCL. */
    NONE
};

const char* to_string(StageDeviceType type);

struct StageCapabilities {
    bool deterministic = false;
    bool full_precision = false;
    bool async = false;
    bool device_buffers = false;
};

enum class StageEventStatus {
    Pending,
    Running,
    Complete,
    Failed
};

const char* to_string(StageEventStatus status);

/*
 * Completion handle for a submission that may still be running.
 *
 * The handle is created by the queue that accepted the work. Its status only
 * changes through poll() and wait(), so an observed Pending or Running state
 * always means the work has not finished and the output must not be consumed.
 */
class StageSubmission
{
public:
    virtual ~StageSubmission() = default;

    /* Block until the submission finishes; returns its result. */
    virtual RtStageStatus wait() = 0;

    /* Refresh the status without blocking. */
    virtual StageEventStatus poll() = 0;

    /* Time from submission to the last observed completion. */
    virtual std::uint64_t elapsed_ns() const = 0;
};

/*
 * Result of one queue submission.
 *
 * `device` is the device that actually executed the node, which can differ
 * from the queue type when a node has no device implementation or the device
 * allocation budget is exhausted, and is NONE when nothing executed.
 * `asynchronous` is true only when submit() returned before the work finished;
 * a submission that completes inline sets it false and carries a final status.
 */
struct StageEvent {
    StageEventStatus status = StageEventStatus::Pending;
    RtStageStatus result = RT_STAGE_OK;
    std::uint64_t elapsed_ns = 0;
    StageDeviceType device = StageDeviceType::NONE;
    bool asynchronous = false;
    /* Why a device path was declined; empty for a normal execution. */
    std::string note;
    std::shared_ptr<StageSubmission> submission;

    bool done() const
    {
        return status == StageEventStatus::Complete ||
               status == StageEventStatus::Failed;
    }

    bool succeeded() const
    {
        return status == StageEventStatus::Complete && result == RT_STAGE_OK;
    }
};

/*
 * A queue executes compiled programs on one device.
 *
 * The CPU queue executes inline before submit() returns. A device queue
 * accepts a node request, may run it asynchronously, and falls back to the
 * compiled host program when the node has no device implementation.
 */
class StageQueue
{
public:
    virtual ~StageQueue() = default;

    virtual StageDeviceType type() const = 0;

    virtual StageEvent submit(const StageProgram& program,
                              const std::vector<RtStageArg>& args,
                              RtStageProfiling* profiling = nullptr) = 0;

    /*
     * Execute a node request. The default implementation compiles the request
     * and submits the host program, which is the CPU guarantee every queue
     * keeps. A device queue overrides it to dispatch accepted nodes.
     */
    virtual StageEvent submit(const StageRequest& request,
                              RtStageProfiling* profiling = nullptr);

    /* Refresh an event from its submission; returns the current status. */
    virtual StageEventStatus poll(StageEvent& event) const;

    /* Blocking completion; finalises the event and returns its result. */
    virtual RtStageStatus wait(StageEvent& event) const = 0;

    /* Device residency owned by this queue, or null for a host queue. */
    virtual StageDeviceResidency* residency()
    {
        return nullptr;
    }
};

/*
 * A device creates contexts and queues and describes what it can guarantee.
 *
 * The interface mirrors the OpenCL object model (platform, device, context,
 * queue, program, buffer, event) so a backend can be added without changing
 * the graph, cache or callers.
 */
class StageDevice
{
public:
    virtual ~StageDevice() = default;

    virtual StageDeviceType type() const = 0;
    virtual const char* name() const = 0;
    virtual bool available() const = 0;
    virtual StageCapabilities capabilities() const = 0;

    virtual std::shared_ptr<StageContext> create_context() const = 0;
    virtual std::unique_ptr<StageQueue> create_queue() const = 0;
};

struct StageBufferShape {
    RtStageFormat format = RT_STAGE_F32;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t planes = 1;
};

/*
 * Alignment of host allocator blocks and row pitches. Sixty-four bytes covers
 * the cache line of the supported x86-64 and AArch64 hosts and keeps every
 * plane start aligned.
 */
inline constexpr std::size_t STAGE_BUFFER_ALIGNMENT = 64;

/* Device accounting for one residency. Bytes are those reserved on the
 * device, not the driver's own bookkeeping. */
struct StageResidencyStats {
    std::size_t budget_bytes = 0;
    std::size_t used_bytes = 0;
    std::size_t peak_bytes = 0;
    std::uint64_t allocations = 0;
    std::uint64_t reused = 0;
    std::uint64_t evictions = 0;
    std::uint64_t refusals = 0;
};

/* A device-local allocation; formats and pitches match RtStageBuffer. */
class StageDeviceBuffer
{
public:
    virtual ~StageDeviceBuffer() = default;

    virtual StageDeviceType type() const = 0;
    virtual StageBufferShape shape() const = 0;
    virtual std::size_t bytes() const = 0;
    virtual std::size_t row_pitch() const = 0;   // bytes
    virtual std::size_t plane_pitch() const = 0; // bytes
    virtual bool valid() const = 0;
};

using StageDeviceBufferPtr = std::shared_ptr<StageDeviceBuffer>;

/*
 * Padded layout of a buffer shape: 64-byte aligned rows, packed planes and a
 * 64-byte aligned block. U8 is packed RGB and must be a single plane, which is
 * the only U8 shape the engine views produce; stage::row_bytes() also triples
 * the row for U8, so refusing multi-plane U8 keeps the two helpers in step.
 * Returns false for an invalid shape and sets error.
 */
bool stage_buffer_layout(const StageBufferShape& shape, std::size_t& row_pitch,
                         std::size_t& plane_pitch, std::size_t& bytes,
                         std::string* error = nullptr);

/*
 * Device-resident buffers.
 *
 * A residency binds a device allocation to a host buffer address: the node
 * output the buffer belongs to. An entry is reused while the address and the
 * shape are unchanged, and it is released when the caller drops the binding,
 * when the byte budget forces an eviction, or when the residency dies. A
 * request that cannot be met reports a null buffer and a reason, so the
 * caller can run the node on the host instead.
 */
class StageDeviceResidency
{
public:
    virtual ~StageDeviceResidency() = default;

    virtual StageDeviceType type() const = 0;
    virtual StageResidencyStats stats() const = 0;

    /*
     * Set the byte budget. Idle entries are evicted until the residency fits;
     * returns false when in-use entries still exceed the budget.
     */
    virtual bool set_budget(std::size_t bytes) = 0;

    /* Find or create the allocation bound to key. */
    virtual StageDeviceBufferPtr acquire(const void* key,
                                         const StageBufferShape& shape,
                                         std::string* error = nullptr) = 0;

    /* Copy the host contents into the whole device buffer. */
    virtual RtStageStatus upload(const StageDeviceBufferPtr& buffer,
                                 const RtStageBuffer& host) = 0;

    /* Copy the whole device buffer back into the host buffer. */
    virtual RtStageStatus readback(const StageDeviceBufferPtr& buffer,
                                   const RtStageBuffer& host) = 0;

    virtual StageDeviceBufferPtr find(const void* key) const = 0;

    /* Drop the binding for key and free its allocation. */
    virtual bool release(const void* key) = 0;

    virtual void clear() = 0;
};

/*
 * Buffer allocator owned by a StageContext.
 *
 * The CPU allocator owns aligned, zeroed host memory and releases it in
 * release(). It is host memory only: a device allocation goes through the
 * StageDeviceResidency of the device queue instead, because a device block
 * has no host address to hand out.
 *
 * All blocks are freed when the allocator is destroyed; descriptors still held
 * at that point are invalid. The context owns the allocator, so a buffer must
 * not outlive its context.
 */
class StageBufferAllocator
{
public:
    explicit StageBufferAllocator(StageDeviceType type);
    ~StageBufferAllocator();

    StageBufferAllocator(const StageBufferAllocator&) = delete;
    StageBufferAllocator& operator=(const StageBufferAllocator&) = delete;

    /*
     * Allocate a zeroed host buffer. Returns a descriptor with a null data
     * pointer and sets error when the shape is invalid or the allocator is
     * device-local.
     */
    RtStageBuffer allocate(const StageBufferShape& shape,
                           std::string* error = nullptr);

    /* Release a buffer previously returned by allocate(). */
    bool release(RtStageBuffer& buffer);

    bool owns(const RtStageBuffer& buffer) const;

    bool device_local() const
    {
        return type_ != StageDeviceType::CPU;
    }

private:
    StageDeviceType type_;
    mutable std::mutex mutex_;
    std::map<void*, std::size_t> blocks_;
};

/*
 * A context owns one device, a per-context program cache and a buffer
 * allocator. Compilation goes through the existing StageCompiler, so built-in
 * and module resolution is unchanged. The default context selects the CPU
 * device.
 *
 * The program cache is keyed by stage name and explicit implementation. It
 * reads the environment on the first compile of a key; clear_program_cache()
 * forces the next compile to resolve the request again.
 */
/* What one StageContext::execute() call actually did. */
struct StageExecution {
    StageDeviceType device = StageDeviceType::NONE;
    /* True when the kernel was submitted and completed asynchronously. */
    bool asynchronous = false;
};

class StageContext
{
public:
    StageContext();
    explicit StageContext(std::shared_ptr<StageDevice> device);
    ~StageContext();

    StageContext(const StageContext&) = delete;
    StageContext& operator=(const StageContext&) = delete;

    const StageDevice& device() const;
    StageDeviceType device_type() const;

    /* Compile through StageCompiler::instance(), caching per context. */
    std::shared_ptr<StageProgram> compile(const StageRequest& request,
                                          std::string* error = nullptr);

    /*
     * Compile (or reuse) and execute a node. On a device context the node is
     * dispatched to the device when selection and the device allow it, and
     * run on the host program otherwise. `execution`, when given, reports the
     * device that actually executed and whether the submission was
     * asynchronous, so a caller never has to infer it from the selection.
     */
    RtStageStatus execute(const StageRequest& request,
                          RtStageProfiling* profiling = nullptr,
                          StageExecution* execution = nullptr);

    StageQueue& queue();
    StageBufferAllocator& allocator();

    /* Host queue used for the per-node CPU fallback of a device context. */
    StageQueue& host_queue();

    /* Device residency of an OpenCL context; null on the CPU device. */
    StageDeviceResidency* residency();

    void clear_program_cache();
    std::size_t program_cache_size() const;

private:
    std::shared_ptr<StageDevice> device_;
    StageBufferAllocator allocator_;
    mutable std::mutex mutex_;
    std::unique_ptr<StageQueue> queue_;
    std::unique_ptr<StageQueue> host_queue_;
    std::map<std::string, std::shared_ptr<StageProgram>> programs_;
};

/*
 * The CPU device resolves built-ins and loadable modules through the existing
 * StageCompiler and executes synchronously. It is deterministic and works in
 * full precision.
 */
class CPUStageDevice final : public StageDevice
{
public:
    StageDeviceType type() const override;
    const char* name() const override;
    bool available() const override;
    StageCapabilities capabilities() const override;
    std::shared_ptr<StageContext> create_context() const override;
    std::unique_ptr<StageQueue> create_queue() const override;
};

/*
 * OpenCL device. Availability follows the backend that is compiled in and a
 * usable device; a CPU device is used only when RT_OPENCL_ALLOW_CPU=1. The
 * implementation lives in stage_opencl_device.cc so this header stays free of
 * OpenCL types.
 */
class OpenCLStageDevice final : public StageDevice
{
public:
    StageDeviceType type() const override;
    const char* name() const override;
    bool available() const override;
    StageCapabilities capabilities() const override;
    std::shared_ptr<StageContext> create_context() const override;
    std::unique_ptr<StageQueue> create_queue() const override;
};

std::shared_ptr<StageDevice> cpu_device();
std::shared_ptr<StageDevice> opencl_device();

/*
 * Select a device. CPU is the default and is always available. A request for
 * OpenCL falls back to CPU, with a reason, when the backend is not enabled.
 */
std::shared_ptr<StageDevice> select_device(StageDeviceType preferred,
                                           std::string* reason = nullptr);

} // namespace rtengine::stage
