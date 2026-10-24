#include "stage_indexer.h"

#include <cstdio>
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

    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<float> plane(width * height, 1.0f);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            plane[y * width + x] = static_cast<float>((x * 37 + y * 13) % 251);
        }
    }

    const TilePlan plan = buildTilePlan(plane.data(), width, height,
                                        width * sizeof(float), 16, 2, 0.0);
    check(plan.full.size() == 2, "top two blocks selected");
    check(!plan.full.empty() && plan.full[0].x == 0 && plan.full[0].y == 0,
          "highest-variance block is first");
    check(plan.full.size() + plan.cheap.size() == 16, "all blocks accounted for");

    const TilePlan empty = buildTilePlan(nullptr, width, height,
                                         width * sizeof(float), 16, 2, 0.0);
    check(empty.full.empty() && empty.cheap.empty(), "null input returns empty plan");

    // Tile ordering: every origin exactly once, descending score, deterministic.
    // The scorer reads a plane subsampled every sample_step image pixels.
    constexpr int sample_step = 4;
    const int sampled_width = tileLuminanceWidth(width, sample_step);
    const int sampled_height = tileLuminanceHeight(height, sample_step);
    std::vector<float> sampled(static_cast<size_t>(sampled_width) * sampled_height);
    for (int y = 0; y < sampled_height; ++y) {
        for (int x = 0; x < sampled_width; ++x) {
            sampled[y * sampled_width + x] = plane[y * sample_step * width + x * sample_step];
        }
    }
    const std::vector<TileOrigin> order =
        orderTilesByScore(sampled.data(), width, height, 16, 16, sample_step);
    check(order.size() == 16, "ordering covers every tile");
    bool descending = true;
    for (size_t i = 1; i < order.size(); ++i) {
        if (order[i - 1].score < order[i].score) {
            descending = false;
        }
    }
    check(descending, "ordering is descending by score");
    check(!order.empty() && order[0].x == 0 && order[0].y == 0,
          "highest-variance tile comes first");

    bool seen[4][4] = {};
    bool unique = true;
    for (const TileOrigin& origin : order) {
        const int gx = origin.x / 16;
        const int gy = origin.y / 16;
        if (gx < 0 || gx >= 4 || gy < 0 || gy >= 4 || seen[gy][gx]) {
            unique = false;
        } else {
            seen[gy][gx] = true;
        }
    }
    check(unique, "every grid origin appears exactly once");

    const std::vector<TileOrigin> repeat =
        orderTilesByScore(sampled.data(), width, height, 16, 16, sample_step);
    bool stable = repeat.size() == order.size();
    for (size_t i = 0; stable && i < order.size(); ++i) {
        stable = repeat[i].x == order[i].x && repeat[i].y == order[i].y &&
                 repeat[i].score == order[i].score;
    }
    check(stable, "ordering is deterministic for equal scores");

    check(orderTilesByScore(nullptr, width, height, 16, 16, 4).empty(),
          "null plane yields no order");
    check(orderTilesByScore(plane.data(), width, height, 0, 16, 4).empty(),
          "non-positive step yields no order");
    check(tileLuminanceWidth(64, 8) == 8 && tileLuminanceHeight(65, 8) == 9,
          "sample plane rounds up");

    check(tileOrderFlagEnabled("1"), "RT_TILE_ORDER=1 enables ordering");
    check(!tileOrderFlagEnabled(nullptr), "unset RT_TILE_ORDER keeps the old order");
    check(!tileOrderFlagEnabled(""), "empty RT_TILE_ORDER keeps the old order");
    check(!tileOrderFlagEnabled("0"), "RT_TILE_ORDER=0 keeps the old order");
    check(!tileOrderFlagEnabled("off"), "RT_TILE_ORDER=off keeps the old order");
    check(!tileOrderFlagEnabled("10"), "RT_TILE_ORDER=10 keeps the old order");

    if (failures == 0) {
        std::puts("stage-indexer-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
