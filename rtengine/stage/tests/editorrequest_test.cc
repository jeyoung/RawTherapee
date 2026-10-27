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
 * Specification test for the typed editor request and its legacy adapter.
 *
 * The expected rows below are the contract. Every `M_*` bit the coordinator
 * consumes must map to exactly one request kind and set of facts, M_VOID must
 * suppress the render, M_MINUPDATE alone must yield RepaintOnly, and an
 * unmapped bit or a combined M_MINUPDATE must fail fast instead of silently
 * degrading to ParamsOnly.
 */

#include "rtengine/editorrequest.h"

#include "rtengine/procparams.h"
#include "rtengine/refreshmap.h"
#include "rtengine/stage/stage_work.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

using rtengine::EditorRequest;
using rtengine::LegacyMaskContext;
using rtengine::RequestIntent;
using rtengine::coalesce;
using rtengine::compatibility_mask;
using rtengine::crop_request_kind;
using rtengine::from_legacy_mask;
using rtengine::make_request;
using rtengine::planningAgreesWithLegacyMask;
using rtengine::producerEventKey;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

void checkBit(int mask, const char* name, EditorRequest::RequestKind kind,
              bool highDetail, bool fullFrame, bool spot, bool controlOnly)
{
    const EditorRequest request = from_legacy_mask(mask);
    const std::string label = name;

    check(request.kind == kind, label + ": kind");
    check(request.present, label + ": present");
    check(request.controlOnly == controlOnly, label + ": controlOnly");
    check(request.highDetailIntent == highDetail,
          label + ": highDetailIntent");
    check(request.fullFrameIntent == fullFrame, label + ": fullFrameIntent");
    check(request.spotPending == spot, label + ": spotPending");
    check(request.legacyGateMask == mask, label + ": legacyGateMask");
    check(request.derivesFromParameters(),
          label + ": derives from parameters");
    check(request.dirty.blocks.empty(), label + ": no producer dirty set");

    /*
     * The mask-shaped facts. For a single bit, only M_MONITOR admits the
     * full-frame path, only M_WB requests the Color/Coloropp DEMOSAIC, only a
     * bare M_CROP or M_MINUPDATE suppresses monitor.output, and only
     * M_MINUPDATE is an imperative repaint.
     */
    check(request.fullFrameAdmission == (mask == M_MONITOR),
          label + ": fullFrameAdmission");
    check(request.colorHlrWbRequest == (mask == M_WB),
          label + ": colorHlrWbRequest");
    check(request.monitorOutputRequest == (mask != M_CROP && mask != M_MINUPDATE),
          label + ": monitorOutputRequest");
    check(request.imperativeRepaint == (mask == M_MINUPDATE),
          label + ": imperativeRepaint");
}

struct BitExpectation {
    int bit;
    const char* name;
    EditorRequest::RequestKind kind;
    bool highDetail;
    bool fullFrame;
    bool spot;
    bool controlOnly;
};

