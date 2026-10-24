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

/*
 * Image pixels per sample in the subsampled luminance plane used to rank
 * tiles. A step of 8 keeps the scoring pass below a few milliseconds at 45 MP
 * while still ranking textured tiles above flat ones. Valid range: 1 or more;
 * 8 is the default used by the denoise and wavelet tile loops.
 */
inline constexpr int TILE_LUMINANCE_STEP = 8;

struct TileOrigin {
    int x = 0;
    int y = 0;
    double score = 0.0;
};

/* Column and row count of the plane read by orderTilesByScore. */
int tileLuminanceWidth(int image_width, int sample_step = TILE_LUMINANCE_STEP);
int tileLuminanceHeight(int image_height, int sample_step = TILE_LUMINANCE_STEP);

/*
 * Rank every tile of the regular origin grid (step_x, step_y) over an image of
 * image_width x image_height. Each origin is returned exactly once, in
 * descending cheap-score order with ties broken by row and then column, so the
 * result is a deterministic permutation of the grid.
 *
 * The luminance plane is contiguous with tileLuminanceWidth() columns and
 * tiles are scored over the subsampled region of their own step area, so the
 * helper changes only the visitation order: tile geometry, tile count and the
 * arithmetic of the caller are untouched. A null plane, a non-positive
 * dimension or an invalid step yields an empty result.
 */
std::vector<TileOrigin> orderTilesByScore(
    const float* luminance, int image_width, int image_height,
    int step_x, int step_y, int sample_step = TILE_LUMINANCE_STEP);

/*
 * RT_TILE_ORDER semantics: exactly "1" enables tile ordering. Unset, empty,
 * "0" and "off" (or any other value) keep the historical tile order.
 */
bool tileOrderFlagEnabled(const char* value);

/* Read RT_TILE_ORDER from the environment. */
bool tileOrderingEnabled();

} // namespace rtengine::stage
