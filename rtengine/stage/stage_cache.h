#pragma once

#include "stage_abi.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtengine::stage
{

struct StageKey {
    std::string stage;
    uint64_t params = 0;
    uint64_t inputs[4] = {0, 0, 0, 0};
    uint32_t level = 0;
    uint8_t consumer = RT_STAGE_EXACT;

    bool operator==(const StageKey& other) const
    {
        return stage == other.stage && params == other.params &&
               inputs[0] == other.inputs[0] && inputs[1] == other.inputs[1] &&
               inputs[2] == other.inputs[2] && inputs[3] == other.inputs[3] &&
               level == other.level && consumer == other.consumer;
    }

    bool operator!=(const StageKey& other) const
    {
        return !(*this == other);
    }
};

struct StageKeyHash {
    size_t operator()(const StageKey& key) const;
};

struct OwnedStageBuffer {
    RtStageBuffer view{};
    std::vector<unsigned char> storage;
};

/*
 * Accounting of one cache instance.
 *
 * `hits` and `misses` count probes: every peek() and every get() records
 * exactly one outcome; `fetch()` is the uncounted copy used after a probe.
 * `refused` counts probes the caller declined for policy reasons, such as the
 * work-order rule of the node cache. `inserts` counts accepted put() calls,
 * `rejects` the refused ones (invalid or larger than the whole budget).
 * `evictions` and `bytes_evicted` count entries removed to stay inside the
 * budget, including the removals a budget shrink triggers; replacing an
 * existing key is not an eviction. `bytes_used` and `bytes_budget` are
 * instantaneous, and the byte counters include the auxiliary payload.
 */
struct StageCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t refused = 0;
    uint64_t inserts = 0;
    uint64_t rejects = 0;
    uint64_t evictions = 0;
    uint64_t bytes_inserted = 0;
    uint64_t bytes_evicted = 0;
    uint64_t bytes_used = 0;
    uint64_t bytes_budget = 0;
};

class StageCache
{
public:
    explicit StageCache(size_t budget_bytes = 256u * 1024u * 1024u);

    void set_budget(size_t bytes);
    size_t budget() const;
    size_t used() const;

    /*
     * Number of pixel or auxiliary buffers the cache has allocated. It is a
     * test seam for the disabled path: a caller that never reaches put() must
     * leave it at zero. It is not an entry count and is never reset by
     * reset_stats().
     */
    uint64_t allocationCount() const;

    /*
     * Store a copy of `source`. `aux` is an optional opaque payload that shares
     * the entry and the budget; it is stored verbatim and handed back by get().
     * An entry is refused when the buffer is malformed or when the payload and
     * the auxiliary bytes together exceed the whole budget.
     */
    bool put(const StageKey& key, const RtStageBuffer& source,
             const void* aux = nullptr, size_t aux_bytes = 0);
    /*
     * Restore an entry. get() records a probe and refreshes the entry's
     * recency; fetch() is the uncounted copy that follows a probe and
     * refreshes recency without changing the counters. contains() is a
     * non-touching introspection and is deliberately not an access for the
     * least-recently-used order.
     */
    bool get(const StageKey& key, RtStageBuffer& destination,
             std::vector<unsigned char>* aux = nullptr) const;
    /* Copy an entry that a preceding peek() already accounted for. */
    bool fetch(const StageKey& key, RtStageBuffer& destination,
               std::vector<unsigned char>* aux = nullptr) const;
    bool contains(const StageKey& key) const;
    /* Membership probe that records a hit or a miss and refreshes recency. */
    bool peek(const StageKey& key) const;
    /* Record a probe the caller declined for policy reasons. */
    void note_refused();
    void clear();

    StageCacheStats stats() const;
    void reset_stats();

private:
    struct Entry {
        StageKey key;
        OwnedStageBuffer buffer;
        std::vector<unsigned char> aux;
        size_t bytes = 0;
        std::list<StageKey>::iterator lru;
    };

    /* Remove one entry; `evicted` separates capacity removals from replaces. */
    void erase_locked(
        std::unordered_map<StageKey, Entry, StageKeyHash>::iterator it,
        bool evicted);
    /* Move one entry to the front of the least-recently-used list. */
    void touch_locked(
        std::unordered_map<StageKey, Entry, StageKeyHash>::iterator it) const;

    bool peek_locked(const StageKey& key) const;
    bool copy_locked(const Entry& entry, RtStageBuffer& destination,
                     std::vector<unsigned char>* aux) const;

    mutable std::mutex mutex_;
    size_t budget_ = 0;
    size_t used_ = 0;
    mutable std::list<StageKey> lru_;
    mutable std::unordered_map<StageKey, Entry, StageKeyHash> entries_;
    mutable StageCacheStats stats_;
    mutable uint64_t allocations_ = 0;
};

} // namespace rtengine::stage