const std::vector<BitExpectation>& expectations()
{
    static const std::vector<BitExpectation> rows = {
        {M_PREPROC, "M_PREPROC", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_RAW, "M_RAW", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_CSHARP, "M_CSHARP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_WB, "M_WB", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_SPOT, "M_SPOT", EditorRequest::RequestKind::ParamsOnly,
         false, false, true, false},
        {M_INIT, "M_INIT", EditorRequest::RequestKind::FullPreview,
         false, true, false, false},
        {M_LINDENOISE, "M_LINDENOISE",
         EditorRequest::RequestKind::FullPreview, false, true, false, false},
        {M_HDR, "M_HDR", EditorRequest::RequestKind::FullPreview,
         false, true, false, false},
        {M_TRANSFORM, "M_TRANSFORM", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_RETINEX, "M_RETINEX", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_BLURMAP, "M_BLURMAP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_AUTOEXP, "M_AUTOEXP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_RGBCURVE, "M_RGBCURVE", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_LUMACURVE, "M_LUMACURVE", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_LUMINANCE, "M_LUMINANCE", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_COLOR, "M_COLOR", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_CROP, "M_CROP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_MONITOR, "M_MONITOR", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_VOID, "M_VOID", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, true},
        {M_MINUPDATE, "M_MINUPDATE", EditorRequest::RequestKind::RepaintOnly,
         false, false, false, false},
        {M_HIGHQUAL, "M_HIGHQUAL", EditorRequest::RequestKind::HighQuality,
         true, false, false, false}
    };
    return rows;
}

void checkEveryBit()
{
    int covered = 0;
    for (const BitExpectation& row : expectations()) {
        checkBit(row.bit, row.name, row.kind, row.highDetail, row.fullFrame,
                 row.spot, row.controlOnly);
        covered |= row.bit;
    }

    /* Every low bit the engine defines, and nothing above them. */
    const int expected = (1 << 21) - 1;
    check(covered == expected,
          "the table covers every consumed M_* bit exactly once");
}

void checkControlAndImperative()
{
    const EditorRequest control = from_legacy_mask(M_VOID);
    check(control.controlOnly, "M_VOID is control-only");
    check(control.present, "M_VOID is still a request");
    check(from_legacy_mask(0).present == false, "zero mask is not a request");
    check(from_legacy_mask(0).controlOnly, "zero mask is control-only");

    const EditorRequest repaint = from_legacy_mask(M_MINUPDATE);
    check(repaint.kind == EditorRequest::RequestKind::RepaintOnly,
          "M_MINUPDATE alone is RepaintOnly");
    check(!repaint.controlOnly, "M_MINUPDATE is imperative");

    const EditorRequest twice = coalesce(repaint, repaint);
    check(twice.kind == EditorRequest::RequestKind::RepaintOnly,
          "two repaints coalesce to RepaintOnly");
    check(twice.legacyGateMask == M_MINUPDATE,
          "two repaints keep the bare MINUPDATE mask");

    /*
     * A control-only request combined with a repaint is the old
     * `M_MINUPDATE | M_VOID`: no longer exactly M_MINUPDATE, so ParamsOnly.
     */
    const EditorRequest poisoned = coalesce(repaint, control);
    check(poisoned.kind == EditorRequest::RequestKind::ParamsOnly,
          "control + repaint is ParamsOnly");
    check(poisoned.legacyGateMask == (M_MINUPDATE | M_VOID),
          "control + repaint unions the gate mask");

    const EditorRequest withParams =
        coalesce(repaint, from_legacy_mask(M_CROP));
    check(withParams.kind == EditorRequest::RequestKind::ParamsOnly,
          "repaint + parameter change is ParamsOnly");
}

void checkCombinedBits()
{
    const EditorRequest reload =
        from_legacy_mask(M_INIT | M_HDR | M_CROP);
    check(reload.kind == EditorRequest::RequestKind::FullPreview,
          "reload + crop is FullPreview");
    check(reload.fullFrameIntent, "reload sets fullFrameIntent");
    check(!reload.spotPending, "reload + crop does not request spot work");
    check(!reload.controlOnly, "reload is not control-only");

    const EditorRequest accurate =
        from_legacy_mask(M_HIGHQUAL | M_INIT | M_SPOT);
    check(accurate.kind == EditorRequest::RequestKind::HighQuality,
          "M_HIGHQUAL wins over reload bits");
    check(accurate.highDetailIntent, "M_HIGHQUAL sets highDetailIntent");
    check(accurate.fullFrameIntent,
          "M_HIGHQUAL with M_INIT still records full-frame intent");
    check(accurate.spotPending, "M_HIGHQUAL with M_SPOT records spot intent");

    const EditorRequest coalesced =
        coalesce(from_legacy_mask(M_INIT),
                 from_legacy_mask(M_HIGHQUAL | M_CROP));
    check(coalesced.kind == EditorRequest::RequestKind::HighQuality,
          "coalescing keeps the strongest kind");
    check(coalesced.fullFrameIntent,
          "coalescing unions the request facts");
    check(coalesced.legacyGateMask == (M_INIT | M_HIGHQUAL | M_CROP),
          "coalescing unions the gate mask");
}

/*
 * The viewport fast-path facts. Each is a pure bit test the admission and LUT
 * selection read, so the adapter must set it for every single bit, and a
 * coalesced request must recompute it from the union.
 */
void checkViewportFacts()
{
    constexpr int baseChain =
        M_PREPROC | M_RAW | M_WB | M_INIT | M_LINDENOISE | M_MONITOR
        | M_TRANSFORM | M_CSHARP | M_RETINEX | M_BLURMAP;

    for (const BitExpectation& row : expectations()) {
        const EditorRequest request = from_legacy_mask(row.bit);
        const std::string label = std::string("facts ") + row.name;

        check(request.baseChainIntent == ((row.bit & baseChain) != 0),
              label + ": baseChainIntent");
        check(request.cropIntent == (row.bit == M_CROP),
              label + ": cropIntent");
        check(request.repaintRequest == (row.bit == M_MINUPDATE),
              label + ": repaintRequest");
        check(request.controlBitRequest == (row.bit == M_VOID),
              label + ": controlBitRequest");
        check(request.lumaCurveIntent == (row.bit == M_LUMACURVE),
              label + ": lumaCurveIntent");
        check(request.labPixelIntent
                  == (row.bit == M_LUMINANCE || row.bit == M_COLOR),
              label + ": labPixelIntent");
        check(request.luminanceIntent == (row.bit == M_LUMINANCE),
              label + ": luminanceIntent");
        check(request.colorIntent == (row.bit == M_COLOR),
              label + ": colorIntent");
        check(request.transformRequest == (row.bit == M_TRANSFORM),
              label + ": transformRequest");
    }

    const EditorRequest merged = coalesce(from_legacy_mask(M_LUMACURVE),
                                          from_legacy_mask(M_CROP));
    check(merged.lumaCurveIntent && merged.cropIntent,
          "coalescing unions the viewport facts");
    check(merged.baseChainIntent == false,
          "a Lab and crop union stays outside the base chain");
    check(planningAgreesWithLegacyMask(merged),
          "the coalesced viewport facts agree with the union mask");
}

/*
 * Viewport admission and LUT selection for the documented request shapes.
 * The exhaustive decision table lives in viewport_admission_test.cc; this
 * pins the reason strings and the selections the coordinator consumes.
 */
void checkViewportAdmission()
{
    using rtengine::ViewportAdmissionInputs;
    using rtengine::ViewportAdmissionState;
    using rtengine::ViewportEligibility;
    using rtengine::viewportCropSelection;
    using rtengine::viewportCropTodo;
    using rtengine::viewportEligibility;
    using rtengine::viewportFastPathFallback;
    using rtengine::viewportLutSelection;

    check(viewportEligibility(from_legacy_mask(M_CROP))
              == ViewportEligibility::Admit,
          "a crop request is admissible");
    check(viewportEligibility(from_legacy_mask(M_HIGHQUAL))
              == ViewportEligibility::Base,
          "a high-detail request is a base request");
    check(viewportEligibility(from_legacy_mask(M_VOID))
              == ViewportEligibility::Unsupported,
          "a control-only request is unsupported");

    {
        rtengine::ViewportLutSelection selection =
            viewportLutSelection(from_legacy_mask(M_CROP));
        check(selection.toneCurves && selection.lumaCurveFromCrop
                  && !selection.labCurves && !selection.wavelet,
              "a crop request rebuilds the tone LUTs and lumacurve only");

        const EditorRequest luma = from_legacy_mask(M_LUMACURVE);
        selection = viewportLutSelection(luma);
        check(selection.labCurves && selection.rebuildsLumaCurve()
                  && !selection.wavelet,
              "a luminance-curve request rebuilds the whole Lab family");

        const EditorRequest autoExp = from_legacy_mask(M_AUTOEXP);
        selection = viewportLutSelection(autoExp);
        check(selection.wavelet && !selection.lumaCurveFromCrop,
              "auto exposure rebuilds the wavelet LUT");
    }

    {
        const EditorRequest crop = from_legacy_mask(M_CROP);
        const rtengine::ViewportCropSelection selection =
            viewportCropSelection(crop);
        check(viewportCropTodo(selection) == M_CROP,
              "a crop request renders only the crop subset");

        const EditorRequest widened =
            from_legacy_mask(M_LUMACURVE | M_LUMINANCE | M_COLOR);
        rtengine::ViewportCropSelection dependent =
            viewportCropSelection(widened);
        rtengine::resolveCropDependencies(dependent);
        check(viewportCropTodo(dependent)
                  == (M_LUMACURVE | M_LUMINANCE | M_COLOR),
              "the Lab pixel dependency keeps the exact bits");
    }

    {
        rtengine::procparams::ProcParams params;
        params.toneCurve.autoexp = true;
        ViewportAdmissionInputs inputs{params, params, params, {}};
        inputs.state.fastPathEnabled = true;
        inputs.state.fullStateComplete = true;
        check(std::string(viewportFastPathFallback(
                  from_legacy_mask(M_CROP), inputs))
                  == "auto-exposure",
              "auto exposure keeps its fallback reason");
    }
}

/*
 * Every pair of bits that can legally be coalesced must produce exactly the
 * request a single mask of the union produces: same kind, same facts, same
 * gate mask and same identity. Pairs whose union contains a combined
 * M_MINUPDATE are contract violations and are checked separately.
 */
void checkEveryPair()
{
    for (const BitExpectation& left : expectations()) {
        for (const BitExpectation& right : expectations()) {
            const int unionMask = left.bit | right.bit;

            if ((unionMask & M_MINUPDATE) != 0
                    && unionMask != M_MINUPDATE) {
                continue;
            }

            const EditorRequest merged =
                coalesce(from_legacy_mask(left.bit),
                         from_legacy_mask(right.bit));
            const EditorRequest single = from_legacy_mask(unionMask);
            const std::string label =
                std::string("pair ") + left.name + "+" + right.name;

            check(merged.present, label + ": present");
            check(merged.kind == single.kind, label + ": kind");
            check(merged.controlOnly == single.controlOnly,
                  label + ": controlOnly");
            check(merged.spotPending == single.spotPending,
                  label + ": spotPending");
            check(merged.highDetailIntent == single.highDetailIntent,
                  label + ": highDetailIntent");
            check(merged.fullFrameIntent == single.fullFrameIntent,
                  label + ": fullFrameIntent");
            check(merged.fullFrameAdmission == single.fullFrameAdmission,
                  label + ": fullFrameAdmission");
            check(merged.colorHlrWbRequest == single.colorHlrWbRequest,
                  label + ": colorHlrWbRequest");
            check(merged.monitorOutputRequest == single.monitorOutputRequest,
                  label + ": monitorOutputRequest");
            check(merged.imperativeRepaint == single.imperativeRepaint,
                  label + ": imperativeRepaint");
            check(merged.transformFamilyIntent == single.transformFamilyIntent,
                  label + ": transformFamilyIntent");
            check(merged.hdrIntent == single.hdrIntent, label + ": hdrIntent");
            check(merged.rgbProcIntent == single.rgbProcIntent,
                  label + ": rgbProcIntent");
            check(merged.labChainIntent == single.labChainIntent,
                  label + ": labChainIntent");
            check(merged.autoExposureIntent == single.autoExposureIntent,
                  label + ": autoExposureIntent");
            check(merged.localAdjustmentsIntent
                      == single.localAdjustmentsIntent,
                  label + ": localAdjustmentsIntent");
            check(merged.baseChainIntent == single.baseChainIntent,
                  label + ": baseChainIntent");
            check(merged.cropIntent == single.cropIntent,
                  label + ": cropIntent");
            check(merged.repaintRequest == single.repaintRequest,
                  label + ": repaintRequest");
            check(merged.controlBitRequest == single.controlBitRequest,
                  label + ": controlBitRequest");
            check(merged.lumaCurveIntent == single.lumaCurveIntent,
                  label + ": lumaCurveIntent");
            check(merged.labPixelIntent == single.labPixelIntent,
                  label + ": labPixelIntent");
            check(merged.luminanceIntent == single.luminanceIntent,
                  label + ": luminanceIntent");
            check(merged.colorIntent == single.colorIntent,
                  label + ": colorIntent");
            check(merged.transformRequest == single.transformRequest,
                  label + ": transformRequest");
            check(merged.legacyGateMask == unionMask,
                  label + ": gate mask");
            check(merged.identity() == single.identity(),
                  label + ": identity");
        }
    }
}

/*
 * Facts that are monotone in the mask are unioned; facts that interpret the
 * mask as a whole are recomputed from the union.
 */
void checkFactUnion()
{
    LegacyMaskContext moved;
    moved.viewportMoved = true;

    const EditorRequest spotAndReload =
        coalesce(from_legacy_mask(M_SPOT), from_legacy_mask(M_INIT));
    check(spotAndReload.spotPending && spotAndReload.fullFrameIntent,
          "coalescing unions spot and full-frame intent");

    const EditorRequest movedAndHigh =
        coalesce(from_legacy_mask(M_CROP, moved),
                 from_legacy_mask(M_HIGHQUAL));
    check(movedAndHigh.viewportMoved && movedAndHigh.highDetailIntent,
          "coalescing unions viewportMoved and high-detail intent");
    check(movedAndHigh.kind == EditorRequest::RequestKind::HighQuality,
          "high detail keeps its precedence over a viewport move");

    const EditorRequest controlAndCrop =
        coalesce(from_legacy_mask(M_VOID), from_legacy_mask(M_CROP));
    check(!controlAndCrop.controlOnly,
          "coalescing ANDs controlOnly over a rendering request");

    /*
     * Both inputs suppress monitor.output. Their union,
     * `M_CROP | M_MINUPDATE`, is neither exactly `M_CROP` nor exactly
     * `M_MINUPDATE`, so the legacy predicate renders it. This proves the
     * fact is recomputed from the union rather than ORed.
     */
    const EditorRequest cropAndRepaint =
        coalesce(from_legacy_mask(M_CROP), from_legacy_mask(M_MINUPDATE));
    check(!from_legacy_mask(M_CROP).monitorOutputRequest,
          "a bare crop suppresses monitor.output");
    check(!from_legacy_mask(M_MINUPDATE).monitorOutputRequest,
          "a bare repaint suppresses monitor.output");
    check(cropAndRepaint.monitorOutputRequest,
          "the union of two monitor-suppressing masks renders monitor.output");

    /*
     * Neither input sets every ALL bit, but their union does, so the merged
     * request admits the full-frame path. Admission is not a monotone union.
     */
    const int left = M_PREPROC | M_RAW;
    const int right = ALL & ~left;
    check(!from_legacy_mask(left).fullFrameAdmission,
          "half of ALL does not admit the full frame");
    check(!from_legacy_mask(right).fullFrameAdmission,
          "the other half of ALL does not admit the full frame");
    check(coalesce(from_legacy_mask(left), from_legacy_mask(right))
              .fullFrameAdmission,
          "the union of two halves of ALL admits the full frame");
}

void checkEmptyAccumulator()
{
    const EditorRequest empty;
    check(!empty.present, "an empty accumulator is not present");
    check(empty.controlOnly, "an empty accumulator is control-only");

    const EditorRequest request = from_legacy_mask(M_HIGHQUAL | M_CROP);

    const EditorRequest withEmptyFirst = coalesce(empty, request);
    check(withEmptyFirst.identity() == request.identity(),
          "an empty first accumulator returns the second request");
    check(withEmptyFirst.legacyGateMask == request.legacyGateMask,
          "an empty first accumulator keeps the second mask");

    const EditorRequest withEmptySecond = coalesce(request, empty);
    check(withEmptySecond.identity() == request.identity(),
          "an empty second accumulator returns the first request");
    check(withEmptySecond.legacyGateMask == request.legacyGateMask,
          "an empty second accumulator keeps the first mask");

    check(!coalesce(empty, empty).present,
          "coalescing two empty accumulators stays empty");
}

/*
 * The coordinator patches the typed kind and facts after it widens a legacy
 * mask at four sites. These are the exact shapes it produces; the debug
 * cross-check in the coordinator asserts the same relation.
 */
void checkCoordinatorPatches()
{
    {
        EditorRequest requeued = from_legacy_mask(M_MINUPDATE);
        check(requeued.imperativeRepaint,
              "a bare repaint is imperative before the requeue");
        requeued.setLegacyGateMask(M_MINUPDATE | M_MONITOR);
        requeued.viewportMoved = true;
        requeued.kind = EditorRequest::RequestKind::ParamsOnly;
        check(!requeued.imperativeRepaint,
              "widening the requeue clears the imperative repaint");
        check(planningAgreesWithLegacyMask(requeued),
              "the panning requeue patch agrees with its widened mask");
    }
    {
        EditorRequest refreshed = from_legacy_mask(M_CROP);
        refreshed.setLegacyGateMask(M_CROP | M_INIT | M_HDR | M_MONITOR);
        refreshed.fullFrameIntent = true;
        refreshed.kind = EditorRequest::RequestKind::FullPreview;
        check(planningAgreesWithLegacyMask(refreshed),
              "the base-refresh patch agrees with its widened mask");
    }
    {
        EditorRequest absorbed = from_legacy_mask(M_MINUPDATE);
        absorbed.setLegacyGateMask(
            M_MINUPDATE | M_MONITOR | M_RGBCURVE | M_LUMACURVE
            | M_LUMINANCE | M_COLOR);
        absorbed.kind = EditorRequest::RequestKind::ParamsOnly;
        check(planningAgreesWithLegacyMask(absorbed),
              "the deferred-absorb patch agrees with its widened mask");
    }
    {
        EditorRequest color = from_legacy_mask(M_WB);
        color.setLegacyGateMask(M_WB | DEMOSAIC);
        color.fullFrameIntent = true;
        color.spotPending = true;
        color.kind = EditorRequest::RequestKind::FullPreview;
        check(planningAgreesWithLegacyMask(color),
              "the Color/Coloropp patch agrees with its widened mask");
    }

    /* The cross-check is not vacuous: a wrong kind must be rejected. */
    EditorRequest wrong = from_legacy_mask(M_WB);
    wrong.kind = EditorRequest::RequestKind::FullPreview;
    check(!planningAgreesWithLegacyMask(wrong),
          "the cross-check rejects a kind the mask does not imply");
}

void checkViewportMovedContext()
{
    LegacyMaskContext context;
    context.viewportMoved = true;

    const EditorRequest moved = from_legacy_mask(M_CROP, context);
    check(moved.viewportMoved, "the adapter carries viewportMoved");
    check(!from_legacy_mask(M_CROP).viewportMoved,
          "viewportMoved defaults to false");
}

/*
 * The typed producer API. Each intent must produce the request its legacy
 * composite produced, so converting a tool cannot change planning; the
 * compatibility mask must be the composite the rest of the coordinator still
 * consumes.
 */
struct IntentExpectation {
    RequestIntent intent;
    const char* name;
    int mask;
};

const std::vector<IntentExpectation>& intentExpectations()
{
    static const std::vector<IntentExpectation> rows = {
        {RequestIntent::AllNoRaw, "AllNoRaw", ALLNORAW},
        {RequestIntent::Demosaic, "Demosaic", DEMOSAIC},
        {RequestIntent::Hdr, "Hdr", HDR},
        {RequestIntent::SpotAdjust, "SpotAdjust", SPOTADJUST},
        {RequestIntent::Transform, "Transform", TRANSFORM},
        {RequestIntent::WhiteBalance, "WhiteBalance", WB},
        {RequestIntent::RepaintOnly, "RepaintOnly", M_MINUPDATE},
        {RequestIntent::ControlOnly, "ControlOnly", M_VOID},
        {RequestIntent::Compr, "Compr", COMPR},
        {RequestIntent::CaptureSharpen, "CaptureSharpen", CAPTURESHARPEN},
        {RequestIntent::DarkFrame, "DarkFrame", DARKFRAME},
        {RequestIntent::First, "First", FIRST},
        {RequestIntent::AutoExposure, "AutoExposure", AUTOEXP},
        {RequestIntent::LuminanceCurve, "LuminanceCurve", LUMINANCECURVE},
        {RequestIntent::DirPyrEqualizer, "DirPyrEqualizer", DIRPYREQUALIZER},
        {RequestIntent::RgbCurve, "RgbCurve", RGBCURVE},
        {RequestIntent::Sharpening, "Sharpening", SHARPENING}
    };
    return rows;
}

void checkProducerIntents()
{
    for (const IntentExpectation& row : intentExpectations()) {
        const std::string label = std::string("intent ") + row.name;
        const EditorRequest typed = make_request(row.intent);
        const EditorRequest legacy = from_legacy_mask(row.mask);

        check(compatibility_mask(row.intent) == row.mask,
              label + ": compatibility mask");
        check(typed.legacyGateMask == row.mask, label + ": gate mask");
        check(typed.kind == legacy.kind, label + ": kind");
        check(typed.controlOnly == legacy.controlOnly,
              label + ": controlOnly");
        check(typed.present, label + ": present");
        check(typed.spotPending == legacy.spotPending, label + ": spot");
        check(typed.fullFrameIntent == legacy.fullFrameIntent,
              label + ": full frame");
        check(typed.colorHlrWbRequest == legacy.colorHlrWbRequest,
              label + ": colour HLR WB");
        check(typed.transformFamilyIntent == legacy.transformFamilyIntent,
              label + ": transform family");
        check(typed.hdrIntent == legacy.hdrIntent, label + ": HDR");
        check(typed.rgbProcIntent == legacy.rgbProcIntent, label + ": rgb.proc");
        check(typed.labChainIntent == legacy.labChainIntent, label + ": Lab");
        check(typed.autoExposureIntent == legacy.autoExposureIntent,
              label + ": auto exposure");
        check(typed.localAdjustmentsIntent == legacy.localAdjustmentsIntent,
              label + ": local adjustments");
        check(typed.derivesFromParameters(),
              label + ": no producer dirty set");
        check(planningAgreesWithLegacyMask(typed),
              label + ": agrees with its compatibility mask");
        check(typed.identity() == legacy.identity(),
              label + ": identical planning identity");
    }

    check(make_request(RequestIntent::RepaintOnly).kind
              == EditorRequest::RequestKind::RepaintOnly,
          "the repaint intent stays RepaintOnly");
    check(make_request(RequestIntent::WhiteBalance).colorHlrWbRequest,
          "the white-balance intent carries the Color/Coloropp fact");
}

/*
 * A producer that names its dirty blocks gets an execution set: the seeds,
 * the forward closure and the path residents. The set replaces the parameter
 * comparison, but the request identity records it.
 */
void checkProducerDirtyBlocks()
{
    const EditorRequest seeded = make_request(
        RequestIntent::Transform, {"transform"}, rtengine::stage::StagePath::Preview);

    check(seeded.dirtySource == EditorRequest::DirtySource::ProducerProvided,
          "producer blocks set ProducerProvided");
    check(seeded.dirty.valid, "producer dirty set is valid");
    check(seeded.dirty.ids.count("transform") != 0,
          "producer dirty set contains the seed");
    check(seeded.dirty.ids.count("local.adjustments") != 0,
          "producer dirty set closes over the first consumer");
    check(seeded.dirty.ids.count("monitor.output") != 0,
          "producer dirty set contains the path resident");
    check(seeded.dirty.forced == false, "producer dirty set is not forced");
    check(!seeded.derivesFromParameters(),
          "producer dirty set does not derive from parameters");
    check(seeded.identity() != make_request(RequestIntent::Transform).identity(),
          "producer dirty blocks change the identity");

    const EditorRequest second = make_request(
        RequestIntent::Transform, {"rgb.proc"}, rtengine::stage::StagePath::Preview);
    const EditorRequest merged = coalesce(seeded, second);
    check(merged.dirtySource
              == EditorRequest::DirtySource::ProducerProvided,
          "coalescing two producer sets keeps ProducerProvided");
    check(merged.dirty.ids.count("transform") != 0,
          "coalescing unions the producer sets");
    check(merged.dirty.ids.count("rgb.proc") != 0,
          "coalescing keeps both seeds");

    const EditorRequest derived = make_request(RequestIntent::Transform);
    const EditorRequest mixed = coalesce(derived, second);
    check(mixed.dirtySource
              == EditorRequest::DirtySource::ProducerProvided,
          "a derived request does not erase a producer set");
}

/*
 * The RefreshMapper carries the typed request for a converted event and the
 * compatibility mask for every consumer that still asks for one. An event
 * registered with a mask stays on the compatibility path.
 */
void checkRefreshMapper()
{
    auto* mapper = rtengine::RefreshMapper::getInstance();

    const rtengine::ProcEvent typedEvent = mapper->newEvent();
    const EditorRequest typed = make_request(RequestIntent::WhiteBalance);
    mapper->mapRequest(typedEvent, typed);

    EditorRequest found;
    check(mapper->getRequest(typedEvent, found),
          "a typed event returns its request");
    check(found.identity() == typed.identity(),
          "the typed request survives the mapper");
    check(mapper->getAction(typedEvent) == WB,
          "a typed event still reports its compatibility mask");

    const rtengine::ProcEvent maskedEvent = mapper->newEvent();
    mapper->mapEvent(maskedEvent, AUTOEXP);
    check(!mapper->getRequest(maskedEvent, found),
          "a mask event has no typed request");
    check(mapper->getAction(maskedEvent) == AUTOEXP,
          "a mask event keeps its action");

    const rtengine::ProcEvent reused = mapper->newEvent();
    mapper->mapRequest(reused, typed);
    mapper->mapEvent(reused, M_CROP);
    check(!mapper->getRequest(reused, found),
          "re-registering with a mask clears the typed request");
}

/*
 * Producer actions with the same planning identity must still be distinct
 * events when their compatibility masks differ. The event key therefore
 * includes the identity *and* the mask: a mask difference that carries no
 * planning fact today (M_LUMINANCE and M_COLOR are both the Lab pixel family)
 * must not collapse two producer actions into one registry entry.
 */
void checkProducerEventKey()
{
    const EditorRequest lab = from_legacy_mask(M_LUMINANCE);
    const EditorRequest colour = from_legacy_mask(M_COLOR);

    check(lab.identity() == colour.identity(),
          "M_LUMINANCE and M_COLOR share a planning identity");
    check(lab.legacyGateMask != colour.legacyGateMask,
          "M_LUMINANCE and M_COLOR differ in their compatibility mask");
    check(producerEventKey(lab, "HISTORY_MSG_LAB")
              != producerEventKey(colour, "HISTORY_MSG_LAB"),
          "same identity and message with different masks get distinct keys");
    check(producerEventKey(lab, "HISTORY_MSG_LAB")
              == producerEventKey(lab, "HISTORY_MSG_LAB"),
          "the same producer action keeps its key");

    /*
     * The base-chain fact is a planning field: ALLNORAW asks for the
     * full-frame base and RGBCURVE does not, so the two cannot share an
     * identity even though their other facts overlap.
     */
    const EditorRequest allNoRaw = make_request(RequestIntent::AllNoRaw);
    const EditorRequest rgbCurve = make_request(RequestIntent::RgbCurve);

    check(allNoRaw.baseChainIntent && !rgbCurve.baseChainIntent,
          "ALLNORAW is a base-chain request and RGBCURVE is not");
    check(allNoRaw.identity() != rgbCurve.identity(),
          "ALLNORAW and RGBCURVE differ in their planning identity");

    for (const IntentExpectation& left : intentExpectations()) {
        for (const IntentExpectation& right : intentExpectations()) {
            if (left.intent == right.intent) {
                continue;
            }

            const EditorRequest leftRequest = make_request(left.intent);
            const EditorRequest rightRequest = make_request(right.intent);

            /*
             * The key carries the planning identity and the compatibility
             * mask. Two intents that differ in either must therefore stay
             * distinct even when they share a history message.
             */
            if (leftRequest.identity() == rightRequest.identity()
                    && leftRequest.legacyGateMask
                        == rightRequest.legacyGateMask) {
                continue;
            }

            const std::string label =
                std::string("key uniqueness ") + left.name + "/" + right.name;
            check(producerEventKey(leftRequest, "msg")
                      != producerEventKey(rightRequest, "msg"),
                  label);
        }
    }

    /*
     * DIRPYREQUALIZER and SHARPENING have the same legacy composite and the
     * same planning identity, so the history message is the only discriminator
     * between two such actions. Real producers always pass distinct messages.
     */
    const EditorRequest dirPyr = make_request(RequestIntent::DirPyrEqualizer);
    const EditorRequest sharpening = make_request(RequestIntent::Sharpening);
    check(dirPyr.identity() == sharpening.identity()
              && dirPyr.legacyGateMask == sharpening.legacyGateMask,
          "DirPyrEqualizer and Sharpening share their planning payload");
    check(producerEventKey(dirPyr, "HISTORY_MSG_DIRPYREQUALIZER")
              != producerEventKey(sharpening, "HISTORY_MSG_SHARPENING"),
          "the history message keeps same-payload actions distinct");
}

/*
 * The fast path augments the execution payload after the request was built.
 * Every augmentation must refresh the typed facts before the crop consumes
 * them. These are the two augmentation shapes used in updatePreviewImage().
 */
void checkFastPathAugmentation()
{
    EditorRequest crop = from_legacy_mask(M_CROP);
    check(!crop.transformFamilyIntent,
          "a bare crop does not claim transform-family work");
    crop.setLegacyGateMask(crop.legacyGateMask | TRANSFORM);
    crop.refreshFromLegacyMask();
    check(crop.transformFamilyIntent,
          "the PC-vignette augmentation refreshes transformFamilyIntent");
    check(crop.kind == EditorRequest::RequestKind::FullPreview,
          "the PC-vignette augmentation widens the kind like the old mask");
    check(planningAgreesWithLegacyMask(crop),
          "the PC-vignette augmentation agrees with its widened mask");

    EditorRequest lumacurve = from_legacy_mask(M_LUMACURVE);
    check(!lumacurve.labChainIntent,
          "a bare luminance curve does not claim the Lab chain");
    lumacurve.setLegacyGateMask(
        lumacurve.legacyGateMask | M_LUMINANCE | M_COLOR);
    check(lumacurve.labChainIntent,
          "the fast-path Lab augmentation refreshes labChainIntent");
    check(planningAgreesWithLegacyMask(lumacurve),
          "the fast-path Lab augmentation agrees with its widened mask");
}

/*
 * A crop reallocation must refill the base even when the editor request is a
 * plain parameter update. The force is carried in the render-state request
 * kind, which derive_crop_work() reads.
 */
void checkCropReallocationWorkOrder()
{
    rtengine::procparams::ProcParams params;

    const rtengine::stage::BlockFingerprints fingerprints =
        rtengine::stage::stage_fingerprints(
            rtengine::stage::StagePath::Crop, params);

    rtengine::stage::RenderStateKey state;
    state.cropSkip = 1;
    state.requestKind = crop_request_kind(
        EditorRequest::RequestKind::ParamsOnly, false);

    rtengine::stage::WorkInputs inputs;
    inputs.path = rtengine::stage::StagePath::Crop;
    inputs.previousState = &state;
    inputs.previousFingerprints = &fingerprints;
    inputs.previousParams = &params;
    inputs.currentState = &state;
    inputs.currentFingerprints = &fingerprints;
    inputs.requestKind = state.requestKind;
    inputs.params = &params;

    const rtengine::stage::WorkOrder steady =
        rtengine::stage::derive_crop_work(inputs);
    check(!rtengine::stage::workItemRuns(
              steady, rtengine::stage::WorkItem::rawBaseRebuild),
          "a steady parameter update does not rebuild the crop base");

    state.requestKind = crop_request_kind(
        EditorRequest::RequestKind::ParamsOnly, true);
    inputs.requestKind = state.requestKind;

    const rtengine::stage::WorkOrder reallocated =
        rtengine::stage::derive_crop_work(inputs);
    check(rtengine::stage::workItemRuns(
              reallocated, rtengine::stage::WorkItem::rawBaseRebuild),
          "a reallocation forces the crop base rebuild");
    check(rtengine::stage::workItemRuns(
              reallocated, rtengine::stage::WorkItem::analysisRun),
          "a reallocation forces the crop analysis");

    check(crop_request_kind(EditorRequest::RequestKind::HighQuality, true)
              == rtengine::stage::RequestKind::FullCrop,
          "a reallocation wins over a high-quality crop request");
    check(crop_request_kind(EditorRequest::RequestKind::FullPreview, false)
              == rtengine::stage::RequestKind::FullCrop,
          "a full preview maps to a full crop");
}

/*
 * Tool -> intent wiring. Read the converted producer sources and check that
 * each documented action registers the intent the migration table names. This
 * is deliberately a source check: the tools are GUI widgets, and constructing
 * them in this engine test would pull in the whole toolkit.
 */
void checkProducerSource(
    const char* file, const char* historyMsg, const char* intent,
    const char* label)
{
    const std::string path = std::string(RT_SOURCE_DIR) + "/" + file;
    std::ifstream stream(path);

    if (!stream) {
        check(false, std::string(label) + ": cannot read " + path);
        return;
    }

    std::string line;
    bool foundMessage = false;
    bool foundIntent = false;

    while (std::getline(stream, line)) {
        if (line.find(historyMsg) == std::string::npos) {
            continue;
        }
        foundMessage = true;
        if (line.find(std::string("RequestIntent::") + intent)
                != std::string::npos) {
            foundIntent = true;
        }
    }

    check(foundMessage, std::string(label) + ": message not found");
    check(foundIntent, std::string(label) + ": wrong intent");
}

void checkConvertedProducerIntents()
{
    checkProducerSource("rtgui/tools/filmnegative.cc",
                        "HISTORY_MSG_FILMNEGATIVE_VALUES", "AllNoRaw",
                        "film negative");
    checkProducerSource("rtgui/tools/spot.cc", "HISTORY_MSG_SPOT_ENTRY",
                        "SpotAdjust", "spot entry");
    checkProducerSource("rtgui/tools/whitebalance.cc",
                        "HISTORY_MSG_WBALANCE_OBSERVER10", "WhiteBalance",
                        "white balance");
    checkProducerSource("rtgui/tools/dehaze.cc", "HISTORY_MSG_DEHAZE_ENABLED",
                        "Hdr", "dehaze");
    checkProducerSource("rtgui/tools/perspective.cc",
                        "HISTORY_MSG_PERSP_METHOD", "Transform",
                        "perspective");
    checkProducerSource("rtgui/tools/compressgamut.cc",
                        "HISTORY_MSG_CG_COLORSPACE", "Compr",
                        "gamut compression");
    checkProducerSource("rtgui/tools/pdsharpening.cc",
                        "HISTORY_MSG_PDSHARPEN_CONTRAST", "CaptureSharpen",
                        "capture sharpening");
    checkProducerSource("rtgui/tools/bayerpreprocess.cc",
                        "HISTORY_MSG_PREPROCESS_PDAFLINESFILTER", "DarkFrame",
                        "dark frame");
    checkProducerSource("rtgui/tools/cropguide.cc",
                        "HISTORY_MSG_CROP_GUIDE_ENABLED", "RepaintOnly",
                        "crop guide");
    checkProducerSource("rtgui/tools/preprocesswb.cc",
                        "HISTORY_MSG_PREPROCWB_MODE", "First",
                        "preprocess white balance");
    checkProducerSource("rtgui/tools/resize.cc",
                        "HISTORY_MSG_RESIZE_LONGEDGE", "ControlOnly",
                        "resize");
}

/*
 * Remove comments from a source file so a commented-out producer cannot make
 * the scan pass or fail for the wrong reason. String and character literals
 * are respected, because a history message may contain `//`.
 */
std::string stripComments(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool lineComment = false;
    bool blockComment = false;
    bool inString = false;
    bool inChar = false;

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        const char next =
            index + 1 < text.size() ? text[index + 1] : '\0';

        if (lineComment) {
            if (current == '\n') {
                lineComment = false;
                result += current;
            }
            continue;
        }
        if (blockComment) {
            if (current == '*' && next == '/') {
                blockComment = false;
                ++index;
            } else if (current == '\n') {
                result += current;
            }
            continue;
        }
        if (inString || inChar) {
            result += current;
            if (current == '\\' && next != '\0') {
                result += next;
                ++index;
            } else if ((inString && current == '"')
                       || (inChar && current == '\'')) {
                inString = false;
                inChar = false;
            }
            continue;
        }
        if (current == '/' && next == '/') {
            lineComment = true;
            ++index;
            continue;
        }
        if (current == '/' && next == '*') {
            blockComment = true;
            ++index;
            continue;
        }
        if (current == '"') {
            inString = true;
        } else if (current == '\'') {
            inChar = true;
        }
        result += current;
    }

    return result;
}

