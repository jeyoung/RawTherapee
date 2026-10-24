#pragma once

#include "stage_abi.h"

#include <cstdint>

namespace rtengine::stage
{

struct ParityResult {
    int64_t max_abs = 0;
    uint64_t differing = 0;
    uint64_t total = 0;
    uint64_t hash_a = 0;
    uint64_t hash_b = 0;
    bool same_geometry = false;
};

ParityResult compare(const RtStageBuffer& a, const RtStageBuffer& b);
bool passes(const ParityResult& result, int64_t tolerance);

} // namespace rtengine::stage
