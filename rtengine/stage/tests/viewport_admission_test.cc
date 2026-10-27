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
 * Decision-table replay for viewport fast-path admission and LUT selection.
 *
 * The typed request is the only implementation in the engine. This test keeps
 * the pre-retirement mask-based decision as a local oracle and proves that the
 * two agree:
 *
 * - exhaustively over every 21-bit request mask the adapter can accept;
 * - over every producer intent, every pair of intents, the coalesced pair, and
 *   the cross product of reachable request kinds and families;
 * - over the deferred-analysis flush shape and the coordinator's request
 *   augmentations;
 * - over the parameter-veto matrix, crossed with the coordinator-state matrix.
 *
 * The oracle is deliberately a transcription of the hand-written gate
 * (including its reason strings and its order), so a transcription error in
 * the typed policy shows up as a divergence rather than as two copies of the
 * same mistake. It never ships: the engine has no mask-based gate left.
 */

#include "rtengine/editorrequest.h"

#include "rtengine/procparams.h"
#include "rtengine/refreshmap.h"
#include "rtengine/stage/stage_dirty_blocks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{

using rtengine::EditorRequest;
using rtengine::RequestIntent;
using rtengine::ViewportAdmissionInputs;
using rtengine::ViewportAdmissionState;
using rtengine::ViewportCropSelection;
using rtengine::ViewportEligibility;
using rtengine::ViewportLutSelection;
using rtengine::coalesce;
using rtengine::compatibility_mask;
using rtengine::from_legacy_mask;
using rtengine::make_request;
using rtengine::resolveCropDependencies;
using rtengine::sameHighlightRecovery;
using rtengine::viewportCropPayload;
using rtengine::viewportCropSelection;
using rtengine::viewportCropTodo;
using rtengine::viewportEligibility;
using rtengine::viewportEligibilityReason;
using rtengine::viewportFastPathFallback;
using rtengine::viewportLutSelection;

using rtengine::procparams::ProcParams;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

/*
 * The pre-retirement gate, transcribed.
 *
 * These constants and functions are the oracle. Do not "improve" them: their
 * only purpose is to say what the old code decided.
 */
constexpr int LEGACY_ALLOWED_TODO =
    M_AUTOEXP | M_RGBCURVE | M_LUMACURVE | M_LUMINANCE | M_COLOR
    | M_CROP | M_MINUPDATE | M_HDR | M_SPOT;

constexpr int LEGACY_REJECTED_TODO =
    M_PREPROC | M_RAW | M_WB | M_INIT | M_LINDENOISE | M_MONITOR
    | M_TRANSFORM | M_CSHARP | M_RETINEX | M_HIGHQUAL | M_BLURMAP;

constexpr int LEGACY_CROP_TODO =
    M_CROP | M_MINUPDATE | M_AUTOEXP | M_TRANSFORM | M_RGBCURVE
    | M_LUMACURVE | M_LUMINANCE | M_COLOR;

enum class LegacyEligibility {
    Admit,
    Base,
    Unsupported
};

const char* legacyEligibilityReason(LegacyEligibility eligibility)
{
    switch (eligibility) {
    case LegacyEligibility::Admit:
        return nullptr;
    case LegacyEligibility::Base:
        return "base-request";
    case LegacyEligibility::Unsupported:
        return "unsupported-request";
    }
    return "unsupported-request";
}

LegacyEligibility legacyEligibility(int todo)
{
    if ((todo & LEGACY_REJECTED_TODO) != 0) {
        return LegacyEligibility::Base;
    }
    if ((todo & ~LEGACY_ALLOWED_TODO) != 0) {
        return LegacyEligibility::Unsupported;
    }
    return LegacyEligibility::Admit;
}

struct LegacyLutSelection {
    bool toneCurves = false;
    bool labCurves = false;
    bool lumaCurveFromCrop = false;
    bool wavelet = false;
};

LegacyLutSelection legacyLutSelection(int todo)
{
    LegacyLutSelection selection;

    selection.toneCurves = (todo & (M_RGBCURVE | M_CROP)) != 0;
    selection.labCurves = (todo & M_LUMACURVE) != 0;
    selection.lumaCurveFromCrop = (todo & M_CROP) != 0;
    selection.wavelet =
        (todo & (M_LUMINANCE | M_COLOR | M_AUTOEXP)) != 0;

    return selection;
}

bool legacyRebuildsLumaCurve(const LegacyLutSelection& selection)
{
    return selection.labCurves || selection.lumaCurveFromCrop;
}

