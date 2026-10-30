/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 The RawTherapee Team
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

#include "stage/local_adjustments_context.h"

#include "color.h"
#include "curves.h"
#include "dirpyr_equalizer.h"
#include "gauss.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "ipshadowshighlights.h"
#include "labimage.h"
#include "localadjustments.h"
#include "mytime.h"
#include "procparams.h"
#include "settings.h"
#include "stage/lab_tools_context.h"
#include "stage/stage_buffer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>

namespace rtengine::stage
{

namespace
{

bool finiteValue(double value)
{
    return std::isfinite(value);
}

/* The host's curve helpers treat an unrecognised or empty method as zero, so
 * the predicate accepts the empty string and the device record writes the
 * mapped value. */
bool isNoMerge(const Glib::ustring& method)
{
    return method.empty() || method == "mone";
}

bool isQualityCurve(const Glib::ustring& method)
{
    return method.empty() || method == "none" || method == "std";
}

bool isAvoidMethod(const Glib::ustring& method)
{
    return method.empty() || method == "NONE" || method == "MUNS";
}

/* CurveFactory::diagonalCurve2Lut() reports a curve as active only when it is
 * present and not the identity. The default spot carries identity RGB and
 * exposure curves, so emptiness is the wrong test. */
bool diagonalCurveActive(const std::vector<double>& points)
{
    if (points.empty() || points[0] == 0) {
        return false;
    }
    DiagonalCurve curve(points, CURVES_MIN_POLY_POINTS);
    return !curve.isIdentity();
}

bool labGridIdle(const procparams::LocallabParams::LocallabSpot& spot)
{
    /* strengthgrid only scales an active grid, and the merge corners are read
     * by the residual merge path; the four colour-toning corners decide
     * whether the LabGrid pass runs at all. */
    return spot.labgridALow == 0.0 && spot.labgridBLow == 0.0 &&
           spot.labgridAHigh == 0.0 && spot.labgridBHigh == 0.0;
}

bool explicitMaskIdle(const procparams::LocallabParams::LocallabSpot& spot)
{
    /* fftColorMask is an optimisation switch for a mask that is not drawn;
     * it does not make a mask explicit. */
    /* A mask flag for a tool that is switched off is inert: the block's
     * `ena*Mask` predicates require the tool as well. enacontrast and
     * enaretiMasktmap default to true, so only the flags of tools that can be
     * active here are checked. */
    return !spot.enaColorMask && !spot.enaExpMask &&
           !spot.enaExpMaskaft && !spot.enaSHMask && !spot.enavibMask &&
           !spot.enablMask && !spot.enatmMask && !spot.enatmMaskaft &&
           !spot.enaretiMask && !spot.enalcMask && !spot.enacbMask &&
           !spot.enaLMask && !spot.enamask && !spot.enacieMask &&
           !spot.enacieMaskall;
}

/* Every tool other than the six this node runs. The exposure tool also
 * decides the clamp arm; it is a residual here. */
bool otherToolEnabled(const procparams::LocallabParams::LocallabSpot& spot)
{
    return spot.expexpose || spot.expblur ||
           spot.exptonemap || spot.expreti || spot.expsharp ||
           spot.explog || spot.expmask || spot.expcie ||
           spot.expprecam;
}

/* Whether the local CBDL tool's parameters are inside the device set.
 *
 * The block's guard is
 *   (any mulloc != 1 || clarityml != 0 || contresid != 0 || enacbMask ||
 *    showmaskcbmet 2/3/4 || prevdE) && cbdlena
 * and the export call site passes no mask selector and no previous-deltaE
 * flag, so with the explicit mask already excluded by the block predicate the
 * tool runs whenever cbdlena is set. The block's own ROI guard (bfw > 65 and
 * bfh > 65) is applied when the record is built; a smaller ROI is a no-op,
 * not a residual.
 *
 * contresid is a residual: the block builds a DiagonalCurve from the mean of
 * the coarsest decomposition, which is image data. The other parameters are
 * the GUI's ranges and feed the block's own dirpyrFillLut() and multipliers.
 * Soft radius (softradiuscb) is a residual, as is an explicit mask. */
bool cbdlAdmitted(const procparams::LocallabParams::LocallabSpot& spot)
{
    if (spot.softradiuscb != 0.0) {
        return false;
    }
    if (spot.contresid != 0.0) {
        return false;
    }
    if (!finiteValue(spot.chromacbdl) || spot.chromacbdl < 0.0 ||
        spot.chromacbdl > 1.5) {
        return false;
    }
    if (!finiteValue(spot.threshold) || spot.threshold < 0.0 ||
        spot.threshold > 1.0) {
        return false;
    }
    if (!finiteValue(spot.clarityml) || spot.clarityml < 0.1 ||
        spot.clarityml > 100.0) {
        return false;
    }
    if (spot.sensicb < 0 || spot.sensicb > 100) {
        return false;
    }
    for (double multiplier : spot.mult) {
        if (!finiteValue(multiplier) || multiplier < 0.0 ||
            multiplier > 4.0) {
            return false;
        }
    }
    return true;
}

/* Whether the shadows/highlights tool's parameters are inside the device set.
 * The device runs the block's Lab-mode "std" method. The other methods are
 * residuals: "tone" needs workingtrc()/tone_eq() and the tone-curve helpers,
 * "ghs" is GHS (with its own loccont() arm) and "micha" is the Michaelis-Menten
 * mapper. The std method's own blend is the block's deltaE blend; the
 * recovery blend (reparsh), the gradient, tone-eq's multizone multipliers and
 * a non-default tone gamma are separate paths and are refused. */
bool shadowsHighlightsAdmitted(
    const procparams::LocallabParams::LocallabSpot& spot)
{
    if (spot.shMethod != "std") {
        return false;
    }
    if (spot.highlights < 0 || spot.highlights > 100 ||
        spot.shadows < 0 || spot.shadows > 100) {
        return false;
    }
    if (spot.h_tonalwidth < 10 || spot.h_tonalwidth > 100 ||
        spot.s_tonalwidth < 10 || spot.s_tonalwidth > 100) {
        return false;
    }
    if (spot.sh_radius < 0 || spot.sh_radius > 100) {
        return false;
    }
    if (spot.sensihs < 0 || spot.sensihs > 100) {
        return false;
    }
    if (spot.blurSHde < 2 || spot.blurSHde > 100) {
        return false;
    }
    if (spot.reparsh != 100.0) {
        return false;
    }
    if (spot.strSH != 0.0) {
        return false;
    }
    /* tone_eq() reads the six multizone multipliers; tonecurv reads gamSH and
     * sloSH. Both are residuals even though the std method does not apply
     * them, because they would otherwise be silent profile data. */
    for (double multiplier : spot.multsh) {
        if (multiplier != 0.0) {
            return false;
        }
    }
    return spot.gamSH == 2.4 && spot.sloSH == 12.92;
}

/* The local contrast method the node runs: the Gaussian/IIR variant, spelled
 * exactly `"loc"`. `calcLocalParams()` assigns `lp.locmet` only for `"loc"`
 * and `"wav"`, and `local_params` has no initialiser, so an empty or unknown
 * method leaves the host's arm indeterminate. Only the exact string has a
 * defined host arm; everything else, including `"wav"`, is a residual. */
bool isLocalContrastMethod(const Glib::ustring& method)
{
    return method == "loc";
}

/* Whether the vibrance tool's parameters are inside the device set. The host's
 * gamma (vibgam) wraps vibrance in igammalog()/gammalog(); the three gradient
 * strengths rebuild the tool's output from a local gradient; warm switches to
 * ciecamloc_02float(). None of those is ported, so each is a residual. */
bool vibranceAdmitted(const procparams::LocallabParams::LocallabSpot& spot)
{
    if (spot.vibgam != 1.0 || spot.warm != 0) {
        return false;
    }
    if (spot.strvib != 0.0 || spot.strvibab != 0.0 || spot.strvibh != 0.0) {
        return false;
    }
    if (spot.pastels < -100 || spot.pastels > 100 ||
        spot.saturated < -100 || spot.saturated > 100) {
        return false;
    }
    if (spot.psthreshold.getTopLeft() < 0 ||
        spot.psthreshold.getTopLeft() > 100 ||
        spot.psthreshold.getBottomLeft() < 0 ||
        spot.psthreshold.getBottomLeft() > 100) {
        return false;
    }
    return spot.sensiv >= 0 && spot.sensiv <= 100;
}

/* Whether the local contrast tool's parameters are inside the device set. The
 * Gaussian variant runs ImProcFunctions::localContrast(); the wavelet variant
 * (localcontMethod == "wav") and the FFT variant (fftwlc) are residuals, as
 * are the wavelet-trigger settings the block's guard reads and the recovery
 * weight that would blend the result back towards the original. */
bool localContrastAdmitted(
    const procparams::LocallabParams::LocallabSpot& spot)
{
    if (!isLocalContrastMethod(spot.localcontMethod) || spot.fftwlc) {
        return false;
    }
    if (spot.reparw != 100.0) {
        return false;
    }
    if (spot.lcamount < 0.0 || spot.lcamount > 1.0 ||
        spot.lcradius < 10 || spot.lcradius > 100 ||
        spot.lcdarkness < 0.0 || spot.lcdarkness > 3.0) {
        return false;
    }
    if (spot.sensilc < 0 || spot.sensilc > 100) {
        return false;
    }
    if (spot.strwav != 0.0 || spot.wavedg || spot.wavgradl || spot.origlc) {
        return false;
    }
    if (spot.residblur > 0.0 || spot.levelblur > 0.0 ||
        spot.residcont != 0.0 || spot.clarilres != 0.0 ||
        spot.claricres != 0.0) {
        return false;
    }
    return true;
}

/* Whether the soft-light tool's parameters are inside the device set. The
 * per-pixel curve is softMethod "soft"; "reti" runs retinex_pde() and is a
 * residual. The GUI's strength and sensitivity ranges are 1..100, and the
 * device records the host guard's `strng > 1` run test separately. */
bool softLightAdmitted(const procparams::LocallabParams::LocallabSpot& spot)
{
    if (spot.softMethod != "soft") {
        return false;
    }
    if (spot.streng < 1 || spot.streng > 100) {
        return false;
    }
    return spot.sensisf >= 1 && spot.sensisf <= 100;
}

/* LocLHCurve/LocHHCurve/LocCHCurve's own activity test: the host scans the
 * first 500 entries for a value other than the identity 0.5. */
template<typename Curve>
bool locCurveActive(const Curve& curve)
{
    if (!curve) {
        return false;
    }
    for (int index = 0; index < 500; ++index) {
        if (curve[static_cast<float>(index)] != 0.5f) {
            return true;
        }
    }
    return false;
}

bool copyFromBuffer(const RtStageBuffer& lab, LabImage& image)
{
    if (lab.data == nullptr || lab.format != RT_STAGE_F32 ||
        lab.planes != 3 || lab.width <= 0 || lab.height <= 0 ||
        image.W != lab.width || image.H != lab.height) {
        return false;
    }
    if (lab.row_pitch < static_cast<std::size_t>(lab.width) * sizeof(float) ||
        lab.plane_pitch <
            lab.row_pitch * static_cast<std::size_t>(lab.height)) {
        return false;
    }
    const auto* base = static_cast<const unsigned char*>(lab.data);
    for (int y = 0; y < lab.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            const auto* row = reinterpret_cast<const float*>(
                base + plane * lab.plane_pitch + y * lab.row_pitch);
            float* dst = plane == 0 ? image.L[y]
                        : plane == 1 ? image.a[y]
                                     : image.b[y];
            std::memcpy(dst, row, static_cast<std::size_t>(lab.width) *
                                      sizeof(float));
        }
    }
    return true;
}

bool copyToBuffer(const LabImage& image, RtStageBuffer& lab)
{
    if (lab.data == nullptr || lab.format != RT_STAGE_F32 ||
        lab.planes != 3 || lab.width != image.W || lab.height != image.H) {
        return false;
    }
    if (lab.row_pitch < static_cast<std::size_t>(lab.width) * sizeof(float) ||
        lab.plane_pitch <
            lab.row_pitch * static_cast<std::size_t>(lab.height)) {
        return false;
    }
    auto* base = static_cast<unsigned char*>(lab.data);
    for (int y = 0; y < lab.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            auto* row = reinterpret_cast<float*>(
                base + plane * lab.plane_pitch + y * lab.row_pitch);
            const float* src = plane == 0 ? image.L[y]
                               : plane == 1 ? image.a[y]
                                            : image.b[y];
            std::memcpy(row, src, static_cast<std::size_t>(lab.width) *
                                     sizeof(float));
        }
    }
    return true;
}

} // namespace

