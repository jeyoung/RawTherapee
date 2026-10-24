#include "stage_graph.h"

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

    StageGraph graph;
    const size_t init = graph.addNode("init", RT_STAGE_RECOMPUTE);
    const size_t curve = graph.addNode("curve", RT_STAGE_REUSE_INPUT);
    const size_t output = graph.addNode("output", RT_STAGE_REUSE_OUTPUT);
    graph.connect(init, curve);
    graph.connect(curve, output);

    std::vector<size_t> order;
    check(graph.topologicalOrder(order), "graph is acyclic");
    check(order == std::vector<size_t>({init, curve, output}), "topological order");

    auto decisions = graph.plan();
    check(decisions[init].action == StageAction::Recompute, "dirty init recomputes");
    check(decisions[curve].action == StageAction::ReuseInput, "curve reuses input");
    check(decisions[output].action == StageAction::Recompute, "downstream recomputes");

    graph.markClean(init);
    graph.markClean(curve);
    graph.markClean(output);
    decisions = graph.plan();
    check(decisions[init].action == StageAction::ReuseOutput,
          "clean recompute node reuses its output");
    check(decisions[curve].action == StageAction::ReuseInput, "curve reuses input");
    check(decisions[output].action == StageAction::Recompute,
          "output follows changed curve");

    if (failures == 0) {
        std::puts("stage-graph-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