int legacyCropTodo(int todo)
{
    int cropTodo = todo & LEGACY_CROP_TODO;

    if ((cropTodo & (M_LUMACURVE | M_AUTOEXP)) != 0) {
        cropTodo |= M_LUMINANCE | M_COLOR;
    }

    return cropTodo;
}

int legacyCropPayload(int todo)
{
    if (((todo & LEGACY_CROP_TODO) & (M_LUMACURVE | M_AUTOEXP)) != 0) {
        return todo | M_LUMINANCE | M_COLOR;
    }

    return todo;
}

/*
 * The full gate, transcribed in the order the hand-written function applied
 * its checks. A null return means "admit".
 */
const char* legacyFallback(int todo, const ViewportAdmissionState& state,
                           const ProcParams& params,
                           const ProcParams& last,
                           const ProcParams& lastLut)
{
    if (!state.fastPathEnabled) {
        return "switch-off";
    }
    if (!state.fullStateComplete) {
        return "no-complete-full-state";
    }
    if (state.highDetailNeeded != state.highDetailComplete) {
        return "high-detail-transition";
    }
    if (state.highDetailNeeded
            && (!state.highDetailPreprocessComputed
                || !state.highDetailRawComputed)) {
        return "high-detail-pending";
    }
    if ((todo & LEGACY_REJECTED_TODO) != 0) {
        return "base-request";
    }
    if ((todo & ~LEGACY_ALLOWED_TODO) != 0) {
        return "unsupported-request";
    }

    if (params.toneCurve.autoexp || last.toneCurve.autoexp) {
        return "auto-exposure";
    }
    if (params.toneCurve.histmatching || last.toneCurve.histmatching
            || params.toneCurve.fromHistMatching
            || last.toneCurve.fromHistMatching) {
        return "histogram-matching";
    }
    if ((params.blackwhite.enabled && params.blackwhite.autoc)
            || (last.blackwhite.enabled && last.blackwhite.autoc)) {
        return "auto-black-white";
    }
    if ((params.colorToning.enabled && params.colorToning.autosat)
            || (last.colorToning.enabled && last.colorToning.autosat)) {
        return "auto-colour-toning";
    }
    if (params.colorappearance.enabled) {
        return "cie";
    }
    if (params.locallab.enabled) {
        return "local-lab";
    }
    if (params.labCurve.contrast != 0) {
        return "lab-contrast";
    }
    if (params.fattal.enabled || params.dehaze.enabled) {
        return "hdr-processing";
    }
    if (params.spot.enabled && !params.spot.entries.empty()) {
        return "spot-removal";
    }
    if (params.raw != last.raw) {
        return "raw";
    }
    if (params.wb != last.wb) {
        return "white-balance";
    }
    if (params.icm != last.icm) {
        return "color-management";
    }
    if (params.coarse != last.coarse) {
        return "coarse-transform";
    }
    if (params.commonTrans != last.commonTrans) {
        return "common-transform";
    }
    if (params.rotate != last.rotate) {
        return "rotate";
    }
    if (params.distortion != last.distortion) {
        return "distortion";
    }
    if (params.lensProf != last.lensProf) {
        return "lens-profile";
    }
    if (params.perspective != last.perspective) {
        return "perspective";
    }
    if (params.gradient != last.gradient) {
        return "gradient";
    }
    if (params.pcvignette != last.pcvignette) {
        return "post-crop-vignette";
    }
    if (params.cacorrection != last.cacorrection) {
        return "ca-correction";
    }
    if (params.vignetting != last.vignetting) {
        return "vignetting";
    }
    if (params.spot != last.spot) {
        return "spot";
    }
    if (params.filmNegative != last.filmNegative) {
        return "film-negative";
    }
    if (params.dehaze != last.dehaze) {
        return "dehaze";
    }
    if (params.fattal != last.fattal) {
        return "fattal";
    }
    if (params.retinex != last.retinex) {
        return "retinex";
    }
    if (params.pdsharpening != last.pdsharpening) {
        return "capture-sharpening";
    }
    if (params.dirpyrDenoise != last.dirpyrDenoise) {
        return "denoise";
    }
    if (params.cg != last.cg) {
        return "gamut-compression";
    }
    if (params.locallab != last.locallab) {
        return "local-lab-params";
    }
    if (!sameHighlightRecovery(params.toneCurve, last.toneCurve)) {
        return "highlight-recovery";
    }
    if (params.toneCurve.autoexp != last.toneCurve.autoexp
            || params.toneCurve.histmatching
                != last.toneCurve.histmatching
            || params.blackwhite.autoc != last.blackwhite.autoc
            || params.colorToning.autosat != last.colorToning.autosat) {
        return "auto-flags";
    }
    if ((params.toneCurve != lastLut.toneCurve
            || params.rgbCurves != lastLut.rgbCurves
            || params.colorToning != lastLut.colorToning
            || params.blackwhite != lastLut.blackwhite)
            && (todo & M_RGBCURVE) == 0) {
        return "tone-curve-mask";
    }
    if (params.labCurve != lastLut.labCurve && (todo & M_LUMACURVE) == 0) {
        return "lab-curve-mask";
    }
    if (params.wavelet != lastLut.wavelet
            && (todo & (M_LUMINANCE | M_COLOR)) == 0) {
        return "wavelet-mask";
    }

    return nullptr;
}

