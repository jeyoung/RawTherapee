#include "stage_cache.h"

#include <cstdio>
#include <cstring>
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

    RtStageBuffer source{};
    source.size = sizeof(source);
    source.data = source_data.data();
    source.width = width;
    source.height = height;
    source.row_pitch = width * sizeof(float);
    source.plane_pitch = source.row_pitch * height;
    source.planes = 3;
    source.format = RT_STAGE_F32;
    source.flags = RT_STAGE_BUFFER_READ;

    std::vector<float> destination_data(source_data.size(), 0.0f);
    RtStageBuffer destination{};
    destination.size = sizeof(destination);
    destination.data = destination_data.data();
    destination.width = width;
    destination.height = height;
    destination.row_pitch = width * sizeof(float);
    destination.plane_pitch = destination.row_pitch * height;
    destination.planes = 3;
    destination.format = RT_STAGE_F32;

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

    if (failures == 0) {
        std::puts("stage-cache-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
