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
 * Specification test for the typed raw/core work order.
 *
 * The test proves the contract of stage_work.h without running image
 * processing:
 *
 * - the bitset is a value with exact membership, count and a stable name;
 * - a stable pass derives an empty order;
 * - one parameter change per feeding block names exactly the jobs that block
 *   invalidates, and the crop names none because its destructive bodies are
 *   gated by the reload request only;
 * - a source reload, a high-detail decision change and each RequestKind name
 *   the documented sets;
 * - the spot rules distinguish a pending request, a live buffer and the crop's
 *   pointer-swap restore;
 * - the snapshot facts that no raw job reads (viewport, guides, display) do
 *   not move the order.
 *
 * Every expectation is an exact set, so an over- or under-approximation fails
 * the test instead of being hidden behind a non-empty check.
 */

#include "stage_work.h"

#include "rtengine/procparams.h"
#include "rtengine/refreshmap.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>

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

WorkOrder orderOf(std::initializer_list<WorkItem> items)
{
    WorkOrder order;
    for (WorkItem item : items) {
        order.set(item);
    }
    return order;
}

void expectOrder(const WorkOrder& actual,
                 std::initializer_list<WorkItem> expected,
                 const std::string& message)
{
    const WorkOrder wanted = orderOf(expected);
    if (actual == wanted) {
        check(true, message);
        return;
    }

    check(false, message + ": expected " + workOrderName(wanted)
                     + ", derived " + workOrderName(actual));
}

/*
 * The legacy gate answers for the raw/core items of one edit, read from the
 * refreshmap action the GUI sends. This is the oracle the typed order must
 * never exceed: `typedOnly` is exactly the set of items the typed order names
 * that this function does not set.
 *
 * Only the raw/core items are modelled. Spot state is carried in the render
 * key rather than in the action, so spot cases keep their explicit checks in
 * testSpotTransitions().
 */
WorkOrder legacyPreviewRawOrder(const procparams::ProcParams& params, int mask)
{
    WorkOrder order;

    /*
     * The caller folds a white-balance edit on the Color/Coloropp path into
     * DEMOSAIC before the pass runs (improccoordinator.cc:4610-4618). The
     * oracle must model that augmentation, or the demosaic the typed order
     * names for the opposed recovery would look like a typed-only run.
     */
    if ((mask & M_WB) != 0 && params.toneCurve.hrenabled
        && params.toneCurve.method == "Coloropp") {
        mask |= DEMOSAIC;
    }

    const bool preprocess = (mask & M_PREPROC) != 0;
    const bool demosaic = (mask & M_RAW) != 0
        || ((mask & M_CSHARP) != 0 && params.pdsharpening.enabled);
    const bool base = (mask & (M_INIT | M_LINDENOISE | M_HDR)) != 0;
    const bool spotConfigured =
        params.spot.enabled && !params.spot.entries.empty();
    const bool spotApply = (mask & M_SPOT) != 0 && spotConfigured;
    const bool retinex = ((mask & (M_RETINEX | M_INIT)) != 0)
        && params.retinex.enabled;

    if (preprocess) {
        order.set(WorkItem::sourceSelect);
        order.set(WorkItem::rawPreprocess);
    }
    if (demosaic) {
        order.set(WorkItem::rawDemosaic);
    }
    if (base) {
        order.set(WorkItem::rawBaseRebuild);
        order.set(WorkItem::analysisRun);
    }
    if (spotApply) {
        order.set(WorkItem::spotApply);
        /* Without a base rebuild the spots are applied to the buffer and
         * copied back; a base rebuild applies them directly. */
        if (!base) {
            order.set(WorkItem::spotRestore);
        }
    }
    if (retinex) {
        order.set(WorkItem::retinexPrepare);
    }

    return order;
}

void expectNoTypedOnly(const WorkOrder& typed, const WorkOrder& legacy,
                       const std::string& message)
{
    WorkOrder extra;
    for (size_t i = 0; i < workItemCount; ++i) {
        const WorkItem item = static_cast<WorkItem>(i);
        if (typed.has(item) && !legacy.has(item)) {
            extra.set(item);
        }
    }
    check(extra.empty(), message + ": typedOnly " + workOrderName(extra)
                             + " against legacy " + workOrderName(legacy)
                             + ", derived " + workOrderName(typed));
}


/*
 * One derivation under test.
 *
 * The fixture owns the fingerprints and the keys because WorkInputs borrows
 * them; a caller that let them die before the derivation would dangle.
 */
struct Derivation {
    procparams::ProcParams previousParams;
    procparams::ProcParams currentParams;
    BlockFingerprints previousFingerprints;
    BlockFingerprints currentFingerprints;
    RenderStateKey previousState;
    RenderStateKey currentState;
    WorkInputs inputs;

    Derivation(StagePath path,
               const procparams::ProcParams& previousParamsIn,
               procparams::ProcParams currentParamsIn,
               RenderStateKey previousStateIn,
               RenderStateKey currentStateIn,
               RequestKind kind)
        : previousParams(previousParamsIn),
          currentParams(std::move(currentParamsIn)),
          previousFingerprints(stage_fingerprints(path, previousParams)),
          currentFingerprints(stage_fingerprints(path, currentParams)),
          previousState(std::move(previousStateIn)),
          currentState(std::move(currentStateIn))
    {
        previousState.requestKind = RequestKind::ParamsOnly;
        currentState.requestKind = kind;
        inputs.previousState = &previousState;
        inputs.currentState = &currentState;
        inputs.previousFingerprints = &previousFingerprints;
        inputs.currentFingerprints = &currentFingerprints;
        inputs.previousParams = &this->previousParams;
        inputs.requestKind = kind;
        inputs.path = path;
        inputs.params = &this->currentParams;
    }

    WorkOrder preview() const
    {
        return derive_preview_work(inputs);
    }

    WorkOrder crop() const
    {
        return derive_crop_work(inputs);
    }
};