bool localAdjustmentsSupported(const procparams::ProcParams& params)
{
    /*
     * The block is atomic: every spot must be in the set below. An inactive
     * spot is the clamp-only case; an active spot runs CBDL, Colour & Light,
     * vibrance, shadows/highlights (Lab-mode std), soft light (per-pixel
     * soft), local contrast or any combination, with optional L/C/H curves on
     * the colour tool and MUNS. Curves, Munsell, rectangular spots, inactive
     * spots and multiple spots are supported; every other tool and the masks,
     * merge, soft radius, gradients and LAB/XYZ gamut methods are residuals.
     */
    if (!params.locallab.enabled || params.locallab.spots.empty() ||
        params.locallab.spots.size() > RT_LOCAL_ADJUSTMENTS_MAX_SPOTS) {
        return false;
    }
    if (params.dirpyrequalizer.enabled || params.dehaze.enabled ||
        params.toneCurve.hrenabled) {
        return false;
    }

    for (const auto& spot : params.locallab.spots) {
        if (spot.spotMethod != "norm") {
            return false;
        }
        if (spot.shape != "ELI" && spot.shape != "RECT") {
            return false;
        }
        if (spot.recurs || spot.invers || spot.inversex || spot.inverssh ||
            spot.inverssha || spot.inversret) {
            return false;
        }
        /* expprecam can force the pre-CBDL pass even when the CBDL tool is
         * off; expsharp on an inactive spot only touches shardamping, which
         * no active tool reads. */
        if (spot.expprecam) {
            return false;
        }
        /* Lab_Local()'s notlaplacian arm also fires when ghsactiv
         * (ghs_D != 0) and ghs_BLP > 0, even on an inactive spot whose GHS
         * tool does not run. That arm uses clipR(); the device has no such
         * arm, so these leftovers are residual. */
        if (spot.ghs_D != 0.0 && spot.ghs_BLP > 0.0) {
            return false;
        }
        if (!isNoMerge(spot.merMethod)) {
            return false;
        }
        if (!(spot.qualitycurveMethod.empty() ||
              spot.qualitycurveMethod == "none" ||
              spot.qualitycurveMethod == "std")) {
            return false;
        }
        /* The per-spot clamp is bit-exact against the host, so the MUNS pass
         * can follow it; LAB, XYZ and XYZREL still need the host gamut
         * helpers and remain residuals. */
        if (!(spot.avoidgamutMethod.empty() ||
              spot.avoidgamutMethod == "NONE" ||
              spot.avoidgamutMethod == "MUNS")) {
            return false;
        }
        if (spot.softradiuscol != 0.0 || spot.avoidrad != 0.0) {
            return false;
        }
        if (spot.gamc != 1.0 || spot.reparcol != 100.0 ||
            spot.strcol != 0.0 || spot.strcolab != 0.0 ||
            spot.strcolh != 0.0 || spot.structcol != 0) {
            return false;
        }
        if (spot.special || spot.toolcol || spot.blwh) {
            return false;
        }
        if (diagonalCurveActive(spot.rgbcurve) ||
            diagonalCurveActive(spot.excurve)) {
            return false;
        }
        if (!labGridIdle(spot) || !explicitMaskIdle(spot)) {
            return false;
        }
        if (spot.loc.size() != 4) {
            return false;
        }
        if (spot.chroma < -100 || spot.chroma > 100 ||
            spot.lightness < -100 || spot.lightness > 100 ||
            spot.contrast < -100 || spot.contrast > 100) {
            return false;
        }
        if (spot.blurcolde < 0 || spot.blurcolde > 100) {
            return false;
        }
        /* transit == 100 makes the host's 1 - ach zero in calcLocalFactor();
         * it is outside the finite domain this node promises. */
        if (!(spot.transit >= 0.0 && spot.transit < 100.0)) {
            return false;
        }
        if (!finiteValue(spot.transit) || !finiteValue(spot.transitweak) ||
            !finiteValue(spot.transitgrad) || !finiteValue(spot.iter) ||
            !finiteValue(spot.balan) || !finiteValue(spot.balanh) ||
            !finiteValue(spot.sensi) || !finiteValue(spot.thresh) ||
            !finiteValue(spot.colorde) || !finiteValue(spot.circrad) ||
            !finiteValue(spot.centerX) || !finiteValue(spot.centerY) ||
            !finiteValue(spot.loc[0]) || !finiteValue(spot.loc[1]) ||
            !finiteValue(spot.loc[2]) || !finiteValue(spot.loc[3])) {
            return false;
        }
        if (spot.transitweak < 0.0 || spot.transitweak > 10.0 ||
            spot.transitgrad < -10.0 || spot.transitgrad > 10.0) {
            return false;
        }
        /* An active spot runs at least one of the six tools in the device
         * set; an inactive spot does no work, so its tool settings are inert
         * and admitted. */
        if (spot.activ) {
            if ((!spot.expcolor && !spot.expvibrance && !spot.expcontrast &&
                 !spot.expshadhigh && !spot.expsoft && !spot.expcbdl) ||
                otherToolEnabled(spot)) {
                return false;
            }
            if (spot.expcbdl && !cbdlAdmitted(spot)) {
                return false;
            }
            if (spot.expvibrance && !vibranceAdmitted(spot)) {
                return false;
            }
            if (spot.expcontrast && !localContrastAdmitted(spot)) {
                return false;
            }
            if (spot.expshadhigh && !shadowsHighlightsAdmitted(spot)) {
                return false;
            }
            if (spot.expsoft && !softLightAdmitted(spot)) {
                return false;
            }
        }
    }
    return true;
}

LocalAdjustmentsDispatch localAdjustmentsDispatch(
    const procparams::ProcParams& params)
{
    LocalAdjustmentsDispatch out;
    out.supported = localAdjustmentsSupported(params);
    out.submit = params.locallab.enabled && !params.locallab.spots.empty();
    out.prefill = out.submit && out.supported;
    return out;
}

uint32_t localAdjustmentsClampMode(const procparams::ProcParams& params,
                                   int spot)
{
    /*
     * Lab_Local()'s tail round trip runs when notlaplacian or notzero is set.
     * The tone curve mode is one of the six enumerated modes in every
     * profile, so notzero is always true. The Laplacian arm also needs
     * ghsactiv (ghs_D != 0) with ghs_BLP > 0; the predicate refuses that
     * combination, but the mode is derived so a future wider predicate keeps
     * the right arm. The soft-light block's `strng > 2` arm is the same
     * clipR() clamp; exposure remains a residual.
     */
    bool notLaplacian = params.dirpyrequalizer.enabled;
    if (spot >= 0 &&
        spot < static_cast<int>(params.locallab.spots.size())) {
        const auto& profileSpot = params.locallab.spots[spot];
        if (profileSpot.ghs_D != 0.0 && profileSpot.ghs_BLP > 0.0) {
            notLaplacian = true;
        }
        if (profileSpot.expsoft && profileSpot.activ &&
            profileSpot.softMethod == "soft" &&
            static_cast<float>(profileSpot.streng) > 2.f) {
            notLaplacian = true;
        }
    }
    return notLaplacian ? RT_LOCAL_ADJUSTMENTS_CLAMP_LAPLACIAN
                        : RT_LOCAL_ADJUSTMENTS_CLAMP_NOTZERO;
}

