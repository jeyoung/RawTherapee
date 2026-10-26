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
 * One derivation under test.
 *
 * The fixture owns the fingerprints and the keys because WorkInputs borrows
 * them; a caller that let them die before the derivation would dangle.
 */
struct Derivation {
    procparams::ProcParams currentParams;
    BlockFingerprints previousFingerprints;
    BlockFingerprints currentFingerprints;
    RenderStateKey previousState;
    RenderStateKey currentState;
    WorkInputs inputs;

    Derivation(StagePath path,
               const procparams::ProcParams& previousParams,
               procparams::ProcParams currentParamsIn,
               RenderStateKey previousStateIn,
               RenderStateKey currentStateIn,
               RequestKind kind)
        : currentParams(std::move(currentParamsIn)),
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
    };

    const ParamCase cases[] = {
        {"white balance (raw.base)",
         [](procparams::ProcParams& params) {
             params.wb.temperature += 500;
         },
         {WorkItem::rawBaseRebuild, WorkItem::analysisRun}},
        {"raw sensor level (raw.preprocess, raw.demosaic, raw.base)",
         [](procparams::ProcParams& params) {
             params.raw.bayersensor.black0 += 1.0;
         },
         {WorkItem::sourceSelect, WorkItem::rawPreprocess,
          WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::analysisRun}},
        {"capture sharpening (raw.demosaic)",
         [](procparams::ProcParams& params) {
             params.pdsharpening.contrast += 0.1;
         },
         {WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
          WorkItem::analysisRun}},
        {"retinex (raw.base, enabled)",
         [](procparams::ProcParams& params) {
             params.retinex.enabled = true;
         },
         {WorkItem::rawBaseRebuild, WorkItem::retinexPrepare,
          WorkItem::analysisRun}},
        {"lens profile (raw.preprocess only)",
         [](procparams::ProcParams& params) {
             params.lensProf.lcMode =
                 procparams::LensProfParams::LcMode::LENSFUNMANUAL;
         },
         {WorkItem::sourceSelect, WorkItem::rawPreprocess,
          WorkItem::rawBaseRebuild, WorkItem::analysisRun}},
        {"colour management (raw.base, analysis)",
         [](procparams::ProcParams& params) {
             params.icm.workingProfile = "ProPhoto";
         },
         {WorkItem::rawBaseRebuild, WorkItem::analysisRun}},
        {"denoise strength (raw.preprocess, raw.base, analysis)",
         [](procparams::ProcParams& params) {
             params.dirpyrDenoise.luma += 5;
         },
         {WorkItem::sourceSelect, WorkItem::rawPreprocess,
          WorkItem::rawBaseRebuild, WorkItem::analysisRun}},
    };

    for (const ParamCase& testCase : cases) {
        const procparams::ProcParams previous;
        procparams::ProcParams current = previous;
        testCase.mutate(current);

        const Derivation preview(StagePath::Preview, previous, current,
                                 RenderStateKey{}, RenderStateKey{},
                                 RequestKind::ParamsOnly);
        expectOrder(preview.preview(), testCase.previewExpected,
                    std::string("parameter ") + testCase.name);

        const Derivation crop(StagePath::Crop, previous, current,
                              RenderStateKey{}, RenderStateKey{},
                              RequestKind::ParamsOnly);
        expectOrder(crop.crop(), {},
                    std::string("parameter ") + testCase.name + " on crop");
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

    const Derivation crop(StagePath::Crop, params, params, previousState,
                          currentState, RequestKind::ParamsOnly);
    expectOrder(crop.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "source reload on crop");

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

    const Derivation cropGained(StagePath::Crop, params, params,
                                previousState, needed,
                                RequestKind::ParamsOnly);
    expectOrder(cropGained.crop(),
                {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                 WorkItem::analysisRun},
                "high detail gained: the crop reloads its ROI");

    /*
     * The other direction has both flags already computed, so only the
     * decision moved; the raw core still has to rerun at the new scale.
     */
    RenderStateKey dropped;
    dropped.highDetailNeeded = false;
    dropped.highDetailPreprocessComputed = true;
    dropped.highDetailRawComputed = true;
    dropped.highDetailStateKnown = true;
    const Derivation lost(StagePath::Preview, params, params, needed, dropped,
                          RequestKind::ParamsOnly);
    expectOrder(lost.preview(), rawCore,
                "high detail dropped: the raw core runs");
}