Derivation stablePreview(RequestKind kind = RequestKind::ParamsOnly)
{
    const procparams::ProcParams params;
    return Derivation(StagePath::Preview, params, params, RenderStateKey{},
                      RenderStateKey{}, kind);
}

Derivation stableCrop(RequestKind kind = RequestKind::ParamsOnly)
{
    const procparams::ProcParams params;
    return Derivation(StagePath::Crop, params, params, RenderStateKey{},
                      RenderStateKey{}, kind);
}

void testBitset()
{
    WorkOrder order;
    check(order.empty(), "bitset: a default order is empty");
    check(order.size() == workItemCount, "bitset: size is the universe");
    check(order.count() == 0, "bitset: count of a default order is zero");

    order.set(WorkItem::rawBaseRebuild);
    order.set(WorkItem::spotRestore);
    order.set(WorkItem::analysisRun);
    check(order.has(WorkItem::rawBaseRebuild), "bitset: set item is present");
    check(!order.has(WorkItem::rawDemosaic),
          "bitset: an unset item is absent");
    check(order.count() == 3, "bitset: count of three items is three");
    check(!order.empty(), "bitset: a set order is not empty");

    const WorkOrder copy = order;
    check(copy == order, "bitset: a copy compares equal");
    check(workOrderName(order)
              == "[rawBaseRebuild spotRestore analysisRun]",
          "bitset: the diagnostic name is stable and ordered");

    order.clear(WorkItem::spotRestore);
    check(!order.has(WorkItem::spotRestore), "bitset: clear removes an item");
    check(copy != order, "bitset: a cleared copy differs");
    check(workOrderName(WorkOrder{}) == "[]",
          "bitset: an empty order renders as []");

    check(std::string(workItemName(WorkItem::denoiseRun)) == "denoiseRun",
          "bitset: item names are stable");
    check(std::string(workItemName(WorkItem::Count)) == "unknown",
          "bitset: the sentinel has no item name");

    check(static_cast<int>(WorkItem::sourceSelect) == 0
              && static_cast<int>(WorkItem::Count) == 9,
          "bitset: the item order is part of the log contract");
}

/*
 * The conversion switch. The exact order names only the derived items; the
 * conservative order (kill switch off) names every parameter-applicable item.
 * The default is on; the environment variable is the kill switch.
 */
void testWorkOrderSwitch()
{
    WorkOrder order;
    order.set(WorkItem::rawDemosaic);

    check(workItemRuns(order, WorkItem::rawDemosaic),
          "switch: a typed item runs its body");
    check(!workItemRuns(order, WorkItem::rawBaseRebuild),
          "switch: an unset typed item skips its body");
    check(!workItemRuns(WorkOrder{}, WorkItem::rawDemosaic),
          "switch: an empty order runs nothing");

    /* Unset is the default and enables the typed order. */
    check(workOrderValueEnables(nullptr),
          "switch: unset RT_WORK_ORDER keeps the typed order enabled");
    /* Any other value enables it, including an explicitly empty value. */
    check(workOrderValueEnables("1"), "switch: 1 enables");
    check(workOrderValueEnables("yes"), "switch: yes enables");
    check(workOrderValueEnables(""), "switch: an empty value enables");
    check(workOrderValueEnables("offf"),
          "switch: only the literal off disables");
    /* The two documented kill values disable it, case-insensitively. */
    check(!workOrderValueEnables("0"), "switch: 0 disables");
    check(!workOrderValueEnables("off"), "switch: off disables");
    check(!workOrderValueEnables("OFF"), "switch: OFF disables");
}

/*
 * The kill switch derives with no usable history, so every
 * parameter-applicable raw/core item is named. This is the conservative
 * fallback that replaces the legacy mask: safe and slower, never empty.
 */
void testConservativeFallback()
{
    procparams::ProcParams params;
    params.retinex.enabled = true;
    params.dirpyrDenoise.enabled = true;
    params.spot.enabled = true;
    params.spot.entries.emplace_back();

    RenderStateKey state;
    state.cropSkip = 1;
    state.spotBufferLive = true;

    Derivation preview(StagePath::Preview, params, params, state, state,
                       RequestKind::HighQuality);
    preview.inputs.previousState = nullptr;
    preview.inputs.previousFingerprints = nullptr;
    preview.inputs.previousParams = nullptr;
    expectOrder(preview.preview(),
                {WorkItem::sourceSelect, WorkItem::rawPreprocess,
                 WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::retinexPrepare, WorkItem::spotApply,
                 WorkItem::spotRestore, WorkItem::analysisRun},
                "conservative preview: every raw-core item");

    Derivation crop(StagePath::Crop, params, params, state, state,
                    RequestKind::HighQuality);
    crop.inputs.previousState = nullptr;
    crop.inputs.previousFingerprints = nullptr;
    crop.inputs.previousParams = nullptr;
    expectOrder(crop.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::spotApply, WorkItem::spotRestore,
                 WorkItem::analysisRun, WorkItem::denoiseRun},
                "conservative crop: every crop raw item");

    /*
     * The buffer clear is covered by spotRestore: a live buffer with the tool
     * disabled is still named, so the site can clear it without the mask.
     */
    procparams::ProcParams disabled;
    RenderStateKey live;
    live.spotBufferLive = true;
    Derivation clear(StagePath::Crop, disabled, disabled, live, live,
                     RequestKind::ParamsOnly);
    expectOrder(clear.crop(), {WorkItem::spotRestore},
                "conservative crop: a live buffer is named for the clear");
}

void testNoChange()
{
    expectOrder(stablePreview().preview(), {}, "stable preview: no work");
    expectOrder(stableCrop().crop(), {}, "stable crop: no work");

    expectOrder(stablePreview(RequestKind::RepaintOnly).preview(), {},
                "repaint-only preview: no destructive work");
    expectOrder(stableCrop(RequestKind::RepaintOnly).crop(), {},
                "repaint-only crop: no destructive work");
}

