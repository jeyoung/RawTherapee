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
 * Both paths run their raw/core gates from this order by default. RT_WORK_ORDER=0
 * or =off is the kill switch: it derives a conservative order that names every
 * parameter-applicable raw/core item for the pass, so no body is skipped at the
 * cost of redundant work. The kill switch never falls back to the legacy `M_*`
 * mask.
 *
 * The scope is deliberately the destructive raw/core jobs only: the jobs that
 * regenerate pixels, rebuild the raw base or write the spot buffer. The
 * downstream Lab, transform and monitor jobs keep the dirty-block derivation
 * of stage_dirty_blocks.h in this step; adding them here would mix a
 * pixel-invalidation order with an execution-set order.
 *
 * The derivation reads exactly nine inputs (WorkInputs): the previous and
 * current RenderStateKey, the previous and current BlockFingerprints, the
 * previous and current ProcParams, the RequestKind, the StagePath and the
 * Color/Coloropp white-balance request fact. It never reads the mask, a
 * coordinator, a crop or a pipeline object. The mask still reaches the
 * prediction indirectly because the caller folds its request bits into
 * RequestKind, into the request facts of the key and into the white-balance
 * request fact before it derives; that folding happens at the call boundary
 * and is documented for each item below.
 *
 * Exactness contract:
 *
 * - Every item is expressed as a predicate over those eight inputs.
 * - The raw consumer fingerprints are split at the same boundary as the
 *   legacy gates: `source.select` hashes the frame-number member,
 *   `raw.preprocess` the preprocess-only raw and lens-profile fields, and
 *   `raw.demosaic` the sensor demosaic fields plus `pdsharpening`. A raw edit
 *   therefore moves only the blocks whose body reads it. The field lists are
 *   in stage_param_map.cc and mirror stage_pipeline.cc.
 * - The raw.base block fingerprint still merges several parameter families
 *   that the legacy gates treat differently. The previous parameter set is
 *   therefore carried as well, and the raw.base items compare the individual
 *   families (wb, toneCurve, dirpyrDenoise, filmNegative, retinex, cg, icm)
 *   instead of that one fingerprint. The raw families reach the base through
 *   the preprocess/demosaic forward edges, so they are not compared twice. A
 *   family comparison is exact at the ProcParams member boundary.
 * - A member that genuinely feeds two bodies is listed in both consumer
 *   groups. The shared members are `raw.bayersensor.border` and `raw.expos`
 *   (preprocess and demosaic). `raw.bayersensor.method` also reaches the
 *   preprocess green-equilibration guard, but the legacy M_PREPROC family does
 *   not carry M_RAW and does not re-preprocess on a method edit; preprocess
 *   therefore omits it and the demosaic consumer owns it. That is a bounded
 *   subset of the body's ideal inputs, chosen to match the gate the order
 *   describes, not a typed-only superset.
 * - The order is the execution-set authority at every converted site. The
 *   downstream preview and crop nodes keep their own gates; this unit only
 *   decides which raw/core bodies run.
 * - When the switch is off the caller derives with no usable history, so every
 *   parameter-applicable item is named. That is the conservative fallback: a
 *   superset of the exact set, safe and slower, never the mask.
 *
 * The order names an item only when a request, a parameter delta or a live
 * buffer state carries it. Where the legacy gate is coarser than the body's
 * inputs, that keeps the item out; the legacy over-approximation is documented
 * as redundant work below.
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
 * The raw/core sites no longer probe a legacy answer; the typed order is the
 * execution-set authority at every converted site. The switch is retained for
 * the diagnostic sweep harness, which reports the environment state before a
 * run; it has no effect on the engine.
 */
inline bool workShadowEnabled()
{
    static const bool enabled = std::getenv("RT_WORK_SHADOW") != nullptr;
    return enabled;
}

/*
 * Whether one `RT_WORK_ORDER` value leaves the typed order enabled.
 *
 * Unset (`nullptr`) is the default and enables it. The literals "0" and
 * "off" (case-insensitive) disable it. Every other value, including an empty
 * string, enables it. The parser is exposed so a unit test can pin the rule
 * without a child process; `workOrderEnabled()` reads the environment once.
 */
