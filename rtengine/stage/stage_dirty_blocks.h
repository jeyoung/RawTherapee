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

#include "stage_pipeline.h"
#include "stage_render_state.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace rtengine::procparams
{
class ProcParams;
}

namespace rtengine::stage
{

/*
 * Graph dirty-block derivation.
 *
 * The legacy `M_*` bitset names a parameter family, not the blocks that read
 * it, and its coarse gates over-approximate: `M_LUMINANCE` dirties both the
 * Lab tools and the sharpening block, and `M_CROP` re-runs four blocks that a
 * crop change does not touch. The preview and crop paths need a block set that
 * follows the actual inputs of a block so the graph can decide execution
 * without the bitset.
 *
 * This unit derives that set from two inputs, and nothing else:
 *
 * - the per-block parameter fingerprints of the previous and the current
 *   `ProcParams` (`stage_param_map.h`), compared block by block;
 * - the previous and current `RenderStateKey` (`stage_render_state.h`),
 *   compared by field group, because a render-state field such as the monitor
 *   profile dirties one display sink and must not dirty the raw core.
 *
 * It does not read the legacy mask, does not execute anything and does not
 * depend on a pipeline object, so both the crop path and the preview path use
 * the same derivation and the same diagnostics format. The mask stays in
 * charge of GUI notification and coalescing during this phase; a caller
 * compares the two sets with compare_dirty_shadow() before it trusts the
 * derived set.
 *
 * Returned ids are canonical descriptor ids of the requested path, in
 * pipeline order, without duplicates. A path that carries no parameter
 * fingerprints (thumbnail) and a path whose render state is not modelled
 * (export) degrade to the conservative full set rather than to an empty one.
 */

/*
 * Field groups of RenderStateKey, and therefore of the dirtiness they imply.
 *
 * The groups exist because a render-state change is not always a parameter
 * change: a new source reloads the raw core, a monitor profile change reruns
 * the display sink only, and a request kind says how much work to do rather
 * than which block to run. Each group maps to blocks per path in
 * blocks_for_render_state().
 */
enum class RenderStateAspect : int32_t {
    /* Preview scale and dimensions, crop or ROI rectangle, skip and borders. */
    Geometry = 0,
    /*
     * Source identity: generation, a modified RGB source, rotation, sensor
     * type, unrotated full size and frame count. The source facts move with
     * the generation, so they cannot widen a dirty set beyond a reload.
     */
    Source = 1,
    /* The high-detail decision and the work already done for it. */
    HighDetail = 2,
    /*
     * Derived state. The source exposure compensation the denoise analysis
     * consumes and the validity of that analysis dirty the analysis and the
     * base that consumes it without naming a new source. The live spot
     * buffer is base state, not an analysis input: it changes the next base
     * pass whether or not the analysis moves.
     */
    Derived = 3,
    /* Monitor profile, intent, soft proof and gamut check. */
    Display = 4,
    /* Editor-only switches: sharpening mask and the fast-preview family. */
    Editor = 5,
    /* Request kind and the base the previous pass used. */
    Request = 6,
    Count = 7
};

/* Stable lower-case name for diagnostics. */
const char* renderStateAspectName(RenderStateAspect aspect);

/*
 * Aspects whose fields differ between two keys, in enumeration order.
 *
 * An empty result means the keys are equal. The comparison is field by field,
 * so it does not depend on the whole-key fingerprint and stays meaningful
 * when only one group changes.
 */
std::vector<RenderStateAspect> changed_render_state_aspects(
    const RenderStateKey& previous, const RenderStateKey& current);

/*
 * Whether a request kind asks for a whole-path rebuild instead of a repaint.
 *
 * FullCrop, FullPreview and HighQuality are absolute requests: they say "this
 * pass rebuilds the path", not "these inputs changed". A caller that carries
 * such a request has to treat the whole path as dirty, because the request may
 * stand for work whose inputs are not represented in ProcParams or in the
 * render state (the crop reloads its destructive raw stage for M_INIT, for
 * example). ParamsOnly and RepaintOnly are deltas and rebuild nothing.
 */
bool request_rebuilds_path(RequestKind kind);

/*
 * Blocks one changed aspect dirties on `path`, in canonical pipeline order.
 *
 * The table is an over-approximation in the same sense as the parameter map: a
 * block named here may still decide inside its own body that this request has
 * nothing to do. It is never an under-approximation. `StagePath::Export` and
 * `StagePath::Thumbnail` do not use RenderStateKey, so every aspect returns the
 * full path as the conservative answer.
 */
std::vector<std::string> blocks_for_render_state(StagePath path,
                                                RenderStateAspect aspect);

/*
 * Blocks the owning caller executes on every invocation, whatever the mask or
 * the parameters say.
 *
 * These are the unconditional steps of the legacy sequences, not a cache
 * policy: the crop runs its base finish, geometry transform and viewport sink
 * on every update, and the preview publishes its monitor sink on every full
 * pass. Including them keeps a derived execution set a superset of the legacy
 * one, so a clean shadow comparison really means "nothing legacy did is
 * skipped".
 */
const std::vector<std::string>& resident_blocks(StagePath path);

/*
 * Blocks that share one gate and one buffer seed, and therefore run together.
 *
 * The crop and preview Lab chains are seeded from one `laboCrop -> labnCrop`
 * copy and are gated as a group by the legacy sequences; running one member
 * without the others would consume a stale seed or no seed at all. The group
 * therefore expresses a buffer dependency of the path, not the mask: a derived
 * set that names any member must contain the whole group.
 *
 * Only blocks present on `path` are returned, in canonical order. Paths
 * without coupled blocks return an empty table.
 */
const std::vector<std::vector<std::string>>& coupled_block_groups(
    StagePath path);

/* Per-block parameter fingerprints of one path, in pipeline order. */
struct BlockFingerprints {
    std::vector<std::string> ids;
    std::vector<ParamFingerprint> values;

