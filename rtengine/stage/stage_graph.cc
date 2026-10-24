#include "stage_graph.h"

#include <algorithm>
#include <functional>
#include <queue>

namespace rtengine::stage
{

size_t StageGraph::addNode(std::string name, RtReuseMode reuse)
{
    nodes_.push_back(StageNode{std::move(name), reuse, {}});
    edges_.emplace_back();
    dirty_.push_back(true);
    return nodes_.size() - 1;
}

void StageGraph::connect(size_t from, size_t to)
{
    if (from >= nodes_.size() || to >= nodes_.size() || from == to) {
        return;
    }
    if (std::find(edges_[from].begin(), edges_[from].end(), to) == edges_[from].end()) {
        edges_[from].push_back(to);
        nodes_[to].inputs.push_back(from);
    }
}

size_t StageGraph::size() const
{
    return nodes_.size();
}

const StageNode& StageGraph::node(size_t index) const
{
    return nodes_[index];
}

bool StageGraph::topologicalOrder(std::vector<size_t>& order) const
{
    std::vector<size_t> indegree(nodes_.size(), 0);
    for (const auto& outgoing : edges_) {
        for (size_t to : outgoing) {
            ++indegree[to];
        }
    }

    std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> ready;
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) {
            ready.push(i);
        }
    }

    order.clear();
    while (!ready.empty()) {
        const size_t current = ready.top();
        ready.pop();
        order.push_back(current);
        for (size_t to : edges_[current]) {
            if (--indegree[to] == 0) {
                ready.push(to);
            }
        }
    }
    return order.size() == nodes_.size();
}

void StageGraph::markDirty(size_t index)
{
    if (index < dirty_.size()) {
        dirty_[index] = true;
    }
}

void StageGraph::markClean(size_t index)
{
    if (index < dirty_.size()) {
        dirty_[index] = false;
    }
}

bool StageGraph::dirty(size_t index) const
{
    return index < dirty_.size() && dirty_[index];
}

std::vector<StageDecision> StageGraph::plan() const
{
    std::vector<size_t> order;
    topologicalOrder(order);
    std::vector<StageDecision> decisions(nodes_.size());
    for (size_t index : order) {
        bool upstream_changed = false;
        for (size_t input : nodes_[index].inputs) {
            if (decisions[input].action != StageAction::ReuseOutput) {
                upstream_changed = true;
                break;
            }
        }

        StageAction action;
        if (dirty_[index] || upstream_changed) {
            action = nodes_[index].reuse == RT_STAGE_REUSE_INPUT
                         ? StageAction::ReuseInput
                         : StageAction::Recompute;
        } else if (nodes_[index].reuse == RT_STAGE_REUSE_INPUT) {
            action = StageAction::ReuseInput;
        } else {
            action = StageAction::ReuseOutput;
        }
        decisions[index] = StageDecision{index, action};
    }
    return decisions;
}

} // namespace rtengine::stage
