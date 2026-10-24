#include "stage_cache.h"

#include <cstring>

namespace rtengine::stage
{

namespace
{

size_t element_size(RtStageFormat format)
{
    switch (format) {
    case RT_STAGE_F32:
        return sizeof(float);
    case RT_STAGE_U16:
        return sizeof(uint16_t);
    case RT_STAGE_U8:
        return sizeof(uint8_t);
    }
    return 0;
}

size_t active_row_bytes(const RtStageBuffer& buffer)
{
    const size_t elements = buffer.format == RT_STAGE_U8 ? 3u : 1u;
    return static_cast<size_t>(buffer.width) * element_size(buffer.format) * elements;
}

void mix(uint64_t& hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

} // namespace

size_t StageKeyHash::operator()(const StageKey& key) const
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : key.stage) {
        mix(hash, c);
    }
    mix(hash, key.params);
    for (uint64_t value : key.inputs) {
        mix(hash, value);
    }
    mix(hash, key.level);
    mix(hash, key.consumer);
    return static_cast<size_t>(hash);
}

StageCache::StageCache(size_t budget_bytes) : budget_(budget_bytes) {}

void StageCache::set_budget(size_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    while (used_ > budget_ && !lru_.empty()) {
        auto found = entries_.find(lru_.back());
        if (found == entries_.end()) {
            lru_.pop_back();
        } else {
            erase_locked(found);
        }
    }
}

size_t StageCache::budget() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_;
}

size_t StageCache::used() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return used_;
}

void StageCache::erase_locked(
    std::unordered_map<StageKey, Entry, StageKeyHash>::iterator it)
{
    used_ -= it->second.bytes;
    lru_.erase(it->second.lru);
    entries_.erase(it);
}

bool StageCache::put(const StageKey& key, const RtStageBuffer& source)
{
    if (source.data == nullptr || source.width <= 0 || source.height <= 0 ||
        source.planes == 0) {
        return false;
    }

    const size_t row = active_row_bytes(source);
    const size_t bytes = row * static_cast<size_t>(source.height) *
                         static_cast<size_t>(source.planes);
    if (bytes == 0 || bytes > budget_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        erase_locked(existing);
    }
    while (used_ + bytes > budget_ && !lru_.empty()) {
        auto victim = entries_.find(lru_.back());
        if (victim == entries_.end()) {
            lru_.pop_back();
        } else {
            erase_locked(victim);
        }
    }

    OwnedStageBuffer owned;
    owned.storage.resize(bytes);
    owned.view.size = sizeof(RtStageBuffer);
    owned.view.data = owned.storage.data();
    owned.view.width = source.width;
    owned.view.height = source.height;
    owned.view.row_pitch = row;
    owned.view.plane_pitch = row * static_cast<size_t>(source.height);
    owned.view.planes = source.planes;
    owned.view.format = source.format;
    owned.view.flags = source.flags;

    for (uint32_t plane = 0; plane < source.planes; ++plane) {
        const unsigned char* src = static_cast<const unsigned char*>(source.data) +
                                   static_cast<size_t>(plane) * source.plane_pitch;
        unsigned char* dst = owned.storage.data() +
                             static_cast<size_t>(plane) * owned.view.plane_pitch;
        for (int y = 0; y < source.height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * row,
                        src + static_cast<size_t>(y) * source.row_pitch, row);
        }
    }

    lru_.push_front(key);
    entries_.emplace(key, Entry{key, std::move(owned), bytes, lru_.begin()});
    used_ += bytes;
    return true;
}

bool StageCache::get(const StageKey& key, RtStageBuffer& destination) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    const OwnedStageBuffer& owned = found->second.buffer;
    if (destination.data == nullptr || destination.width != owned.view.width ||
        destination.height != owned.view.height ||
        destination.planes != owned.view.planes ||
        destination.format != owned.view.format ||
        destination.row_pitch < owned.view.row_pitch) {
        return false;
    }

    const size_t row = owned.view.row_pitch;
    for (uint32_t plane = 0; plane < owned.view.planes; ++plane) {
        const unsigned char* src = owned.storage.data() +
                                   static_cast<size_t>(plane) * owned.view.plane_pitch;
        unsigned char* dst = static_cast<unsigned char*>(destination.data) +
                             static_cast<size_t>(plane) * destination.plane_pitch;
        for (int y = 0; y < owned.view.height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * destination.row_pitch,
                        src + static_cast<size_t>(y) * row, row);
        }
    }
    return true;
}

bool StageCache::contains(const StageKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(key) != entries_.end();
}

void StageCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
    used_ = 0;
}

} // namespace rtengine::stage