void testSpotTransitions()
{
    const procparams::ProcParams params;

    /*
     * A pending request alone has no base delta, so the apply body (which
     * lives inside the base rebuild branch) does not run; the restore copy
     * still does. The realistic transition carries the spot parameter delta
     * and is covered by the parameter table.
     */
    RenderStateKey previousState;
    RenderStateKey pendingState;
    pendingState.spotPending = true;

    const Derivation pending(StagePath::Preview, params, params, previousState,
                             pendingState, RequestKind::ParamsOnly);
    expectOrder(pending.preview(), {WorkItem::spotRestore},
                "spot pending without a delta: restore only");

    const Derivation cropPending(StagePath::Crop, params, params,
                                 previousState, pendingState,
                                 RequestKind::ParamsOnly);
    expectOrder(cropPending.crop(), {WorkItem::spotRestore},
                "spot pending on crop without a reload: restore only");

    /*
     * The standing rule: a live buffer changes the next base pass on the
     * preview, so the base and the analysis run even with no delta.
     */
    RenderStateKey liveState;
    liveState.spotBufferLive = true;

    const Derivation live(StagePath::Preview, params, params, previousState,
                          liveState, RequestKind::ParamsOnly);
    expectOrder(live.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::spotRestore,
                 WorkItem::analysisRun},
                "live spot buffer forces the base rebuild and the restore");

    /*
     * The crop restores by swapping baseCrop at dcrop.cc:1511, so the live
     * buffer names the restore and not the destructive body.
     */
    const Derivation cropLive(StagePath::Crop, params, params, previousState,
                              liveState, RequestKind::ParamsOnly);
    expectOrder(cropLive.crop(), {WorkItem::spotRestore},
                "live spot buffer on crop: pointer-swap restore only");

    /* A carried spot delta plus the pending fact applies the spots. */
    procparams::ProcParams spotted = params;
    spotted.spot.enabled = true;
    const Derivation applied(StagePath::Preview, params, spotted,
                             previousState, pendingState,
                             RequestKind::ParamsOnly);
    expectOrder(applied.preview(),
                {WorkItem::rawBaseRebuild, WorkItem::spotApply,
                 WorkItem::spotRestore, WorkItem::analysisRun},
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
    const std::initializer_list<WorkItem> previewRebuild = {
        WorkItem::sourceSelect, WorkItem::rawPreprocess,
        WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
        WorkItem::analysisRun};
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

    const Derivation highQualityPreview(StagePath::Preview, params, params,
                                        RenderStateKey{}, RenderStateKey{},
                                        RequestKind::HighQuality);
    expectOrder(highQualityPreview.preview(), previewRebuild,
                "HighQuality on preview: the destructive raw set");

    const Derivation highQualityCrop(StagePath::Crop, params, params,
                                     RenderStateKey{}, RenderStateKey{},
                                     RequestKind::HighQuality);
    expectOrder(highQualityCrop.crop(), cropRebuild,
                "HighQuality on crop: the crop reload set");

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

/* Every item is reachable from at least one documented case. */
void testCoverage()
{
    WorkOrder seen;
    const procparams::ProcParams params;

    procparams::ProcParams busy = params;
    busy.retinex.enabled = true;
    busy.dirpyrDenoise.enabled = true;
    busy.spot.enabled = true;
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
    testNoChange();
    testParameterChanges();
    testSourceReload();
    testHighDetailTransitions();
    testSpotTransitions();
    testRequestKinds();
    testCropDenoise();
    testNoHistory();
    testUnreadFacts();
    testCoverage();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("stage-work: all checks passed\n");
    return 0;
}
