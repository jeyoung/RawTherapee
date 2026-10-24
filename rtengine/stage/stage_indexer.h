#pragma once

#include <cstddef>
#include <vector>

namespace rtengine::stage
{

struct TileRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double score = 0.0;
};

struct TilePlan {
    std::vector<TileRect> full;
    std::vector<TileRect> cheap;
};

/*
 * Cheap block scorer used to bound expensive per-tile work. It is a pure
 * function of a luminance plane and the requested block size, so it can be
 * unit tested without an engine context.
 */
TilePlan buildTilePlan(const float* luminance, int width, int height,
                       size_t row_pitch, int block_size, int max_full,
                       double score_threshold);

} // namespace rtengine::stage
