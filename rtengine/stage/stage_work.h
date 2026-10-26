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
#include <cstdlib>
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
 * The derivation reads exactly eight inputs (WorkInputs): the previous and
 * current RenderStateKey, the previous and current BlockFingerprints, the
 * previous and current ProcParams, the RequestKind and the StagePath. It never
 * reads the mask, a coordinator, a crop or a pipeline object. The mask still
 * reaches the prediction indirectly because the caller folds its request bits
 * into RequestKind and into the request facts of the key before it derives;
 * that folding happens at the call boundary and is documented for each item
 * below.
 *
 * Exactness contract:
 *
 * - Every item is expressed as a predicate over those eight inputs.
 * - The raw.base block fingerprint merges several parameter families that the
 *   legacy gates treat differently. The previous parameter set is therefore
 *   carried as well, and the raw.base items compare the individual families
 *   (raw, wb, toneCurve, spot, dirpyrDenoise, filmNegative, retinex, cg, icm)
 *   instead of that one fingerprint. A family comparison is exact at the
 *   ProcParams member boundary; it is still coarse within a member.
 * - The order is not executed anywhere in this step. It is a specification
 *   that a later step consumes, so an error here must be visible in the
 *   predicate table rather than in an image.
 *
 * The order is a subset of the legacy gates, never a superset: a fact that
 * the legacy gate does not consult must not name an item, or the shadow
 * comparison would report a typed-only run. Where a legacy bit is coarser
 * than the inputs (an HDR event rebuilding raw.base, for example), the item
 * is deliberately not named and the difference is measured as legacy-only.
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
 * Whether the read-only shadow comparison is enabled.
 *
 * `RT_WORK_SHADOW` is read once per process. The caller gates every shadow
 * computation and every probe on this function; when it is unset, the added
 * cost is one branch and no allocation. No log line is emitted when disabled.
 *
 * Contract of the comparison:
 *
 * - `typedOnly` means the typed order names an item that the legacy gate does
 *   not run. It is a missing input or a derivation error and must be zero.
 * - `legacyOnly` means the legacy gate runs an item that the typed order does
 *   not name. It is the legacy over-approximation that the typed order intends
 *   to save: it is measured, not treated as a defect.
 */
inline bool workShadowEnabled()
{
    static const bool enabled = std::getenv("RT_WORK_SHADOW") != nullptr;
    return enabled;
}

/*
 * Accumulator for one pass.
 *
 * A gate site reports the legacy answer for one item; repeated sites for the
 * same item are ORed, so the pass compares "did any legacy site run this item"
 * with the typed order. Probing is a no-op until a site has been added.
 *
 * The accumulator is a value with fixed storage, safe to keep in a caller's
 * per-pass state. It allocates only in report(), which is called only when the
 * shadow is enabled.
 */
struct WorkShadow {
    void probe(WorkItem item, bool legacyRuns);
    /* The raw.base node-entry gate (baseWorkMayRun), which is an aggregate. */
    void probeRawBaseNode(bool legacyRuns);
    /*
     * Mark a pass whose typed order is the forced full destructive set because
     * the caller has no usable history. The legacy mask is not required to
     * carry every bit on such a pass, so the pass is reported separately and
     * does not contribute to typedOnly.
     */
    void skipForcedPass();
    /* Emit the one-line pass report to stderr. */
    void report(const char* path, const WorkOrder& order) const;

private:
    std::array<bool, workItemCount> probed_{};
    std::array<bool, workItemCount> legacy_{};
    bool nodeProbed_ = false;
    bool nodeLegacy_ = false;
    bool forced_ = false;
};

