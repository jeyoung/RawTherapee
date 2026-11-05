#pragma once

#include "stage_abi.h"

#include <cstdint>

namespace rtengine::stage
{

struct ParityResult {
    double max_abs = 0.0;
    uint64_t differing = 0;
    uint64_t total = 0;
    uint64_t hash_a = 0;
    uint64_t hash_b = 0;
    bool same_geometry = false;
};

ParityResult compare(const RtStageBuffer& a, const RtStageBuffer& b);
/* Bit equality; the strict form for decoder and raw-loading gates. */
bool passes_exact(const ParityResult& result);
/* Finite differences inside `tolerance` are accepted. */
bool passes_within(const ParityResult& result, double tolerance);
/* Compatibility name for the tolerance form. */
bool passes(const ParityResult& result, double tolerance);

} // namespace rtengine::stage