/*
 * One parameter change per feeding block.
 *
 * The preview expectation is the exact job set. The crop expectation is empty
 * for every row: the crop's destructive bodies are gated by the reload request
 * only, so a parameter delta alone schedules nothing there in this step.
 */
void testParameterChanges()
{
    using Mutate = void (*)(procparams::ProcParams&);
    struct ParamCase {
        const char* name;
        Mutate mutate;
        std::initializer_list<WorkItem> previewExpected;
        /* The crop stage bodies still read the spot families without a reload. */
        std::initializer_list<WorkItem> cropExpected = {};
        /* The refreshmap action the editor sends for this edit. */
        int mask = 0;
        /* False for facts the raw/core oracle does not model (spot state). */
        bool useOracle = true;
    };

    const ParamCase cases[] = {
        {"white balance (raw.base)",
         [](procparams::ProcParams& params) {
             params.wb.temperature += 500;
         },
         {WorkItem::rawBaseRebuild, WorkItem::analysisRun}, {}, WB},
        {"raw sensor level (raw.preprocess, raw.demosaic, raw.base)",
         [](procparams::ProcParams& params) {
             params.raw.bayersensor.black0 += 1.0;
         },
         {WorkItem::sourceSelect, WorkItem::rawPreprocess,
          WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::analysisRun}, {}, DARKFRAME},
        {"capture sharpening enabled (raw.demosaic)",
         [](procparams::ProcParams& params) {
             params.pdsharpening.enabled = true;
             params.pdsharpening.contrast += 0.1;
         },
         {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::analysisRun}, {}, CAPTURESHARPEN},
        {"capture sharpening disabled (no demosaic body)",
         [](procparams::ProcParams& params) {
             params.pdsharpening.contrast += 0.1;
         },
         {}, {}, CAPTURESHARPEN},
        {"retinex (raw.demosaic buffers, raw.base, enabled)",
         [](procparams::ProcParams& params) {
             params.retinex.enabled = true;
         },
         {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::retinexPrepare, WorkItem::analysisRun}, {}, DEMOSAIC},
        {"lens profile (raw.preprocess, forward edge to demosaic)",
         [](procparams::ProcParams& params) {
             params.lensProf.lcMode =
                 procparams::LensProfParams::LcMode::LENSFUNMANUAL;
         },
         {WorkItem::sourceSelect, WorkItem::rawPreprocess,
          WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::analysisRun}, {}, DARKFRAME},
        {"lens CA flag (transform, not preprocess)",
         [](procparams::ProcParams& params) {
             params.lensProf.useCA = true;
         },
         {}, {}, HDR},
        {"colour management (raw.base, analysis)",
         [](procparams::ProcParams& params) {
             params.icm.workingProfile = "ProPhoto";
         },
         {WorkItem::rawBaseRebuild, WorkItem::analysisRun}, {}, DEMOSAIC},
        {"denoise strength (raw.base, analysis)",
         [](procparams::ProcParams& params) {
             params.dirpyrDenoise.luma += 5;
         },
         {WorkItem::rawBaseRebuild, WorkItem::analysisRun}, {}, ALLNORAW},
        {"demosaic method (raw.demosaic, raw.base)",
         [](procparams::ProcParams& params) {
             params.raw.bayersensor.method = "RCD";
         },
         {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::analysisRun}, {}, DEMOSAIC},
        {"coarse rotate (transform, not preprocess)",
         [](procparams::ProcParams& params) {
             params.coarse.rotate = 90;
         },
         {}, {}, ALLNORAW},
        {"fine rotate (transform, not preprocess)",
         [](procparams::ProcParams& params) {
             params.rotate.degree = 1.5;
         },
         {}, {}, HDR},
        {"spot entry (spot buffer, no base rebuild)",
         [](procparams::ProcParams& params) {
             params.spot.enabled = true;
             params.spot.entries.emplace_back();
         },
         {WorkItem::spotApply, WorkItem::spotRestore},
         {WorkItem::spotApply, WorkItem::spotRestore}, SPOTADJUST, false},
        {"dehaze (transform, not raw.base)",
         [](procparams::ProcParams& params) {
             params.dehaze.enabled = true;
         },
         {}},
        {"output profile (monitor sink, not raw.base)",
         [](procparams::ProcParams& params) {
             params.icm.outputProfile = "sRGB";
         },
         {}},
        {"retinex disabled (no preparation)",
         [](procparams::ProcParams& params) {
             params.retinex.enabled = false;
             params.retinex.str += 5;
         },
         {WorkItem::rawBaseRebuild, WorkItem::analysisRun}, {}, RETINEX},
    };

    for (const ParamCase& testCase : cases) {
        const procparams::ProcParams previous;
        procparams::ProcParams current = previous;
        testCase.mutate(current);

        const Derivation preview(StagePath::Preview, previous, current,
                                 RenderStateKey{}, RenderStateKey{},
                                 RequestKind::ParamsOnly);
        const WorkOrder previewOrder = preview.preview();
        expectOrder(previewOrder, testCase.previewExpected,
                    std::string("parameter ") + testCase.name);
        if (testCase.useOracle) {
            expectNoTypedOnly(
                previewOrder, legacyPreviewRawOrder(current, testCase.mask),
                std::string("parameter ") + testCase.name);
        }

        const Derivation crop(StagePath::Crop, previous, current,
                              RenderStateKey{}, RenderStateKey{},
                              RequestKind::ParamsOnly);
        expectOrder(crop.crop(), testCase.cropExpected,
                    std::string("parameter ") + testCase.name + " on crop");
    }
}

/*
 * The request kind the coordinator derives from the refreshmap action. The
 * shadow comparison runs on the same folding, so an edit family must not name
 * raw work beyond the action it sends.
 */
