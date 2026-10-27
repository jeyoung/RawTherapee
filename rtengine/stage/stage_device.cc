#include "stage_device.h"

#include "stage_buffer.h"
#include "stage_opencl_device.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace rtengine::stage
{

namespace
{

std::size_t bytes_per_sample(RtStageFormat format)
{
    switch (format) {
    case RT_STAGE_F32:
        return sizeof(float);
    case RT_STAGE_U16:
        return sizeof(std::uint16_t);
    case RT_STAGE_U8:
        return sizeof(std::uint8_t);
    }
    return 0;
}

std::size_t round_up(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

class CPUStageQueue final : public StageQueue
{
public:
    StageDeviceType type() const override
    {
        return StageDeviceType::CPU;
    }

    StageEvent submit(const StageProgram& program,
                      const std::vector<RtStageArg>& args,
                      RtStageProfiling* profiling) override
    {
        RtStageProfiling local{};
        const auto begin = std::chrono::steady_clock::now();
        const RtStageStatus status = program.execute(args, local);
        const auto end = std::chrono::steady_clock::now();
        local.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());

        StageEvent event;
        event.status = status == RT_STAGE_OK ? StageEventStatus::Complete
                                             : StageEventStatus::Failed;
        event.result = status;
        event.elapsed_ns = local.elapsed_ns;
        event.device = StageDeviceType::CPU;
        event.asynchronous = false;
        if (profiling != nullptr) {
            *profiling = local;
        }
        return event;
    }

    RtStageStatus wait(StageEvent& event) const override
    {
        return event.result;
    }
};

} // namespace

bool stage_buffer_layout(const StageBufferShape& shape, std::size_t& row_pitch,
                         std::size_t& plane_pitch, std::size_t& bytes,
                         std::string* error)
{
    const auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (shape.width <= 0 || shape.height <= 0 || shape.planes == 0 ||
        shape.planes > 3) {
        return fail("invalid buffer shape");
    }

    const std::size_t sample_bytes = bytes_per_sample(shape.format);
    if (sample_bytes == 0) {
        return fail("unsupported buffer format");
    }

    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    const std::size_t width = static_cast<std::size_t>(shape.width);
    const std::size_t height = static_cast<std::size_t>(shape.height);
    const std::size_t planes = static_cast<std::size_t>(shape.planes);

    if (shape.format == RT_STAGE_U8 && shape.planes != 1) {
        return fail("packed U8 is a single-plane format");
    }

    if (width > limit / sample_bytes) {
        return fail("buffer row overflows");
    }
    std::size_t row_bytes = width * sample_bytes;
    if (shape.format == RT_STAGE_U8 && shape.planes == 1) {
        /* Single-plane U8 is packed RGB, matching stage::row_bytes(). */
        if (row_bytes > limit / 3) {
            return fail("buffer row overflows");
        }
        row_bytes *= 3;
    }

    row_pitch = round_up(row_bytes, STAGE_BUFFER_ALIGNMENT);
    if (height > 0 && row_pitch > limit / height) {
        return fail("buffer plane overflows");
    }
    plane_pitch = row_pitch * height;
    if (plane_pitch > limit / planes) {
        return fail("buffer allocation overflows");
    }
    bytes = round_up(plane_pitch * planes, STAGE_BUFFER_ALIGNMENT);
    return true;
}

const char* to_string(StageDeviceType type)
{
    switch (type) {
    case StageDeviceType::CPU:
        return "cpu";
    case StageDeviceType::OPENCL:
        return "opencl";
    case StageDeviceType::NONE:
        return "none";
    }
    assert(false);
    return "unknown";
}

const char* to_string(StageEventStatus status)
{
    switch (status) {
    case StageEventStatus::Pending:
        return "pending";
    case StageEventStatus::Running:
        return "running";
    case StageEventStatus::Complete:
        return "complete";
    case StageEventStatus::Failed:
        return "failed";
    }
    assert(false);
    return "unknown";
}

StageEvent StageQueue::submit(const StageRequest& request,
                              RtStageProfiling* profiling)
{
    std::string error;
    auto program = StageCompiler::instance().compile(request, &error);
    if (!program) {
        StageEvent event;
        event.status = StageEventStatus::Failed;
        event.result = RT_STAGE_FALLBACK;
        event.device = StageDeviceType::NONE;
        event.note = error;
        return event;
    }

    /*
     * The host guarantee every queue keeps: a node without a device
     * implementation runs here, inline, on the host. It is deliberately not
     * submitted to `this`, because a device queue cannot execute a host
     * function pointer.
     */
    CPUStageQueue host;
    StageEvent event = host.submit(*program, request.args, profiling);
    event.device = StageDeviceType::CPU;
    return event;
}

StageEventStatus StageQueue::poll(StageEvent& event) const
{
    if (event.submission) {
        event.status = event.submission->poll();
        event.elapsed_ns = event.submission->elapsed_ns();
    }
    return event.status;
}

StageBufferAllocator::StageBufferAllocator(StageDeviceType type) : type_(type)
{
}

StageBufferAllocator::~StageBufferAllocator()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& block : blocks_) {
        std::free(block.first);
    }
    blocks_.clear();
}

