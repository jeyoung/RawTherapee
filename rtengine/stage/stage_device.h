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

enum class StageDeviceType {
    CPU,
    OPENCL
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
 * Result of one queue submission. There is no asynchronous dependency yet:
 * every queue in this phase completes or fails before submit() returns, so the
 * event only carries status and timing.
 */
struct StageEvent {
    StageEventStatus status = StageEventStatus::Pending;
    RtStageStatus result = RT_STAGE_OK;
    std::uint64_t elapsed_ns = 0;

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
 * The CPU queue executes inline before submit() returns. The OpenCL skeleton
 * has no execution path in this phase and reports RT_STAGE_FALLBACK.
 */
class StageQueue
{
public:
    virtual ~StageQueue() = default;

    virtual StageDeviceType type() const = 0;

    virtual StageEvent submit(const StageProgram& program,
                              const std::vector<RtStageArg>& args,
                              RtStageProfiling* profiling = nullptr) = 0;

    /* Synchronous queues have already finished by the time submit() returns. */
    virtual RtStageStatus wait(const StageEvent& event) const = 0;
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

/*
 * Buffer allocator owned by a StageContext.
 *
 * The CPU allocator owns aligned, zeroed host memory and releases it in
 * release(). The OpenCL allocator is a skeleton: it refuses allocation until
 * a device allocator is wired, so no OpenCL call is made from here.
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
     * Allocate a zeroed buffer. Returns a descriptor with a null data pointer
     * and sets error when the shape is invalid or the device has no allocator.
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

    /* Compile (or reuse) and execute synchronously on the context queue. */
    RtStageStatus execute(const StageRequest& request,
                          RtStageProfiling* profiling = nullptr);

    StageQueue& queue();
    StageBufferAllocator& allocator();

    void clear_program_cache();
    std::size_t program_cache_size() const;

private:
    std::shared_ptr<StageDevice> device_;
    StageBufferAllocator allocator_;
    mutable std::mutex mutex_;
    std::unique_ptr<StageQueue> queue_;
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
 * OpenCL skeleton. Availability maps to the existing backend's enabled state;
 * it adds no OpenCL calls of its own. The device advertises no execution
 * capability until the backend is wired, so its queue reports
 * RT_STAGE_FALLBACK.
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