RequestKind kindForAction(int mask)
{
    if ((mask & M_HIGHQUAL) != 0) {
        return RequestKind::HighQuality;
    }
    if ((mask & (M_INIT | M_LINDENOISE | M_HDR)) != 0) {
        return RequestKind::FullPreview;
    }
    return RequestKind::ParamsOnly;
}

/*
 * Each edit family with the request kind its editor action produces. This is
 * the end-to-end predicate check: the typed order may be smaller than the
 * legacy raw set where the mask over-approximates, but it must never name an
 * item the mask does not run.
 */
void testEditFamiliesAgainstLegacyOracle()
{
    using Mutate = void (*)(procparams::ProcParams&);
    struct Edit {
        const char* name;
        Mutate mutate;
        int mask;
        /* Whether the typed raw set is expected to equal the legacy one. */
        bool exact;
    };

    const Edit edits[] = {
        {"white balance", [](procparams::ProcParams& params) {
             params.wb.temperature += 500;
         }, WB, true},
        {"demosaic method", [](procparams::ProcParams& params) {
             params.raw.bayersensor.method = "RCD";
         }, DEMOSAIC, true},
        {"denoise", [](procparams::ProcParams& params) {
             params.dirpyrDenoise.luma += 5;
         }, ALLNORAW, true},
        {"coarse rotate", [](procparams::ProcParams& params) {
             params.coarse.rotate = 90;
         }, ALLNORAW, true},
        {"fine rotate", [](procparams::ProcParams& params) {
             params.rotate.degree = 1.5;
         }, HDR, true},
        {"capture sharpening", [](procparams::ProcParams& params) {
             params.pdsharpening.enabled = true;
             params.pdsharpening.contrast += 0.1;
         }, CAPTURESHARPEN, true},
        {"lens profile", [](procparams::ProcParams& params) {
             params.lensProf.lcMode =
                 procparams::LensProfParams::LcMode::LENSFUNMANUAL;
         }, DARKFRAME, true},
        {"retinex", [](procparams::ProcParams& params) {
             params.retinex.enabled = true;
         }, DEMOSAIC, true},
        {"colour management", [](procparams::ProcParams& params) {
             params.icm.workingProfile = "ProPhoto";
         }, DEMOSAIC, false}
    };

    for (const Edit& edit : edits) {
        const procparams::ProcParams previous;
        procparams::ProcParams current = previous;
        edit.mutate(current);

        const Derivation preview(StagePath::Preview, previous, current,
                                 RenderStateKey{}, RenderStateKey{},
                                 kindForAction(edit.mask));
        const WorkOrder typed = preview.preview();
        const WorkOrder legacy = legacyPreviewRawOrder(current, edit.mask);

        expectNoTypedOnly(typed, legacy,
                          std::string("action ") + edit.name);
        if (edit.exact) {
            check(typed == legacy,
                  std::string("action ") + edit.name +
                      ": typed order equals the legacy raw set: expected " +
                      workOrderName(legacy) + ", derived " +
                      workOrderName(typed));
        }
    }
}

