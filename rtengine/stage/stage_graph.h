#pragma once

#include "stage_abi.h"

#include <cstddef>
#include <string>
#include <vector>

namespace rtengine::stage
{

struct StageNode {
    std::string name;
    RtReuseMode reuse = RT_STAGE_RECOMPUTE;
    std::vector<size_t> inputs;
};

enum class StageAction {
    Recompute,
    ReuseInput,
    ReuseOutput
};

struct StageDecision {
    size_t node = 0;
    StageAction action = StageAction::Recompute;
};

/*
 * A small dependency graph with explicit reuse semantics. It mirrors the
 * stage runtime object model: a node is a kernel invocation, edges are buffer
 * dependencies, and the cache is consulted by the caller using the returned
 * decision.
 */
class StageGraph
{
public:
    size_t addNode(std::string name, RtReuseMode reuse = RT_STAGE_RECOMPUTE);
    void connect(size_t from, size_t to);

    size_t size() const;
    const StageNode& node(size_t index) const;

    /* Topological order; returns false on a cycle. */
    bool topologicalOrder(std::vector<size_t>& order) const;

    void markDirty(size_t index);
    void markClean(size_t index);
    bool dirty(size_t index) const;

    /*
     * Resolve actions for every node. A dirty node recomputes its output; a
     * clean node may reuse its input or its output according to its mode.
     */
    std::vector<StageDecision> plan() const;

private:
    std::vector<StageNode> nodes_;
    std::vector<std::vector<size_t>> edges_;
    std::vector<bool> dirty_;
};

} // namespace rtengine::stage