RtStageBuffer StageBufferAllocator::allocate(const StageBufferShape& shape,
                                             std::string* error)
{
    const auto fail = [error](const char* message) -> RtStageBuffer {
        if (error != nullptr) {
            *error = message;
        }
        return RtStageBuffer{};
    };

    if (type_ != StageDeviceType::CPU) {
        return fail("device-local buffers are allocated by the device residency");
    }

    std::size_t row_pitch = 0;
    std::size_t plane_pitch = 0;
    std::size_t total = 0;
    std::string layout_error;
    if (!stage_buffer_layout(shape, row_pitch, plane_pitch, total,
                             &layout_error)) {
        return fail(layout_error.c_str());
    }

    void* data = std::aligned_alloc(STAGE_BUFFER_ALIGNMENT, total);
    if (data == nullptr) {
        return fail("buffer allocation failed");
    }
    std::memset(data, 0, total);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_[data] = total;
    }

    RtStageBuffer buffer{};
    buffer.size = sizeof(buffer);
    buffer.data = data;
    buffer.width = shape.width;
    buffer.height = shape.height;
    buffer.row_pitch = row_pitch;
    buffer.plane_pitch = plane_pitch;
    buffer.planes = shape.planes;
    buffer.format = shape.format;
    buffer.flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;
    return buffer;
}

bool StageBufferAllocator::release(RtStageBuffer& buffer)
{
    if (buffer.data == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = blocks_.find(buffer.data);
        if (found == blocks_.end()) {
            return false;
        }
        blocks_.erase(found);
    }

    std::free(buffer.data);
    buffer = RtStageBuffer{};
    return true;
}

bool StageBufferAllocator::owns(const RtStageBuffer& buffer) const
{
    if (buffer.data == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.find(buffer.data) != blocks_.end();
}

StageContext::StageContext() : StageContext(select_device(StageDeviceType::CPU))
{
}

StageContext::StageContext(std::shared_ptr<StageDevice> device)
    : device_(device != nullptr ? std::move(device)
                                : select_device(StageDeviceType::CPU)),
      allocator_(device_ != nullptr ? device_->type() : StageDeviceType::CPU)
{
    assert(device_ != nullptr);
}

StageContext::~StageContext() = default;

const StageDevice& StageContext::device() const
{
    assert(device_ != nullptr);
    return *device_;
}

StageDeviceType StageContext::device_type() const
{
    assert(device_ != nullptr);
    return device_->type();
}

std::shared_ptr<StageProgram> StageContext::compile(const StageRequest& request,
                                                    std::string* error)
{
    const std::string key = request.name + '\n' + request.impl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = programs_.find(key);
        if (found != programs_.end()) {
            return found->second;
        }
    }

    auto program = StageCompiler::instance().compile(request, error);
    if (!program) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto inserted = programs_.emplace(key, std::move(program));
    return inserted.first->second;
}