inline bool workOrderValueEnables(const char* value)
{
    if (value == nullptr) {
        return true;
    }
    if (value[0] == '0' && value[1] == '\0') {
        return false;
    }
    const auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return !(lower(value[0]) == 'o' && lower(value[1]) == 'f'
             && lower(value[2]) == 'f' && value[3] == '\0');
}

/*
 * Whether the exact typed order drives the raw/core gates.
 *
 * `RT_WORK_ORDER` is read once per process. The default is on: the typed order
 * is the sole scheduling authority at every converted gate.
 *
 * The variable is the kill switch for the conversion, not a preference: any
 * value keeps the exact typed order enabled, while "0" and "off" select the
 * conservative fallback. There is no user-facing setting; the switch exists
 * so a regression can be isolated without rebuilding.
 *
 * The kill switch does not restore the legacy `M_*` gates. The caller derives
 * the order with no usable history, which names every parameter-applicable
 * raw/core item for the pass, so a converted site runs a safe superset of the
 * exact set instead of skipping a body. The plan, the dirty-block derivation
 * and every downstream node gate are unchanged by the switch.
 */
inline bool workOrderEnabled()
{
    static const bool enabled =
        workOrderValueEnables(std::getenv("RT_WORK_ORDER"));
    return enabled;
}

/*
 * Answer of one converted gate site.
 *
 * The caller supplies the order it derived. The exact order names a body only
 * when its predicate holds; the conservative order (kill switch off) names
 * every parameter-applicable body. The site never consults the mask, so a
 * missing item skips the body only on the exact order.
 */
inline bool workItemRuns(const WorkOrder& order, WorkItem item)
{
    return order.has(item);
}


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
    /*
     * The pass mask carries the `M_WB` request bit. This is a request fact of
     * the same kind as RequestKind: the caller folds it at the boundary
     * because the mask is never an input. It exists for the caller's
     * Color/Coloropp white-balance augmentation, which sets `DEMOSAIC` before
     * the pass; a `wb` parameter delta alone cannot express it, because the
     * same delta can arrive without the request bit.
     */
    bool colorHlrWbRequest = false;
};