void localAdjustmentsHostBlock(ImProcFunctions& ipf,
                               procparams::ProcParams& params,
                               Imagefloat* baseImg,
                               LabImage* lab,
                               int fw, int fh)
{
    assert(lab != nullptr);
    if (lab == nullptr || baseImg == nullptr) {
        return;
    }

        for (int sp = 0; sp < (int)params.locallab.spots.size(); sp++) {
            if (params.locallab.spots.at(sp).expsharp  && params.dirpyrequalizer.cbdlMethod == "bef") {
                if (params.locallab.spots.at(sp).shardamping < 1) {
                    params.locallab.spots.at(sp).shardamping = 1;
                }
            }
        }
        bool execcam = false;

        //execcam => work around for pre-ciecam in LA: about 0.1 second
        for (int sp = 0; sp < (int)params.locallab.spots.size(); sp++) {
            if (params.locallab.spots.at(sp).expprecam) {
                execcam = true;
            }
        }
        if ((params.dirpyrequalizer.cbdlMethod == "bef") && (params.dirpyrequalizer.enabled || execcam) && !params.colorappearance.enabled) {
            if (execcam  && !params.dirpyrequalizer.enabled) {
                params.dirpyrequalizer.enabled = true;

                if (params.dirpyrequalizer.mult[0] == 1.) {
                    params.dirpyrequalizer.mult[0] = 1.01;
                }
            }
            const int W = baseImg->getWidth();
            const int H = baseImg->getHeight();
            LabImage labcbdl(W, H);
            ipf.rgb2lab(*baseImg, labcbdl, params.icm.workingProfile);
            ipf.dirpyrequalizer(&labcbdl, 1);
            ipf.lab2rgb(labcbdl, *baseImg, params.icm.workingProfile);
        }

        // RGB processing

        LabImage* labView = lab;

        if (params.locallab.enabled && params.locallab.spots.size() > 0) {
            ipf.rgb2lab(*baseImg, *labView, params.icm.workingProfile);

            MyTime t1, t2;
            t1.set();

            const std::unique_ptr<LabImage> reservView(new LabImage(*labView, true));
            const std::unique_ptr<LabImage> lastorigView(new LabImage(*labView, true));
            std::unique_ptr<LabImage> savenormtmView;
            std::unique_ptr<LabImage> savenormretiView;
            LocretigainCurve locRETgainCurve;
            LocretitransCurve locRETtransCurve;
            LocLHCurve loclhCurve;
            LocHHCurve lochhCurve;
            LocCHCurve locchCurve;
            LocHHCurve lochhCurvejz;
            LocCHCurve locchCurvejz;
            LocLHCurve loclhCurvejz;
            LocCCmaskCurve locccmasCurve;
            LocLLmaskCurve locllmasCurve;
            LocHHmaskCurve lochhmasCurve;
            LocHHmaskCurve lochhhmasCurve;
            LocHHmaskCurve lochhhmascieCurve;
            LocCCmaskCurve locccmasexpCurve;
            LocLLmaskCurve locllmasexpCurve;
            LocHHmaskCurve lochhmasexpCurve;
            LocCCmaskCurve locccmasSHCurve;
            LocLLmaskCurve locllmasSHCurve;
            LocHHmaskCurve lochhmasSHCurve;
            LocCCmaskCurve locccmasvibCurve;
            LocLLmaskCurve locllmasvibCurve;
            LocHHmaskCurve lochhmasvibCurve;
            LocCCmaskCurve locccmaslcCurve;
            LocLLmaskCurve locllmaslcCurve;
            LocHHmaskCurve lochhmaslcCurve;
            LocCCmaskCurve locccmascbCurve;
            LocLLmaskCurve locllmascbCurve;
            LocHHmaskCurve lochhmascbCurve;
            LocCCmaskCurve locccmasretiCurve;
            LocLLmaskCurve locllmasretiCurve;
            LocHHmaskCurve lochhmasretiCurve;
            LocCCmaskCurve locccmastmCurve;
            LocLLmaskCurve locllmastmCurve;
            LocHHmaskCurve lochhmastmCurve;
            LocCCmaskCurve locccmasblCurve;
            LocLLmaskCurve locllmasblCurve;
            LocHHmaskCurve lochhmasblCurve;
            LocCCmaskCurve locccmaslogCurve;
            LocLLmaskCurve locllmaslogCurve;
            LocHHmaskCurve lochhmaslogCurve;
            LocCCmaskCurve locccmascieCurve;
            LocLLmaskCurve locllmascieCurve;
            LocHHmaskCurve lochhmascieCurve;

            LocCCmaskCurve locccmas_Curve;
            LocLLmaskCurve locllmas_Curve;
            LocHHmaskCurve lochhmas_Curve;
            LocHHmaskCurve lochhhmas_Curve;

            LocwavCurve loclmasCurveblwav;
            LocwavCurve loclmasCurvecolwav;
            LocwavCurve loclmasCurveciewav;
            LocwavCurve loclmasCurve_wav;
            LocwavCurve locwavCurve;
            LocwavCurve locwavCurvejz;
            LocwavCurve loclevwavCurve;
            LocwavCurve locconwavCurve;
            LocwavCurve loccompwavCurve;
            LocwavCurve loccomprewavCurve;
            LocwavCurve locedgwavCurve;
            LocwavCurve locwavCurvehue;
            LocwavCurve locwavCurvehuecont;
            LocwavCurve locwavCurveden;
            LUTf lllocalcurve(65536, LUT_CLIP_OFF);
            LUTf lclocalcurve(65536, LUT_CLIP_OFF);
            LUTf cllocalcurve(65536, LUT_CLIP_OFF);
            LUTf cclocalcurve(65536, LUT_CLIP_OFF);
            LUTf rgblocalcurve(65536, LUT_CLIP_OFF);
            LUTf hltonecurveloc(65536, LUT_CLIP_OFF);
            LUTf shtonecurveloc(65536, LUT_CLIP_OFF);
            LUTf tonecurveloc(65536, LUT_CLIP_OFF);
            LUTf lightCurveloc(32770, LUT_CLIP_OFF);
            LUTf exlocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmasklocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskexplocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskSHlocalcurve(65536, LUT_CLIP_OFF);
          //  LUTf ghslocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskviblocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmasktmlocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskretilocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskcblocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskbllocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmasklclocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmaskloglocalcurve(65536, LUT_CLIP_OFF);
            LUTf lmasklocal_curve(65536, LUT_CLIP_OFF);
            LUTf lmaskcielocalcurve(65536, LUT_CLIP_OFF);
            LUTf cielocalcurve(65536, LUT_CLIP_OFF);
            LUTf cielocalcurve2(65536, LUT_CLIP_OFF);
            LUTf jzlocalcurve(65536, LUT_CLIP_OFF);
            LUTf czlocalcurve(65536, LUT_CLIP_OFF);
            LUTf czjzlocalcurve(65536, LUT_CLIP_OFF);
            LUTf redlocalcurve(65536, LUT_CLIP_OFF);
            LUTf greenlocalcurve(65536, LUT_CLIP_OFF);
            LUTf bluelocalcurve(65536, LUT_CLIP_OFF);

            array2D<float> shbuffer;

            for (size_t sp = 0; sp < params.locallab.spots.size(); sp++) {
                if (params.locallab.spots.at(sp).inverssha) {
                    shbuffer(fw, fh);
                    break;
                }
            }

            for (size_t sp = 0; sp < params.locallab.spots.size(); sp++) {

                // Set local curves of current spot to LUT
                locRETgainCurve.Set(params.locallab.spots.at(sp).localTgaincurve);
                locRETtransCurve.Set(params.locallab.spots.at(sp).localTtranscurve);
                const bool LHutili = loclhCurve.Set(params.locallab.spots.at(sp).LHcurve);
                const bool HHutili = lochhCurve.Set(params.locallab.spots.at(sp).HHcurve);
                const bool CHutili = locchCurve.Set(params.locallab.spots.at(sp).CHcurve);
                const bool HHutilijz = lochhCurvejz.Set(params.locallab.spots.at(sp).HHcurvejz);
                const bool CHutilijz = locchCurvejz.Set(params.locallab.spots.at(sp).CHcurvejz);
                const bool LHutilijz = loclhCurvejz.Set(params.locallab.spots.at(sp).LHcurvejz);
                const bool lcmasutili = locccmasCurve.Set(params.locallab.spots.at(sp).CCmaskcurve);
                const bool llmasutili = locllmasCurve.Set(params.locallab.spots.at(sp).LLmaskcurve);
                const bool lhmasutili = lochhmasCurve.Set(params.locallab.spots.at(sp).HHmaskcurve);
                const bool lhhmasutili = lochhhmasCurve.Set(params.locallab.spots.at(sp).HHhmaskcurve);
                const bool lhhmascieutili = lochhhmascieCurve.Set(params.locallab.spots.at(sp).HHhmaskciecurve);
                const bool lcmasexputili = locccmasexpCurve.Set(params.locallab.spots.at(sp).CCmaskexpcurve);
                const bool llmasexputili = locllmasexpCurve.Set(params.locallab.spots.at(sp).LLmaskexpcurve);
                const bool lhmasexputili = lochhmasexpCurve.Set(params.locallab.spots.at(sp).HHmaskexpcurve);
                const bool lcmasSHutili = locccmasSHCurve.Set(params.locallab.spots.at(sp).CCmaskSHcurve);
                const bool llmasSHutili = locllmasSHCurve.Set(params.locallab.spots.at(sp).LLmaskSHcurve);
                const bool lhmasSHutili = lochhmasSHCurve.Set(params.locallab.spots.at(sp).HHmaskSHcurve);
                const bool lcmasvibutili = locccmasvibCurve.Set(params.locallab.spots.at(sp).CCmaskvibcurve);
                const bool llmasvibutili = locllmasvibCurve.Set(params.locallab.spots.at(sp).LLmaskvibcurve);
                const bool lhmasvibutili = lochhmasvibCurve.Set(params.locallab.spots.at(sp).HHmaskvibcurve);
                const bool lcmascbutili = locccmascbCurve.Set(params.locallab.spots.at(sp).CCmaskcbcurve);
                const bool llmascbutili = locllmascbCurve.Set(params.locallab.spots.at(sp).LLmaskcbcurve);
                const bool lhmascbutili = lochhmascbCurve.Set(params.locallab.spots.at(sp).HHmaskcbcurve);
                const bool lcmasretiutili = locccmasretiCurve.Set(params.locallab.spots.at(sp).CCmaskreticurve);
                const bool llmasretiutili = locllmasretiCurve.Set(params.locallab.spots.at(sp).LLmaskreticurve);
                const bool lhmasretiutili = lochhmasretiCurve.Set(params.locallab.spots.at(sp).HHmaskreticurve);
                const bool lcmastmutili = locccmastmCurve.Set(params.locallab.spots.at(sp).CCmasktmcurve);
                const bool lhmaslcutili = lochhmaslcCurve.Set(params.locallab.spots.at(sp).HHmasklccurve);
                const bool llmastmutili = locllmastmCurve.Set(params.locallab.spots.at(sp).LLmasktmcurve);
                const bool lhmastmutili = lochhmastmCurve.Set(params.locallab.spots.at(sp).HHmasktmcurve);
                const bool lcmasblutili = locccmasblCurve.Set(params.locallab.spots.at(sp).CCmaskblcurve);
                const bool llmasblutili = locllmasblCurve.Set(params.locallab.spots.at(sp).LLmaskblcurve);
                const bool lhmasblutili = lochhmasblCurve.Set(params.locallab.spots.at(sp).HHmaskblcurve);
                const bool lcmaslogutili = locccmaslogCurve.Set(params.locallab.spots.at(sp).CCmaskcurveL);
                const bool llmaslogutili = locllmaslogCurve.Set(params.locallab.spots.at(sp).LLmaskcurveL);
                const bool lhmaslogutili = lochhmaslogCurve.Set(params.locallab.spots.at(sp).HHmaskcurveL);
                const bool lcmascieutili = locccmascieCurve.Set(params.locallab.spots.at(sp).CCmaskciecurve);
                const bool llmascieutili = locllmascieCurve.Set(params.locallab.spots.at(sp).LLmaskciecurve);
                const bool lhmascieutili = lochhmascieCurve.Set(params.locallab.spots.at(sp).HHmaskciecurve);

                const bool lcmas_utili = locccmas_Curve.Set(params.locallab.spots.at(sp).CCmask_curve);
                const bool llmas_utili = locllmas_Curve.Set(params.locallab.spots.at(sp).LLmask_curve);
                const bool lhmas_utili = lochhmas_Curve.Set(params.locallab.spots.at(sp).HHmask_curve);
                const bool lhhmas_utili = lochhhmas_Curve.Set(params.locallab.spots.at(sp).HHhmask_curve);
                const bool lmasutiliblwav = loclmasCurveblwav.Set(params.locallab.spots.at(sp).LLmaskblcurvewav);
                const bool lmasutilicolwav = loclmasCurvecolwav.Set(params.locallab.spots.at(sp).LLmaskcolcurvewav);
                const bool lmasutiliciewav = loclmasCurveciewav.Set(params.locallab.spots.at(sp).LLmaskciecurvewav);
                const bool lcmaslcutili = locccmaslcCurve.Set(params.locallab.spots.at(sp).CCmasklccurve);
                const bool llmaslcutili = locllmaslcCurve.Set(params.locallab.spots.at(sp).LLmasklccurve);
                const bool lmasutili_wav = loclmasCurve_wav.Set(params.locallab.spots.at(sp).LLmask_curvewav);
                const bool locwavutili = locwavCurve.Set(params.locallab.spots.at(sp).locwavcurve);
                const bool locwavutilijz = locwavCurvejz.Set(params.locallab.spots.at(sp).locwavcurvejz);
                const bool locwavhueutili = locwavCurvehue.Set(params.locallab.spots.at(sp).locwavcurvehue);
                const bool locwavhueutilicont = locwavCurvehuecont.Set(params.locallab.spots.at(sp).locwavcurvehuecont);
                const bool locwavdenutili = locwavCurveden.Set(params.locallab.spots.at(sp).locwavcurveden);
                const bool loclevwavutili = loclevwavCurve.Set(params.locallab.spots.at(sp).loclevwavcurve);
                const bool locconwavutili = locconwavCurve.Set(params.locallab.spots.at(sp).locconwavcurve);
                const bool loccompwavutili = loccompwavCurve.Set(params.locallab.spots.at(sp).loccompwavcurve);
                const bool loccomprewavutili = loccomprewavCurve.Set(params.locallab.spots.at(sp).loccomprewavcurve);
                const bool locedgwavutili = locedgwavCurve.Set(params.locallab.spots.at(sp).locedgwavcurve);
                const bool locallutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).llcurve, lllocalcurve, 1);
                const bool localclutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).clcurve, cllocalcurve, 1);
                const bool locallcutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).lccurve, lclocalcurve, 1);
                const bool localcutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).cccurve, cclocalcurve, 1);
                const bool localrgbutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).rgbcurve, rgblocalcurve, 1);
                const bool localexutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).excurve, exlocalcurve, 1);
                const bool localmaskutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskcurve, lmasklocalcurve, 1);
                const bool localmaskexputili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskexpcurve, lmaskexplocalcurve, 1);
                const bool localmaskSHutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).LmaskSHcurve, lmaskSHlocalcurve, 1);
             //   const bool localghsutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).ghscurve, ghslocalcurve, 1);
                const bool localmaskvibutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskvibcurve, lmaskviblocalcurve, 1);
                const bool localmasktmutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmasktmcurve, lmasktmlocalcurve, 1);
                const bool localmaskretiutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskreticurve, lmaskretilocalcurve, 1);
                const bool localmaskcbutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskcbcurve, lmaskcblocalcurve, 1);
                const bool localmaskblutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskblcurve, lmaskbllocalcurve, 1);
                const bool localmasklcutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmasklccurve, lmasklclocalcurve, 1);
                const bool localmasklogutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).LmaskcurveL, lmaskloglocalcurve, 1);
                const bool localmask_utili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmask_curve, lmasklocal_curve, 1);
                const bool localmaskcieutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskciecurve, lmaskcielocalcurve, 1);
                const bool localcieutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).ciecurve, cielocalcurve, 1);
                const bool localcieutili2 = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).ciecurve2, cielocalcurve2, 1);
                const bool localjzutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).jzcurve, jzlocalcurve, 1);
                const bool localczutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).czcurve, czlocalcurve, 1);
                const bool localczjzutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).czjzcurve, czjzlocalcurve, 1);
                const bool localredutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).redcurve, redlocalcurve, 1);
                const bool localgreenutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).greencurve, greenlocalcurve, 1);
                const bool localblueutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).bluecurve, bluelocalcurve, 1);

                //provisory
                double ecomp = params.locallab.spots.at(sp).expcomp;
                double lblack = params.locallab.spots.at(sp).black;
                double lhlcompr = params.locallab.spots.at(sp).hlcompr;
                double lhlcomprthresh = params.locallab.spots.at(sp).hlcomprthresh;
                double shcompr = params.locallab.spots.at(sp).shcompr;
                double br = params.locallab.spots.at(sp).lightness;
                double cont = params.locallab.spots.at(sp).contrast;

                if (lblack < 0. && params.locallab.spots.at(sp).expMethod == "pde") {
                    lblack *= 1.5;
                }

                float contsig = params.locallab.spots.at(sp).contsigqcie;

                float lightsig = params.locallab.spots.at(sp).lightsigqcie;

                // Reference parameters computation
                double huere, chromare, lumare, huerefblu, chromarefblu, lumarefblu, sobelre;
                int lastsav;
                float avge;
                float meantme;
                float stdtme;
                float meanretie;
                float stdretie;
                float fab = 1.f;
                float maxicam = -1000.f;
                float rdx, rdy, grx, gry, blx, bly = 0.f;
                float meanx, meany, meanxe, meanye, maxdat = 0.f;
                int ill = 2;
                int prim = 3;

                if (params.locallab.spots.at(sp).spotMethod == "exc") {
                    ipf.calc_ref(sp, reservView.get(), reservView.get(), 0, 0, fw, fh, 1, huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, avge, locwavCurveden, locwavdenutili);
                } else {
                    ipf.calc_ref(sp, labView, labView, 0, 0, fw, fh, 1, huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, avge, locwavCurveden, locwavdenutili);
                }

                CurveFactory::complexCurvelocal(ecomp, lblack / 65535., lhlcompr, lhlcomprthresh, shcompr, br, cont, lumare,
                                                hltonecurveloc, shtonecurveloc, tonecurveloc, lightCurveloc, avge,
                                                1);
                float minCD;
                float maxCD;
                float mini;
                float maxi;
                float Tmean;
                float Tsigma;
                float Tmin;
                float Tmax;
                float resi[8];

                float sharc = 0.f;
                float denocont = 0.f;
                int ghsbpwp[2];
                ghsbpwp[0] = 0;
                ghsbpwp[1] = 0;
                float ghsbpwpvalue[2];
                ghsbpwpvalue[0] = 0.f;
                ghsbpwpvalue[1] = 1.f;
                float savmadl[21]  = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
                float ghsbwslider[2];
                ghsbwslider[0] = 0.f;
                ghsbwslider[1] = 1.f;
                float michbwslider[2];//added to facilitate a possible modification requested by users, but is not currently in use
                michbwslider[0] = 0.f;
                michbwslider[1] = 1.f;
                float maxdatend2 = 0.f;
                float satdatend2 = 0.f;
                bool gamaut2 = false;
                float ghscolor[4];
                float ghssym = 0.f;
                float ghsmid = 0.f;
                float ghsmaxrgb = 0.f;
                float ghs3sig = 0.f;
                bool ghsautsp = false;
                float slopeg = 1.f;
                bool linkrgb = true;
                // No Locallab mask is shown in exported picture
                ipf.Lab_Local(2, sp, shbuffer, labView, labView, reservView.get(), savenormtmView.get(), savenormretiView.get(), lastorigView.get(), fw, fh, 0, 0, fw, fh, fw, fh, fw, fh,  1, locRETgainCurve, locRETtransCurve,
                              lllocalcurve, locallutili,
                              cllocalcurve, localclutili,
                              lclocalcurve, locallcutili,
                              loclhCurve, lochhCurve, locchCurve,
                              lochhCurvejz, locchCurvejz, loclhCurvejz,
                              lmasklocalcurve, localmaskutili,
                              lmaskexplocalcurve, localmaskexputili,
                              lmaskSHlocalcurve, localmaskSHutili,
                              lmaskviblocalcurve, localmaskvibutili,
                              lmasktmlocalcurve, localmasktmutili,
                              lmaskretilocalcurve, localmaskretiutili,
                              lmaskcblocalcurve, localmaskcbutili,
                              lmaskbllocalcurve, localmaskblutili,
                              lmasklclocalcurve, localmasklcutili,
                              lmaskloglocalcurve, localmasklogutili,
                              lmasklocal_curve, localmask_utili,
                              lmaskcielocalcurve, localmaskcieutili,
                              cielocalcurve, localcieutili,
                              cielocalcurve2, localcieutili2,
                              jzlocalcurve, localjzutili,
                              czlocalcurve, localczutili,
                              czjzlocalcurve, localczjzutili,
                              redlocalcurve, localredutili,
                              greenlocalcurve, localgreenutili,
                              bluelocalcurve, localblueutili,

                              locccmasCurve, lcmasutili, locllmasCurve, llmasutili, lochhmasCurve, lhmasutili, lochhhmasCurve, lhhmasutili, lochhhmascieCurve, lhhmascieutili, locccmasexpCurve, lcmasexputili, locllmasexpCurve, llmasexputili, lochhmasexpCurve, lhmasexputili,
                              locccmasSHCurve, lcmasSHutili, locllmasSHCurve, llmasSHutili, lochhmasSHCurve, lhmasSHutili,
                              locccmasvibCurve, lcmasvibutili, locllmasvibCurve, llmasvibutili, lochhmasvibCurve, lhmasvibutili,
                              locccmascbCurve, lcmascbutili, locllmascbCurve, llmascbutili, lochhmascbCurve, lhmascbutili,
                              locccmasretiCurve, lcmasretiutili, locllmasretiCurve, llmasretiutili, lochhmasretiCurve, lhmasretiutili,
                              locccmastmCurve, lcmastmutili, locllmastmCurve, llmastmutili, lochhmastmCurve, lhmastmutili,
                              locccmasblCurve, lcmasblutili, locllmasblCurve, llmasblutili, lochhmasblCurve, lhmasblutili,
                              locccmaslcCurve, lcmaslcutili, locllmaslcCurve, llmaslcutili, lochhmaslcCurve, lhmaslcutili,
                              locccmaslogCurve, lcmaslogutili, locllmaslogCurve, llmaslogutili, lochhmaslogCurve, lhmaslogutili,

                              locccmas_Curve, lcmas_utili, locllmas_Curve, llmas_utili, lochhmas_Curve, lhmas_utili,
                              locccmascieCurve, lcmascieutili, locllmascieCurve, llmascieutili, lochhmascieCurve, lhmascieutili,
                              lochhhmas_Curve, lhhmas_utili,
                              loclmasCurveblwav, lmasutiliblwav,
                              loclmasCurvecolwav, lmasutilicolwav,
                              loclmasCurveciewav, lmasutiliciewav,
                              locwavCurve, locwavutili,
                              locwavCurvejz, locwavutilijz,
                              loclevwavCurve, loclevwavutili,
                              locconwavCurve, locconwavutili,
                              loccompwavCurve, loccompwavutili,
                              loccomprewavCurve, loccomprewavutili,
                              locwavCurvehue, locwavhueutili,
                              locwavCurvehuecont, locwavhueutilicont,
                              locwavCurveden, locwavdenutili,
                              locedgwavCurve, locedgwavutili,
                              loclmasCurve_wav, lmasutili_wav,
                              LHutili, HHutili, CHutili, HHutilijz, CHutilijz, LHutilijz, cclocalcurve, localcutili, rgblocalcurve, localrgbutili, localexutili, exlocalcurve, hltonecurveloc, shtonecurveloc, tonecurveloc, lightCurveloc,
                              huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, lastsav, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax,
                              meantme, stdtme, meanretie, stdretie, fab, maxicam, rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, prim, ill, contsig, lightsig, slopeg, linkrgb,
                              resi, sharc, denocont, ghsbpwp, ghsbpwpvalue, savmadl, ghsbwslider, ghssym, ghsautsp, ghscolor, ghsmid, ghsmaxrgb, ghs3sig, michbwslider, maxdatend2, satdatend2, gamaut2);

                if (sp + 1u < params.locallab.spots.size()) {
                    // do not copy for last spot as it is not needed anymore
                    lastorigView->CopyFrom(labView);
                }

                if (params.locallab.spots.at(sp).spotMethod == "exc") {
                    ipf.calc_ref(sp, reservView.get(), reservView.get(), 0, 0, fw, fh, 1, huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, avge, locwavCurveden, locwavdenutili);
                } else {
                    ipf.calc_ref(sp, labView, labView, 0, 0, fw, fh, 1, huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, avge, locwavCurveden, locwavdenutili);
                }
            }

            t2.set();
            ipf.lab2rgb(*labView, *baseImg, params.icm.workingProfile);

            if (settings->verbose) {
                printf("Total local:- %d usec\n", t2.etime(t1));
            }

        }


    }