    bool empty() const
    {
        return ids.empty();
    }
};

/*
 * Fingerprint every block of `path` once.
 *
 * The ids are the canonical ids of buildPipelineSpec(path); the values come
 * from the matching fingerprint function in stage_param_map.h. A path without
 * fingerprints returns an empty result. The caller keeps the previous result
 * and passes it back as history, which avoids hashing the previous parameters
 * again on every update.
 */
BlockFingerprints stage_fingerprints(StagePath path,
                                     const procparams::ProcParams& params);

/*
 * Forward closure of a dirty set over the pipeline edges.
 *
 * A block that regenerates its output buffer forces every consumer to run,
 * because the buffer chain carries one in-place image and there is no
 * intermediate cache yet. The closure adds those consumers, so a set of
 * directly dirty blocks becomes an execution set. Seeds are kept; unknown ids
 * trip an assertion and are ignored; the result is in canonical order.
 */
std::vector<std::string> close_dirty_blocks(
    StagePath path, const std::vector<std::string>& blocks);

/* Inputs of one derivation. Pointers are borrowed and may be null. */
struct DirtyBlockInputs {
    StagePath path = StagePath::Crop;
    /* Previous fingerprints, or null on the first update of a caller. */
    const BlockFingerprints* previousFingerprints = nullptr;
    /* Current fingerprints; required and non-empty for a fingerprinted path. */
    const BlockFingerprints* currentFingerprints = nullptr;
    /* Previous render state, or null on the first update of a caller. */
    const RenderStateKey* previousState = nullptr;
    const RenderStateKey* currentState = nullptr;
    /*
     * Force a full recompute: a reallocation, a forced high-quality pass or a
     * request kind that asks for a whole path. Also implied by missing
     * history.
     */
    bool forceFull = false;
};

struct DirtyBlocks {
    /*
     * The execution set as a set, for a caller that unions two derivations.
     *
     * It is the same membership as `blocks`, which stays the ordered form the
     * execution list and the diagnostics use.
     */
    std::set<std::string> ids;
    /*
     * Whether the result may be used as a derivation.
     *
     * False only when the requested path cannot be derived at all; `reason`
     * then names the limitation. A forced result is valid: it is the whole
     * path, which is the conservative answer rather than a failure.
     * derive_dirty_blocks() always returns valid; the compatibility adapters
     * return false for a form the path does not support, such as a render
     * state on the export path.
     */
    bool valid = false;
    /* Why the result is not a derivation, for the log and the plan refusal. */
    std::string reason;
    /* True when the result is the whole path instead of a derivation. */
    bool forced = false;
    /* Blocks whose parameter fingerprint changed. */
    std::vector<std::string> fromParams;
    /* Blocks the render state schedules. */
    std::vector<std::string> fromState;
    /* Blocks added because a coupled group was seeded, excluding the seeds. */
    std::vector<std::string> fromGroups;
    /* Blocks added by the forward closure, excluding the seeds above. */
    std::vector<std::string> fromClosure;
    /* Directly dirty blocks, before the closure and without the residents. */
    std::vector<std::string> seeds;
    /* Execution set: seeds, residents and closure, in canonical order. */
    std::vector<std::string> blocks;
};

/* Derive the dirty block set. See the unit comment for the contract. */
DirtyBlocks derive_dirty_blocks(const DirtyBlockInputs& inputs);

/*
 * Compatibility forms of the same derivation.
 *
 * The preview coordinator was written against a first, standalone draft of
 * this unit that took a fingerprint map and a render state per call. The
 * functions below keep that call shape so the coordinator compiles unchanged,
 * while every answer is computed by the shared rules above: the parameter
 * forms run stage_fingerprints() and derive_dirty_blocks(), the state form runs
 * changed_render_state_aspects() and blocks_for_render_state(), and the closing
 * and merging forms run close_dirty_blocks(). They return the execution set,
 * residents and forward closure included, exactly as derive_dirty_blocks()
 * does, so a caller that marks every returned id dirty is never missing work.
 *
 * A path that cannot be derived for the requested form answers with the whole
 * path and sets `valid` false, so a caller fails closed instead of mistaking a
 * fallback for a derivation. derive_dirty_blocks() itself always answers with
 * `valid` true and marks a full-path fallback as `forced`.
 */

/* One fingerprint per canonical block of `path`, keyed by block id. */
using ParamSnapshot = std::map<std::string, ParamFingerprint>;

ParamSnapshot snapshotParams(const procparams::ProcParams& params,
                             StagePath path);

/* Parameter delta between two full parameter sets. */
DirtyBlocks dirtyBlocksFromParams(const procparams::ProcParams& previous,
                                  const procparams::ProcParams& current,
                                  StagePath path);

/*
 * Parameter delta against a stored snapshot.
 *
 * A snapshot that does not carry every canonical block of the path is not
 * usable history; the answer is then the whole path, which is the conservative
 * reading of "the missing ids changed".
 */
DirtyBlocks dirtyBlocksFromSnapshot(const ParamSnapshot& previous,
                                    const procparams::ProcParams& current,
                                    StagePath path);

/* Render-state delta; no parameters are consulted. */
DirtyBlocks dirtyBlocksFromRenderState(const RenderStateKey& previous,
                                       const RenderStateKey& current,
                                       StagePath path);

/* Every canonical block of `path`, for a caller with no previous snapshot. */
DirtyBlocks allDirtyBlocks(StagePath path);

/* Union of two dirty sets; valid only when both inputs are valid. */
DirtyBlocks mergeDirtyBlocks(const DirtyBlocks& first,
                             const DirtyBlocks& second);

/* Forward closure of a set over the pipeline edges. */
DirtyBlocks closeOverGraph(const DirtyBlocks& blocks, StagePath path);

/*
 * Shadow comparison of the legacy execution set with the derived one.
 *
 * `legacy` is the set the mask-driven sequence executes today; `derived` is the
 * set derive_dirty_blocks() returned. Both are reduced to canonical order.
 *
 * - `missing` is in the legacy set but not in the derived set. That is the
 *   unsafe direction: executing the derived set would skip work the legacy
 *   sequence performs, so a caller must fail closed to the legacy set.
 * - `extra` is in the derived set but not in the legacy set. It is extra
 *   dirtiness only; the block bodies keep their own mask gates in this phase,
 *   so the extra calls are inert. It is reported, not treated as a failure.
 *
 * `equivalent()` is true when nothing is missing. The extra list is expected
 * to be non-empty: the fingerprint map is finer than the legacy gates.
 */
struct DirtyShadow {
    std::vector<std::string> missing;
    std::vector<std::string> extra;

    bool equivalent() const
    {
        return missing.empty();
    }
};

DirtyShadow compare_dirty_shadow(const std::vector<std::string>& legacy,
                                 const std::vector<std::string>& derived);

/*
 * One-line rendering of a block list for a log record: `[a b c]` or `[]`.
 *
 * Both consumers log the same shape so a shadow mismatch can be compared
 * between the crop and the preview paths.
 */
std::string format_block_ids(const std::vector<std::string>& blocks);

} // namespace rtengine::stage