void testSourceReload()
{
    RenderStateKey previousState;
    previousState.sourceGeneration = 4;
    RenderStateKey currentState;
    currentState.sourceGeneration = 7;

    const procparams::ProcParams params;

    const Derivation preview(StagePath::Preview, params, params, previousState,
                             currentState, RequestKind::ParamsOnly);
    expectOrder(preview.preview(),
                {WorkItem::sourceSelect, WorkItem::rawPreprocess,
                 WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "source reload on preview");

    /*
     * The crop gates are mask-only. A source generation bump reaches the crop
     * through M_INIT (RequestKind::FullCrop), not through a parameter delta,
     * so the work order is empty when the request does not carry it. The
     * dirty-block derivation still models the Source aspect separately.
     */
    const Derivation crop(StagePath::Crop, params, params, previousState,
                          currentState, RequestKind::ParamsOnly);
    expectOrder(crop.crop(), {}, "source reload without a crop reload request");

    /* A bare RGB modification is not a reload: only the demosaic reads it. */
    RenderStateKey modified;
    modified.sourceGeneration = previousState.sourceGeneration;
    modified.rgbSourceModified = true;

    procparams::ProcParams matched;
    matched.toneCurve.hrenabled = true;
    matched.toneCurve.method = "Color";
    const Derivation hlr(StagePath::Preview, matched, matched, previousState,
                         modified, RequestKind::ParamsOnly);
    expectOrder(hlr.preview(), {},
                "RGB modification with a matching HLR method: no work");

    /* A mismatching HLR method re-demosaices and therefore rebuilds the base. */
    procparams::ProcParams mismatched;
    mismatched.toneCurve.hrenabled = true;
    mismatched.toneCurve.method = "Standard";
    const Derivation hlrMismatch(StagePath::Preview, mismatched, mismatched,
                                 previousState, modified,
                                 RequestKind::ParamsOnly);
    expectOrder(hlrMismatch.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "RGB modification with a mismatching HLR method demosaics");

    /*
     * A white-balance change on the Coloropp path demosaics in the legacy
     * sequence through the caller's DEMOSAIC augmentation. The typed order
     * names the same body: getImage() applies the opposed recovery with the
     * current white-balance multipliers, so only fresh demosaic planes let the
     * new multipliers take effect. This is pixel-affecting, not a saving.
     */
    procparams::ProcParams coloropp;
    coloropp.toneCurve.hrenabled = true;
    coloropp.toneCurve.method = "Coloropp";
    procparams::ProcParams coloroppWb = coloropp;
    coloroppWb.wb.temperature += 500;

    Derivation wbHlr(StagePath::Preview, coloropp, coloroppWb,
                     previousState, previousState,
                     RequestKind::ParamsOnly);
    wbHlr.inputs.colorHlrWbRequest = true;
    expectOrder(wbHlr.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "white balance on the Coloropp path demosaics for the recovery");
}

/*
 * The predicates the conversion added, and the legacy-only remainders they
 * leave. A white-balance edit on the Coloropp recovery path and a recovery
 * configuration edit on recovered planes are pixel-affecting and named. A
 * working-profile edit while retinex is enabled is an input of both the
 * retinex preparation and the buffer preparation inside the demosaic body, so
 * both are named. While retinex is disabled the demosaic body remains
 * legacy-only redundant work; a white-balance-only M_INIT is not an input of
 * the preparation either.
 */
void testRecoveryAndWorkingProfile()
{
    using procparams::ProcParams;

    /* Coloropp recovery plus a white-balance request needs fresh planes. */
    ProcParams coloropp;
    coloropp.toneCurve.hrenabled = true;
    coloropp.toneCurve.method = "Coloropp";
    ProcParams coloroppWb = coloropp;
    coloroppWb.wb.temperature += 500;

    Derivation wb(StagePath::Preview, coloropp, coloroppWb,
                  RenderStateKey{}, RenderStateKey{},
                  RequestKind::ParamsOnly);
    wb.inputs.colorHlrWbRequest = true;
    expectOrder(wb.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "Coloropp white balance demosaics for the opposed recovery");
    expectNoTypedOnly(wb.preview(), legacyPreviewRawOrder(coloroppWb, WB),
                      "Coloropp white balance against the augmented legacy mask");

    /*
     * The same parameter delta in a pass whose mask does not carry the
     * request bit does not demosaic: the legacy augmentation does not run
     * either, and getImage() would apply the recovery to the current planes.
     */
    const Derivation bareWb(StagePath::Preview, coloropp, coloroppWb,
                            RenderStateKey{}, RenderStateKey{},
                            RequestKind::ParamsOnly);
    expectOrder(bareWb.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::analysisRun},
                "a bare white-balance delta does not demosaic");
    expectNoTypedOnly(bareWb.preview(),
                      legacyPreviewRawOrder(coloroppWb, ALLNORAW),
                      "a bare white-balance delta against the legacy mask");

    /* The same edit with recovery disabled does not touch the planes. */
    ProcParams plainWb;
    plainWb.wb.temperature += 500;
    const Derivation plain(StagePath::Preview, ProcParams{}, plainWb,
                           RenderStateKey{}, RenderStateKey{},
                           RequestKind::ParamsOnly);
    expectOrder(plain.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::analysisRun},
                "white balance without recovery rebuilds the base only");

    /*
     * A recovery configuration change is only able to take effect on fresh
     * planes once the current planes carry recovery output.
     */
    RenderStateKey recovered;
    recovered.rgbSourceModified = true;
    ProcParams color;
    color.toneCurve.hrenabled = true;
    color.toneCurve.method = "Color";
    ProcParams coloroppMethod = color;
    coloroppMethod.toneCurve.method = "Coloropp";

    const Derivation config(StagePath::Preview, color, coloroppMethod,
                            recovered, recovered, RequestKind::ParamsOnly);
    expectOrder(config.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "recovery method change on recovered planes demosaics");
    expectNoTypedOnly(config.preview(),
                      legacyPreviewRawOrder(coloroppMethod, DEMOSAIC),
                      "recovery method change against the legacy mask");

    /*
     * Without recovery output the demosaic body is redundant: getImage()
     * applies the new setting to the current planes.
     */
    const Derivation idle(StagePath::Preview, color, coloroppMethod,
                          RenderStateKey{}, RenderStateKey{},
                          RequestKind::ParamsOnly);
    expectOrder(idle.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::analysisRun},
                "recovery method change on plain planes rebuilds the base only");
    expectNoTypedOnly(idle.preview(),
                      legacyPreviewRawOrder(coloroppMethod, DEMOSAIC),
                      "plain-plane recovery change against the legacy mask");

    /*
     * The working profile is an input of the retinex preparation, which
     * converts the planes with the working-space matrix and back.
     */
    ProcParams retinex;
    retinex.retinex.enabled = true;
    ProcParams retinexProfile = retinex;
    retinexProfile.icm.workingProfile = "ProPhoto";

    const Derivation profile(StagePath::Preview, retinex, retinexProfile,
                             RenderStateKey{}, RenderStateKey{},
                             RequestKind::ParamsOnly);
    expectOrder(profile.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::retinexPrepare, WorkItem::analysisRun},
                "working profile reprepares the retinex buffers");
    expectNoTypedOnly(profile.preview(),
                      legacyPreviewRawOrder(retinexProfile, DEMOSAIC),
                      "working profile against the legacy mask");

    /* Without retinex the working profile names no preparation. */
    ProcParams plainProfile;
    plainProfile.icm.workingProfile = "ProPhoto";
    const Derivation noRetinex(StagePath::Preview, ProcParams{}, plainProfile,
                               RenderStateKey{}, RenderStateKey{},
                               RequestKind::ParamsOnly);
    expectOrder(noRetinex.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::analysisRun},
                "working profile without retinex rebuilds the base only");

    /*
     * A white-balance-only M_INIT does not move the planes or read the white
     * balance in the preparation, so the legacy-only preparation is redundant.
     */
    ProcParams retinexWb = retinex;
    retinexWb.wb.temperature += 500;
    const Derivation wbRetinex(StagePath::Preview, retinex, retinexWb,
                               RenderStateKey{}, RenderStateKey{},
                               RequestKind::ParamsOnly);
    expectOrder(wbRetinex.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::analysisRun},
                "white balance does not reprepare the retinex buffers");
    expectNoTypedOnly(wbRetinex.preview(),
                      legacyPreviewRawOrder(retinexWb, WB),
                      "white balance with retinex against the legacy mask");
}

