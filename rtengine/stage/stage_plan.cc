/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Unified stage plan.
 *
 * The plan composes the three planning authorities without owning any of
 * their rules: the path specification, the dirty-block derivation and the
 * work order. The only new decision here is execution resolution: a dirty
 * folded node keeps its consumers dirty, a clean node is not executed, and a
 * dirty node that the graph would reuse is a refusal rather than a silently
 * skipped body.
 */

#include "stage_plan.h"

#include "stage_contract.h"
#include "stage_graph.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

namespace
{

bool isKnownPath(StagePath path)
{
    switch (path) {
    case StagePath::Export:
    case StagePath::FastExport:
    case StagePath::Preview:
    case StagePath::Crop:
    case StagePath::Thumbnail:
        return true;
    }
    return false;
}

/* Only the interactive paths carry a typed work order in this revision. */
bool carriesWorkOrder(StagePath path)
{
    return path == StagePath::Preview || path == StagePath::Crop;
}

StagePlan refuse(StagePlan plan, std::string reason)
{
    plan.valid = false;
    plan.reason = std::move(reason);
    plan.execute.clear();
    return plan;
}

size_t nodeIndex(const StagePipelineSpec& spec, const std::string& id)
{
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        if (spec.nodes[i].id == id) {
            return i;
        }
    }
    return spec.nodes.size();
}

} // namespace

StagePlan buildStagePlan(const StagePlanInputs& inputs)
{
    StagePlan plan;

    const StagePath path = inputs.dirty.path;

    if (!isKnownPath(path)) {
        return refuse(std::move(plan), "unknown stage path");
    }

    /*
     * The work-order flag is part of the path contract: export, fast export
     * and thumbnail have no destructive work order, while preview and crop do.
     * A mismatch is a caller error, not a reason to derive an empty order.
     */
    if (inputs.deriveWork != carriesWorkOrder(path)) {
        return refuse(std::move(plan), inputs.deriveWork
                         ? "path carries no work order"
                         : "path requires a work order");
    }

    if (inputs.deriveWork && inputs.work.path != path) {
        return refuse(std::move(plan), "dirty and work paths disagree");
    }

    plan.spec = buildPipelineSpec(path);

    if (plan.spec.path != path) {
        return refuse(std::move(plan), "spec path disagrees with inputs");
    }
    if (plan.spec.nodes.empty()) {
        return refuse(std::move(plan), "empty pipeline specification");
    }

    /*
     * The derivation asserts on a missing current fingerprint table, so a
     * caller that reached this point without one gets a refusal instead of a
     * debug abort.
     */
    if (inputs.dirty.currentFingerprints == nullptr) {
        return refuse(std::move(plan),
                      "dirty derivation has no current fingerprints");
    }

    plan.dirty = derive_dirty_blocks(inputs.dirty);

    if (!plan.dirty.valid) {
        return refuse(std::move(plan),
                      "dirty derivation refused: " + plan.dirty.reason);
    }

    if (inputs.deriveWork) {
        if (inputs.work.currentState == nullptr
            || inputs.work.currentFingerprints == nullptr
            || inputs.work.params == nullptr) {
            return refuse(std::move(plan),
                          "work derivation has a missing input");
        }
        if (inputs.work.requestKind != inputs.work.currentState->requestKind) {
            return refuse(std::move(plan),
                          "work request kind disagrees with the render state");
        }

        plan.work = path == StagePath::Preview
                        ? derive_preview_work(inputs.work)
                        : derive_crop_work(inputs.work);
    }

    /*
     * Project the contract's buffer columns for every spec node. The
     * requirement is descriptive in this revision: the adapters still own the
     * allocation, and the residency work moves it in a later step.
     */
    plan.buffers.reserve(plan.spec.nodes.size());
    for (const StageDescriptorSpec& node : plan.spec.nodes) {
        const StageBlockContract* block = findStageBlockContract(node.id);
        if (block == nullptr) {
            return refuse(std::move(plan),
                          "spec names an unknown block: " + node.id);
        }

        StageBufferRequirement requirement;
        requirement.id = node.id;
        requirement.ownership = node.ownership;
        requirement.workingSet = block->workingSet;
        requirement.inputs = node.inputs;
        if (node.ownership != StageOwnership::Consumes) {
            requirement.output = node.id;
        }
        plan.buffers.push_back(std::move(requirement));

        if (foldHost(path, node.id) != nullptr) {
            plan.folded.push_back(node.id);
        }
    }

    /*
     * Resolve the execution order. The graph decides reuse for every node;
     * the plan only turns that decision into a list of bodies to run.
     */
    StageGraph graph;
    std::vector<size_t> nodes;
    nodes.reserve(plan.spec.nodes.size());
    for (const StageDescriptorSpec& node : plan.spec.nodes) {
        nodes.push_back(graph.addNode(node.id, node.reuse));
    }

    for (const auto& edge : plan.spec.edges) {
        const size_t from = nodeIndex(plan.spec, edge.first);
        const size_t to = nodeIndex(plan.spec, edge.second);
        if (from == plan.spec.nodes.size()
            || to == plan.spec.nodes.size()) {
            return refuse(std::move(plan),
                          "spec edge names an unknown block");
        }
        graph.connect(nodes[from], nodes[to]);
    }

    for (const std::string& id : plan.dirty.ids) {
        if (nodeIndex(plan.spec, id) == plan.spec.nodes.size()) {
            return refuse(std::move(plan),
                          "dirty set names a block outside the path: " + id);
        }
    }

    for (size_t i = 0; i < plan.spec.nodes.size(); ++i) {
        if (plan.dirty.ids.count(plan.spec.nodes[i].id) != 0) {
            graph.markDirty(nodes[i]);
        } else {
            graph.markClean(nodes[i]);
        }
    }

    std::vector<size_t> order;
    if (!graph.topologicalOrder(order)
        || order.size() != plan.spec.nodes.size()) {
        return refuse(std::move(plan), "pipeline graph is not acyclic");
    }

    const StagePlanResult decisions = graph.plan();
    if (!decisions.valid) {
        return refuse(std::move(plan),
                      "pipeline plan refused: " + decisions.reason);
    }
    if (decisions.decisions.size() != plan.spec.nodes.size()) {
        return refuse(std::move(plan), "pipeline plan is incomplete");
    }

    for (size_t index : order) {
        const std::string& id = plan.spec.nodes[index].id;

        if (plan.dirty.ids.count(id) == 0) {
            continue;
        }

        /* A folded node runs its host's body, not its own executor step. */
        if (foldHost(path, id) != nullptr) {
            continue;
        }

        plan.execute.push_back(id);
    }

    plan.valid = true;
    plan.reason.clear();
    return plan;
}

bool executeOrderMatches(const StagePlan& plan,
                         const std::vector<std::string>& merged)
{
    if (!plan.valid) {
        return false;
    }

    /*
     * The merged order carries clean `ReuseInput` nodes that the plan's
     * dirty-only `execute` omits. Walk the merged order and require every
     * dirty node to appear in the plan's order; the comparison is complete
     * only when the plan order is exhausted at the same time.
     */
    size_t next = 0;
    for (const std::string& id : merged) {
        if (plan.dirty.ids.count(id) == 0) {
            continue;
        }
        if (next == plan.execute.size() || plan.execute[next] != id) {
            return false;
        }
        ++next;
    }

    return next == plan.execute.size();
}

} // namespace rtengine::stage