namespace
{

/* The block's own chroma scale for the working profile, used to index the
 * C=f(C) and L=f(C) curves. It is constant per submission. */
float workingChromaScale(const Glib::ustring& profile)
{
    if (profile == "ProPhoto" || profile == "WideGamut") {
        return 1.2f;
    }
    if (profile == "Adobe RGB" || profile == "BruceRGB") {
        return 1.8f;
    }
    if (profile == "sRGB") {
        return 2.0f;
    }
    if (profile == "Beta RGB" || profile == "BestRGB") {
        return 1.4f;
    }
    return 1.0f;
}

uint32_t avoidMethod(const Glib::ustring& method)
{
    if (method.empty() || method == "NONE") {
        return RT_LOCAL_ADJUSTMENTS_AVOID_NONE;
    }
    if (method == "MUNS") {
        return RT_LOCAL_ADJUSTMENTS_AVOID_MUNS;
    }
    return RT_LOCAL_ADJUSTMENTS_AVOID_NONE;
}

/* A DiagonalCurve's sampled table for the two chroma-slider curves. The host
 * evaluates these curves per pixel; the device table is a host-sampled
 * piecewise-linear approximation over the block's [0,1] argument. */
void sampleCurve(const std::vector<double>& points, std::vector<float>& table)
{
    DiagonalCurve curve(points);
    table.resize(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES);
    for (uint32_t index = 0; index < RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES;
         ++index) {
        table[index] = static_cast<float>(
            curve.getVal(static_cast<double>(index) /
                         static_cast<double>(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES -
                                             1u)));
    }
}

/* The host's Gaussian coefficients in the three forms gauss.cc's paths read.
 * The colour, vibrance and local-contrast passes each need their own sigma;
 * `double_path` records which of the host's paths the sigma selects. */
bool fillIir(double sigma, RtLabToolsIir& out)
{
    const GaussianIirCoefficients coefficients = gaussianIirCoefficients(sigma);
    const auto fill = [&coefficients](RtLabToolsIirDirection& direction) {
        direction.lane[0] = static_cast<float>(coefficients.B);
        direction.lane[1] = static_cast<float>(coefficients.b1);
        direction.lane[2] = static_cast<float>(coefficients.b2);
        direction.lane[3] = static_cast<float>(coefficients.b3);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const std::size_t index =
                    4u + static_cast<std::size_t>(row) * 3u +
                    static_cast<std::size_t>(column);
                direction.lane[index] =
                    static_cast<float>(coefficients.sse_M[row][column]);
                direction.border[index] = coefficients.sse_M[row][column];
                direction.wide[index] = coefficients.M[row][column];
            }
        }
        direction.border[0] = coefficients.B;
        direction.border[1] = coefficients.b1;
        direction.border[2] = coefficients.b2;
        direction.border[3] = coefficients.b3;
        direction.wide[0] = coefficients.B;
        direction.wide[1] = coefficients.b1;
        direction.wide[2] = coefficients.b2;
        direction.wide[3] = coefficients.b3;
    };
    out = RtLabToolsIir{};
    out.size = sizeof(RtLabToolsIir);
    out.version = RT_LAB_TOOLS_IIR_VERSION;
    out.double_path = sigma >= GAUSS_DOUBLE_LIMIT ? 1u : 0u;
    fill(out.horizontal);
    fill(out.vertical);
    for (const RtLabToolsIirDirection* direction :
         {&out.horizontal, &out.vertical}) {
        for (std::size_t index = 0; index < RT_LAB_TOOLS_IIR_TERMS; ++index) {
            if (!std::isfinite(direction->lane[index]) ||
                !std::isfinite(direction->border[index]) ||
                !std::isfinite(direction->wide[index])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

LocalAdjustmentsContext::LocalAdjustmentsContext(
    procparams::ProcParams& params, ImProcFunctions& ipf,
    Imagefloat* baseImg, LabImage*& labView, int fw, int fh)
    : params_(params), ipf_(ipf), base_(baseImg), labView_(labView),
      fw_(fw), fh_(fh)
{
}

LocalAdjustmentsContext::~LocalAdjustmentsContext() = default;

RtLocalAdjustmentsContext LocalAdjustmentsContext::descriptor(bool supported)
{
    RtLocalAdjustmentsContext out{};
    out.size = sizeof(RtLocalAdjustmentsContext);
    out.version = RT_LOCAL_ADJUSTMENTS_CONTEXT_VERSION;
    out.magic = RT_LOCAL_ADJUSTMENTS_CONTEXT_MAGIC;
    out.supported = supported ? 1u : 0u;
    out.spot_count = static_cast<uint32_t>(params_.locallab.spots.size());
    out.self = this;
    out.build = [](void* self, int32_t spot, const RtStageBuffer* lab,
                   RtLocalAdjustmentsResult* result) {
        return static_cast<LocalAdjustmentsContext*>(self)->build(
            spot, lab, *result);
    };
    out.run_host = [](void* self, RtStageBuffer* lab) {
        return static_cast<LocalAdjustmentsContext*>(self)->runHost(*lab);
    };
    return out;
}

int LocalAdjustmentsContext::build(int spot, const RtStageBuffer* lab,
                                   RtLocalAdjustmentsResult& out)
{
    if (!localAdjustmentsSupported(params_)) {
        return 1;
    }
    if (spot < 0 ||
        spot >= static_cast<int>(params_.locallab.spots.size())) {
        return 1;
    }
    if (fw_ <= 0 || fh_ <= 0) {
        return 1;
    }

    const auto& profileSpot = params_.locallab.spots[static_cast<std::size_t>(
        spot)];
    LocalAdjustmentsSpotParams lp;
    ipf_.localAdjustmentsSpotParams(spot, fw_, fh_, lp);

    /*
     * The global tables are shared by every spot. The clamp's matrices and
     * caches and the Munsell set are static for the submission; a process
     * without them cannot run the host block either, so the setup refuses.
     */
    if (!globals_built_) {
        const TMatrix ws =
            ICCStore::getInstance()->workingSpaceMatrix(
                params_.icm.workingProfile);
        const TMatrix iws =
            ICCStore::getInstance()->workingSpaceInverseMatrix(
                params_.icm.workingProfile);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                ws_[row * 3 + col] = static_cast<float>(ws[row][col]);
                iws_[row * 3 + col] = static_cast<float>(iws[row][col]);
            }
        }

        if (Color::cachef.getSize() < RT_LAB_TOOLS_CACHE_ENTRIES ||
            Color::cachefy.getSize() < RT_LAB_TOOLS_CACHE_ENTRIES) {
            return 1;
        }
        cachef_.resize(RT_LAB_TOOLS_CACHE_ENTRIES);
        cachefy_.resize(RT_LAB_TOOLS_CACHE_ENTRIES);
        for (uint32_t index = 0; index < RT_LAB_TOOLS_CACHE_ENTRIES;
             ++index) {
            cachef_[index] = Color::cachef[static_cast<int>(index)];
            cachefy_[index] = Color::cachefy[static_cast<int>(index)];
        }

        if (Color::gammatab_srgb.getSize() <
                RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES ||
            Color::igammatab_srgb.getSize() <
                RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES) {
            return 1;
        }
        gammaSrgb_.resize(RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES);
        igammaSrgb_.resize(RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES);
        for (uint32_t index = 0;
             index < RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES; ++index) {
            gammaSrgb_[index] =
                Color::gammatab_srgb[static_cast<int>(index)];
            igammaSrgb_[index] =
                Color::igammatab_srgb[static_cast<int>(index)];
        }

        if (Color::munsellLutCount() !=
            static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES)) {
            return 1;
        }
        munsell_.assign(
            RT_LAB_TOOLS_MUNSELL_TABLES * RT_LAB_TOOLS_MUNSELL_STRIDE, 0.f);
        munsell_sizes_.assign(RT_LAB_TOOLS_MUNSELL_TABLES, 0.f);
        for (uint32_t table = 0; table < RT_LAB_TOOLS_MUNSELL_TABLES;
             ++table) {
            const LUTf* lut =
                Color::munsellLut(static_cast<int>(table));
            if (lut == nullptr) {
                return 1;
            }
            munsell_sizes_[table] = static_cast<float>(lut->getSize());
            for (uint32_t index = 0;
                 index < RT_LAB_TOOLS_MUNSELL_STRIDE; ++index) {
                munsell_[table * RT_LAB_TOOLS_MUNSELL_STRIDE + index] =
                    (*lut)[static_cast<int>(index)];
            }
        }
        globals_built_ = true;
    }

    /*
     * calc_ref() reads the image as it stands before this spot. A device
     * launcher must therefore call build() once per spot on the read-back
     * result, not once for the block.
     */
    double huerefBlur = 0.0;
    double chromarefBlur = 0.0;
    double lumarefBlur = 0.0;
    double hueref = 0.0;
    double chromaref = 0.0;
    double lumaref = 0.0;
    double sobelref = 0.0;
    float avge = 0.f;
    if (lab != nullptr) {
        LabImage current(fw_, fh_);
        if (!copyFromBuffer(*lab, current)) {
            return 1;
        }
        LocwavCurve noDenoiseCurve;
        ipf_.calc_ref(spot, &current, &current, 0, 0, fw_, fh_, 1,
                      huerefBlur, chromarefBlur, lumarefBlur, hueref,
                      chromaref, lumaref, sobelref, avge, noDenoiseCurve,
                      false);
    }

    spots_.assign(RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE, 0.f);
    luts_.clear();
    clamp_mode_ = localAdjustmentsClampMode(params_, spot);

    const auto appendTable = [this](const LUTf& table) -> float {
        const unsigned int entries = table.getSize();
        if (luts_.size() + entries >
            RT_LOCAL_ADJUSTMENTS_MAX_LUT_ENTRIES) {
            return RT_LOCAL_ADJUSTMENTS_NO_LUT;
        }
        const float offset = static_cast<float>(luts_.size());
        for (unsigned int index = 0; index < entries; ++index) {
            luts_.push_back(table[static_cast<int>(index)]);
        }
        return offset;
    };
    const auto appendSamples = [this](const std::vector<float>& table)
        -> float {
        if (luts_.size() + table.size() >
            RT_LOCAL_ADJUSTMENTS_MAX_LUT_ENTRIES) {
            return RT_LOCAL_ADJUSTMENTS_NO_LUT;
        }
        const float offset = static_cast<float>(luts_.size());
        luts_.insert(luts_.end(), table.begin(), table.end());
        return offset;
    };
    /* LocLHCurve and its siblings expose a float index operator rather than
     * the LUTf, so the table is read one integer sample at a time. */
    const auto appendLocCurve = [this](const auto& curve) -> float {
        if (luts_.size() + RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES >
            RT_LOCAL_ADJUSTMENTS_MAX_LUT_ENTRIES) {
            return RT_LOCAL_ADJUSTMENTS_NO_LUT;
        }
        const float offset = static_cast<float>(luts_.size());
        for (uint32_t index = 0; index < RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES;
             ++index) {
            luts_.push_back(curve[static_cast<float>(index)]);
        }
        return offset;
    };

    /* The four diagonal curves and the three hue curves are gated by the
     * quality-curve method exactly as the block gates them. */
    const bool curves = lp.qualcurvemet != 0;

    LUTf lllocalcurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    LUTf cllocalcurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    LUTf lclocalcurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    LUTf cclocalcurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    const bool llActive = curves &&
                          CurveFactory::diagonalCurve2Lut(
                              profileSpot.llcurve, lllocalcurve, 1);
    const bool clActive = curves &&
                          CurveFactory::diagonalCurve2Lut(
                              profileSpot.clcurve, cllocalcurve, 1);
    const bool lcActive = curves &&
                          CurveFactory::diagonalCurve2Lut(
                              profileSpot.lccurve, lclocalcurve, 1);
    const bool ccActive = curves &&
                          CurveFactory::diagonalCurve2Lut(
                              profileSpot.cccurve, cclocalcurve, 1);

    LocLHCurve lhCurve;
    LocHHCurve hhCurve;
    LocCHCurve chCurve;
    lhCurve.Set(profileSpot.LHcurve);
    hhCurve.Set(profileSpot.HHcurve);
    chCurve.Set(profileSpot.CHcurve);
    const bool lhActive = curves && locCurveActive(lhCurve);
    const bool hhActive = curves && locCurveActive(hhCurve);
    const bool chActive = curves && locCurveActive(chCurve);

    /* The lightness/contrast tone curve complexCurvelocal() builds. */
    LUTf hlCurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    LUTf shCurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    LUTf toneCurve(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES, LUT_CLIP_OFF);
    LUTf lightCurve(RT_LOCAL_ADJUSTMENTS_LIGHT_LUT_ENTRIES, LUT_CLIP_OFF);
    double black = profileSpot.black;
    if (black < 0.0 && profileSpot.expMethod == "pde") {
        black *= 1.5;
    }
    CurveFactory::complexCurvelocal(
        profileSpot.expcomp, black / 65535.0, profileSpot.hlcompr,
        profileSpot.hlcomprthresh, profileSpot.shcompr,
        profileSpot.lightness, profileSpot.contrast, lumaref, hlCurve,
        shCurve, toneCurve, lightCurve, avge, 1);
    const bool lightActive = lp.ligh != 0.f || lp.cont != 0;

    /* The two chroma-slider curves, built exactly as the block builds them. */
    std::vector<float> saturTable;
    std::vector<float> saturMoinsTable;
    const float satreal = static_cast<float>(lp.chro);
    if (lp.chro != 0) {
        std::vector<double> saturPoints;
        saturPoints.push_back(static_cast<double>(DCT_NURBS));
        saturPoints.push_back(0.0);
        saturPoints.push_back(0.0);
        saturPoints.push_back(0.2);
        saturPoints.push_back(0.2 + satreal / 250.f);
        saturPoints.push_back(0.6);
        saturPoints.push_back(static_cast<double>(
            std::min(1.f, 0.6f + satreal / 250.f)));
        saturPoints.push_back(1.0);
        saturPoints.push_back(1.0);
        sampleCurve(saturPoints, saturTable);

        std::vector<double> moinsPoints;
        moinsPoints.push_back(static_cast<double>(DCT_NURBS));
        moinsPoints.push_back(0.0);
        moinsPoints.push_back(0.0);
        moinsPoints.push_back(static_cast<double>(0.1f - satreal / 150.f));
        moinsPoints.push_back(0.1);
        moinsPoints.push_back(static_cast<double>(
            std::min(1.f, 0.7f - satreal / 300.f)));
        moinsPoints.push_back(0.7);
        moinsPoints.push_back(1.0);
        moinsPoints.push_back(1.0);
        sampleCurve(moinsPoints, saturMoinsTable);
    }

    /*
     * Which of the two extra tools the host's own guards would run. The
     * vibrance block's guard needs a non-zero pastel/saturated slider (the
     * skin curve alone does not trigger it, because the guard does not read
     * the curve); the local-contrast block needs a non-zero amount. The
     * gamma, gradients, warm path, masks and wavelet variant are residuals,
     * so no other trigger can fire.
     */
    const bool vibranceRun = profileSpot.expvibrance && lp.activspot &&
                             (profileSpot.pastels != 0 ||
                              profileSpot.saturated != 0);
    bool skinCurveSet = false;
    std::vector<float> skinTable;
    if (vibranceRun) {
        DiagonalCurve curve(profileSpot.skintonescurve,
                            CURVES_MIN_POLY_POINTS);
        skinCurveSet = !curve.isIdentity();
        if (skinCurveSet) {
            /* ipvibrance.cc's skbeg, skend and ask, as lab.tools builds it. */
            constexpr float SKIN_HUE_BEGIN = -0.05f;
            constexpr float SKIN_HUE_END = 1.60f;
            const float ask = 65535.f / (SKIN_HUE_END - SKIN_HUE_BEGIN);
            skinTable.resize(RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES);
            for (uint32_t index = 0;
                 index < RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES; ++index) {
                skinTable[index] = static_cast<float>(
                    65535.0 * curve.getVal(
                                 static_cast<double>(index) / 65535.0)) /
                    ask;
            }
        }
    }
    const bool localContrastRun = profileSpot.expcontrast && lp.activspot &&
                                  lp.lcamount > 0.f;
    const float skinOffset =
        skinCurveSet ? appendSamples(skinTable)
                     : RT_LOCAL_ADJUSTMENTS_NO_LUT;
    if (skinCurveSet && skinOffset == RT_LOCAL_ADJUSTMENTS_NO_LUT) {
        return 1;
    }

    /*
     * shadows/highlights. The host guard needs the tool on, an active spot and
     * a positive highlights or shadows slider; a zero/zero tool would run the
     * block but change nothing, so the device skips it too. Each positive
     * round's tone table is built by the block's own shadowsHighlightsLut().
     */
    const bool shadowsHighlightsRun =
        profileSpot.expshadhigh && lp.activspot &&
        (profileSpot.highlights > 0 || profileSpot.shadows > 0);
    float shHighlightOffset = RT_LOCAL_ADJUSTMENTS_NO_LUT;
    float shShadowOffset = RT_LOCAL_ADJUSTMENTS_NO_LUT;
    if (shadowsHighlightsRun) {
        const TMatrix ws =
            ICCStore::getInstance()->workingSpaceMatrix(
                params_.icm.workingProfile);
        const TMatrix iws =
            ICCStore::getInstance()->workingSpaceInverseMatrix(
                params_.icm.workingProfile);
        LUTf highlightLut(RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES, LUT_CLIP_OFF);
        LUTf shadowLut(RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES, LUT_CLIP_OFF);
        if (profileSpot.highlights > 0) {
            shadowsHighlightsLut(
                ws, iws, true,
                shadowsHighlightsAmount(profileSpot.highlights, true), true,
                false, highlightLut);
            shHighlightOffset = appendTable(highlightLut);
            if (shHighlightOffset == RT_LOCAL_ADJUSTMENTS_NO_LUT) {
                return 1;
            }
        }
        if (profileSpot.shadows > 0) {
            shadowsHighlightsLut(
                ws, iws, true,
                shadowsHighlightsAmount(profileSpot.shadows, false), false,
                false, shadowLut);
            shShadowOffset = appendTable(shadowLut);
            if (shShadowOffset == RT_LOCAL_ADJUSTMENTS_NO_LUT) {
                return 1;
            }
        }
    }

    /*
     * The soft-light block's own guard is `(strng > 1 || prevdE) && call <= 3
     * && sfena`; the export call site passes no previous-deltaE flag and runs
     * call 2, so a strength of one is a no-op and is not recorded as a pass.
     * The method and the mask state are already narrowed by the predicate.
     */
    const bool softLightRun = profileSpot.expsoft && lp.activspot &&
                              lp.softmet == 0 && lp.strng > 1.f;

    /*
     * Local CBDL. cbdl_local_temp() trims the trailing identity levels, then
     * decomposes every level up to the last non-identity one. The luminance
     * pass uses lp.mulloc directly; the chroma pass derives
     * max(chromacb * (mulloc - 1) + 1, 0.01) per level. The range table of
     * each level is the block's own dirpyrFillLut(). The block itself does no
     * CBDL work when the ROI is 65 px or narrower on either axis.
     */
    const int cbdlX0 = std::max(
        static_cast<int>(lp.xc - lp.lxL), 0);
    const int cbdlY0 = std::max(
        static_cast<int>(lp.yc - lp.lyT), 0);
    const int cbdlX1 = std::min(
        static_cast<int>(lp.xc + lp.lx), fw_);
    const int cbdlY1 = std::min(
        static_cast<int>(lp.yc + lp.ly), fh_);
    const bool cbdlRoi =
        lp.cbdlena && cbdlX1 - cbdlX0 >= RT_LOCAL_ADJUSTMENTS_CBDL_MIN_ROI &&
        cbdlY1 - cbdlY0 >= RT_LOCAL_ADJUSTMENTS_CBDL_MIN_ROI;
    int cbdlLumaLevels = 0;
    int cbdlChromaLevels = 0;
    float cbdlLumaMult[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS] = {};
    float cbdlChromaMult[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS] = {};
    /*
     * Lab_Local() forces the coarsest level on when clarity or the residual
     * contrast is set and the user left level 5 neutral. The clarity merge
     * reads that level, so the block mutates its local copy of mulloc before
     * cbdl_local_temp(). The record carries the same effective multipliers.
     */
    for (uint32_t level = 0; level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS;
         ++level) {
        cbdlLumaMult[level] = lp.mulloc[level];
    }
    if ((lp.clarityml != 0.f || lp.contresid != 0.f) &&
        cbdlLumaMult[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS - 1] == 1.f) {
        cbdlLumaMult[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS - 1] = 1.001f;
    }
    /*
     * The chroma block also forces its finest level on when the clarity
     * merge (or the residual contrast) is set and level 0 was neutral. The
     * mutation happens after the luminance pass, so it affects the chroma
     * multipliers only.
     */
    float chromaEffective[RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS] = {};
    for (uint32_t level = 0; level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS;
         ++level) {
        chromaEffective[level] = cbdlLumaMult[level];
    }
    const float clarich = 0.5f * lp.clarityml;
    if ((clarich > 0.f || lp.contresid != 0.f) && chromaEffective[0] == 1.f) {
        chromaEffective[0] = 1.01f;
    }
    for (uint32_t level = 0; level < RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS;
         ++level) {
        cbdlChromaMult[level] = std::max(
            lp.chromacb * (chromaEffective[level] - 1.f) + 1.f, 0.01f);
    }
    if (cbdlRoi) {
        cbdlLumaLevels = static_cast<int>(
            RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS);
        while (cbdlLumaLevels > 0 &&
               std::fabs(cbdlLumaMult[cbdlLumaLevels - 1] - 1.f) < 0.001f) {
            --cbdlLumaLevels;
        }
        cbdlChromaLevels = static_cast<int>(
            RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS);
        while (cbdlChromaLevels > 0 &&
               std::fabs(cbdlChromaMult[cbdlChromaLevels - 1] - 1.f) <
                   0.001f) {
            --cbdlChromaLevels;
        }
    }
    const bool cbdlLumaRun = cbdlRoi && cbdlLumaLevels > 0;
    const bool cbdlChromaRun =
        cbdlRoi && lp.chromacb > 0.f && cbdlChromaLevels > 0;
    const auto buildCbdlTables = [&](const float* mult, int levels,
                                     std::vector<float>& tables) {
        tables.resize(static_cast<std::size_t>(levels) *
                      RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES);
        for (int level = 0; level < levels; ++level) {
            LUTf table(static_cast<int>(
                RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES));
            dirpyrFillLut(table, level, static_cast<double>(lp.threshol),
                          mult[level], 0.f);
            float* target =
                tables.data() +
                static_cast<std::size_t>(level) *
                    RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES;
            for (uint32_t index = 0;
                 index < RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES; ++index) {
                target[index] = table[static_cast<int>(index)];
            }
        }
    };
    std::vector<float> cbdlLumaTables;
    std::vector<float> cbdlChromaTables;
    float cbdlLumaOffset = RT_LOCAL_ADJUSTMENTS_NO_LUT;
    float cbdlChromaOffset = RT_LOCAL_ADJUSTMENTS_NO_LUT;
    if (cbdlLumaRun) {
        buildCbdlTables(cbdlLumaMult, cbdlLumaLevels, cbdlLumaTables);
        cbdlLumaOffset = appendSamples(cbdlLumaTables);
        if (cbdlLumaOffset == RT_LOCAL_ADJUSTMENTS_NO_LUT) {
            return 1;
        }
    }
    if (cbdlChromaRun) {
        buildCbdlTables(cbdlChromaMult, cbdlChromaLevels, cbdlChromaTables);
        cbdlChromaOffset = appendSamples(cbdlChromaTables);
        if (cbdlChromaOffset == RT_LOCAL_ADJUSTMENTS_NO_LUT) {
            return 1;
        }
    }

    float* record = spots_.data();
    record[RT_LOCAL_ADJUSTMENTS_SLOT_XC] = lp.xc;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_YC] = lp.yc;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LX] = lp.lx;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LY] = lp.ly;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LXL] = lp.lxL;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LYT] = lp.lyT;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_TRANS] = lp.trans;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_TRANSWEAK] = lp.transweak;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_TRANSGRAD] = lp.transgrad;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_RAD] = lp.rad;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CIR] = lp.cir;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CHRO] = static_cast<float>(lp.chro);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CONT] = static_cast<float>(lp.cont);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LIGH] = lp.ligh;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_STRUCO] = lp.struco;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SENS] = static_cast<float>(lp.sens);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_THR] = lp.thr;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_ITERAT] = lp.iterat;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_BALANCE] = lp.balance;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_BALANCEH] = lp.balanceh;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_BLURCOL] = lp.blurcol;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_HUEREF] =
        static_cast<float>(hueref);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CHROMAREF] =
        static_cast<float>(chromaref);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LUMAREF] =
        static_cast<float>(lumaref);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SOBELREF] =
        static_cast<float>(sobelref);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_AVGE] = avge;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SHAPE] =
        static_cast<float>(lp.shapmet);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_ACTIV] = lp.activspot ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_ISLOCAL] = lp.islocal ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_COLORENA] =
        lp.colorena ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_DELTAE] =
        lp.deltaem ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_QCURVE] =
        static_cast<float>(lp.qualcurvemet);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_MERGE] =
        static_cast<float>(lp.mergemet);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDGAMUT] =
        static_cast<float>(avoidMethod(profileSpot.avoidgamutMethod));
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CLAMPMODE] =
        static_cast<float>(clamp_mode_);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_HRENABLED] =
        params_.toneCurve.hrenabled ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_BLWH] = lp.blwh ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCOLOR] =
        profileSpot.expcolor ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_GAMC] =
        static_cast<float>(profileSpot.gamc);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_REPARCOL] =
        static_cast<float>(profileSpot.reparcol);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_STRCOL] =
        static_cast<float>(profileSpot.strcol);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_STRCOLAB] =
        static_cast<float>(profileSpot.strcolab);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_STRCOLH] =
        static_cast<float>(profileSpot.strcolh);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_ADJUSTR] =
        workingChromaScale(params_.icm.workingProfile);
    float amountChroma = 0.01f * static_cast<float>(settings->amchroma);
    amountChroma = std::max(0.05f, std::min(2.f, amountChroma));
    record[RT_LOCAL_ADJUSTMENTS_SLOT_AMOUNTCHROM] = amountChroma;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDNEG] =
        profileSpot.avoidneg ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPVIBRANCE] =
        vibranceRun ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSV] =
        static_cast<float>(lp.sensv);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PASTELS] =
        static_cast<float>(profileSpot.pastels);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SATURATED] =
        static_cast<float>(profileSpot.saturated);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_TOP] =
        static_cast<float>(profileSpot.psthreshold.getTopLeft());
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_BOTTOM] =
        static_cast<float>(profileSpot.psthreshold.getBottomLeft());
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PROTECTSKINS] =
        profileSpot.protectskins ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_AVOIDCOLORSHIFT] =
        profileSpot.avoidcolorshift ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_OFFSET] = skinOffset;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE] =
        skinCurveSet && skinOffset != RT_LOCAL_ADJUSTMENTS_NO_LUT ? 1.f
                                                                  : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCONTRAST] =
        localContrastRun ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSLC] =
        static_cast<float>(lp.senslc);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LC_AMOUNT] = lp.lcamount;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LC_DARKNESS] =
        static_cast<float>(profileSpot.lcdarkness);
    /* ImProcFunctions::Lab_Local()'s local-contrast block assigns the Colour &
     * Light lightness slider to LocalContrastParams::lightness (not
     * lclightness), so the device must read the same field. */
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LC_LIGHTNESS] =
        static_cast<float>(profileSpot.lightness);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_LC_RADIUS] =
        static_cast<float>(profileSpot.lcradius);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHADHIGHLIGHTS] =
        shadowsHighlightsRun ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSH] =
        static_cast<float>(lp.senshs);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS] =
        static_cast<float>(profileSpot.highlights);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS] =
        static_cast<float>(profileSpot.shadows);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_RADIUS] =
        static_cast<float>(profileSpot.sh_radius);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_TONAL] =
        static_cast<float>(profileSpot.h_tonalwidth);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_TONAL] =
        static_cast<float>(profileSpot.s_tonalwidth);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SH_BLUR] = lp.blurSH;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPSOFT] = softLightRun ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SOFT_STRENGTH] = lp.strng;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSSOFT] =
        static_cast<float>(lp.senssf);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_EXPCBDL] =
        (cbdlLumaRun || cbdlChromaRun) ? 1.f : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_SENSCB] =
        static_cast<float>(lp.senscb);
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_THRESHOLD] = lp.threshol;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CLARITY] = lp.clarityml;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA] = lp.chromacb;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT0] = cbdlLumaMult[0];
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT1] = cbdlLumaMult[1];
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT2] = cbdlLumaMult[2];
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT3] = cbdlLumaMult[3];
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT4] = cbdlLumaMult[4];
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT5] = cbdlLumaMult[5];
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_LEVELS] =
        cbdlLumaRun ? static_cast<float>(cbdlLumaLevels) : 0.f;
    record[RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_LEVELS] =
        cbdlChromaRun ? static_cast<float>(cbdlChromaLevels) : 0.f;

    const auto setTable = [record](int offsetSlot, int activeSlot,
                                   bool active, float offset) {
        record[offsetSlot] =
            active ? offset : RT_LOCAL_ADJUSTMENTS_NO_LUT;
        record[activeSlot] = active && offset != RT_LOCAL_ADJUSTMENTS_NO_LUT
                                 ? 1.f
                                 : 0.f;
    };
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_LL_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LL_ACTIVE, llActive,
             llActive ? appendTable(lllocalcurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_CL_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CL_ACTIVE, clActive,
             clActive ? appendTable(cllocalcurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_LC_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LC_ACTIVE, lcActive,
             lcActive ? appendTable(lclocalcurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_CC_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CC_ACTIVE, ccActive,
             ccActive ? appendTable(cclocalcurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_LH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LH_ACTIVE, lhActive,
             lhActive ? appendLocCurve(lhCurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_HH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_HH_ACTIVE, hhActive,
             hhActive ? appendLocCurve(hhCurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_CH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CH_ACTIVE, chActive,
             chActive ? appendLocCurve(chCurve)
                      : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_ACTIVE,
             lp.chro > 0 && !saturTable.empty(),
             lp.chro > 0 && !saturTable.empty()
                 ? appendSamples(saturTable)
                 : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_ACTIVE,
             lp.chro < 0 && !saturMoinsTable.empty(),
             lp.chro < 0 && !saturMoinsTable.empty()
                 ? appendSamples(saturMoinsTable)
                 : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_ACTIVE, lightActive,
             lightActive ? appendTable(lightCurve)
                         : RT_LOCAL_ADJUSTMENTS_NO_LUT);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_ACTIVE,
             shHighlightOffset != RT_LOCAL_ADJUSTMENTS_NO_LUT,
             shHighlightOffset);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_ACTIVE,
             shShadowOffset != RT_LOCAL_ADJUSTMENTS_NO_LUT,
             shShadowOffset);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_ACTIVE, cbdlLumaRun,
             cbdlLumaOffset);
    setTable(RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_OFFSET,
             RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_ACTIVE, cbdlChromaRun,
             cbdlChromaOffset);

    /*
     * trans-shapedetect2()'s origblur: gaussianBlur() with radius
     * (2 + 0.2 * lp.blurcol) / sk and sk = 1. The host builds the
     * coefficients; only the SSE path is admitted, so double_path is 0 for
     * every supported blurcolde. The vibrance and local-contrast tools use
     * the same blur at the default radius 3/sk = 3, and local contrast has its
     * own radius sigma = lcradius / sk.
     */
    if (!fillIir(static_cast<double>(2.0f + 0.2f * lp.blurcol), iir_)) {
        return 1;
    }
    if (!fillIir(3.0, iirVib_)) {
        return 1;
    }
    if (!fillIir(static_cast<double>(profileSpot.lcradius), iirLc_)) {
        return 1;
    }
    if (!fillIir(static_cast<double>(2.0f + 0.2f * lp.blurSH), iirSh_)) {
        return 1;
    }

    out.spots = spots_.data();
    out.spot_count = 1;
    out.spot_stride = RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE;
    out.luts = luts_.empty() ? nullptr : luts_.data();
    out.lut_count = static_cast<uint32_t>(luts_.size());
    out.ws = ws_;
    out.iws = iws_;
    out.cachef = cachef_.data();
    out.cachefy = cachefy_.data();
    out.gamma_srgb = gammaSrgb_.data();
    out.igamma_srgb = igammaSrgb_.data();
    out.munsell = munsell_.data();
    out.munsell_sizes = munsell_sizes_.data();
    out.munsell_tables = RT_LAB_TOOLS_MUNSELL_TABLES;
    out.clamp_mode = clamp_mode_;
    out.iir = &iir_;
    out.iir_vibrance = &iirVib_;
    out.iir_local_contrast = &iirLc_;
    out.iir_shadows_highlights = &iirSh_;
    built_ = true;
    return 0;
}

int LocalAdjustmentsContext::runHost(RtStageBuffer& lab)
{
    if (labView_ == nullptr || base_ == nullptr) {
        return 1;
    }
    localAdjustmentsHostBlock(ipf_, params_, base_, labView_, fw_, fh_);
    return copyToBuffer(*labView_, lab) ? 0 : 1;
}

} // namespace rtengine::stage
