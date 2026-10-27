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
            erase_locked(found, true);
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
    std::unordered_map<StageKey, Entry, StageKeyHash>::iterator it,
    bool evicted)
{
    const size_t bytes = it->second.bytes;
    used_ -= bytes;
    lru_.erase(it->second.lru);
    entries_.erase(it);
    if (evicted) {
        ++stats_.evictions;
        stats_.bytes_evicted += bytes;
    }
}

bool StageCache::put(const StageKey& key, const RtStageBuffer& source,
                     const void* aux, size_t aux_bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (aux == nullptr && aux_bytes != 0) {
        ++stats_.rejects;
        return false;
    }
    if (source.data == nullptr || source.width <= 0 || source.height <= 0 ||
        source.planes == 0) {
        ++stats_.rejects;
        return false;
    }

    const size_t row = active_row_bytes(source);
    const size_t pixels = row * static_cast<size_t>(source.height) *
                          static_cast<size_t>(source.planes);
    const size_t bytes = pixels + aux_bytes;
    if (pixels == 0 || bytes > budget_) {
        ++stats_.rejects;
        return false;
    }

    // A re-insert of the same key replaces the old entry instead of evicting
    // it; the replacement is not a capacity event.
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        erase_locked(existing, false);
    }
    while (used_ + bytes > budget_ && !lru_.empty()) {
        auto victim = entries_.find(lru_.back());
        if (victim == entries_.end()) {
            lru_.pop_back();
        } else {
            erase_locked(victim, true);
        }
    }

    OwnedStageBuffer owned;
    owned.storage.resize(pixels);
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

    Entry entry;
    entry.key = key;
    entry.buffer = std::move(owned);
    if (aux_bytes != 0) {
        const unsigned char* bytes_in =
            static_cast<const unsigned char*>(aux);
        entry.aux.assign(bytes_in, bytes_in + aux_bytes);
    }
    entry.bytes = bytes;
    lru_.push_front(key);
    entry.lru = lru_.begin();
    entries_.emplace(key, std::move(entry));
    used_ += bytes;
    ++stats_.inserts;
    stats_.bytes_inserted += bytes;
    return true;
}

bool StageCache::peek_locked(const StageKey& key) const
{
    const bool found = entries_.find(key) != entries_.end();
    if (found) {
        ++stats_.hits;
    } else {
        ++stats_.misses;
    }
    return found;
}

bool StageCache::get(const StageKey& key, RtStageBuffer& destination,
                     std::vector<unsigned char>* aux) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found == entries_.end()) {
        ++stats_.misses;
        return false;
    }
    ++stats_.hits;
    return copy_locked(found->second, destination, aux);
}

bool StageCache::fetch(const StageKey& key, RtStageBuffer& destination,
                       std::vector<unsigned char>* aux) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    return copy_locked(found->second, destination, aux);
}

bool StageCache::copy_locked(const Entry& entry, RtStageBuffer& destination,
                             std::vector<unsigned char>* aux) const
{
    const OwnedStageBuffer& owned = entry.buffer;
    if (destination.data == nullptr || destination.width != owned.view.width ||
        destination.height != owned.view.height ||
        destination.planes != owned.view.planes ||
        destination.format != owned.view.format ||
        destination.row_pitch < owned.view.row_pitch) {
        // The entry is present but cannot be delivered into this view. Count
        // the probe as a hit, because lookup succeeded, and leave the caller
        // to treat the failed restore as unusable.
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
    if (aux != nullptr) {
        *aux = entry.aux;
    }
    return true;
}

bool StageCache::contains(const StageKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(key) != entries_.end();
}

bool StageCache::peek(const StageKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return peek_locked(key);
}

void StageCache::note_refused()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refused;
}

void StageCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
    used_ = 0;
}

StageCacheStats StageCache::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    StageCacheStats snapshot = stats_;
    snapshot.bytes_used = used_;
    snapshot.bytes_budget = budget_;
    return snapshot;
}

void StageCache::reset_stats()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = StageCacheStats{};
}

} // namespace rtengine::stage
