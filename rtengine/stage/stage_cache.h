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
};

struct StageKeyHash {
    size_t operator()(const StageKey& key) const;
};

struct OwnedStageBuffer {
    RtStageBuffer view{};
    std::vector<unsigned char> storage;
};

class StageCache
{
public:
    explicit StageCache(size_t budget_bytes = 256u * 1024u * 1024u);

    void set_budget(size_t bytes);
    size_t budget() const;
    size_t used() const;

    bool put(const StageKey& key, const RtStageBuffer& source);
    bool get(const StageKey& key, RtStageBuffer& destination) const;
    bool contains(const StageKey& key) const;
    void clear();

private:
    struct Entry {
        StageKey key;
        OwnedStageBuffer buffer;
        size_t bytes = 0;
        std::list<StageKey>::iterator lru;
    };

    void erase_locked(std::unordered_map<StageKey, Entry, StageKeyHash>::iterator it);

    mutable std::mutex mutex_;
    size_t budget_ = 0;
    size_t used_ = 0;
    std::list<StageKey> lru_;
    std::unordered_map<StageKey, Entry, StageKeyHash> entries_;
};

} // namespace rtengine::stage
