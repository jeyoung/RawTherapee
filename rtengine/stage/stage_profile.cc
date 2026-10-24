#include "stage_profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace rtengine::stage
{

namespace
{

std::mutex profile_mutex;

const char* profile_target()
{
    return std::getenv("RT_STAGE_PROFILE");
}

uint64_t fnv1a(const unsigned char* data, size_t bytes, uint64_t hash)
{
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

bool profiling_enabled()
{
    const char* target = profile_target();
    return target != nullptr && target[0] != '\0' && std::strcmp(target, "0") != 0;
}

uint64_t hash_buffer(const RtStageBuffer& buffer)
{
    uint64_t hash = 1469598103934665603ULL;
    const size_t pixel_bytes = buffer.format == RT_STAGE_F32
                                   ? sizeof(float)
                                   : (buffer.format == RT_STAGE_U16 ? sizeof(uint16_t)
                                                                    : sizeof(uint8_t));
    const size_t row = static_cast<size_t>(buffer.width) * pixel_bytes *
                       (buffer.format == RT_STAGE_U8 ? 3u : 1u);
    const unsigned char* base = static_cast<const unsigned char*>(buffer.data);
    for (uint32_t plane = 0; plane < buffer.planes; ++plane) {
        const unsigned char* plane_base = base + static_cast<size_t>(plane) * buffer.plane_pitch;
        for (int y = 0; y < buffer.height; ++y) {
            hash = fnv1a(plane_base + static_cast<size_t>(y) * buffer.row_pitch, row, hash);
        }
    }
    return hash;
}

void profile_duration(const char* name, uint64_t elapsed_ns)
{
    if (!profiling_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(profile_mutex);
    std::fprintf(stderr, "stage %s: %.3f ms\n", name,
                 static_cast<double>(elapsed_ns) / 1.0e6);
}

void profile(const StageRecord& record)
{
    const char* target = profile_target();
    if (target == nullptr || target[0] == '\0' || std::strcmp(target, "0") == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(profile_mutex);
    FILE* file = std::strcmp(target, "1") == 0 ? stderr : std::fopen(target, "a");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "{\"stage\":\"%s\",\"impl\":\"%s\",\"source\":\"%s\","
                 "\"consumer\":\"%s\",\"ns\":%llu,\"width\":%d,\"height\":%d,"
                 "\"threads\":%d,\"cache_hit\":%u,\"input_hash\":\"%016llx\","
                 "\"output_hash\":\"%016llx\"}\n",
                 record.stage, record.impl, record.source, record.consumer,
                 static_cast<unsigned long long>(record.elapsed_ns), record.width,
                 record.height, record.threads, record.cache_hit,
                 static_cast<unsigned long long>(record.input_hash),
                 static_cast<unsigned long long>(record.output_hash));
    if (file != stderr) {
        std::fclose(file);
    }
}

} // namespace rtengine::stage