void testHighDetailTransitions()
{
    RenderStateKey previousState;
    previousState.highDetailNeeded = false;
    previousState.highDetailPreprocessComputed = true;
    previousState.highDetailRawComputed = true;
    previousState.highDetailStateKnown = true;

    RenderStateKey needed;
    needed.highDetailNeeded = true;
    needed.highDetailPreprocessComputed = false;
    needed.highDetailRawComputed = false;
    needed.highDetailStateKnown = true;

    const procparams::ProcParams params;
    const std::initializer_list<WorkItem> rawCore = {
        WorkItem::sourceSelect, WorkItem::rawPreprocess,
        WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
        WorkItem::analysisRun};

    const Derivation gained(StagePath::Preview, params, params, previousState,
                            needed, RequestKind::ParamsOnly);
    expectOrder(gained.preview(), rawCore,
                "high detail gained: the raw core runs");

    /*
     * The crop gates are mask-only: the high-detail transition reaches a crop
     * as a mask request, not as a parameter delta, so a ParamsOnly update
     * names no crop item even though the render-state aspect moved.
     */
    const Derivation cropGained(StagePath::Crop, params, params,
                                previousState, needed,
                                RequestKind::ParamsOnly);
    expectOrder(cropGained.crop(), {},
                "high detail gained without a crop reload request");

    /*
     * The other direction has both flags already computed, so no pending
     * high-detail work remains. The decision still changes the demosaic mode:
     * the fit pass substitutes the FAST method, so the body must re-demosaic
     * even though no parameter moved. Source selection and preprocessing are
     * unchanged and stay legacy-only redundant work.
     */
    RenderStateKey dropped;
    dropped.highDetailNeeded = false;
    dropped.highDetailPreprocessComputed = true;
    dropped.highDetailRawComputed = true;
    dropped.highDetailStateKnown = true;
    const Derivation lost(StagePath::Preview, params, params, needed, dropped,
                          RequestKind::ParamsOnly);
    expectOrder(lost.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "high detail dropped: the demosaic mode changes to FAST");
}

void testSpotTransitions()
{
    const procparams::ProcParams params;

    /*
     * A pending request alone has no base delta, so the spots are applied to
     * the buffer path and copied back. The realistic base transition carries
     * the request fact and applies them in the rebuild branch instead.
     */
    RenderStateKey previousState;
    RenderStateKey pendingState;
    pendingState.spotPending = true;

    const Derivation pending(StagePath::Preview, params, params, previousState,
                             pendingState, RequestKind::ParamsOnly);
    expectOrder(pending.preview(),
                {WorkItem::spotApply, WorkItem::spotRestore},
                "spot pending without a delta: buffer apply and restore");

    const Derivation cropPending(StagePath::Crop, params, params,
                                 previousState, pendingState,
                                 RequestKind::ParamsOnly);
    expectOrder(cropPending.crop(),
                {WorkItem::spotApply, WorkItem::spotRestore},
                "spot pending on crop without a reload: buffer apply and swap");

    /*
     * The standing rule: a live buffer is copied back even with no delta. The
     * tool must still be configured, because a disabled or empty tool deletes
     * the buffer instead of restoring it.
     */
    procparams::ProcParams liveParams = params;
    liveParams.spot.enabled = true;
    liveParams.spot.entries.emplace_back();
    RenderStateKey liveState;
    liveState.spotBufferLive = true;

    const Derivation live(StagePath::Preview, liveParams, liveParams,
                          previousState, liveState, RequestKind::ParamsOnly);
    expectOrder(live.preview(), {WorkItem::spotRestore},
                "live spot buffer restores without rebuilding the base");

    /*
     * The crop restores by swapping baseCrop at dcrop.cc:1600, so the live
     * buffer names the restore and not the destructive body.
     */
    const Derivation cropLive(StagePath::Crop, liveParams, liveParams,
                              previousState, liveState,
                              RequestKind::ParamsOnly);
    expectOrder(cropLive.crop(), {WorkItem::spotRestore},
                "live spot buffer on crop: pointer-swap restore only");

    /* A carried spot delta plus the pending fact applies the spots. */
    procparams::ProcParams spotted = params;
    spotted.spot.enabled = true;
    spotted.spot.entries.emplace_back();
    const Derivation applied(StagePath::Preview, params, spotted,
                             previousState, pendingState,
                             RequestKind::ParamsOnly);
    expectOrder(applied.preview(),
                {WorkItem::spotApply, WorkItem::spotRestore},
                "spot delta with a pending request applies and restores");

    /* A cleared buffer and a cleared request need no spot work. */
    RenderStateKey cleared;
    const Derivation clearedOrder(StagePath::Preview, params, params,
                                  liveState, cleared,
                                  RequestKind::ParamsOnly);
    expectOrder(clearedOrder.preview(), {},
                "cleared spot buffer and request: no spot work");
}

void testRequestKinds()
{
    const procparams::ProcParams params;
    /*
     * A rebuild request names the destructive base body. It does not select,
     * preprocess or demosaic the source: RequestKind folds M_INIT,
     * M_LINDENOISE and M_HDR together and only those bits reach the base.
     */
    const std::initializer_list<WorkItem> previewRebuild = {
        WorkItem::rawBaseRebuild, WorkItem::analysisRun};
    const std::initializer_list<WorkItem> cropRebuild = {
        WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
        WorkItem::analysisRun};

    const Derivation fullPreview(StagePath::Preview, params, params,
                                 RenderStateKey{}, RenderStateKey{},
                                 RequestKind::FullPreview);
    expectOrder(fullPreview.preview(), previewRebuild,
                "FullPreview: the destructive raw set");

    const Derivation fullCrop(StagePath::Crop, params, params,
                              RenderStateKey{}, RenderStateKey{},
                              RequestKind::FullCrop);
    expectOrder(fullCrop.crop(), cropRebuild, "FullCrop: the crop reload set");

    /*
     * M_HIGHQUAL asks for the accurate chain but is not part of the legacy
     * raw reload gate (M_INIT | M_LINDENOISE | M_HDR). A HighQuality-only
     * request therefore names no destructive raw body on either path.
     */
    const Derivation highQualityPreview(StagePath::Preview, params, params,
                                        RenderStateKey{}, RenderStateKey{},
                                        RequestKind::HighQuality);
    expectOrder(highQualityPreview.preview(), {},
                "HighQuality-only on preview: no raw rebuild");

    const Derivation highQualityCrop(StagePath::Crop, params, params,
                                     RenderStateKey{}, RenderStateKey{},
                                     RequestKind::HighQuality);
    expectOrder(highQualityCrop.crop(), {},
                "HighQuality-only on crop: no raw reload");

    /* A full preview request also forces the raw set on the other path. */
    const Derivation previewOnCropPath(StagePath::Crop, params, params,
                                       RenderStateKey{}, RenderStateKey{},
                                       RequestKind::FullPreview);
    expectOrder(previewOnCropPath.crop(), cropRebuild,
                "FullPreview carried on the crop path rebuilds the crop");

    /* A full crop request on the preview is still an absolute rebuild. */
    const Derivation cropOnPreviewPath(StagePath::Preview, params, params,
                                       RenderStateKey{}, RenderStateKey{},
                                       RequestKind::FullCrop);
    expectOrder(cropOnPreviewPath.preview(), previewRebuild,
                "FullCrop carried on the preview path rebuilds the raw set");
}

