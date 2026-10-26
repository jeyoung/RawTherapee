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
#include "stage_render_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rtengine::procparams
{
class ProcParams;
}

namespace rtengine::stage
{

/*
 * Typed work order for the destructive raw and core jobs.
 *
 * The preview and the crop still decide which body jobs run from the legacy
 * `M_*` mask. Before those gates can be retired, each destructive job needs a
 * name and an exact membership predicate that is derived from the same inputs
 * the gate uses, not from the mask. This unit is that order.
 *
 * The scope is deliberately the destructive raw/core jobs only: the jobs that
 * regenerate pixels, rebuild the raw base or write the spot buffer. The
 * downstream Lab, transform and monitor jobs keep the dirty-block derivation
 * of stage_dirty_blocks.h in this step; adding them here would mix a
 * pixel-invalidation order with an execution-set order.
 *
 * The derivation reads exactly seven inputs (WorkInputs): the previous and
 * current RenderStateKey, the previous and current BlockFingerprints, the
 * RequestKind, the StagePath and the current ProcParams. It never reads the
 * mask, a coordinator, a crop or a pipeline object. The mask still reaches the
 * prediction indirectly because the caller folds its request bits into
 * RequestKind and into the request facts of the key before it derives; that
 * folding happens at the call boundary and is documented for each item below.
 *
 * Exactness contract:
 *
 * - Every item is expressed as a predicate over those seven inputs.
 * - A predicate that the inputs cannot express is marked UNRESOLVED in the
 *   comment for that item, and the missing fact is named. An unresolved item
 *   answers with the conservative (superset) predicate instead of silently
 *   dropping the gate.
 * - The order is not executed anywhere in this step. It is a specification
 *   that a later step consumes, so an error here must be visible in the
 *   predicate table rather than in an image.
 *
 * A block fingerprint is the finest parameter fact the unit carries. Where a
 * legacy bit named a narrower parameter family than the block's field list,
 * the predicate is the block-level answer: the item is named when any field of
 * the owning block moved, and the body keeps its own internal gates for the
 * sub-steps. That is a superset only where the field list merges families, it
 * is never a subset, and the merge is recorded here rather than left implicit.
 * The one merge that changes which outer body runs (raw.base) is called out as
 * UNRESOLVED under that item.
 */

/*
 * One destructive body job.
 *
 * The names are the diagnostic vocabulary and must stay stable: a later step
 * logs and compares orders, so renumbering or renaming an item breaks the log
 * contract. `Count` is the sentinel and is never a member of an order.
 *
 * The enumerator spelling follows the job name used by the gates rather than
 * the UpperCamelCase used by other stage enums, so a log line can be matched
 * to the gate without a translation table.
 */
enum class WorkItem : uint8_t {
    /* Select the frame and, on the crop, reload the ROI bookkeeping. */
    sourceSelect = 0,
    /* Black/white levels, flat field and the pre-interpolation pass. */
    rawPreprocess = 1,
    /* Demosaic, capture sharpening and the retinex buffer preparation. */
    rawDemosaic = 2,
    /* The destructive raw.base body: getImage, spot, denoise, film negative. */
    rawBaseRebuild = 3,
    /* The retinex pre-pass that rewrites the shared raw planes. */
    retinexPrepare = 4,
    /* Spot removal into the base image. */
    spotApply = 5,
    /* Spot buffer restore or copy-back. */
    spotRestore = 6,
    /* The analysis body, including the automatic-denoise analysis. */
    analysisRun = 7,
    /* The directional-pyramid denoise body. */
    denoiseRun = 8,
    Count = 9
};

/* Number of real WorkItem values, excluding the sentinel. */
constexpr size_t workItemCount = static_cast<size_t>(WorkItem::Count);

/* Stable lower-case name of one item, for diagnostics. Never null. */
const char* workItemName(WorkItem item);

/*
 * Fixed-size bitset over WorkItem.
 *
 * The type is a value: copyable, assignable, trivially destructible and
 * pointer-free. It is safe to store in a caller's history, compare between two
 * passes and log, with no ownership question.
 *
 * `size()` is the size of the universe (workItemCount); `count()` is the
 * number of items that are set.
 */
struct WorkOrder {
    static constexpr size_t wordBits = 64;
    static constexpr size_t wordCount = (workItemCount + wordBits - 1) / wordBits;

    bool has(WorkItem item) const;
    void set(WorkItem item);
    void clear(WorkItem item);

    static constexpr size_t size()
    {
        return workItemCount;
    }

    size_t count() const;
    bool empty() const;