/*
 * Work order of one preview pass.
 *
 * Predicates, with the gate each replaces. `changed(id)` is the fingerprint
 * delta of that canonical block, true when no usable history exists.
 * `source` is `changed("source.select") || sourceReload`; `preprocessChanged`
 * is `changed("raw.preprocess")`; `sensorDemosaicChanged` is the demosaic
 * part of `changed("raw.demosaic")` (that block also hashes pdsharpening) and
 * `captureSharpening` is a pdsharpening delta while the tool is enabled.
 * `family(member)` is the previous/current ProcParams delta of one raw.base
 * parameter family, true when either parameter set is unavailable.
 * `hdPre`/`hdRaw` are the high-detail work still pending, `hlr` is the colour
 * highlight-recovery condition, and `rawReload` is
 * `requestKind == FullPreview || FullCrop`. A high-detail decision change also
 * changes the demosaic mode (the caller substitutes the FAST method below
 * 100%), so both directions name rawDemosaic; the mask-level zoom request
 * carries `M_RAW` for exactly that reason.
 *
 * - sourceSelect: `preprocess`, below. Replaces `rawWorkRuns()` at
 *   improccoordinator.cc:1346, called from stageSourceSelect at :1531. Fed by
 *   the `raw.preprocess` and `source.select` blocks (the `M_PREPROC` row of
 *   stage_mask_map.cc:87) and by the Source and HighDetail aspects. A rebuild
 *   request is deliberately not a disjunct: `M_INIT` and `M_LINDENOISE`
 *   rebuild the base without selecting or preprocessing the source. Legacy-only
 *   remainder: a mask event that carries `M_PREPROC` but moves neither the
 *   frame number nor a source fact (for example the DARKFRAME-tagged Clamp OOG
 *   toggle, rtgui/tools/tonecurve.cc:44) calls `setCurrentFrame()` with the
 *   frame already loaded; that method marks the source only when the frame
 *   actually changes (rawimagesource.h), so the call is redundant work.
 * - rawPreprocess: `preprocessChanged || source || hdPre`. Replaces the
 *   `rawWorkRuns()` call at :1546. Same blocks and aspects. The same
 *   over-approximating event re-runs preprocess with identical raw and
 *   lens-profile inputs, so the planes it produces are unchanged; that
 *   remainder is redundant work, and the base rebuild the edit needs is named
 *   separately.
 * - rawDemosaic: `sensorDemosaicChanged || captureSharpening || source ||
 *   preprocessChanged || hdRaw || highDetailModeChanged || hlr ||
 *   hlrWbDemosaic || hlrConfigChanged || retinexPlanes`.
 *   Replaces `rawSourceWorkRuns()` at improccoordinator.cc:1363, called at
 *   :1599. Fed by the `raw.demosaic` block (`M_RAW` and `M_CSHARP` rows,
 *   stage_mask_map.cc:89-92), the forward edges from the selected frame and the
 *   preprocessed planes (every DARKFRAME/FLATFIELD event also sets M_RAW), the
 *   HighDetail aspect, and the parameter/`rgbSourceModified` pair of the colour
 *   highlight-recovery condition. Capture sharpening is inside the same body:
 *   it modifies the demosaiced planes in place, so `captureSharpening` is a
 *   `pdsharpening` delta while the tool is enabled *or* was enabled in the
 *   previous pass. Disabling it must re-run the demosaic to remove the
 *   sharpening the planes still carry, and the legacy DEMOSAIC event carries
 *   `M_RAW` in that direction too. `retinexPlanes` names the body for the
 *   retinex buffer preparation that lives at the end of it: the buffers must
 *   be built when retinex is newly enabled, removed when it is disabled (only
 *   a fresh demosaic discards the retinex output written into the planes), and
 *   rebuilt when the working profile moves while it is enabled. A bare
 *   retinex strength edit carries `M_RETINEX` and reuses the buffers, so it is
 *   not a disjunct. A rebuild request is not a disjunct for the same reason as
 *   sourceSelect.
 *   `hlrWbDemosaic` mirrors the caller's Color/Coloropp white-balance
 *   augmentation (the `M_WB` request fact, recovery enabled and method
 *   `Coloropp`): getImage() applies the opposed recovery with the current
 *   white-balance multipliers and marks the planes recovered, so only fresh
 *   demosaic planes let the new multipliers take effect. It is pixel-affecting
 *   and therefore named; the request fact is carried by WorkInputs because a
 *   bare `wb` delta cannot tell the request from a re-sent value.
 *   `hlrConfigChanged` names a recovery method, blend or threshold
 *   delta while the planes already carry recovery output
 *   (`rgbSourceModified`), because the new setting can only be applied to
 *   fresh planes. An event that moves none of the body's inputs leaves the
 *   demosaic legacy-only redundant work; see the remaining cases below.
 * - rawBaseRebuild: `preprocess || demosaic || family(wb) ||
 *   family(toneCurve) || family(dirpyrDenoise) || family(filmNegative) ||
 *   family(retinex) || family(cg) || family(icm-without-output) ||
 *   rawReload`. Replaces `baseRebuildRuns()` (improccoordinator.cc:1379), the
 *   `stageRawBase` branch at :1694, the `getImage()` call at :2064 and the
 *   first analysis at :2185. `family(dirpyrDenoise)` is included because the
 *   legacy base gate carries `M_LINDENOISE`; the raw families reach the base
 *   through the preprocess/demosaic forward edges and are not compared twice.
 *   `family(spot)` is excluded: a spot delta does not rebuild the base.
 *   `family(icm)` ignores the three output-profile fields, because
 *   `EvOProfile` only dirties the monitor sink. `rawReload` excludes
 *   `HighQuality`: `M_HIGHQUAL` is not part of the legacy base gate, and a
 *   bare M_HIGHQUAL request is a real pass (imagearea.cc:809), not a
 *   theoretical one. `spotBufferLive` is not a disjunct either: a live buffer
 *   forces the restore, not the destructive body.
 * - retinexPrepare: `params.retinex.enabled && (family(retinex) ||
 *   preprocess || demosaic || icm-without-output)`. Replaces the gate at
 *   improccoordinator.cc:1719 (`M_RETINEX | M_INIT` plus the enabled check).
 *   Retinex rewrites the shared raw planes, so it is named when its own
 *   parameters move or when a step that regenerates those planes runs. The
 *   working profile is a real input of the preparation: the buffers are
 *   converted with `workingSpaceMatrix(cmp.workingProfile)` and back with its
 *   inverse (rawimagesource.cc: retinexPrepareBuffers/retinex), so
 *   `icm-without-output` names it even when the planes did not move.
 *   A white-balance-only `M_INIT` does not regenerate the planes and the
 *   preparation does not read the white balance (getImage() applies the
 *   multipliers afterwards), so that remainder is redundant work.
 * - spotApply: `spotPending || (spotsConfigured && family(spot))`. Replaces
 *   the gates at improccoordinator.cc:2088 and :2188/:2191. `spotPending` is
 *   the request fact carried by the key; the family delta is the parameter
 *   half. When a base rebuild runs, the spots are removed from the freshly
 *   rebuilt image even if their parameters did not move, but that case is
 *   already carried by `spotPending` for an M_SPOT request. Fed by the `spot`
 *   family and by the Derived aspect.
 * - spotRestore: `spotBufferLive || (spotApply && !base)`. Replaces the
 *   copy-back gate at improccoordinator.cc:2207. The live-buffer disjunct is
 *   the standing rule: a live buffer changes the next base pass and must be
 *   kept in step even when nothing else moves. A spot parameter delta
 *   allocates a new buffer in the same pass, so it names the restore as well
 *   when the base did not run. A live buffer whose tool is now disabled is
 *   named by the same disjunct: the buffer-maintenance site clears it, and
 *   the copy-back that follows finds no buffer. Naming the clear is what lets
 *   the site stop reading `M_SPOT`.
 * - analysisRun: the same predicate as rawBaseRebuild. The preview folds
 *   analysis into raw.base (`isFoldedPreviewNode("analysis")`), so the body
 *   at improccoordinator.cc:2185 runs exactly with the rebuild branch.
 * - denoiseRun: never derived. The preview's directional-pyramid denoise call
 *   is commented out (improccoordinator.cc:2102-2141); `M_LINDENOISE` only
 *   participates through `baseRebuildRuns()`. There is no preview body to
 *   schedule, so this is exact, not a disabled approximation.
 *
 * Remaining typed supersets: none for the edit families the tests and GUI
 * evidence drive. The deliberate legacy-only cases are:
 * - A white-balance `M_INIT` for retinex preparation. The preparation reads
 *   the raw planes, the retinex parameters and the working profile, none of
 *   which a white-balance edit moves; getImage() applies the multipliers
 *   later. The job is redundant, not skipped (see retinexPrepare above).
 * - A working-profile `DEMOSAIC` event for the demosaic body while retinex is
 *   disabled. The demosaic reads `RAWParams` and `pdsharpening`, not `icm`, so
 *   the fresh planes are identical; the base rebuild the edit needs is named
 *   separately by rawBaseRebuild. While retinex is enabled the body is named
 *   through `retinexPlanes`, because the buffers are converted with the
 *   working-space matrix.
 * - A recovery method, blend or threshold event while the planes carry no
 *   recovery output (`rgbSourceModified` false): getImage() applies the new
 *   setting to the current planes, so a fresh demosaic changes nothing.
 * - A `DEMOSAIC` or DARKFRAME mask event that moves none of the source,
 *   preprocess or demosaic inputs (for example a profile save that re-sends an
 *   already-enabled capture-sharpening flag, or the DARKFRAME-tagged Clamp OOG
 *   toggle): `setCurrentFrame()` sees the frame already loaded, preprocess
 *   reproduces the same planes, and demosaic and capture sharpening are
 *   idempotent under the same white balance. The base rebuild and analysis the
 *   event asks for are named separately. The per-item argument is in the
 *   sourceSelect, rawPreprocess and rawDemosaic bullets above.
 * - A high-detail fit transition (`ALL | M_MONITOR` with `highDetailNeeded`
 *   false after the 100% pass) for sourceSelect and rawPreprocess. Nothing
 *   moved in the source or the preprocessed planes, so the legacy
 *   re-selection and re-preprocess are redundant. rawDemosaic is named by the
 *   order in this direction because the fit pass switches the demosaic mode
 *   to FAST, and rawBaseRebuild is named separately by the reload request.
 *
 * Each is redundant work rather than a pixel-affecting job. The `typedOnly=0`
 * shadow measurement that established this before the retirement is recorded
 * in devnotes/perf/STAGE_PIPELINE.md. The caller's Color/Coloropp
 * `DEMOSAIC` augmentation for a white-balance edit was in this class before
 * the conversion; it is pixel-affecting, so `hlrWbDemosaic` now names it
 * instead.
 *
 * Facts that no preview item reads, and why:
 *
 * - `viewportMoved`: the preview reports the panning heuristic under this
 *   name, but no raw gate consults it; a repaint keeps the raw buffers.
 * - `cropGuideFingerprint`: guides are an overlay and no stage consumes them.
 * - `deferredAnalysisMask`: the deferred mask carries only monitor and
 *   curve/Lab bits by construction (improccoordinator.cc:1019-1027), never a
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
 * Predicates, with the body each decides. `reload` is `!history || rawReload`,
 * where `rawReload` is `requestKind == FullCrop || FullPreview`. The crop folds
 * the reload bits (`M_INIT | M_LINDENOISE | M_HDR`) into
 * `RequestKind::FullCrop` at dcrop.cc before it builds the key; that request
 * fact is the only way the mask reaches this function. A source reload or a
 * high-detail transition does not add an independent disjunct: it reaches the
 * crop through the same bits, while the dirty-block derivation models the
 * Source and HighDetail render-state aspects separately. `HighQuality` is not a
 * reload: it maps `M_HIGHQUAL`, which no crop gate consults.
 *
 * - sourceSelect: `reload`. Fed by the Request aspect (the crop's only
 *   mask-derived fact), the Source aspect and the HighDetail aspect.
 * - rawBaseRebuild: `reload`. The crop always runs the base tail
 *   (`createBuffer()`) as a resident step; only this destructive body is gated,
 *   which is why the predicate is the reload alone and not the base-block
 *   fingerprint.
 * - retinexPrepare: never derived. The crop receives the base image from the
 *   coordinator and has no retinex buffer preparation call.
 * - rawPreprocess, rawDemosaic: never derived. The crop never preprocesses or
 *   demosaics; it reads the coordinator's source through `getImage()`.
 * - spotApply: `spotPending || (spotsConfigured && family(spot))`.
 *   `spotPending` is `(todo & M_SPOT)` plus the enabled/entries test carried by
 *   the key; that folding is the request fact. The crop has no base rebuild of
 *   its own that would re-apply the spots.
 * - spotRestore: `spotBufferLive || (spotApply && !reload)`. The live-buffer
 *   disjunct is the standing rule; on the crop the restore is a pointer swap,
 *   so it does not force the destructive body and rawBaseRebuild stays
 *   reload-only. A live buffer whose tool is now disabled is named by the same
 *   disjunct so the buffer-maintenance site clears it; the copy-back that
 *   follows then finds no buffer.
 * - analysisRun: `reload`. The item covers the automatic-denoise analysis and
 *   the PRE/MAN ROI reload inside the same body; the `leveldnautsimpl` and
 *   method branches are internal and do not change whether the body is entered.
 * - denoiseRun: `reload && params.dirpyrDenoise.enabled && cropSkip == 1`.
 *   The skip test is exact: the crop denoises only at skip 1, and `cropSkip`
 *   is a field of the key.
 *
 * Remaining typed supersets: none. The spot buffer clear is named by the
 * spotRestore disjunct, so no crop site reads the mask.
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
 * The mask is never an input. The crop's reload and spot request bits reach
 * this function only through `RequestKind` and `spotPending`, at the boundary
 * that builds the key.
 */
WorkOrder derive_crop_work(const WorkInputs& inputs);

} // namespace rtengine::stage