bool sameReason(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

std::string reasonText(const char* reason)
{
    return reason == nullptr ? std::string("admit") : std::string(reason);
}

struct Counters {
    long long masks = 0;
    long long shapes = 0;
    long long paramStates = 0;
    long long admissionStates = 0;
    long long fallbackCases = 0;
    long long selectionCases = 0;
};

Counters counters;

/*
 * Compare the typed and legacy selections for one request and its mask.
 *
 * `label` names the shape; the caller has already checked that the request
 * carries `mask` unchanged, so the oracle can be evaluated on the mask.
 */
void compareSelection(const EditorRequest& request, int mask,
                      const std::string& label)
{
    counters.selectionCases++;

    const LegacyLutSelection legacy = legacyLutSelection(mask);
    const ViewportLutSelection typed = viewportLutSelection(request);

    check(typed.toneCurves == legacy.toneCurves, label + ": tone LUTs");
    check(typed.labCurves == legacy.labCurves, label + ": Lab LUTs");
    check(typed.lumaCurveFromCrop == legacy.lumaCurveFromCrop,
          label + ": crop luma LUT");
    check(typed.rebuildsLumaCurve() == legacyRebuildsLumaCurve(legacy),
          label + ": lumacurve rebuild");
    check(typed.wavelet == legacy.wavelet, label + ": wavelet LUT");

    ViewportCropSelection crop = viewportCropSelection(request);
    resolveCropDependencies(crop);

    check(viewportCropTodo(crop) == legacyCropTodo(mask),
          label + ": crop todo");
    check(viewportCropPayload(mask, crop) == legacyCropPayload(mask),
          label + ": crop payload");
}

void compareEligibility(const EditorRequest& request, int mask,
                        const std::string& label)
{
    const LegacyEligibility legacy = legacyEligibility(mask);
    const ViewportEligibility typed = viewportEligibility(request);

    check(sameReason(viewportEligibilityReason(typed),
                     legacyEligibilityReason(legacy)),
          label + ": eligibility ("
              + reasonText(legacyEligibilityReason(legacy)) + " vs "
              + reasonText(viewportEligibilityReason(typed)) + ")");
}

void compareFallback(const EditorRequest& request, int mask,
                     const ViewportAdmissionState& state,
                     const ProcParams& params, const ProcParams& last,
                     const ProcParams& lastLut, const std::string& label)
{
    counters.fallbackCases++;

    ViewportAdmissionInputs inputs{params, last, lastLut, state};
    const char* legacy = legacyFallback(mask, state, params, last, lastLut);
    const char* typed = viewportFastPathFallback(request, inputs);

    check(sameReason(legacy, typed),
          label + ": fallback (" + reasonText(legacy) + " vs "
              + reasonText(typed) + ")");
}

/* One request shape: the request and the mask the adapter would have seen. */
struct Shape {
    std::string name;
    EditorRequest request;
    int mask;
};

/*
 * Every mask the adapter can accept, except zero.
 *
 * A combined M_MINUPDATE is a contract violation the adapter aborts on, so it
 * is not a request shape. Zero is excluded and covered separately: the
 * coordinator's loop stops on an empty accumulator, so no absent request
 * reaches admission.
 */
void checkEveryMask()
{
    constexpr int lastBit = 21;

    for (int mask = 1; mask < (1 << lastBit); ++mask) {
        if ((mask & M_MINUPDATE) != 0 && mask != M_MINUPDATE) {
            continue;
        }

        const EditorRequest request = from_legacy_mask(mask);
        counters.masks++;

        compareEligibility(request, mask, "mask");
        compareSelection(request, mask, "mask");
    }

    std::printf("viewport-admission-test: exhaustive masks: %lld\n",
                counters.masks);
}

/* Every producer intent, and the compatibility mask it carries. */
const std::vector<Shape>& intentShapes()
{
    struct IntentRow {
        RequestIntent intent;
        const char* name;
    };
    static const IntentRow rows[] = {
        {RequestIntent::AllNoRaw, "AllNoRaw"},
        {RequestIntent::Demosaic, "Demosaic"},
        {RequestIntent::Hdr, "Hdr"},
        {RequestIntent::SpotAdjust, "SpotAdjust"},
        {RequestIntent::Transform, "Transform"},
        {RequestIntent::WhiteBalance, "WhiteBalance"},
        {RequestIntent::RepaintOnly, "RepaintOnly"},
        {RequestIntent::ControlOnly, "ControlOnly"},
        {RequestIntent::Compr, "Compr"},
        {RequestIntent::CaptureSharpen, "CaptureSharpen"},
        {RequestIntent::DarkFrame, "DarkFrame"},
        {RequestIntent::First, "First"},
        {RequestIntent::AutoExposure, "AutoExposure"},
        {RequestIntent::LuminanceCurve, "LuminanceCurve"},
        {RequestIntent::DirPyrEqualizer, "DirPyrEqualizer"},
        {RequestIntent::RgbCurve, "RgbCurve"},
        {RequestIntent::Sharpening, "Sharpening"}
    };

    static std::vector<Shape> shapes;
    if (!shapes.empty()) {
        return shapes;
    }

    for (const IntentRow& row : rows) {
        shapes.push_back({std::string("intent ") + row.name,
                          make_request(row.intent),
                          compatibility_mask(row.intent)});
    }

    return shapes;
}

/* Every reachable request kind, crossed with every family subset. */
const std::vector<Shape>& kindFamilyShapes()
{
    static std::vector<Shape> shapes;
    if (!shapes.empty()) {
        return shapes;
    }

    /*
     * Kinds the preview consumes. FullCrop is a crop-path kind, not a preview
     * request kind, so it cannot reach this gate.
     */
    const int kindBits[] = {
        0,
        M_MINUPDATE,
        M_INIT,
        M_HIGHQUAL,
        M_INIT | M_HIGHQUAL
    };
    const char* kindNames[] = {
        "ParamsOnly", "RepaintOnly", "FullPreview", "HighQuality",
        "HighQuality+FullPreview"
    };

    /* One representative bit per viewport family. */
    const int familyBits[] = {
        M_CROP, M_RGBCURVE, M_LUMACURVE, M_LUMINANCE, M_COLOR,
        M_AUTOEXP, M_SPOT, M_HDR
    };
    constexpr int familyCount = sizeof(familyBits) / sizeof(familyBits[0]);

    for (int k = 0; k < 5; ++k) {
        for (int subset = 0; subset < (1 << familyCount); ++subset) {
            int mask = kindBits[k];

            for (int bit = 0; bit < familyCount; ++bit) {
                if ((subset & (1 << bit)) != 0) {
                    mask |= familyBits[bit];
                }
            }

            if ((mask & M_MINUPDATE) != 0 && mask != M_MINUPDATE) {
                continue;
            }

            const std::string name = std::string("kind ")
                + kindNames[k] + " families " + std::to_string(subset);

            EditorRequest request;
            if (mask == 0) {
                /*
                 * An empty family with a delta kind is the bare-parameter
                 * shape; the adapter represents it as an absent request.
                 */
                continue;
            }
            request = from_legacy_mask(mask);

            shapes.push_back({name, request, mask});
        }
    }

    return shapes;
}

/*
 * Intent pairs and the coalesced request they produce.
 *
 * `coalesce()` must keep the planning data of the ORed mask: kind, facts,
 * admission, LUT selection and crop selection.
 */
void checkIntentPairs(std::vector<Shape>& shapes)
{
    const std::vector<Shape>& intents = intentShapes();
    long long pairs = 0;

    for (const Shape& left : intents) {
        for (const Shape& right : intents) {
            const EditorRequest merged =
                coalesce(left.request, right.request);
            const int unionMask = left.mask | right.mask;

            const std::string label =
                "pair " + left.name + "+" + right.name;

            check(merged.legacyGateMask == unionMask,
                  label + ": coalesced mask");
            compareEligibility(merged, unionMask, label);
            compareSelection(merged, unionMask, label);

            /*
             * The same pair as a request shape for the full fallback matrix.
             * A coalesced M_MINUPDATE beside other work is not a mask the
             * adapter would accept, but it is exactly what the coordinator
             * accumulates, so the oracle is evaluated on the mask directly.
             */
            shapes.push_back({label, merged, unionMask});
            ++pairs;
        }
    }

    std::printf("viewport-admission-test: intent pairs: %lld\n", pairs);
}

/*
 * The deferred-analysis flush and the coordinator's request augmentations.
 *
 * These are the shapes the coordinator builds itself; each carries the
 * execution payload the legacy gate would have seen.
 */
void checkCoordinatorShapes(std::vector<Shape>& shapes)
{
    {
        /* deferredAnalysisRequest(): ParamsOnly with the forced analysis. */
        EditorRequest request;
        request.present = true;
        request.controlOnly = false;
        request.kind = rtengine::stage::RequestKind::ParamsOnly;
        request.setLegacyGateMask(
            M_MONITOR | M_RGBCURVE | M_LUMACURVE | M_LUMINANCE | M_COLOR);
        shapes.push_back({"deferred-flush", request,
                          request.legacyGateMask});
    }
    {
        /* PC-vignette augmentation: a crop change derives a transform. */
        EditorRequest request = from_legacy_mask(M_CROP);
        request.setLegacyGateMask(M_CROP | TRANSFORM);
        request.refreshFromLegacyMask();
        shapes.push_back({"pc-vignette", request, request.legacyGateMask});
    }
    {
        /* Panning requeue: a bare repaint gains M_MONITOR. */
        EditorRequest request = from_legacy_mask(M_MINUPDATE);
        request.setLegacyGateMask(M_MINUPDATE | M_MONITOR);
        request.viewportMoved = true;
        request.kind = rtengine::stage::RequestKind::ParamsOnly;
        shapes.push_back({"panning-requeue", request, request.legacyGateMask});
    }
    {
        /* Base refresh: a crop change forces the base block. */
        EditorRequest request = from_legacy_mask(M_CROP);
        request.setLegacyGateMask(M_CROP | M_INIT | M_HDR | M_MONITOR);
        request.refreshFromLegacyMask();
        shapes.push_back({"base-refresh", request, request.legacyGateMask});
    }
    {
        /* Deferred absorb: a repaint absorbs the queued analysis. */
        EditorRequest request = from_legacy_mask(M_MINUPDATE);
        request.setLegacyGateMask(
            M_MINUPDATE | M_MONITOR | M_RGBCURVE | M_LUMACURVE
            | M_LUMINANCE | M_COLOR);
        request.kind = rtengine::stage::RequestKind::ParamsOnly;
        shapes.push_back({"deferred-absorb", request, request.legacyGateMask});
    }
    {
        /* A bare crop with the Lab widening applied by prepareViewportLuts. */
        EditorRequest request = from_legacy_mask(M_CROP);
        ViewportCropSelection crop = viewportCropSelection(request);
        resolveCropDependencies(crop);
        request.setLegacyGateMask(
            viewportCropPayload(request.legacyGateMask, crop));
        shapes.push_back({"crop-lab-widening", request,
                          request.legacyGateMask});
    }
}

/*
 * A parameter set the gate compares: the current request, the last completed
 * full-frame pass and the parameters of the last parent-LUT rebuild.
 */
struct ParamCase {
    std::string name;
    ProcParams current;
    ProcParams last;
    ProcParams lastLut;
    /* The reason a plain crop request must report for this state. */
    std::string expected;
};

ProcParams baselineParams()
{
    ProcParams params;

    /* Every veto trigger starts off, so the baseline is admissible. */
    params.toneCurve.autoexp = false;
    params.toneCurve.histmatching = false;
    params.toneCurve.fromHistMatching = false;
    params.blackwhite.enabled = false;
    params.blackwhite.autoc = false;
    params.colorToning.enabled = false;
    params.colorToning.autosat = false;
    params.colorappearance.enabled = false;
    params.locallab.enabled = false;
    params.labCurve.contrast = 0;
    params.fattal.enabled = false;
    params.dehaze.enabled = false;
    params.spot.enabled = false;
    params.spot.entries.clear();

    return params;
}

std::vector<ParamCase> paramCases()
{
    enum class Target { Current, Last, LastLut };

    std::vector<ParamCase> cases;
    const ProcParams base = baselineParams();

    auto add = [&cases, &base](const std::string& name,
                               const std::string& expected,
                               const std::function<void(ProcParams&)>& mutate,
                               Target target = Target::Current) {
        ParamCase entry;
        entry.name = name;
        entry.current = base;
        entry.last = base;
        entry.lastLut = base;
        switch (target) {
        case Target::Current:
            mutate(entry.current);
            break;
        case Target::Last:
            mutate(entry.last);
            break;
        case Target::LastLut:
            mutate(entry.lastLut);
            break;
        }
        entry.expected = expected;
        cases.push_back(entry);
    };

    add("baseline", "admit", [](ProcParams&) {});

    add("auto-exposure-current", "auto-exposure",
        [](ProcParams& p) { p.toneCurve.autoexp = true; });
    add("auto-exposure-last", "auto-exposure",
        [](ProcParams& p) { p.toneCurve.autoexp = true; }, Target::Last);
    add("histogram-matching-current", "histogram-matching",
        [](ProcParams& p) { p.toneCurve.histmatching = true; });
    add("histogram-matching-last", "histogram-matching",
        [](ProcParams& p) { p.toneCurve.histmatching = true; }, Target::Last);
    add("from-hist-matching", "histogram-matching",
        [](ProcParams& p) { p.toneCurve.fromHistMatching = true; });
    add("auto-black-white", "auto-black-white", [](ProcParams& p) {
        p.blackwhite.enabled = true;
        p.blackwhite.autoc = true;
    });
    add("auto-colour-toning", "auto-colour-toning", [](ProcParams& p) {
        p.colorToning.enabled = true;
        p.colorToning.autosat = true;
    });
    add("cie", "cie", [](ProcParams& p) {
        p.colorappearance.enabled = true;
    });
    add("local-lab", "local-lab", [](ProcParams& p) {
        p.locallab.enabled = true;
    });
    add("lab-contrast", "lab-contrast",
        [](ProcParams& p) { p.labCurve.contrast = 20; });
    add("fattal-enabled", "hdr-processing",
        [](ProcParams& p) { p.fattal.enabled = true; });
    add("dehaze-enabled", "hdr-processing",
        [](ProcParams& p) { p.dehaze.enabled = true; });
    add("spot-removal", "spot-removal", [](ProcParams& p) {
        p.spot.enabled = true;
        p.spot.entries.emplace_back();
    });

    add("raw", "raw",
        [](ProcParams& p) { p.raw.bayersensor.imageNum += 1; });
    add("white-balance", "white-balance",
        [](ProcParams& p) { p.wb.temperature += 1; });
    add("color-management", "color-management",
        [](ProcParams& p) { p.icm.workingProfile = "ProPhoto"; });
    add("coarse-transform", "coarse-transform",
        [](ProcParams& p) { p.coarse.rotate = (p.coarse.rotate + 90) % 360; });
    add("common-transform", "common-transform",
        [](ProcParams& p) { p.commonTrans.scale += 0.01; });
    add("rotate", "rotate",
        [](ProcParams& p) { p.rotate.degree += 1.0; });
    add("distortion", "distortion",
        [](ProcParams& p) { p.distortion.amount += 0.01; });
    add("lens-profile", "lens-profile", [](ProcParams& p) {
        p.lensProf.lcMode =
            rtengine::procparams::LensProfParams::LcMode::LENSFUNMANUAL;
    });
    add("perspective", "perspective",
        [](ProcParams& p) { p.perspective.horizontal += 1.0; });
    add("gradient", "gradient",
        [](ProcParams& p) { p.gradient.strength += 1.0; });
    add("post-crop-vignette", "post-crop-vignette",
        [](ProcParams& p) { p.pcvignette.strength += 1.0; });
    add("ca-correction", "ca-correction",
        [](ProcParams& p) { p.cacorrection.red += 0.01; });
    add("vignetting", "vignetting",
        [](ProcParams& p) { p.vignetting.amount += 1; });
    add("spot-params", "spot",
        [](ProcParams& p) { p.spot.entries.emplace_back(); });
    add("film-negative", "film-negative",
        [](ProcParams& p) { p.filmNegative.redRatio += 0.01; });
    add("dehaze", "dehaze",
        [](ProcParams& p) { p.dehaze.strength += 1; });
    add("fattal", "fattal",
        [](ProcParams& p) { p.fattal.amount += 1; });
    add("retinex", "retinex",
        [](ProcParams& p) { p.retinex.str += 1; });
    add("capture-sharpening", "capture-sharpening",
        [](ProcParams& p) { p.pdsharpening.contrast += 0.1; });
    add("denoise", "denoise",
        [](ProcParams& p) { p.dirpyrDenoise.luma += 0.1; });
    add("gamut-compression", "gamut-compression",
        [](ProcParams& p) { p.cg.enabled = true; });
    add("local-lab-params", "local-lab-params",
        [](ProcParams& p) { p.locallab.selspot += 1; });
    add("highlight-recovery", "highlight-recovery",
        [](ProcParams& p) { p.toneCurve.hlbl += 1; });
    add("auto-flags", "auto-flags",
        [](ProcParams& p) { p.blackwhite.autoc = true; }, Target::Last);

    /*
     * The LUT-mask vetoes compare against the last parent-LUT rebuild, not
     * against the last full pass.
     */
    add("tone-curve-mask", "tone-curve-mask",
        [](ProcParams& p) { p.toneCurve.expcomp += 0.1; }, Target::LastLut);
    add("lab-curve-mask", "lab-curve-mask",
        [](ProcParams& p) { p.labCurve.brightness += 1; }, Target::LastLut);
    add("wavelet-mask", "wavelet-mask",
        [](ProcParams& p) { p.wavelet.enabled = true; }, Target::LastLut);

    {
        ParamCase all;
        all.name = "all-vetoes";
        all.current = base;
        all.last = base;
        all.lastLut = base;
        all.current.toneCurve.autoexp = true;
        all.current.colorappearance.enabled = true;
        all.current.locallab.enabled = true;
        all.current.labCurve.contrast = 20;
        all.current.fattal.enabled = true;
        all.current.dehaze.enabled = true;
        all.current.raw.bayersensor.imageNum += 1;
        all.expected = "auto-exposure";
        cases.push_back(all);
    }

    return cases;
}

/* The coordinator-state matrix. */
std::vector<ViewportAdmissionState> admissionStates()
{
    std::vector<ViewportAdmissionState> states;

    for (int enabled = 0; enabled <= 1; ++enabled) {
        for (int complete = 0; complete <= 1; ++complete) {
            for (int needed = 0; needed <= 1; ++needed) {
                for (int decided = 0; decided <= 1; ++decided) {
                    for (int computed = 0; computed <= 1; ++computed) {
                        ViewportAdmissionState state;
                        state.fastPathEnabled = enabled != 0;
                        state.fullStateComplete = complete != 0;
                        state.highDetailNeeded = needed != 0;
                        state.highDetailComplete = decided != 0;
                        state.highDetailPreprocessComputed = computed != 0;
                        state.highDetailRawComputed = computed != 0;
                        states.push_back(state);
                    }
                }
            }
        }
    }

    return states;
}

void checkParamMatrix(const std::vector<Shape>& shapes)
{
    const std::vector<ParamCase> params = paramCases();
    const std::vector<ViewportAdmissionState> states = admissionStates();
    const EditorRequest crop = from_legacy_mask(M_CROP);

    counters.paramStates = static_cast<long long>(params.size());
    counters.admissionStates = static_cast<long long>(states.size());

    for (const ParamCase& entry : params) {
        /*
         * The reason table, probed with a plain crop request under an
         * admissible coordinator state.
         */
        ViewportAdmissionState admitState;
        admitState.fastPathEnabled = true;
        admitState.fullStateComplete = true;
        admitState.highDetailNeeded = false;
        admitState.highDetailComplete = false;

        ViewportAdmissionInputs probe{entry.current, entry.last,
                                      entry.lastLut, admitState};
        const char* probeReason = viewportFastPathFallback(crop, probe);
        check(reasonText(probeReason) == entry.expected,
              "reason table " + entry.name + ": expected " + entry.expected
                  + ", got " + reasonText(probeReason));

        for (const ViewportAdmissionState& state : states) {
            for (const Shape& shape : shapes) {
                compareFallback(shape.request, shape.mask, state,
                                entry.current, entry.last, entry.lastLut,
                                shape.name + " / " + entry.name);
            }
        }
    }

    std::printf(
        "viewport-admission-test: fallback matrix: %lld shapes x %lld params"
        " states x %lld admission states\n",
        static_cast<long long>(shapes.size()), counters.paramStates,
        counters.admissionStates);
    std::printf("viewport-admission-test: fallback comparisons: %lld\n",
                counters.fallbackCases);
}

/*
 * The shapes whose legacy payload is their own compatibility mask. Every
 * builder above maintains that invariant; a shape that breaks it would make
 * the oracle compare two different requests.
 */
void checkShapeMasks(const std::vector<Shape>& shapes)
{
    for (const Shape& shape : shapes) {
        check(shape.request.legacyGateMask == shape.mask,
              shape.name + ": shape mask matches the adapter payload");
    }
}

/*
 * Producer-provided dirty blocks are the one input the legacy gate never had.
 * They narrow admission for a typed producer and must not widen it.
 */
void checkProducerBlocks()
{
    const EditorRequest delta = make_request(
        RequestIntent::RgbCurve, {"rgb.proc"},
        rtengine::stage::StagePath::Preview);
    check(delta.dirtySource == EditorRequest::DirtySource::ProducerProvided,
          "producer blocks are carried");
    check(viewportEligibility(delta) == ViewportEligibility::Admit,
          "a producer delta block is admissible");

    const EditorRequest baseSeed = make_request(
        RequestIntent::RgbCurve, {"raw.base"},
        rtengine::stage::StagePath::Preview);
    check(viewportEligibility(baseSeed) == ViewportEligibility::Base,
          "a producer base block is rejected as a base request");

    const EditorRequest transformSeed = make_request(
        RequestIntent::RgbCurve, {"transform"},
        rtengine::stage::StagePath::Preview);
    check(viewportEligibility(transformSeed) == ViewportEligibility::Base,
          "a producer transform block is rejected as a base request");

    const EditorRequest foreign = make_request(
        RequestIntent::RgbCurve, {"lab.sharpening"},
        rtengine::stage::StagePath::Crop);
    check(viewportEligibility(foreign) == ViewportEligibility::Unsupported,
          "a block outside the preview path is unsupported");

    EditorRequest invalid = make_request(RequestIntent::RgbCurve);
    invalid.dirtySource = EditorRequest::DirtySource::ProducerProvided;
    invalid.dirty.valid = false;
    check(viewportEligibility(invalid) == ViewportEligibility::Unsupported,
          "an invalid producer dirty set is unsupported");
}

/* The shapes the coordinator never passes to admission, checked explicitly. */
void checkUnreachableShapes()
{
    const EditorRequest absent;
    check(!absent.present, "an empty accumulator is absent");
    check(viewportEligibility(absent) == ViewportEligibility::Unsupported,
          "an absent request is not admissible");

    const EditorRequest control = from_legacy_mask(M_VOID);
    check(viewportEligibility(control) == ViewportEligibility::Unsupported,
          "a control-only request is unsupported");

    const EditorRequest high = from_legacy_mask(M_HIGHQUAL);
    check(viewportEligibility(high) == ViewportEligibility::Base,
          "a high-detail request is a base request");

    /*
     * M_HDR on its own is admissible: it is the over-approximated bit several
     * GUI events carry, and the parameter vetoes decide whether the HDR
     * inputs actually moved.
     */
    const EditorRequest hdr = from_legacy_mask(M_HDR);
    check(viewportEligibility(hdr) == ViewportEligibility::Admit,
          "a bare HDR over-approximation is admissible");
    check(hdr.kind == EditorRequest::RequestKind::FullPreview,
          "a bare HDR over-approximation still plans a full preview");
}

} // namespace

int main()
{
    std::printf("viewport-admission-test: typed admission decision table\n");

    checkEveryMask();

    std::vector<Shape> shapes;
    for (const Shape& shape : intentShapes()) {
        shapes.push_back(shape);
    }
    for (const Shape& shape : kindFamilyShapes()) {
        shapes.push_back(shape);
    }
    checkIntentPairs(shapes);
    checkCoordinatorShapes(shapes);
    checkShapeMasks(shapes);

    counters.shapes = static_cast<long long>(shapes.size());

    for (const Shape& shape : shapes) {
        compareEligibility(shape.request, shape.mask, shape.name);
        compareSelection(shape.request, shape.mask, shape.name);
    }

    checkProducerBlocks();
    checkUnreachableShapes();
    checkParamMatrix(shapes);

    std::printf(
        "viewport-admission-test: %lld masks, %lld shapes,"
        " %lld selection cases, %lld fallback cases\n",
        counters.masks, counters.shapes, counters.selectionCases,
        counters.fallbackCases);

    if (failures == 0) {
        std::puts("viewport-admission-test: PASS");
    }

    return failures == 0 ? 0 : 1;
}
