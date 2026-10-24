#pragma once

#include "stage_abi.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace rtengine::stage
{

struct StageRecord {
    const char* stage = "";
    const char* impl = "";
    const char* source = "";
    const char* consumer = "";
    uint64_t elapsed_ns = 0;
    uint64_t input_hash = 0;
    uint64_t output_hash = 0;
    int width = 0;
    int height = 0;
    int threads = 0;
    uint32_t cache_hit = 0;
};

void profile(const StageRecord& record);
void profile_duration(const char* name, uint64_t elapsed_ns);
bool profiling_enabled();
uint64_t hash_buffer(const RtStageBuffer& buffer);

class ScopedProfile
{
public:
    explicit ScopedProfile(const char* name)
        : name_(name), begin_(std::chrono::steady_clock::now())
    {
    }

    ~ScopedProfile()
    {
        const auto end = std::chrono::steady_clock::now();
        profile_duration(
            name_, static_cast<uint64_t>(
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           end - begin_)
                           .count()));
    }

    ScopedProfile(const ScopedProfile&) = delete;
    ScopedProfile& operator=(const ScopedProfile&) = delete;

private:
    const char* name_;
    std::chrono::steady_clock::time_point begin_;
};

} // namespace rtengine::stage