/*
 * The typed producer API is the only producer entry point. Every `newEvent`
 * call in `rtgui/` must name a typed request; the compatibility overload is
 * retained only in `eventmapper.cc`/`.h`, which define the adapter and are
 * therefore skipped. This is a source check because the tools are GUI
 * widgets and constructing them here would pull in the whole toolkit.
 */
void checkNoLegacyProducerCalls()
{
    namespace fs = std::filesystem;

    const fs::path root = fs::path(RT_SOURCE_DIR) / "rtgui";
    const std::regex callPattern(
        R"(newEvent\s*\(\s*([A-Za-z_][A-Za-z0-9_:]*))");

    std::size_t files = 0;
    std::size_t calls = 0;
    std::vector<std::string> violations;

    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end;
         it != end && !error; it.increment(error)) {
        if (!it->is_regular_file()) {
            continue;
        }

        const std::string suffix = it->path().extension().string();
        if (suffix != ".cc" && suffix != ".h" && suffix != ".hpp") {
            continue;
        }

        const std::string relative =
            fs::relative(it->path(), root).generic_string();
        if (relative == "eventmapper.cc" || relative == "eventmapper.h") {
            continue;
        }

        std::ifstream stream(it->path());
        if (!stream) {
            check(false, "producer scan cannot read " + it->path().string());
            continue;
        }

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string content = stripComments(buffer.str());

        ++files;
        for (std::sregex_iterator match(
                 content.begin(), content.end(), callPattern),
             last;
             match != last; ++match) {
            ++calls;
            const std::string argument = (*match)[1].str();
            if (argument.rfind("rtengine::", 0) != 0) {
                const std::size_t offset =
                    static_cast<std::size_t>(match->position());
                const std::size_t line = static_cast<std::size_t>(
                    std::count(content.begin(), content.begin() + offset,
                               '\n')) + 1;
                violations.push_back(relative + ":" + std::to_string(line)
                                     + ": " + argument);
            }
        }
    }

    check(!error, "producer scan completed: " + error.message());
    check(files > 0, "producer scan found GUI sources");
    check(calls > 0, "producer scan found typed producer calls");
    for (const std::string& violation : violations) {
        check(false, "legacy newEvent producer call: " + violation);
    }

    std::printf("editorrequest-test: producer scan: %zu files, %zu calls\n",
                files, calls);
}

