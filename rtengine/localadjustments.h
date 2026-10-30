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

#pragma once

namespace rtengine
{

/*
 * The per-spot state the local.adjustments device path needs from
 * ImProcFunctions::calcLocalParams().
 *
 * calcLocalParams() is a file-local helper in iplocallab.cc and builds a
 * `local_params` structure with far more fields than the colour-only path
 * reads. The stage context must not re-derive the geometry, the transition
 * parameters or the deltaE weights: a second derivation would drift from the
 * block. This record is the narrow interface: ImProcFunctions fills it from
 * the block's own helper, and the context exports it as a flat device record.
 *
 * The values are post-LIM: calcLocalParams() clamps the transit threshold and
 * the iteration count, so the record carries what the block will use, not the
 * raw profile values.
 */
struct LocalAdjustmentsSpotParams {
    /* Geometry in image pixels. xc/yc is the spot centre, lx/ly the extent to
     * the right and bottom, lxL/lyT the extent to the left and top. */
    float xc = 0.f;
    float yc = 0.f;
    float lx = 0.f;
    float ly = 0.f;
    float lxL = 0.f;
    float lyT = 0.f;
    /* Transition width, the weak-transition exponent and the gradient
     * weighting. */
    float trans = 0.f;
    float transweak = 0.f;
    float transgrad = 0.f;
    float rad = 0.f;
    float cir = 0.f;
    /* Colour & Light sliders and the deltaE weights. chro and cont are the
     * block's integer sliders; ligh is the float lightness slider. */
    int chro = 0;
    int cont = 0;
    float ligh = 0.f;
    float struco = 0.f;
    int sens = 0;
    /* The vibrance and local-contrast scope sensitivities and the local
     * contrast amount, as calcLocalParams() narrows them. */
    int sensv = 0;
    int senslc = 0;
    int senshs = 0;
    float lcamount = 0.f;
    /* The shadows/highlights deltaE reference blur, through blurSHde. */
    float blurSH = 0.f;
    /* The soft-light strength, sensitivity and method selector. sfena is the
     * host's enabled-and-unmasked predicate; only softmet 0 (the per-pixel
     * soft-light curve) is in the device set. */
    float strng = 0.f;
    int senssf = 0;
    bool sfena = false;
    int softmet = 0;
    /* Local CBDL, as calcLocalParams() narrows it. cbdlena is the host's
     * enabled-and-unmasked predicate; mulloc is the per-level multiplier
     * after the block's 0..4 clamp. */
    bool cbdlena = false;
    int senscb = 0;
    float threshol = 0.f;
    float clarityml = 0.f;
    float contresid = 0.f;
    float chromacb = 0.f;
    float mulloc[6] = {};
    float thr = 0.f;
    float iterat = 0.f;
    float balance = 0.f;
    float balanceh = 0.f;
    /* The deltaE reference blur, expressed through blurcolde. */
    float blurcol = 0.f;
    /* 0 selects the elliptical transition, 1 the rectangular one. */
    int shapmet = 0;
    /* 0 when the quality curves are off, 1 for the standard curves. */
    int qualcurvemet = 0;
    /* 0 when no merge method is selected. */
    int mergemet = 0;
    bool activspot = false;
    bool colorena = false;
    bool islocal = false;
    /* spot.deltae, as transit_shapedetect2() reads it. */
    bool deltaem = false;
    /* spot.blwh: the deltaE mask keeps L only. */
    bool blwh = false;
};

} // namespace rtengine