    bool operator==(const WorkOrder& other) const;
    bool operator!=(const WorkOrder& other) const;

private:
    std::array<uint64_t, wordCount> words_{};
};

/*
 * One-line rendering of an order for a log record: `[sourceSelect rawBaseRebuild]`
 * or `[]`, in WorkItem order. The format is stable so two passes can be
 * compared textually.
 */
std::string workOrderName(const WorkOrder& order);

/*
 * Inputs of one derivation.
 *
 * The pointers are borrowed and are not stored. The previous state and the
 * previous fingerprints may be null on the first update of a caller; a null
 * history is read as "every parameter changed" and yields the destructive set,
 * which is what the legacy first pass does (a new coordinator or crop runs
 * `ALL`). A null current state, current fingerprint table or parameter set
 * violates the precondition: the derivation asserts and answers with the whole
 * path-applicable set.
 *
 * `requestKind` is the request this pass serves. It must equal
 * `currentState->requestKind` when a current state is supplied; the duplicate
 * member exists so a caller can state the request without materialising a key
 * for a request-only question. `path` selects the derivation and is asserted
 * against the entry point.
 */
struct WorkInputs {
    /* Previous render state, or null on the first update. */
    const RenderStateKey* previousState = nullptr;
    /* Current render state; required. */
    const RenderStateKey* currentState = nullptr;
    /* Fingerprints of the previous pass, or null on the first update. */
    const BlockFingerprints* previousFingerprints = nullptr;
    /* Fingerprints of this pass; required. */
    const BlockFingerprints* currentFingerprints = nullptr;
    /* Request under service; must agree with currentState->requestKind. */
    RequestKind requestKind = RequestKind::ParamsOnly;
    /* Path the order is derived for. */
    StagePath path = StagePath::Preview;
    /* Current parameters; required, borrowed. */
    const procparams::ProcParams* params = nullptr;
};

/*
 * Work order of one preview pass.
 *
 * Predicates, with the gate each replaces. `changed(id)` is the fingerprint
 * delta of that canonical block, true when no usable history exists.
 * `reload` is a source reload, `hdPre`/`hdRaw` are the high-detail work still
 * pending, `hdMoved` is a change of the high-detail decision, and `rebuild` is
 * `request_rebuilds_path(requestKind)`.
 *
 * - sourceSelect: `changed("raw.preprocess") || hdPre || hdMoved || reload ||
 *   rebuild`. Replaces `rawWorkRuns()` at improccoordinator.cc:1345, called
 *   from stageSourceSelect at :1503. Fed by the `raw.preprocess` block (the
 *   `M_PREPROC` row of stage_mask_map.cc:87) and by the Source and HighDetail
 *   aspects. The source facts are part of the predicate because selecting the
 *   frame is the per-pass half of a reload; a rebuild request forces the step
 *   because the request bits are absolute.
 * - rawPreprocess: the same predicate. Replaces the `rawWorkRuns()` call at
 *   :1514. Same block and aspects.
 * - rawDemosaic: `changed("raw.demosaic") || hdRaw || hdMoved ||
 *   colourHlr || reload || rebuild`. Replaces `rawSourceWorkRuns()` at
 *   improccoordinator.cc:1362, called at :1563. Fed by the `raw.demosaic`
 *   block (`M_RAW` and `M_CSHARP` rows, stage_mask_map.cc:89-92), the Source
 *   and HighDetail aspects, and the parameter/`rgbSourceModified` pair of the
 *   colour highlight-recovery condition. Capture sharpening is inside the
 *   same body and is covered by the same fingerprint.
 * - rawBaseRebuild: `changed("raw.base") || rawPreprocess ||
 *   rawDemosaic || rebuild || spotBufferLive`.
 *   Replaces `baseRebuildRuns()` (improccoordinator.cc:1378) as used by
 *   `baseWorkMayRun()` at :1384 and the rebuild branch at :1660/:1684. Fed by
 *   the `raw.base` block (`M_WB`, `M_INIT`, `M_SPOT`, `M_LINDENOISE`,
 *   `M_RETINEX` rows), the Source, HighDetail and Derived aspects, and the
 *   Request aspect. UNRESOLVED: the legacy split inside raw.base between
 *   `M_INIT`/`M_LINDENOISE`/`M_HDR`, which rebuild, and `M_RETINEX`/`M_SPOT`,
 *   which only prepare, is not representable here because raw.base has one
 *   fingerprint and no previous ProcParams is carried. The missing fact is a
 *   per-family fingerprint (or the previous ProcParams) for raw.base. The
 *   predicate is the block-level answer and is therefore a superset: a
 *   retinex-only or spot-only delta also names the rebuild body, whose own
 *   internal gates then decide. It is not a skipped job.
 * - retinexPrepare: `rawBaseRebuild && params->retinex.enabled`. Replaces the
 *   gate at improccoordinator.cc:1666 (`M_RETINEX | M_INIT` plus the enabled
 *   check). Fed by the `raw.base` block through the `M_RETINEX` row and by the
 *   Request aspect. The `M_RETINEX` half has the same UNRESOLVED split as
 *   rawBaseRebuild; the enabled test is exact.
 * - spotApply: `rawBaseRebuild && spotPending`. Replaces the gate at
 *   improccoordinator.cc:2029. Fed by the `raw.base` block through `M_SPOT`,
 *   by `spotPending` (a request fact deliberately exempt from the aspect map)
 *   and by the standing live-buffer rule.
 * - spotRestore: `spotPending || spotBufferLive`. Replaces the restore at
 *   improccoordinator.cc:2123 and the copy-back at :2137. Fed by `spotPending`
 *   and by the `spotBufferLive` half of the Derived aspect. The live-buffer
 *   disjunct is the standing rule: a live buffer changes the next base pass
 *   and must be kept in step even when nothing else moves.
 * - analysisRun: the same predicate as rawBaseRebuild. The preview folds
 *   analysis into raw.base (`isFoldedPreviewNode("analysis")`), so the body
 *   at improccoordinator.cc:2121 runs exactly with the rebuild branch. Fed by
 *   raw.base and the Derived and HighDetail aspects.
 * - denoiseRun: never derived. The preview's directional-pyramid denoise call
 *   is commented out (improccoordinator.cc:2038-2060); `M_LINDENOISE` only
 *   participates through `baseRebuildRuns()`. There is no preview body to
 *   schedule, so this is exact, not a disabled approximation.
 *
 * Facts that no preview item reads, and why:
 *
 * - `viewportMoved`: the preview reports the panning heuristic under this
 *   name, but no raw gate consults it; a repaint keeps the raw buffers.
 * - `cropGuideFingerprint`: guides are an overlay and no stage consumes them.
 * - `deferredAnalysisMask`: the deferred mask carries only monitor and
 *   curve/Lab bits by construction (improccoordinator.cc:1018-1026), never a
 *   base bit, so it cannot schedule a raw item.
 * - `fastPreviewEnabled`, `mainPreviewFullResolution`, `initialRoughPreview`
 *   and `sharpMask`: they decide `highDetailNeeded` and the demosaic mode
 *   before the key is built, so their effect reaches the order through the
 *   high-detail fields and the editor-triggered `raw.demosaic` delta.
 * - `previousDemoBase`: it selects the base cache preference inside the body,
 *   which is restore-versus-recompute, not whether the body runs.
 *
 * The mask is never an input. Every predicate above is a function of the
 * seven WorkInputs; a gate that could only be expressed with a mask bit would
 * be reported as unresolved rather than added.
 */
WorkOrder derive_preview_work(const WorkInputs& inputs);

/*
 * Work order of one crop pass.
 *
 * Predicates, with the gate each replaces. `reload` is
 * `request_rebuilds_path(requestKind) || sourceReloaded || hdMoved || !history`,
 * which is how the crop expresses the `M_INIT | M_LINDENOISE | M_HDR` reload
 * bits it folds into `RequestKind::FullCrop` (dcrop.cc:679-686).
 *
 * - sourceSelect: `reload`. Replaces the gate at dcrop.cc:929. Fed by the
 *   Request aspect (the crop's only mask-derived fact), the Source aspect and
 *   the HighDetail aspect.
 * - rawBaseRebuild: `reload`. Replaces the branch at dcrop.cc:1395 and the
 *   denoise branch at :1442. Fed by the same aspects. The crop always runs the
 *   base tail (dcrop.cc:1491) as a resident step; only this destructive body
 *   is gated, which is why the predicate is the reload alone and not the
 *   base-block fingerprint.
 * - retinexPrepare: never derived. The crop receives the base image from the
 *   coordinator and has no retinex buffer preparation call.
 * - rawPreprocess, rawDemosaic: never derived. The crop never preprocesses or
 *   demosaics; it reads the coordinator's source through `getImage()`.
 * - spotApply: `reload && spotPending`. Replaces the gate at dcrop.cc:1402.
 * - spotRestore: `spotPending || spotBufferLive`. Replaces the restore at
 *   dcrop.cc:1494 and the pointer swap at :1511. The live-buffer disjunct is
 *   the standing rule; on the crop the restore is a pointer swap, so it does
 *   not force the destructive body and rawBaseRebuild stays reload-only.
 * - analysisRun: `reload`. Replaces the gate at dcrop.cc:1008. The item covers
 *   the automatic-denoise analysis and the PRE/MAN ROI reload inside the same
 *   body; the `leveldnautsimpl` and method branches are internal and do not
 *   change whether the body is entered.
 * - denoiseRun: `reload && params->dirpyrDenoise.enabled && cropSkip == 1`.
 *   Replaces the gate at dcrop.cc:1444-1447. The skip test is exact: the crop
 *   denoises only at skip 1, and `cropSkip` is a field of the key.
 *
 * Facts that no crop item reads, and why:
 *
 * - `denoiseAnalysisValid` and `dirPyrDenoiseExpComp`: they feed the analysis
 *   and denoise arithmetic inside the bodies, not whether the body runs. The
 *   crop rebuilds both when the reload bit is present.
 * - `viewportMoved`: `RequestKind::RepaintOnly` already represents it, and a
 *   repaint schedules no destructive job.
 * - `deferredAnalysisMask`, `cropGuideFingerprint`: as on the preview.
 *
 * The mask is never an input. The crop's reload bits reach this function only
 * through `RequestKind`, at the boundary that builds the key.
 */
WorkOrder derive_crop_work(const WorkInputs& inputs);

} // namespace rtengine::stage