void checkIdentity()
{
    /*
     * The identity is a planning identity: two different masks that carry no
     * planning information must have the same identity. The raw legacy mask
     * is deliberately excluded; every mask-shaped planning fact is included.
     */
    const EditorRequest plain = from_legacy_mask(M_LUMINANCE);
    const EditorRequest color = from_legacy_mask(M_COLOR);
    check(plain.identity() == color.identity(),
          "identity ignores the raw legacy mask");
    check(plain.identity()
              == coalesce(plain, color).identity(),
          "coalescing planning-equivalent requests keeps the identity");

    check(plain.identity() != from_legacy_mask(M_SPOT).identity(),
          "spot intent changes the identity");
    check(plain.identity() != from_legacy_mask(M_INIT).identity(),
          "full-frame intent changes the identity");
    check(plain.identity() != from_legacy_mask(M_HIGHQUAL).identity(),
          "high-detail intent changes the identity");
    check(plain.identity() != from_legacy_mask(M_MINUPDATE).identity(),
          "kind changes the identity");

    LegacyMaskContext moved;
    moved.viewportMoved = true;
    check(plain.identity() != from_legacy_mask(M_LUMINANCE, moved).identity(),
          "viewportMoved changes the identity");

    /*
     * The new request facts are planning fields. An auto-exposure request
     * recomputes different work from a plain Lab request, and a transform or
     * HDR request differs from both.
     */
    check(plain.identity() != from_legacy_mask(M_AUTOEXP).identity(),
          "autoExposureIntent changes the identity");
    check(from_legacy_mask(M_TRANSFORM).identity()
              != from_legacy_mask(M_LUMINANCE).identity(),
          "transformFamilyIntent changes the identity");
    check(from_legacy_mask(M_HDR).identity()
              != from_legacy_mask(M_INIT).identity(),
          "hdrIntent changes the identity");
    check(from_legacy_mask(M_RGBCURVE).identity()
              != from_legacy_mask(M_LUMINANCE).identity(),
          "rgbProcIntent changes the identity");
    check(from_legacy_mask(M_CROP).identity()
              != from_legacy_mask(M_LUMINANCE).identity(),
          "localAdjustmentsIntent changes the identity");

    /*
     * Mask-shaped facts. M_WB and M_AUTOEXP plan differently (the Color/Coloropp
     * DEMOSAIC path), and M_MONITOR and M_COLOR plan differently (full-frame
     * admission), so neither pair may share an identity.
     */
    check(from_legacy_mask(M_WB).identity()
              != from_legacy_mask(M_AUTOEXP).identity(),
          "colorHlrWbRequest changes the identity");
    check(from_legacy_mask(M_MONITOR).identity()
              != from_legacy_mask(M_COLOR).identity(),
          "fullFrameAdmission changes the identity");
    check(from_legacy_mask(M_COLOR).identity()
              != from_legacy_mask(M_CROP).identity(),
          "monitorOutputRequest changes the identity");

    /* The imperative-repaint fact is a planning field of its own. */
    EditorRequest imperative = from_legacy_mask(M_MINUPDATE);
    EditorRequest notImperative = imperative;
    notImperative.imperativeRepaint = false;
    check(imperative.identity() != notImperative.identity(),
          "imperativeRepaint changes the identity");
}

