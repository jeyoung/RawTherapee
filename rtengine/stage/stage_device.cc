#include "stage_device.h"

#include "stage_buffer.h"

#include "opencl/openclengine.h"

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
        if (profiling != nullptr) {
            *profiling = local;
        }
        return event;
    }

    RtStageStatus wait(const StageEvent& event) const override
    {
        return event.result;
    }
};

class OpenCLStageQueue final : public StageQueue
{
public:
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
        return event;
    }

    RtStageStatus wait(const StageEvent& event) const override
    {
        return event.result;
    }
};

} // namespace

const char* to_string(StageDeviceType type)
{
    switch (type) {
    case StageDeviceType::CPU:
        return "cpu";
    case StageDeviceType::OPENCL:
        return "opencl";
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
        return fail("device buffer allocation is not implemented");
    }
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

    const std::size_t row_pitch = round_up(row_bytes, STAGE_BUFFER_ALIGNMENT);
    if (height > 0 && row_pitch > limit / height) {
        return fail("buffer plane overflows");
    }
    const std::size_t plane_pitch = row_pitch * height;
    if (plane_pitch > limit / planes) {
        return fail("buffer allocation overflows");
    }
    const std::size_t total = round_up(plane_pitch * planes, STAGE_BUFFER_ALIGNMENT);

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
                                    RtStageProfiling* profiling)
{
    auto program = compile(request, nullptr);
    if (!program) {
        return RT_STAGE_FALLBACK;
    }

    StageQueue& context_queue = queue();
    const StageEvent event = context_queue.submit(*program, request.args, profiling);
    return context_queue.wait(event);
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
    /* Existing backend state only; this skeleton adds no OpenCL calls. */
    return rtengine::opencl::enabled();
}

StageCapabilities OpenCLStageDevice::capabilities() const
{
    /* No execution path is wired yet, so no guarantee can be advertised. */
    return StageCapabilities{};
}

std::shared_ptr<StageContext> OpenCLStageDevice::create_context() const
{
    return std::make_shared<StageContext>(std::make_shared<OpenCLStageDevice>());
}

std::unique_ptr<StageQueue> OpenCLStageDevice::create_queue() const
{
    return std::make_unique<OpenCLStageQueue>();
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
        std::make_shared<OpenCLStageDevice>();
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
