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

    if (failures == 0) {
        std::puts("stage-indexer-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