/*
 * Inputs of one derivation.
 *
 * The pointers are borrowed and are not stored. The previous state, previous
 * fingerprints and previous parameters may be null on the first update of a
 * caller; a null history is read as "every parameter changed" and yields the
 * destructive set, which is what the legacy first pass does (a new coordinator
 * or crop runs `ALL`). A null current state, current fingerprint table or
 * parameter set violates the precondition: the derivation asserts and answers
 * with the whole path-applicable set.
 *
 * `previousParams` is what makes the raw.base split exact. A caller that
 * supplies usable fingerprints but no previous parameters makes every family
 * comparison true, so the answer is conservative rather than silently empty.
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
    /* Parameters of the previous pass, or null when unavailable. */
    const procparams::ProcParams* previousParams = nullptr;
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
 * `family(member)` is the previous/current ProcParams delta of one raw.base
 * parameter family, true when either parameter set is unavailable.
 * `reload` is a source reload, `hdPre`/`hdRaw` are the high-detail work still
 * pending, and `rebuild` is `request_rebuilds_path(requestKind)`. A bare
 * change of the high-detail decision is not a disjunct: it is a mask-level
 * zoom request, not a parameter delta.
 *
 * - sourceSelect: `changed("raw.preprocess") || hdPre || reload`.
 *   Replaces `rawWorkRuns()` at improccoordinator.cc:1345, called from
 *   stageSourceSelect at :1503. Fed by the `raw.preprocess` block (the
 *   `M_PREPROC` row of stage_mask_map.cc:87) and by the Source and HighDetail
 *   aspects. A rebuild request is deliberately not a disjunct: `M_INIT` and
 *   `M_LINDENOISE` rebuild the base without selecting or preprocessing the
 *   source, and `RequestKind` cannot distinguish those bits from `M_PREPROC`.
 * - rawPreprocess: the same predicate. Replaces the `rawWorkRuns()` call at
 *   :1514. Same block and aspects.
 * - rawDemosaic: `changed("raw.demosaic") || hdRaw ||
 *   colourHlr || reload`. Replaces `rawSourceWorkRuns()` at
 *   improccoordinator.cc:1362, called at :1563. Fed by the `raw.demosaic`
 *   block (`M_RAW` and `M_CSHARP` rows, stage_mask_map.cc:89-92), the Source
 *   and HighDetail aspects, and the parameter/`rgbSourceModified` pair of the
 *   colour highlight-recovery condition. Capture sharpening is inside the
 *   same body and is covered by the same fingerprint. A rebuild request is not
 *   a disjunct for the same reason as sourceSelect.
 * - rawBaseRebuild: `rawPreprocess || rawDemosaic || family(wb) ||
 *   family(toneCurve) || family(filmNegative) || family(retinex) ||
 *   family(cg) || family(icm-without-output) || rebuild`. Replaces
 *   `baseRebuildRuns()` (improccoordinator.cc:1378) and the reuse gate at
 *   :1660/:1684. The raw and denoise families reach it through the
 *   preprocess/demosaic forward edges. `family(spot)` is excluded: a spot
 *   delta does not rebuild the base. `family(icm)` ignores the three output
 *   profile fields, because `EvOProfile` only dirties the monitor sink.
 *   Dehaze, Fattal and the transform families are excluded as well: the
 *   legacy `HDR` event over-approximates and rebuilds the base for them, and
 *   that difference is measured rather than copied. `spotBufferLive` is not a
 *   disjunct either: a live buffer forces the restore, not the destructive
 *   body.
 * - retinexPrepare: `params->retinex.enabled && (family(retinex) ||
 *   rawPreprocess || rawDemosaic)`. Replaces the gate at
 *   improccoordinator.cc:1666 (`M_RETINEX | M_INIT` plus the enabled check).
 *   Retinex rewrites the shared raw planes, so it is named when its own
 *   parameters move or when a step that regenerates those planes runs. A
 *   white-balance-only `M_INIT` does not regenerate the planes and is left to
 *   the legacy-only class.
 * - spotApply: `spotPending || (spotConfigured && (family(spot) ||
 *   rawBaseRebuild))`. Replaces the gates at improccoordinator.cc:2029 and
 *   :2123. `spotPending` is the request fact carried by the key; the family
 *   delta is the parameter half. The base rebuild is a disjunct because the
 *   spots are removed from the freshly rebuilt base even when their
 *   parameters did not move. Fed by the `spot` family and by the Derived
 *   aspect.
 * - spotRestore: `spotPending || spotBufferLive || family(spot)`. Replaces the
 *   restore at improccoordinator.cc:2123 and the copy-back at :2137. The
 *   live-buffer disjunct is the standing rule: a live buffer changes the next
 *   base pass and must be kept in step even when nothing else moves. A spot
 *   parameter delta allocates a new buffer in the same pass, so it names the
 *   restore as well.
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
 * eight WorkInputs; no gate is expressed with a mask bit.
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
 * - spotApply: `spotPending`. Replaces the gates at dcrop.cc:1402 and
 *   :1494. `spotPending` is `(todo & M_SPOT)` plus the enabled/entries test
 *   carried by the key; the crop has no base rebuild of its own that would
 *   re-apply the spots.
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
