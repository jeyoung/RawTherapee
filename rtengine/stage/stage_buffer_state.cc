#include "stage_buffer_state.h"

#include <cassert>

namespace rtengine::stage
{

const char* to_string(StageBufferLocation location)
{
    switch (location) {
    case StageBufferLocation::HostValid:
        return "host-valid";
    case StageBufferLocation::DeviceValid:
        return "device-valid";
    case StageBufferLocation::Both:
        return "both";
    case StageBufferLocation::Dirty:
        return "dirty";
    }
    assert(false);
    return "unknown";
}

StageBufferState::StageBufferState(const RtStageBuffer& buffer,
                                   StageDeviceGeneration generation)
    : buffer_(buffer), generation_(generation)
{
}

const RtStageBuffer& StageBufferState::buffer() const
{
    return buffer_;
}

RtStageBuffer& StageBufferState::buffer()
{
    return buffer_;
}

StageBufferLocation StageBufferState::location() const
{
    return location_.load();
}

StageDeviceGeneration StageBufferState::generation() const
{
    return generation_.load();
}

int StageBufferState::ref_count() const
{
    return ref_count_.load();
}

void StageBufferState::add_ref()
{
    int previous = ref_count_.load();
    do {
        assert(previous > 0);
        if (previous <= 0) {
            return;
        }
    } while (!ref_count_.compare_exchange_weak(previous, previous + 1));
}

bool StageBufferState::release()
{
    int previous = ref_count_.load();
    do {
        if (previous <= 0) {
            assert(false);
            return true;
        }
    } while (!ref_count_.compare_exchange_weak(previous, previous - 1));
    return previous == 1;
}

RtStageStatus StageBufferState::ensure_host()
{
    switch (location()) {
    case StageBufferLocation::HostValid:
    case StageBufferLocation::Both:
        return RT_STAGE_OK;
    case StageBufferLocation::DeviceValid:
        /* No device-to-host readback exists in this phase. */
        return RT_STAGE_FALLBACK;
    case StageBufferLocation::Dirty:
        return RT_STAGE_INVALID;
    }
    return RT_STAGE_INVALID;
}

RtStageStatus StageBufferState::ensure_device(StageDeviceGeneration generation)
{
    (void)generation;
    /* The CPU device has no device memory; OpenCL upload is not wired yet. */
    return RT_STAGE_FALLBACK;
}

void StageBufferState::mark_host_write()
{
    location_.store(StageBufferLocation::HostValid);
}

void StageBufferState::mark_device_write(StageDeviceGeneration generation)
{
    generation_.store(generation);
    location_.store(StageBufferLocation::DeviceValid);
}

void StageBufferState::mark_both_valid(StageDeviceGeneration generation)
{
    generation_.store(generation);
    location_.store(StageBufferLocation::Both);
}

void StageBufferState::mark_dirty()
{
    location_.store(StageBufferLocation::Dirty);
}

} // namespace rtengine::stage
