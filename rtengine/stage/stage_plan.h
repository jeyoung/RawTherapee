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

#pragma once

#include "stage_dirty_blocks.h"
#include "stage_pipeline.h"
#include "stage_work.h"

#include <string>
#include <vector>

namespace rtengine::stage
{

/*
 * Unified stage plan.
 *
 * The plan is the single answer a path adapter needs for one pass: the
 * canonical specification, the blocks whose inputs changed, the destructive
 * work order, the buffer/working-set requirements of every block and the
 * ordered list of blocks to execute.
 *
 * The three planning authorities stay separate units with separate
 * responsibilities. This unit only composes them:
 *
 * - buildPipelineSpec() owns the path inventory, the edges and the folds;
 * - derive_dirty_blocks() owns the parameter and render-state delta;
 * - derive_preview_work()/derive_crop_work() own the destructive work order.
 *
 * Nothing here executes a block, allocates a buffer or reads ProcParams
 * directly. The buffer requirements are a projection of the contract, not a
 * second allocator: the runtime allocation stays in the adapters until the
 * residency work moves it.
 */

/* Inputs of one plan. Pointer members are borrowed and are not stored. */
struct StagePlanInputs {
    DirtyBlockInputs dirty;
    WorkInputs work;
    /*
     * False for paths that carry no work order (export, fast export and
     * thumbnail). The value is validated against the path: a path with a work
     * order must derive one, and a path without one must not.
     */
    bool deriveWork = true;
};

/*
 * Buffer and working-memory requirement of one block.
 *
 * `id` is the canonical block id. `ownership` and `workingSet` are the
 * contract columns, `inputs` are the spec's incoming buffer ids and `output`
 * is the buffer the block produces. A `Consumes` block owns no output buffer,
 * so its `output` is empty; the spec still carries the block id as its node
 * name.
 */
struct StageBufferRequirement {
    std::string id;
    StageOwnership ownership = StageOwnership::InPlace;
    StageWorkingSet workingSet = StageWorkingSet::None;
    std::vector<std::string> inputs;
    std::string output;
};

/*
 * Result of buildStagePlan().
 *
 * `valid` is false only when the plan could not be built: the path and its
 * inputs disagree, a derivation precondition is missing, or a dirty node would
 * be reused instead of executed. `reason` names the refusal and `execute` is
 * then empty, so an adapter must reject the plan rather than index it. A dirty
 * node that plans `ReuseOutput` is exactly such a refusal: the graph would
 * publish a stale buffer, which is a planning error, not a runtime fallback.
 *
 * `execute` holds the non-folded dirty nodes in graph (topological) order.
 * A folded dirty node stays in `dirty` and `folded`; it does not add its host
 * to `execute`, because the host is scheduled by its own dirty state. The
 * adapters that rely on the fold (the preview's analysis inside raw.base, the
 * crop's raw prefix inside source.select) keep their existing host handling
 * until the execution move is complete. `folded` holds every folded node of
 * the path so a diagnostic can name the bodies carried by another block.
 * `buffers` covers every node of `spec`, in spec order.
 */
struct StagePlan {
    StagePipelineSpec spec;
    DirtyBlocks dirty;
    WorkOrder work;
    std::vector<StageBufferRequirement> buffers;
    std::vector<std::string> execute;
    std::vector<std::string> folded;
    bool valid = false;
    std::string reason;
};

/*
 * Build one plan. See the unit comment for the composition and the failure
 * contract. The function is deterministic and side-effect free.
 */
StagePlan buildStagePlan(const StagePlanInputs& inputs);

} // namespace rtengine::stage