/*
 * The adapter must abort on a contract violation in every build, not only
 * where NDEBUG leaves the assertion in. Run the call in a child process.
 */
void expectAbort(int mask, const char* label)
{
    const pid_t pid = fork();

    if (pid == 0) {
        from_legacy_mask(mask);
        _exit(0);
    }

    if (pid < 0) {
        check(false, std::string(label) + ": fork failed");
        return;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        check(false, std::string(label) + ": waitpid failed");
        return;
    }

    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          std::string(label) + ": expected an abort");
}

void checkContractViolations()
{
    expectAbort(1 << 21, "unknown bit");
    expectAbort(M_MINUPDATE | M_CROP, "combined M_MINUPDATE");
    expectAbort(M_MINUPDATE | M_VOID, "M_MINUPDATE with M_VOID");
    expectAbort(M_MINUPDATE | M_HIGHQUAL, "M_MINUPDATE with M_HIGHQUAL");
}

} // namespace

int main()
{
    checkEveryBit();
    checkViewportFacts();
    checkViewportAdmission();
    checkControlAndImperative();
    checkCombinedBits();
    checkEveryPair();
    checkFactUnion();
    checkEmptyAccumulator();
    checkCoordinatorPatches();
    checkViewportMovedContext();
    checkProducerIntents();
    checkProducerDirtyBlocks();
    checkRefreshMapper();
    checkProducerEventKey();
    checkFastPathAugmentation();
    checkCropReallocationWorkOrder();
    checkConvertedProducerIntents();
    checkNoLegacyProducerCalls();
    checkIdentity();
    checkContractViolations();

    if (failures == 0) {
        std::puts("editorrequest-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