RtStageStatus StageContext::execute(const StageRequest& request,
                                    RtStageProfiling* profiling,
                                    StageExecution* execution)
{
    if (execution != nullptr) {
        *execution = StageExecution{};
    }

    if (device_type() == StageDeviceType::OPENCL &&
        node_device(request.name.c_str()) == StageNodeDevice::OpenCL) {
        /*
         * The node was selected for the device. The queue reports the device
         * that actually ran, so a budget or launch refusal inside the queue is
         * visible to the caller instead of being papered over by the
         * selection.
         */
        StageQueue& context_queue = queue();
        StageEvent event = context_queue.submit(request, nullptr);
        const RtStageStatus status = context_queue.wait(event);
        if (status == RT_STAGE_OK) {
            if (profiling != nullptr) {
                *profiling = RtStageProfiling{};
                profiling->elapsed_ns = event.elapsed_ns;
            }
            if (execution != nullptr) {
                execution->device = event.device;
                execution->asynchronous = event.asynchronous;
            }
            return status;
        }
    }

    auto program = compile(request, nullptr);
    if (!program) {
        return RT_STAGE_FALLBACK;
    }

    /*
     * A device queue only runs device kernels. The host program therefore runs
     * on a host queue, never on the device queue that refused it.
     */
    StageQueue& context_queue =
        device_type() == StageDeviceType::OPENCL ? host_queue() : queue();
    StageEvent event = context_queue.submit(*program, request.args, profiling);
    const RtStageStatus status = context_queue.wait(event);
    if (execution != nullptr) {
        execution->device = event.device;
        execution->asynchronous = event.asynchronous;
    }
    return status;
}

StageQueue& StageContext::host_queue()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host_queue_) {
        host_queue_ = cpu_device()->create_queue();
    }
    return *host_queue_;
}

StageQueue& StageContext::queue()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_) {
        queue_ = device_->create_queue();
    }
    assert(queue_ != nullptr);
    return *queue_;
}

StageBufferAllocator& StageContext::allocator()
{
    return allocator_;
}

StageDeviceResidency* StageContext::residency()
{
    if (device_type() != StageDeviceType::OPENCL) {
        return nullptr;
    }
    return queue().residency();
}

void StageContext::clear_program_cache()
{
    std::lock_guard<std::mutex> lock(mutex_);
    programs_.clear();
}

std::size_t StageContext::program_cache_size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return programs_.size();
}

StageDeviceType CPUStageDevice::type() const
{
    return StageDeviceType::CPU;
}

const char* CPUStageDevice::name() const
{
    return "cpu";
}

bool CPUStageDevice::available() const
{
    return true;
}

StageCapabilities CPUStageDevice::capabilities() const
{
    StageCapabilities capabilities;
    capabilities.deterministic = true;
    capabilities.full_precision = true;
    return capabilities;
}

std::shared_ptr<StageContext> CPUStageDevice::create_context() const
{
    return std::make_shared<StageContext>(std::make_shared<CPUStageDevice>());
}

std::unique_ptr<StageQueue> CPUStageDevice::create_queue() const
{
    return std::make_unique<CPUStageQueue>();
}

std::shared_ptr<StageDevice> cpu_device()
{
    static const std::shared_ptr<StageDevice> device =
        std::make_shared<CPUStageDevice>();
    return device;
}

std::shared_ptr<StageDevice> opencl_device()
{
    static const std::shared_ptr<StageDevice> device =
        make_opencl_stage_device();
    return device;
}

std::shared_ptr<StageDevice> select_device(StageDeviceType preferred,
                                           std::string* reason)
{
    if (preferred == StageDeviceType::CPU) {
        if (reason != nullptr) {
            reason->clear();
        }
        return cpu_device();
    }

    assert(preferred == StageDeviceType::OPENCL);
    auto device = opencl_device();
    if (device->available()) {
        if (reason != nullptr) {
            reason->clear();
        }
        return device;
    }

    if (reason != nullptr) {
        *reason = "OpenCL stage device is unavailable; using CPU";
    }
    return cpu_device();
}

} // namespace rtengine::stage
