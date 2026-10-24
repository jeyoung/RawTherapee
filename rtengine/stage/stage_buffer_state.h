#pragma once

#include "stage_abi.h"

#include <atomic>
#include <cstdint>

namespace rtengine::stage
{

/* Monotonic identifier of a device session; a changed generation invalidates
 * device-side copies created by an earlier session. */
using StageDeviceGeneration = std::uint64_t;

/*
 * Where the authoritative contents of a stage buffer live.
 *
 * HostValid   the host copy is current; a device copy is absent or stale
 * DeviceValid the device copy is current; the host copy is stale
 * Both        both copies are current
 * Dirty       neither copy may be read
 */
enum class StageBufferLocation {
    HostValid,
    DeviceValid,
    Both,
    Dirty
};

const char* to_string(StageBufferLocation location);

/*
 * Intrusive, location-aware wrapper around an RtStageBuffer descriptor.
 *
 * The wrapper does not own the memory: the descriptor is a non-owning view of
 * an engine image or a StageBufferAllocator block. It adds the state a device
 * scheduler needs, so the same buffer can move between host and device without
 * changing the descriptor or copying data unnecessarily.
 *
 * The header is deliberately free of OpenCL types; an OpenCL implementation
 * can add readback and upload paths in stage_buffer_state.cc without changing
 * this interface. The wrapper is not copyable because the reference count is
 * per instance; hold it through a pointer or a container that never copies it.
 */
class StageBufferState
{
public:
    explicit StageBufferState(const RtStageBuffer& buffer,
                              StageDeviceGeneration generation = 0);
    ~StageBufferState() = default;

    StageBufferState(const StageBufferState&) = delete;
    StageBufferState& operator=(const StageBufferState&) = delete;

    const RtStageBuffer& buffer() const;
    RtStageBuffer& buffer();

    StageBufferLocation location() const;
    StageDeviceGeneration generation() const;
    int ref_count() const;

    /* Take an additional reference. */
    void add_ref();

    /* Drop a reference; returns true when this call released the last one. */
    bool release();

    /*
     * Ensure the host copy is current.
     *
     * CPU buffers are always host-resident in this phase, so the call performs
     * no copy and no allocation: it only validates the state. A stale device
     * copy cannot be read back yet and reports RT_STAGE_FALLBACK; dirty
     * contents report RT_STAGE_INVALID.
     */
    RtStageStatus ensure_host();

    /*
     * Ensure the device copy is current for the given device generation.
     *
     * The CPU device has no device memory, so this is unsupported and reports
     * RT_STAGE_FALLBACK. A future OpenCL implementation uploads a host-valid
     * buffer and calls mark_both_valid() on success.
     */
    RtStageStatus ensure_device(StageDeviceGeneration generation);

    /* The host wrote the whole buffer; any device copy is now stale. */
    void mark_host_write();

    /* The device produced the whole buffer for the given generation. */
    void mark_device_write(StageDeviceGeneration generation);

    /* Both copies are current for the given generation. */
    void mark_both_valid(StageDeviceGeneration generation);

    /* The contents are no longer readable on either side. */
    void mark_dirty();

private:
    RtStageBuffer buffer_{};
    std::atomic<StageBufferLocation> location_{StageBufferLocation::HostValid};
    std::atomic<StageDeviceGeneration> generation_{0};
    std::atomic<int> ref_count_{1};
};

} // namespace rtengine::stage
