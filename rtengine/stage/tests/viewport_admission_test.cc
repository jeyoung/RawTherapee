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
 * The expected rows are recorded decisions: the eligibility, the LUT set and
 * the crop set each intent must produce, and the parameter-veto table. The
 * test never interprets a request mask.
 */

#include "rtengine/editorrequest.h"

#include "rtengine/procparams.h"
#include "rtengine/stage/stage_dirty_blocks.h"

#include <cstdio>
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
using rtengine::make_request;
using rtengine::resolveCropDependencies;
using rtengine::viewportCropSelection;
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

bool sameReason(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

std::string reasonText(const char* reason)
{
    return reason != nullptr ? reason : "admit";
}

struct IntentDecision {
    RequestIntent intent;
    const char* name;
    ViewportEligibility eligibility;
    bool toneCurves;
    bool labCurves;
    bool lumaCurveFromCrop;
    bool wavelet;
    bool crop;
    bool repaint;
    bool cropLabPixelsImplied;
};

const std::vector<IntentDecision>& decisions()
{
    static const std::vector<IntentDecision> rows = {
        {RequestIntent::AllNoRaw, "AllNoRaw", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::Demosaic, "Demosaic", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::Hdr, "Hdr", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::SpotAdjust, "SpotAdjust", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::Transform, "Transform", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::WhiteBalance, "WhiteBalance", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::RepaintOnly, "RepaintOnly",
         ViewportEligibility::Admit, false, false, false, false, false, true,
         false},
        {RequestIntent::ControlOnly, "ControlOnly",
         ViewportEligibility::Unsupported, false, false, false, false, false,
         false, false},
        {RequestIntent::Compr, "Compr", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::CaptureSharpen, "CaptureSharpen",
         ViewportEligibility::Base, true, true, false, true, false, false,
         true},
        {RequestIntent::DarkFrame, "DarkFrame", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::First, "First", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::AutoExposure, "AutoExposure",
         ViewportEligibility::Admit, true, true, false, true, false, false,
         true},
        {RequestIntent::LuminanceCurve, "LuminanceCurve",
         ViewportEligibility::Admit, false, true, false, true, false, false,
         true},
        {RequestIntent::DirPyrEqualizer, "DirPyrEqualizer",
         ViewportEligibility::Admit, false, false, false, true, false, false,
         true},
        {RequestIntent::RgbCurve, "RgbCurve", ViewportEligibility::Admit,
         true, true, false, true, false, false, true},
        {RequestIntent::Sharpening, "Sharpening", ViewportEligibility::Admit,
         false, false, false, true, false, false, true},
        {RequestIntent::Retinex, "Retinex", ViewportEligibility::Base,
         true, true, false, true, false, false, true},
        {RequestIntent::Crop, "Crop", ViewportEligibility::Admit,
         true, false, true, false, true, false, false},
        {RequestIntent::Monitor, "Monitor", ViewportEligibility::Base,
         false, false, false, false, false, false, false},
        {RequestIntent::NoRequest, "NoRequest",
         ViewportEligibility::Unsupported, false, false, false, false, false,
         false, false},
        {RequestIntent::HighQuality, "HighQuality",
         ViewportEligibility::Base, false, false, false, false, false, false,
         false}
    };
    return rows;
}

void checkIntentDecisions()
{
    for (const IntentDecision& row : decisions()) {
        const std::string label = std::string("intent ") + row.name;
        const EditorRequest request = make_request(row.intent);

        check(viewportEligibility(request) == row.eligibility,
              label + ": eligibility");
        check(sameReason(viewportEligibilityReason(
                             viewportEligibility(request)),
                         viewportEligibilityReason(row.eligibility)),
              label + ": eligibility reason");

        const ViewportLutSelection luts = viewportLutSelection(request);
        check(luts.toneCurves == row.toneCurves, label + ": tone LUTs");
        check(luts.labCurves == row.labCurves, label + ": Lab LUTs");
        check(luts.lumaCurveFromCrop == row.lumaCurveFromCrop,
              label + ": luma curve from crop");
        check(luts.wavelet == row.wavelet, label + ": wavelet LUT");

        ViewportCropSelection crop = viewportCropSelection(request);
        resolveCropDependencies(crop);
        check(crop.crop == row.crop, label + ": crop");
        check(crop.repaint == row.repaint, label + ": repaint");
        check(crop.labPixelsImplied() == row.cropLabPixelsImplied,
              label + ": crop Lab pixels");
    }
}

/* A producer-provided dirty set is held to the same family envelope. */
void checkProducerBlocks()
{
    const rtengine::stage::DirtyBlocks delta = rtengine::stage::closeOverGraph(
        [] {
            rtengine::stage::DirtyBlocks seed;
            seed.valid = true;
            seed.ids.insert("rgb.proc");
            seed.blocks.push_back("rgb.proc");
            return seed;
        }(),
        rtengine::stage::StagePath::Preview);
    EditorRequest request;
    request.present = true;
    request.controlOnly = false;
    request.dirty = delta;
    request.dirtySource = EditorRequest::DirtySource::ProducerProvided;
    check(viewportEligibility(request) == ViewportEligibility::Admit,
          "a delta block set is admitted");

    rtengine::stage::DirtyBlocks base;
    base.valid = true;
    base.ids.insert("transform");
    request.dirty = base;
    check(viewportEligibility(request) == ViewportEligibility::Base,
          "a transform block set is a base request");

    base.ids.clear();
    base.ids.insert("foreign.block");
    request.dirty = base;
    check(viewportEligibility(request) == ViewportEligibility::Unsupported,
          "an unknown block set is unsupported");

    request.dirty.valid = false;
    check(viewportEligibility(request) == ViewportEligibility::Unsupported,
          "an invalid block set is unsupported");
}

/* Coalescing two requests keeps the strongest eligibility. */
void checkCoalesce()
{
    const EditorRequest admitted = make_request(RequestIntent::RgbCurve);
    const EditorRequest base = make_request(RequestIntent::WhiteBalance);
    const EditorRequest control = make_request(RequestIntent::ControlOnly);

    check(viewportEligibility(coalesce(admitted, admitted))
              == ViewportEligibility::Admit,
          "two admitted requests stay admitted");
    check(viewportEligibility(coalesce(admitted, base))
              == ViewportEligibility::Base,
          "a base request wins over an admitted request");
    check(viewportEligibility(coalesce(admitted, control))
              == ViewportEligibility::Unsupported,
          "a control request makes the union unsupported");
}

struct ParamCase {
    std::string name;
    ProcParams current;
    ProcParams last;
    ProcParams lastLut;
    std::string expected;
};

ProcParams baselineParams()
{
    ProcParams params;

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

/*
 * The recorded reason of one coordinator state before the parameter vetoes.
 * Returns the reason, or nullptr when the state admits.
 */
const char* recordedStateReason(const ViewportAdmissionState& state)
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
    return nullptr;
}

void checkParamMatrix()
{
    const std::vector<ParamCase> params = paramCases();
    const std::vector<ViewportAdmissionState> states = admissionStates();

    /*
     * A crop request carries no LUT family, so the last three vetoes are
     * reachable and every parameter row probes the reason table.
     */
    const EditorRequest crop = make_request(RequestIntent::Crop);

    ViewportAdmissionState admitState;
    admitState.fastPathEnabled = true;
    admitState.fullStateComplete = true;
    admitState.highDetailNeeded = false;
    admitState.highDetailComplete = false;

    long long comparisons = 0;
    for (const ParamCase& entry : params) {
        ViewportAdmissionInputs probe{entry.current, entry.last,
                                      entry.lastLut, admitState};
        const char* probeReason = viewportFastPathFallback(crop, probe);
        check(reasonText(probeReason) == entry.expected,
              "reason table " + entry.name + ": expected " + entry.expected
                  + ", got " + reasonText(probeReason));

        for (const ViewportAdmissionState& state : states) {
            ViewportAdmissionInputs inputs{entry.current, entry.last,
                                           entry.lastLut, state};
            const char* actual = viewportFastPathFallback(crop, inputs);
            const char* stateReason = recordedStateReason(state);
            const char* expected = stateReason != nullptr
                ? stateReason
                : (entry.expected == "admit" ? nullptr
                                             : entry.expected.c_str());
            check(sameReason(actual, expected),
                  "state matrix " + entry.name + ": expected "
                      + reasonText(expected) + ", got "
                      + reasonText(actual));
            ++comparisons;
        }
    }

    std::printf("viewport-admission-test: fallback matrix: %lld params"
                " x %lld states = %lld comparisons\n",
                static_cast<long long>(params.size()),
                static_cast<long long>(states.size()), comparisons);
}

void checkUnreachableShapes()
{
    const EditorRequest absent;
    check(!absent.present, "an empty accumulator is absent");
    check(viewportEligibility(absent) == ViewportEligibility::Unsupported,
          "an absent request is unsupported");

    const EditorRequest control = make_request(RequestIntent::ControlOnly);
    check(viewportEligibility(control) == ViewportEligibility::Unsupported,
          "a control-only request is unsupported");

    const EditorRequest high = make_request(RequestIntent::HighQuality);
    check(viewportEligibility(high) == ViewportEligibility::Base,
          "a high-quality request needs the base");

    const EditorRequest hdr = make_request(RequestIntent::Hdr);
    check(viewportEligibility(hdr) == ViewportEligibility::Base,
          "an HDR request needs the base");
}

} // namespace

int main()
{
    checkIntentDecisions();
    checkProducerBlocks();
    checkCoalesce();
    checkParamMatrix();
    checkUnreachableShapes();

    if (failures == 0) {
        std::puts("viewport-admission-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