void testCropDenoise()
{
    procparams::ProcParams denoised;
    denoised.dirpyrDenoise.enabled = true;

    RenderStateKey fullFrame;
    fullFrame.cropSkip = 1;
    RenderStateKey reduced;
    reduced.cropSkip = 2;

    const Derivation atFullResolution(StagePath::Crop, denoised, denoised,
                                      fullFrame, fullFrame,
                                      RequestKind::FullCrop);
    expectOrder(atFullResolution.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun, WorkItem::denoiseRun},
                "crop denoise runs at skip 1");

    const Derivation atReducedResolution(StagePath::Crop, denoised, denoised,
                                         reduced, reduced,
                                         RequestKind::FullCrop);
    expectOrder(atReducedResolution.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "crop denoise does not run above skip 1");

    const Derivation disabled(StagePath::Crop, procparams::ProcParams{},
                              procparams::ProcParams{}, fullFrame, fullFrame,
                              RequestKind::FullCrop);
    expectOrder(disabled.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "disabled crop denoise is not scheduled");
}

void testNoHistory()
{
    const procparams::ProcParams params;

    Derivation preview(StagePath::Preview, params, params, RenderStateKey{},
                       RenderStateKey{}, RequestKind::ParamsOnly);
    preview.inputs.previousState = nullptr;
    preview.inputs.previousFingerprints = nullptr;
    expectOrder(preview.preview(),
                {WorkItem::sourceSelect, WorkItem::rawPreprocess,
                 WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "first preview pass: every raw-core item");

    Derivation crop(StagePath::Crop, params, params, RenderStateKey{},
                    RenderStateKey{}, RequestKind::ParamsOnly);
    crop.inputs.previousState = nullptr;
    crop.inputs.previousFingerprints = nullptr;
    expectOrder(crop.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "first crop pass: every crop raw item");
}

/*
 * A missing previous parameter set is allowed: the family comparisons answer
 * "changed". The pdsharpening split must not mistake that for a
 * pdsharpening-only edit and hide a sensor demosaic delta.
 */
void testMissingPreviousParams()
{
    const procparams::ProcParams previous;
    procparams::ProcParams current = previous;
    current.raw.bayersensor.method = "RCD";

    Derivation preview(StagePath::Preview, previous, current, RenderStateKey{},
                       RenderStateKey{}, RequestKind::ParamsOnly);
    preview.inputs.previousParams = nullptr;
    expectOrder(preview.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "missing previous params: demosaic delta stays named");
}

/*
 * A null parameter set is a violated precondition. Debug builds assert (the
 * contract in stage_work.h); release builds must still fail closed to the
 * whole path-applicable set instead of deriving an empty order. This test
 * exercises the release contract.
 */
void testNullParamsFailClosed()
{
#ifdef NDEBUG
    const procparams::ProcParams params;

    Derivation preview(StagePath::Preview, params, params, RenderStateKey{},
                       RenderStateKey{}, RequestKind::ParamsOnly);
    preview.inputs.params = nullptr;
    expectOrder(preview.preview(),
                {WorkItem::sourceSelect, WorkItem::rawPreprocess,
                 WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::retinexPrepare, WorkItem::spotApply,
                 WorkItem::spotRestore, WorkItem::analysisRun},
                "null params preview: every path-applicable item");

    Derivation crop(StagePath::Crop, params, params, RenderStateKey{},
                    RenderStateKey{}, RequestKind::ParamsOnly);
    crop.inputs.params = nullptr;
    expectOrder(crop.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::spotApply, WorkItem::spotRestore,
                 WorkItem::analysisRun, WorkItem::denoiseRun},
                "null params crop: every path-applicable item");
#endif
}

/*
 * The facts that the derivation deliberately does not read. A change in the
 * display, the viewport or the overlay must not schedule a raw job.
 */
void testUnreadFacts()
{
    const procparams::ProcParams params;

    RenderStateKey previous;
    RenderStateKey current;
    current.monitorProfile = "sRGB";
    current.monitorIntent = 1;
    current.softProof = true;
    current.gamutCheck = true;
    current.viewportMoved = true;
    current.cropGuideFingerprint = 0x1234567890abcdefULL;
    current.deferredAnalysisMask = 0x400;
    current.previousDemoBase =
        static_cast<int32_t>(PreviousDemoBase::Sidecar);
    current.fastPreviewEnabled = true;
    current.mainPreviewFullResolution = true;
    current.initialRoughPreview = true;
    current.sharpMask = true;

    const Derivation preview(StagePath::Preview, params, params, previous,
                             current, RequestKind::ParamsOnly);
    expectOrder(preview.preview(), {},
                "display, viewport, guide and editor facts move no raw job");

    const Derivation crop(StagePath::Crop, params, params, previous, current,
                          RequestKind::ParamsOnly);
    expectOrder(crop.crop(), {},
                "display, viewport, guide and editor facts move no crop job");
}

