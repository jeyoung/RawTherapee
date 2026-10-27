#include "stage_cache.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

RtStageBuffer makeBuffer(std::vector<float>& data, int width, int height,
                         int planes = 3)
{
    RtStageBuffer buffer{};
    buffer.size = sizeof(buffer);
    buffer.data = data.data();
    buffer.width = width;
    buffer.height = height;
    buffer.row_pitch = static_cast<size_t>(width) * sizeof(float);
    buffer.plane_pitch = buffer.row_pitch * static_cast<size_t>(height);
    buffer.planes = static_cast<uint32_t>(planes);
    buffer.format = RT_STAGE_F32;
    buffer.flags = RT_STAGE_BUFFER_READ;
    return buffer;
}

} // namespace

int main()
{
    using namespace rtengine::stage;

    constexpr int width = 16;
    constexpr int height = 12;
    std::vector<float> source_data(width * height * 3);
    for (size_t i = 0; i < source_data.size(); ++i) {
        source_data[i] = static_cast<float>(i);
    }

    RtStageBuffer source = makeBuffer(source_data, width, height);

    std::vector<float> destination_data(source_data.size(), 0.0f);
    RtStageBuffer destination = makeBuffer(destination_data, width, height);

    StageCache cache(4096);
    StageKey key;
    key.stage = "test";
    key.params = 42;
    key.inputs[0] = 7;

    check(cache.put(key, source), "cache stores a buffer");
    check(cache.contains(key), "cache contains the key");
    check(cache.get(key, destination), "cache restores the buffer");
    check(std::memcmp(source_data.data(), destination_data.data(),
                      source_data.size() * sizeof(float)) == 0,
          "restored bytes match");

    StageKey other = key;
    other.params = 43;
    check(!cache.get(other, destination), "different key misses");

    StageCache small(1024);
    check(!small.put(key, source), "oversized entry is rejected");

    cache.clear();
    check(!cache.contains(key), "clear removes entries");

    // Auxiliary payload: stored in the same entry and handed back by get().
    {
        StageCache aux_cache(4096);
        const std::string aux = "tier-state";
        check(aux_cache.put(key, source, aux.data(), aux.size()),
              "entry with an auxiliary payload is stored");
        std::vector<unsigned char> restored;
        check(aux_cache.get(key, destination, &restored),
              "entry with an auxiliary payload is restored");
        check(std::string(restored.begin(), restored.end()) == aux,
              "auxiliary payload round-trips");
    }

    // Counters: probes, stores and refusals.
    {
        StageCache counted(4096);
        counted.reset_stats();
        check(!counted.peek(key), "peek miss is reported");
        StageCacheStats stats = counted.stats();
        check(stats.misses == 1 && stats.hits == 0,
              "peek miss increments misses only");

        check(counted.put(key, source), "counted store succeeds");
        stats = counted.stats();
        check(stats.inserts == 1 && stats.rejects == 0,
              "accepted store increments inserts");
        check(stats.bytes_used == source_data.size() * sizeof(float),
              "used bytes count the entry");

        check(counted.peek(key), "peek hit is reported");
        stats = counted.stats();
        check(stats.hits == 1, "peek hit increments hits");

        // fetch() is the uncounted copy that follows a probe.
        check(counted.fetch(key, destination), "fetch copies without counting");
        stats = counted.stats();
        check(stats.hits == 1 && stats.misses == 1,
              "fetch does not change hit or miss");

        counted.note_refused();
        check(counted.stats().refused == 1, "refusal is counted");

        counted.reset_stats();
        stats = counted.stats();
        check(stats.hits == 0 && stats.misses == 0 && stats.inserts == 0,
              "reset clears the counters");
        check(counted.used() == source_data.size() * sizeof(float),
              "reset keeps the entries");
    }

    // Eviction: a second entry over budget removes the least recently used.
    {
        // One entry is 16 * 12 * 3 * 4 = 2304 bytes; a 3000-byte budget holds
        // exactly one.
        StageCache evicting(3000);
        StageKey first;
        first.stage = "first";
        StageKey second;
        second.stage = "second";

        check(evicting.put(first, source), "first entry fits");
        check(evicting.put(second, source), "second entry replaces the first");
        StageCacheStats stats = evicting.stats();
        check(stats.evictions == 1, "capacity eviction is counted");
        check(stats.bytes_evicted == source_data.size() * sizeof(float),
              "evicted bytes are counted");
        check(!evicting.contains(first), "least recently used entry is gone");
        check(evicting.contains(second), "newest entry is kept");
        check(evicting.used() <= evicting.budget(), "used stays within budget");
    }

    // Replacing the same key is not an eviction.
    {
        StageCache replacing(4096);
        check(replacing.put(key, source), "first insert");
        check(replacing.put(key, source), "replacement insert");
        check(replacing.stats().evictions == 0,
              "replacing a key is not an eviction");
        check(replacing.stats().inserts == 2, "both inserts are counted");
    }

    // A budget shrink evicts down to the new limit.
    {
        StageCache shrinking(4096);
        check(shrinking.put(key, source), "entry before shrink");
        shrinking.set_budget(1024);
        check(shrinking.used() == 0, "budget shrink evicts the entry");
        check(shrinking.stats().evictions == 1, "shrink eviction is counted");
    }

    // A malformed auxiliary request is refused with a counter.
    {
        StageCache refusing(4096);
        check(!refusing.put(key, source, nullptr, 16),
              "aux bytes without a pointer are refused");
        check(refusing.stats().rejects == 1, "refusal is counted as a reject");
    }

    if (failures == 0) {
        std::puts("stage-cache-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
