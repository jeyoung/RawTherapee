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
 * Specification test for the unified stage plan.
 *
 * The test pins the contract the crop and preview adapters rely on:
 *
 * - a plan exists for every planning path and its execute list contains only
 *   non-folded dirty nodes, in a canonical order;
 * - folded nodes and clean nodes never execute, while the dirty state of a
 *   folded node stays represented in the plan;
 * - the buffer requirements cover every spec node with the contract's
 *   ownership, working set and inputs;
 * - the plan's dirty and work values are exactly the direct derivations, so
 *   the composition adds no hidden rule;
 * - an inconsistent or unsafe plan is refused instead of executed.
 *
 * It does not run image processing. The crop and preview adapters are
 * exercised end to end by their own gates.
 */

#include "stage_plan.h"

#include "stage_contract.h"
#include "stage_dirty_blocks.h"
#include "stage_graph.h"
#include "stage_pipeline.h"
#include "stage_work.h"

#include "rtengine/procparams.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace rtengine;
using namespace rtengine::stage;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::vector<std::string> pipelineIds(StagePath path)
{
    std::vector<std::string> ids;
    for (const auto& node : buildPipelineSpec(path).nodes) {
        ids.push_back(node.id);
    }
    return ids;
}

std::vector<std::string> foldedIds(StagePath path)
{
    std::vector<std::string> ids;
    for (const std::string& id : pipelineIds(path)) {
        if (foldHost(path, id) != nullptr) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool contains(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool isSubsequenceOfPath(StagePath path, const std::vector<std::string>& ids)
{
    const std::vector<std::string> canonical = pipelineIds(path);
    size_t position = 0;
    for (const std::string& id : ids) {
        const auto found = std::find(canonical.begin() + position,
                                     canonical.end(), id);
        if (found == canonical.end()) {
            return false;
        }
        position = static_cast<size_t>(found - canonical.begin()) + 1;
    }
    return true;
}

std::vector<std::string> nonFoldedDirty(const std::set<std::string>& dirty,
                                        StagePath path)
{
    std::vector<std::string> ids;
    for (const std::string& id : pipelineIds(path)) {
        if (dirty.count(id) != 0 && foldHost(path, id) == nullptr) {
            ids.push_back(id);
        }
    }
    return ids;
}

struct PlanBundle {
    DirtyBlockInputs dirty;
    WorkInputs work;
    bool deriveWork = false;
};

PlanBundle makeBundle(StagePath path, const procparams::ProcParams& params,
                      const BlockFingerprints* previous,
                      const BlockFingerprints& current,
                      const RenderStateKey* previousState,
                      const RenderStateKey& state)
{
    PlanBundle bundle;

    bundle.dirty.path = path;
    bundle.dirty.previousFingerprints = previous;
    bundle.dirty.currentFingerprints = &current;
    bundle.dirty.previousState = previousState;
    bundle.dirty.currentState = &state;

    bundle.deriveWork = path == StagePath::Preview || path == StagePath::Crop;

    if (bundle.deriveWork) {
        bundle.work.path = path;
        bundle.work.previousState = previousState;
        bundle.work.currentState = &state;
        bundle.work.previousFingerprints = previous;
        bundle.work.currentFingerprints = &current;
        bundle.work.previousParams = previous ? &params : nullptr;
        bundle.work.requestKind = state.requestKind;
        bundle.work.params = &params;
    }

    return bundle;
}

StagePlanInputs makeInputs(const PlanBundle& bundle)
{
    StagePlanInputs inputs;
    inputs.dirty = bundle.dirty;
    inputs.work = bundle.work;
    inputs.deriveWork = bundle.deriveWork;
    return inputs;
}

void checkBuffers(const StagePlan& plan)
{
    std::map<std::string, std::vector<std::string>> incoming;
    for (const auto& edge : plan.spec.edges) {
        incoming[edge.second].push_back(edge.first);
    }

    check(plan.buffers.size() == plan.spec.nodes.size(),
          "buffers: one requirement per spec node");

    if (plan.buffers.size() != plan.spec.nodes.size()) {
        return;
    }

    for (size_t i = 0; i < plan.spec.nodes.size(); ++i) {
        const StageDescriptorSpec& node = plan.spec.nodes[i];
        const StageBufferRequirement& requirement = plan.buffers[i];

        check(requirement.id == node.id,
              "buffers: requirement follows the spec order: " + node.id);

        const StageBlockContract* block = findStageBlockContract(node.id);
        check(block != nullptr, "buffers: block is canonical: " + node.id);
        if (block == nullptr) {
            continue;
        }

        check(requirement.ownership == block->ownership,
              "buffers: ownership matches the contract: " + node.id);
        check(requirement.workingSet == block->workingSet,
              "buffers: working set matches the contract: " + node.id);
        check(requirement.inputs == incoming[node.id],
              "buffers: inputs match the spec edges: " + node.id);

        if (block->ownership == StageOwnership::Consumes) {
            check(requirement.output.empty(),
                  "buffers: a consumer owns no output: " + node.id);
        } else {
            check(requirement.output == node.id,
                  "buffers: a producer owns its own buffer: " + node.id);
        }
    }
}

void testPlanForPath(StagePath path)
{
    const procparams::ProcParams params;
    const BlockFingerprints current = stage_fingerprints(path, params);
    const RenderStateKey state;

    const PlanBundle bundle =
        makeBundle(path, params, nullptr, current, nullptr, state);
    const StagePlanInputs inputs = makeInputs(bundle);
    const StagePlan plan = buildStagePlan(inputs);

    check(plan.valid, std::string(stagePathName(path)) + ": plan is valid");
    check(plan.spec.path == path,
          std::string(stagePathName(path)) + ": spec path matches");

    const DirtyBlocks directDirty = derive_dirty_blocks(inputs.dirty);
    check(plan.dirty.blocks == directDirty.blocks,
          std::string(stagePathName(path))
              + ": dirty blocks are the direct derivation");
    check(plan.dirty.ids == directDirty.ids,
          std::string(stagePathName(path))
              + ": dirty ids are the direct derivation");
    check(plan.dirty.forced == directDirty.forced,
          std::string(stagePathName(path))
              + ": dirty forcing is the direct derivation");

    if (bundle.deriveWork) {
        const WorkOrder directWork = path == StagePath::Preview
                                         ? derive_preview_work(inputs.work)
                                         : derive_crop_work(inputs.work);
        check(plan.work == directWork,
              std::string(stagePathName(path))
                  + ": work order is the direct derivation");
    } else {
        check(plan.work.empty(),
              std::string(stagePathName(path))
                  + ": a path without work order carries an empty order");
    }

    check(plan.folded == foldedIds(path),
          std::string(stagePathName(path)) + ": folded nodes are listed");

    const std::vector<std::string> expected =
        nonFoldedDirty(plan.dirty.ids, path);
    check(plan.execute == expected,
          std::string(stagePathName(path))
              + ": execute is the non-folded dirty set in plan order");

    check(isSubsequenceOfPath(path, plan.execute),
          std::string(stagePathName(path))
              + ": execute follows the canonical path order");

    for (const std::string& id : plan.execute) {
        check(contains(pipelineIds(path), id),
              std::string(stagePathName(path))
                  + ": execute id is on the path: " + id);
        check(foldHost(path, id) == nullptr,
              std::string(stagePathName(path))
                  + ": execute id is not folded: " + id);
        check(plan.dirty.ids.count(id) != 0,
              std::string(stagePathName(path))
                  + ": execute id is dirty: " + id);
        check(!contains(plan.folded, id),
              std::string(stagePathName(path))
                  + ": execute excludes folded nodes: " + id);
    }

    checkBuffers(plan);
}

void testCleanNodesNeverExecute()
{
    const procparams::ProcParams params;

    for (StagePath path : {StagePath::Preview, StagePath::Crop}) {
        const BlockFingerprints current = stage_fingerprints(path, params);
        BlockFingerprints previous = current;
        const RenderStateKey state;

        PlanBundle bundle =
            makeBundle(path, params, &previous, current, &state, state);
        StagePlan plan = buildStagePlan(makeInputs(bundle));

        check(plan.valid, std::string(stagePathName(path))
                              + ": no-change history plans");
        check(plan.dirty.fromParams.empty(),
              std::string(stagePathName(path))
                  + ": no parameter delta on no-change history");
        check(plan.dirty.fromState.empty(),
              std::string(stagePathName(path))
                  + ": no state delta on no-change history");

        /*
         * A clean non-resident node stays out of the plan. source.select is the
         * root of both interactive paths and is not a resident, so it is the
         * sharpest probe for "clean means not executed".
         */
        check(!contains(plan.execute, "source.select"),
              std::string(stagePathName(path))
                  + ": a clean node is not executed");

        for (const std::string& id : plan.execute) {
            check(plan.dirty.ids.count(id) != 0,
                  std::string(stagePathName(path))
                      + ": no clean node in execute: " + id);
        }

        /*
         * One changed fingerprint seeds its block. rgb.proc is not a resident
         * and is not in a coupled group on either path, so the resulting
         * execute set contains it directly rather than through an expansion.
         */
        const auto target = std::find(current.ids.begin(), current.ids.end(),
                                      "rgb.proc");
        check(target != current.ids.end(),
              std::string(stagePathName(path))
                  + ": rgb.proc is a canonical block");
        if (target == current.ids.end()) {
            continue;
        }
        const size_t index = static_cast<size_t>(
            target - current.ids.begin());
        previous.values[index].words[0] ^= 1;

        plan = buildStagePlan(makeInputs(bundle));
        check(plan.valid, std::string(stagePathName(path))
                              + ": one-delta history plans");
        check(contains(plan.execute, "rgb.proc"),
              std::string(stagePathName(path))
                  + ": the changed block is executed");
        check(!contains(plan.execute, "source.select"),
              std::string(stagePathName(path))
                  + ": an unrelated clean block is not executed");
        for (const std::string& id : plan.folded) {
            check(!contains(plan.execute, id),
                  std::string(stagePathName(path))
                      + ": a folded dirty node is not executed: " + id);
        }
    }
}

/*
 * Rebuild the merged order the way the preview adapter does: run every node
 * whose graph decision is not ReuseOutput, in topological order, and skip a
 * folded node because its host carries the body.
 */
std::vector<std::string> mergedExecutionOrder(const StagePlan& plan)
{
    StageGraph graph;
    std::vector<size_t> nodes;
    for (const StageDescriptorSpec& node : plan.spec.nodes) {
        nodes.push_back(graph.addNode(node.id, node.reuse));
    }

    const auto indexOf = [&plan](const std::string& id) {
        for (size_t i = 0; i < plan.spec.nodes.size(); ++i) {
            if (plan.spec.nodes[i].id == id) {
                return i;
            }
        }
        return plan.spec.nodes.size();
    };

    for (const auto& edge : plan.spec.edges) {
        graph.connect(nodes[indexOf(edge.first)],
                      nodes[indexOf(edge.second)]);
    }

    for (size_t i = 0; i < plan.spec.nodes.size(); ++i) {
        if (plan.dirty.ids.count(plan.spec.nodes[i].id) != 0) {
            graph.markDirty(nodes[i]);
        } else {
            graph.markClean(nodes[i]);
        }
    }

    std::vector<size_t> order;
    graph.topologicalOrder(order);
    const StagePlanResult decisions = graph.plan();

    std::vector<std::string> merged;
    for (size_t index : order) {
        const std::string& id = plan.spec.nodes[index].id;
        if (foldHost(plan.spec.path, id) != nullptr) {
            continue;
        }
        if (decisions.decisions[index].action != StageAction::ReuseOutput) {
            merged.push_back(id);
        }
    }
    return merged;
}

void testMergedOrderAgreement()
{
    const procparams::ProcParams params;
    const BlockFingerprints current =
        stage_fingerprints(StagePath::Preview, params);
    BlockFingerprints previous = current;
    const RenderStateKey state;

    const PlanBundle bundle = makeBundle(
        StagePath::Preview, params, &previous, current, &state, state);
    const StagePlan plan = buildStagePlan(makeInputs(bundle));

    check(plan.valid, "merged order: no-change preview plans");
    if (!plan.valid) {
        return;
    }

    /*
     * A guide-only repaint has no parameter or state delta. The preview's
     * only resident is monitor.output, so it is the only dirty node; every
     * in-place `ReuseInput` node is clean.
     */
    check(plan.dirty.ids.size() == 1
              && plan.dirty.ids.count("monitor.output") != 0,
          "merged order: a repaint pass dirties only the resident sink");
    check(!contains(plan.execute, "transform"),
          "merged order: the canonical plan omits a clean ReuseInput node");

    const std::vector<std::string> merged = mergedExecutionOrder(plan);

    check(contains(merged, "transform"),
          "merged order: a clean ReuseInput node still runs");
    check(plan.execute != merged,
          "merged order: the dirty-only list differs from the merged list");
    check(executeOrderMatches(plan, merged),
          "merged order: the dirty nodes agree in order");
    check(!executeOrderMatches(plan, {}),
          "merged order: an empty merged order is a disagreement");

    StagePlan truncated = plan;
    truncated.execute.clear();
    check(!executeOrderMatches(truncated, merged),
          "merged order: a missing dirty node is a disagreement");

    /*
     * Two dirty nodes in the wrong relative order are a real disagreement,
     * even when the merged list carries the same nodes.
     */
    const PlanBundle fullBundle = makeBundle(
        StagePath::Preview, params, nullptr, current, nullptr, state);
    const StagePlan full = buildStagePlan(makeInputs(fullBundle));
    check(full.valid, "merged order: a first pass plans");
    if (full.valid && full.execute.size() >= 2) {
        std::vector<std::string> swapped = full.execute;
        std::swap(swapped[0], swapped[1]);
        check(!executeOrderMatches(full, swapped),
              "merged order: a swapped dirty order is a disagreement");
    }
}

void testRefusals()
{
    const procparams::ProcParams params;
    const BlockFingerprints current =
        stage_fingerprints(StagePath::Crop, params);
    const BlockFingerprints previewCurrent =
        stage_fingerprints(StagePath::Preview, params);
    const RenderStateKey state;

    {
        const PlanBundle bundle = makeBundle(
            StagePath::Crop, params, nullptr, current, nullptr, state);
        StagePlanInputs inputs = makeInputs(bundle);
        inputs.deriveWork = true;
        inputs.work.path = StagePath::Preview;
        const StagePlan plan = buildStagePlan(inputs);
        check(!plan.valid,
              "refusal: dirty and work paths must agree");
        check(!plan.reason.empty(), "refusal: a reason is exposed");
        check(plan.execute.empty(),
              "refusal: a refused plan has no execution list");
    }

    {
        const PlanBundle bundle = makeBundle(
            StagePath::Export, params, nullptr, current, nullptr, state);
        StagePlanInputs inputs = makeInputs(bundle);
        inputs.deriveWork = true;
        const StagePlan plan = buildStagePlan(inputs);
        check(!plan.valid,
              "refusal: a path without work order rejects deriveWork");
        check(!plan.reason.empty(), "refusal: a reason is exposed");
    }

    {
        const PlanBundle bundle = makeBundle(
            StagePath::Crop, params, nullptr, current, nullptr, state);
        StagePlanInputs inputs = makeInputs(bundle);
        inputs.deriveWork = false;
        const StagePlan plan = buildStagePlan(inputs);
        check(!plan.valid,
              "refusal: a path with work order requires deriveWork");
        check(!plan.reason.empty(), "refusal: a reason is exposed");
    }

    {
        const PlanBundle bundle = makeBundle(
            StagePath::Crop, params, nullptr, current, nullptr, state);
        StagePlanInputs inputs = makeInputs(bundle);
        inputs.dirty.currentFingerprints = nullptr;
        const StagePlan plan = buildStagePlan(inputs);
        check(!plan.valid,
              "refusal: a missing current fingerprint table is refused");
        check(!plan.reason.empty(), "refusal: a reason is exposed");
    }

    {
        const PlanBundle bundle = makeBundle(
            StagePath::Preview, params, nullptr, previewCurrent, nullptr,
            state);
        StagePlanInputs inputs = makeInputs(bundle);
        inputs.work.requestKind = RequestKind::RepaintOnly;
        const StagePlan plan = buildStagePlan(inputs);
        check(!plan.valid,
              "refusal: the work request kind must match the state");
        check(!plan.reason.empty(), "refusal: a reason is exposed");
    }
}

} // namespace

int main()
{
    for (StagePath path : {StagePath::Export, StagePath::FastExport,
                           StagePath::Preview, StagePath::Crop,
                           StagePath::Thumbnail}) {
        testPlanForPath(path);
    }

    testCleanNodesNeverExecute();
    testMergedOrderAgreement();
    testRefusals();

    if (failures == 0) {
        std::puts("stage-plan-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
