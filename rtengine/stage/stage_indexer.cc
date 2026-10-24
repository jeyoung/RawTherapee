#include "stage_indexer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace rtengine::stage
{

namespace
{

const float* rowAt(const float* base, size_t row_pitch, int y)
{
    return reinterpret_cast<const float*>(
        reinterpret_cast<const unsigned char*>(base) +
        static_cast<size_t>(y) * row_pitch);
}

double regionScore(const float* base, size_t row_pitch, int x0, int y0,
                   int x1, int y1)
{
    double sum = 0.0;
    double sum_sq = 0.0;
    double gradient = 0.0;
    uint64_t count = 0;

    for (int y = y0; y < y1; ++y) {
        const float* row = rowAt(base, row_pitch, y);
        for (int x = x0; x < x1; ++x) {
            const double value = row[x];
            sum += value;
            sum_sq += value * value;
            ++count;
            if (x + 1 < x1) {
                gradient += std::abs(static_cast<double>(row[x + 1]) - value);
            }
            if (y + 1 < y1) {
                const float* next = rowAt(base, row_pitch, y + 1);
                gradient += std::abs(static_cast<double>(next[x]) - value);
            }
        }
    }

    if (count == 0) {
        return 0.0;
    }
    const double mean = sum / count;
    const double variance = std::max(0.0, sum_sq / count - mean * mean);
    return variance + gradient / static_cast<double>(count);
}

double blockScore(const float* base, int width, int height, size_t row_pitch,
                  int x0, int y0, int block)
{
    return regionScore(base, row_pitch, x0, y0, std::min(width, x0 + block),
                       std::min(height, y0 + block));
}

bool tileScoreOrder(const TileOrigin& a, const TileOrigin& b)
{
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.x < b.x;
}

} // namespace

int tileLuminanceWidth(int image_width, int sample_step)
{
    if (image_width <= 0 || sample_step <= 0) {
        return 0;
    }
    return (image_width + sample_step - 1) / sample_step;
}

int tileLuminanceHeight(int image_height, int sample_step)
{
    if (image_height <= 0 || sample_step <= 0) {
        return 0;
    }
    return (image_height + sample_step - 1) / sample_step;
}

std::vector<TileOrigin> orderTilesByScore(const float* luminance,
                                          int image_width, int image_height,
                                          int step_x, int step_y,
                                          int sample_step)
{
    std::vector<TileOrigin> order;
    const int plane_width = tileLuminanceWidth(image_width, sample_step);
    const int plane_height = tileLuminanceHeight(image_height, sample_step);

    if (luminance == nullptr || plane_width <= 0 || plane_height <= 0 ||
        step_x <= 0 || step_y <= 0) {
        return order;
    }

    const size_t row_pitch = static_cast<size_t>(plane_width) * sizeof(float);

    for (int y = 0; y < image_height; y += step_y) {
        for (int x = 0; x < image_width; x += step_x) {
            const int px0 = x / sample_step;
            const int py0 = y / sample_step;
            const int px1 = std::min(plane_width, (x + step_x) / sample_step);
            const int py1 = std::min(plane_height, (y + step_y) / sample_step);
            TileOrigin origin;
            origin.x = x;
            origin.y = y;
            origin.score = regionScore(luminance, row_pitch, px0, py0,
                                       std::max(px0 + 1, px1),
                                       std::max(py0 + 1, py1));
            if (!std::isfinite(origin.score)) {
                origin.score = 0.0;
            }
            order.push_back(origin);
        }
    }

    std::sort(order.begin(), order.end(), tileScoreOrder);
    return order;
}

bool tileOrderFlagEnabled(const char* value)
{
    // Only the exact value "1" enables ordering; every other value, including
    // unset, empty, "0" and "off", keeps the historical tile order.
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool tileOrderingEnabled()
{
    return tileOrderFlagEnabled(std::getenv("RT_TILE_ORDER"));
}

TilePlan buildTilePlan(const float* luminance, int width, int height,
                       size_t row_pitch, int block_size, int max_full,
                       double score_threshold)
{
    TilePlan plan;
    if (luminance == nullptr || width <= 0 || height <= 0 || block_size <= 0) {
        return plan;
    }

    std::vector<TileRect> tiles;
    for (int y = 0; y < height; y += block_size) {
        for (int x = 0; x < width; x += block_size) {
            TileRect tile;
            tile.x = x;
            tile.y = y;
            tile.width = std::min(block_size, width - x);
            tile.height = std::min(block_size, height - y);
            tile.score = blockScore(luminance, width, height, row_pitch, x, y,
                                    block_size);
            tiles.push_back(tile);
        }
    }

    std::sort(tiles.begin(), tiles.end(),
              [](const TileRect& a, const TileRect& b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  if (a.y != b.y) {
                      return a.y < b.y;
                  }
                  return a.x < b.x;
              });

    for (size_t i = 0; i < tiles.size(); ++i) {
        if (static_cast<int>(i) < max_full && tiles[i].score >= score_threshold) {
            plan.full.push_back(tiles[i]);
        } else {
            plan.cheap.push_back(tiles[i]);
        }
    }
    return plan;
}

} // namespace rtengine::stage
