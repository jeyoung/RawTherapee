#include "stage_indexer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rtengine::stage
{

namespace
{

double blockScore(const float* base, int width, int height, size_t row_pitch,
                  int x0, int y0, int block)
{
    const int x1 = std::min(width, x0 + block);
    const int y1 = std::min(height, y0 + block);
    double sum = 0.0;
    double sum_sq = 0.0;
    double gradient = 0.0;
    uint64_t count = 0;

    for (int y = y0; y < y1; ++y) {
        const float* row = reinterpret_cast<const float*>(
            reinterpret_cast<const unsigned char*>(base) +
            static_cast<size_t>(y) * row_pitch);
        for (int x = x0; x < x1; ++x) {
            const double value = row[x];
            sum += value;
            sum_sq += value * value;
            ++count;
            if (x + 1 < x1) {
                gradient += std::abs(static_cast<double>(row[x + 1]) - value);
            }
            if (y + 1 < y1) {
                const float* next = reinterpret_cast<const float*>(
                    reinterpret_cast<const unsigned char*>(base) +
                    static_cast<size_t>(y + 1) * row_pitch);
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

} // namespace

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