/*
 * The two bodies that write pixels in place rather than only deciding output.
 *
 * Capture sharpening modifies the demosaiced planes, and retinex() writes the
 * raw planes. Enabling either is covered by the parameter cases; disabling
 * them is the direction that needs a fresh demosaic, because the previous
 * pass's output is still in the planes. The legacy DEMOSAIC action carries
 * M_RAW in both directions.
 */
void testInPlaceBodyTransitions()
{
    using procparams::ProcParams;

    ProcParams sharpening;
    sharpening.pdsharpening.enabled = true;
    sharpening.pdsharpening.contrast += 0.1;
    ProcParams sharpeningOff = sharpening;
    sharpeningOff.pdsharpening.enabled = false;

    const Derivation sharpeningDisable(StagePath::Preview, sharpening,
                                       sharpeningOff, RenderStateKey{},
                                       RenderStateKey{},
                                       RequestKind::FullPreview);
    expectOrder(sharpeningDisable.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "capture sharpening disabled: the demosaic body removes it");
    expectNoTypedOnly(sharpeningDisable.preview(),
                      legacyPreviewRawOrder(sharpeningOff, DEMOSAIC),
                      "capture sharpening disabled against the legacy mask");

    /* A disabled-tool parameter edit leaves the planes untouched. */
    ProcParams sharpeningIdle = sharpeningOff;
    sharpeningIdle.pdsharpening.contrast += 0.1;
    const Derivation idle(StagePath::Preview, sharpeningOff, sharpeningIdle,
                          RenderStateKey{}, RenderStateKey{},
                          RequestKind::FullPreview);
    expectOrder(idle.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::analysisRun},
                "capture sharpening idle edit: no demosaic body");
    expectNoTypedOnly(idle.preview(),
                      legacyPreviewRawOrder(sharpeningIdle, DEMOSAIC),
                      "capture sharpening idle edit against the legacy mask");

    ProcParams retinex;
    retinex.retinex.enabled = true;
    const ProcParams retinexOff;
    const Derivation retinexDisable(StagePath::Preview, retinex, retinexOff,
                                    RenderStateKey{}, RenderStateKey{},
                                    RequestKind::FullPreview);
    expectOrder(retinexDisable.preview(),
                {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "retinex disabled: the demosaic body discards its planes");
    expectNoTypedOnly(retinexDisable.preview(),
                      legacyPreviewRawOrder(retinexOff, DEMOSAIC),
                      "retinex disabled against the legacy mask");

    /* A strength edit carries M_RETINEX and reuses the prepared buffers. */
    ProcParams retinexStrength = retinex;
    retinexStrength.retinex.str += 5;
    const Derivation strength(StagePath::Preview, retinex, retinexStrength,
                              RenderStateKey{}, RenderStateKey{},
                              RequestKind::FullPreview);
    expectOrder(strength.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::retinexPrepare,
                 WorkItem::analysisRun},
                "retinex strength reuses the prepared buffers");
    expectNoTypedOnly(strength.preview(),
                      legacyPreviewRawOrder(retinexStrength, RETINEX),
                      "retinex strength against the legacy mask");
}

/* Every item is reachable from at least one documented case. */
void testCoverage()
{
    WorkOrder seen;
    const procparams::ProcParams params;

    procparams::ProcParams busy = params;
    busy.raw.bayersensor.black0 += 1.0;
    busy.retinex.enabled = true;
    busy.dirpyrDenoise.enabled = true;
    busy.spot.enabled = true;
    busy.spot.entries.emplace_back();
    RenderStateKey live;
    live.spotPending = true;
    live.spotBufferLive = true;
    live.cropSkip = 1;

    const Derivation preview(StagePath::Preview, params, busy,
                             RenderStateKey{}, live,
                             RequestKind::FullPreview);
    const WorkOrder previewOrder = preview.preview();
    for (size_t i = 0; i < workItemCount; ++i) {
        const WorkItem item = static_cast<WorkItem>(i);
        if (previewOrder.has(item)) {
            seen.set(item);
        }
    }

    const Derivation crop(StagePath::Crop, params, busy, RenderStateKey{},
                          live, RequestKind::FullCrop);
    const WorkOrder cropOrder = crop.crop();
    for (size_t i = 0; i < workItemCount; ++i) {
        const WorkItem item = static_cast<WorkItem>(i);
        if (cropOrder.has(item)) {
            seen.set(item);
        }
    }

    check(seen.count() == workItemCount,
          "coverage: every WorkItem appears in a case: "
              + workOrderName(seen));

    expectOrder(previewOrder,
                {WorkItem::sourceSelect, WorkItem::rawPreprocess,
                 WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                 WorkItem::retinexPrepare, WorkItem::spotApply,
                 WorkItem::spotRestore, WorkItem::analysisRun},
                "busy preview: the full destructive set");
    expectOrder(cropOrder,
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::spotApply, WorkItem::spotRestore,
                 WorkItem::analysisRun, WorkItem::denoiseRun},
                "busy crop: the full crop reload set");
}

} // namespace

int main()
{
    testBitset();
    testWorkOrderSwitch();
    testConservativeFallback();
    testNoChange();
    testParameterChanges();
    testEditFamiliesAgainstLegacyOracle();
    testSourceReload();
    testRecoveryAndWorkingProfile();
    testInPlaceBodyTransitions();
    testHighDetailTransitions();
    testSpotTransitions();
    testRequestKinds();
    testCropDenoise();
    testNoHistory();
    testMissingPreviousParams();
    testNullParamsFailClosed();
    testUnreadFacts();
    testCoverage();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("stage-work: all checks passed\n");
    return 0;
}
