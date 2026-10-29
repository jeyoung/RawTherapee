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

/*
 * The Lab-curve pass of the lab.tools node: a port of
 * ImProcFunctions::chromiLuminanceCurve() for the supported configuration.
 *
 * The host builds all six tables with the block's own CurveFactory calls and
 * exports them; the kernel applies them with LUTf::operator[](float)
 * semantics. Nothing here re-derives a curve or a Munsell table.
 *
 * Supported: the L, a, b, C=f(C), L=f(C) and C=f(L) tables, the MUN and NONE
 * Munsell methods, the red and skin protection, `lcredsk`, and the block's
 * chroma scale for the working profile. Residual (the launcher never reaches
 * this kernel): a non-zero contrast (it owns the hist16 refill), the
 * chromaticity slider and the C=f(H), L=f(H), H=f(H) flat curves, the LAB,
 * XYZ and XYZREL Munsell methods, black-and-white output and a denoised
 * chroma factor.
 *
 * The source is appended after lab_tools_cl.h, munsell_lch_cl.h and
 * vibrance_cl.h, so it reads their LUT, atan2/sincos and Munsell helpers.
 */
inline constexpr const char* RT_LAB_CURVE_CL = R"CLC(
/* rtengine::min()/max()/LIM(), with the host's comparison order. */
inline float rt_lt_min(const float a, const float b)
{
    return b < a ? b : a;
}

inline float rt_lt_max(const float a, const float b)
{
    return a < b ? b : a;
}

inline float rt_lt_lim(const float value, const float low, const float high)
{
    return rt_lt_max(low, rt_lt_min(value, high));
}

/*
 * chromiLuminanceCurve()'s scalar chroma: `sqrt(float) / 327.68f` in the host
 * is the libm *double* square root divided by the promoted float constant and
 * then rounded to float, because `sqrt` resolves to the double overload there.
 * The kernel must repeat that double rounding: the Munsell tables index the
 * chroma through `int`, so a one-ulp difference moves a pixel to the next
 * table entry and changes its hue by tens of LSB16. The program therefore
 * enables cl_khr_fp64 and the launcher refuses a device without it.
 */
inline float rt_lt_sqrt_f(const float value)
{
    return (float)sqrt((double)value);
}

inline float rt_lt_sqrt_div(const float value)
{
    return (float)(sqrt((double)value) / (double)327.68f);
}

/* The SSE2 precompute the host runs when the a and b tables are inactive:
 * `vsqrtf(SQRV) / 327.68f` in single precision. */
inline float rt_lt_sqrt_div_f32(const float value)
{
    return sqrt(value) / 327.68f;
}

/*
 * LUTf::operator[](float) with LUT_CLIP_OFF, the flags simpleprocess.cc gives
 * lumacurve, satcurve, lhskcurve and clcurve: an out-of-range index
 * extrapolates from the first or last pair of entries instead of clamping.
 */
inline float rt_lt_lut_raw(__global const float* lut, const int lut_size,
                           const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        idx = 0;
    } else if (index > (float)(lut_size - 2)) {
        idx = lut_size - 2;
    }
    const float low = lut[idx];
    const float high = lut[idx + 1] - low;
    return low + high * (index - (float)idx);
}

/*
 * Color::scalered(), with the two out-parameters made explicit.
 *
 * noinline is deliberate: POCL 6.0's LLVM 18 backend aborts with
 * "Instruction Combining did not reach a fixpoint" when this function's two
 * hue-transition expressions are inlined into the Lab-curve kernel. The
 * attribute changes code generation only; the arithmetic is untouched.
 */
__attribute__((noinline)) void rt_lt_scalered(
    const float rstprotection, const float param, const float limit,
    const float HH, const float deltaHH, float* scale, float* scaleext)
{
    if (rstprotection < 99.9999f) {
        if (param > limit) {
            *scale = rstprotection / 100.1f;
        }

        if (HH < (1.3f + deltaHH) && HH >= 1.3f) {
            *scaleext = (HH * (1.0f - *scale) + deltaHH -
                         (1.3f + deltaHH) * (1.0f - *scale)) /
                        deltaHH;
        } else if (HH < 0.15f && HH > (0.15f - deltaHH)) {
            *scaleext = (HH * (*scale - 1.0f) + deltaHH -
                         (0.15f - deltaHH) * (*scale - 1.0f)) /
                        deltaHH;
        }
    }
}

/* Color::transitred(), returning the reference argument. */
inline float rt_lt_transitred(const float HH, const float Chprov1,
                              const float dred, const float factorskin,
                              const float protect_red,
                              const float factorskinext, const float deltaHH,
                              const float factorsat, const float factor)
{
    float result = factor;
    if (HH >= 0.15f && HH < 1.3f) {
        if (Chprov1 < dred) {
            result = factorskin;
        } else if (Chprov1 < (dred + protect_red)) {
            result = ((factorsat - factorskin) * Chprov1 +
                      factorsat * protect_red -
                      (dred + protect_red) * (factorsat - factorskin)) /
                     protect_red;
        }
    } else if (HH > (0.15f - deltaHH) && HH < (1.3f + deltaHH)) {
        if (Chprov1 < dred) {
            result = factorskinext;
        } else if (Chprov1 < (dred + protect_red)) {
            result = ((factorsat - factorskinext) * Chprov1 +
                      factorsat * protect_red -
                      (dred + protect_red) * (factorsat - factorskinext)) /
                     protect_red;
        }
    }
    return result;
}

/*
 * Color::AllMunsellLch(lumaMuns, Lprov1, Loldd, HH, Chprov1, CC,
 * correctionHuechroma, correctlum) with lumaMuns true, the overload
 * chromiLuminanceCurve() calls. The single-correction overload in
 * munsell_lch_cl.h is vibrance's; this one adds the luminance correction and
 * the 6-chroma clamp difference.
 */
inline void rt_lt_all_munsell_lch_lum(
    const float Lprov1, const float Loldd, const float HH,
    const float Chprov1_in, const float CC, __global const float* tables,
    __global const float* sizes, float* correctionHuechroma, float* correctlum)
{
    float correctionHuechroma_v = 0.f;
    float correctlum_v = 0.f;
    if (CC >= 6.f && CC < 140.f) {
        const float huelimit[8] = {-2.48f, -0.55f, 0.44f, 1.52f,
                                   1.87f, 3.09f, -0.27f, 0.44f};
        const float Chprov1 = rt_lt_lim(Chprov1_in, 6.f, 140.f);

        for (int zo = 1; zo <= 4; ++zo) {
            if (HH > huelimit[2 * zo - 2] && HH < huelimit[2 * zo - 1]) {
                int correctL = 0;
                float correctionHue = 0.f;
                float correctionHueLum = 0.f;
                rt_lt_munsell_lch(Lprov1, HH, Chprov1, CC, &correctionHue, zo,
                                  &correctionHueLum, &correctL, tables, sizes);
                correctionHuechroma_v = correctionHue;

                int contin1 = 0;
                float correctlumprov = 0.f;
                if (correctL) {
                    correctlumprov = correctionHueLum;
                    contin1 = 1;
                    correctL = 0;
                }

                if (fabs(Lprov1 - Loldd) > 6.f) {
                    correctionHueLum = 0.f;
                    correctionHue = 0.f;
                    rt_lt_munsell_lch(Loldd, HH, Chprov1, Chprov1,
                                      &correctionHue, zo, &correctionHueLum,
                                      &correctL, tables, sizes);
                    if (correctL) {
                        if (contin1) {
                            correctlum_v = correctionHueLum - correctlumprov;
                        }
                    }
                }
                break;
            }
        }
    }
    *correctionHuechroma = correctionHuechroma_v;
    *correctlum = correctlum_v;
}

/*
 * chromiLuminanceCurve()'s per-pixel body for the supported configuration.
 *
 * `method` is RT_LAB_TOOLS_MUNSELL_NONE or _MUN. `rst_protection` is
 * 100 - rstprotection, `protect_red`/`protect_red_h` are the raw
 * settings->protectred and settings->protectredh values, and `adjust` is the
 * working-profile chroma scale for the C=f(C) and C=f(L) table index.
 */
__kernel void rt_lab_tools_lab_curve(
    __global float* lab,
    __global const float* lcurve, const int lcurve_size,
    __global const float* acurve,
    __global const float* bcurve,
    __global const float* satcurve,
    __global const float* lhskcurve,
    __global const float* clcurve,
    const int curve_size,
    const int l_active, const int a_active, const int b_active,
    const int cc_active, const int lc_active, const int cl_active,
    const int method, const float rst_protection,
    const float protect_red_raw, const float protect_red_h_raw,
    const float adjust, const int lcredsk,
    __global const float* munsell, __global const float* munsell_sizes,
    const int width, const int height,
    const int lab_pitch, const int lab_plane)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float Lin = lab[offset];
    const float LL = Lin / 327.68f;
    float atmp = lab[lab_plane + offset];
    float btmp = lab[2 * lab_plane + offset];

    float Lprov1 = rt_lt_lut_raw(lcurve, lcurve_size, Lin) / 327.68f;

    if (a_active != 0) {
        atmp = rt_lt_lut(acurve, curve_size, atmp + 32768.0f) - 32768.0f;
    }
    if (b_active != 0) {
        btmp = rt_lt_lut(bcurve, curve_size, btmp + 32768.0f) - 32768.0f;
    }

    float CC = (a_active != 0 || b_active != 0)
                   ? rt_lt_sqrt_div(atmp * atmp + btmp * btmp)
                   : rt_lt_sqrt_div_f32(atmp * atmp + btmp * btmp);
    float HH = rt_lt_xatan2f(btmp, atmp);
    float2 sincosval;
    if (CC == 0.0f) {
        sincosval.y = 1.0f;
        sincosval.x = 0.0f;
    } else {
        sincosval.y = atmp / (CC * 327.68f);
        sincosval.x = btmp / (CC * 327.68f);
    }
    const float memChprov = CC;
    float Chprov1 = CC;

    /*
     * The block's first red/skin gate. With chromapro at 1 and the denoise
     * factor at 1 every factor is 1, so the block's product leaves a and b
     * untouched; the sequence is kept so the branch conditions are the
     * host's.
     */
    float dred;
    if (Lprov1 < 25.f) {
        dred = 40.f;
    } else if (Lprov1 < 30.f) {
        dred = 3.f * Lprov1 - 35.f;
    } else if (Lprov1 < 70.f) {
        dred = 55.f;
    } else if (Lprov1 < 75.f) {
        dred = -3.f * Lprov1 + 265.f;
    } else {
        dred = 40.f;
    }

    const float protect_red = rt_lt_lim(protect_red_raw, 20.f, 180.f);
    const float protect_red_h = rt_lt_lim(protect_red_h_raw, 0.1f, 1.f);
    float factor = rt_lt_transitred(HH, Chprov1, dred, 1.f, protect_red, 1.f,
                                    protect_red_h, 1.f, 1.f);
    atmp *= factor;
    btmp *= factor;

    /* C=f(L) */
    if (cl_active != 0 && LL > 0.f) {
        const float curf = 0.7f;
        float scale = 100.0f / 100.1f;
        float scaleext = 1.0f;
        const float chroma = LL * 655.35f;
        const float chromaCfactor =
            rt_lt_lut_raw(clcurve, curve_size, chroma) / chroma;
        float protect_redcur = curf * protect_red_raw;
        if (protect_redcur < 20.f) {
            protect_redcur = 20.0f;
        }
        if (protect_redcur > 180.f) {
            protect_redcur = 180.0f;
        }
        float protect_redhcur = curf * protect_red_h_raw;
        if (protect_redhcur < 0.1f) {
            protect_redhcur = 0.1f;
        }
        if (protect_redhcur > 1.0f) {
            protect_redhcur = 1.0f;
        }
        const float deltaHH = protect_redhcur;

        if (chromaCfactor > 0.f) {
            rt_lt_scalered(rst_protection, chromaCfactor, 0.0f, HH, deltaHH,
                           &scale, &scaleext);
        }

        float factorskin;
        float factorskinext;
        if (chromaCfactor > 1.f) {
            const float interm = (chromaCfactor - 1.0f) * 100.0f;
            factorskin = 1.0f + (interm * scale) / 100.0f;
            factorskinext = 1.0f + (interm * scaleext) / 100.0f;
        } else {
            factorskin = chromaCfactor;
            factorskinext = chromaCfactor;
        }

        const float factorsat = chromaCfactor;
        factor = rt_lt_transitred(HH, Chprov1, dred, factorskin, protect_redcur,
                                  factorskinext, deltaHH, factorsat, factorsat);
        atmp = rt_lt_lim(atmp * factor, rt_lt_min(-42000.f, atmp),
                         rt_lt_max(42000.f, atmp));
        btmp = rt_lt_lim(btmp * factor, rt_lt_min(-42000.f, btmp),
                         rt_lt_max(42000.f, btmp));
    }

    /* C=f(C) */
    if (cc_active != 0) {
        const float curf = 0.7f;
        float scale = 100.0f / 100.1f;
        float scaleext = 1.0f;
        const float chroma = rt_lt_sqrt_f(atmp * atmp + btmp * btmp + 0.001f);
        const float chromaCfactor =
            rt_lt_lut_raw(satcurve, curve_size, chroma * adjust) /
            (chroma * adjust);
        float protect_redcur = curf * protect_red_raw;
        if (protect_redcur < 20.f) {
            protect_redcur = 20.0f;
        }
        if (protect_redcur > 180.f) {
            protect_redcur = 180.0f;
        }
        float protect_redhcur = curf * protect_red_h_raw;
        if (protect_redhcur < 0.1f) {
            protect_redhcur = 0.1f;
        }
        if (protect_redhcur > 1.0f) {
            protect_redhcur = 1.0f;
        }
        const float deltaHH = protect_redhcur;

        if (chromaCfactor > 0.f) {
            rt_lt_scalered(rst_protection, chromaCfactor, 0.0f, HH, deltaHH,
                           &scale, &scaleext);
        }

        float factorskin;
        float factorskinext;
        if (chromaCfactor > 1.f) {
            const float interm = (chromaCfactor - 1.0f) * 100.0f;
            factorskin = 1.0f + (interm * scale) / 100.0f;
            factorskinext = 1.0f + (interm * scaleext) / 100.0f;
        } else {
            factorskin = chromaCfactor;
            factorskinext = chromaCfactor;
        }

        const float factorsat = chromaCfactor;
        factor = rt_lt_transitred(HH, Chprov1, dred, factorskin, protect_redcur,
                                  factorskinext, deltaHH, factorsat, factorsat);
        atmp *= factor;
        btmp *= factor;
    }

    /* L=f(C) */
    if (lc_active != 0) {
        const float xx = 0.25f;
        float skdeltaHH = rt_lt_lim(protect_red_h_raw, 0.1f, 3.5f);
        float skbeg = -0.05f;
        float skend = 1.60f;
        const float chrmin = 50.0f;
        float zz = 0.0f;
        float yy;

        if (Chprov1 < chrmin) {
            yy = (Chprov1 / chrmin) * (Chprov1 / chrmin) * xx;
        } else {
            yy = xx;
        }

        if (lcredsk == 0) {
            skbeg = -3.1415f;
            skend = 3.14159f;
            skdeltaHH = 0.001f;
        }

        if (HH > skbeg && HH < skend) {
            zz = yy;
        } else if (HH > skbeg - skdeltaHH && HH <= skbeg) {
            const float aa = yy / skdeltaHH;
            const float bb = -aa * (skbeg - skdeltaHH);
            zz = aa * HH + bb;
        } else if (HH >= skend && HH < skend + skdeltaHH) {
            const float aa = -yy / skdeltaHH;
            const float bb = -aa * (skend + skdeltaHH);
            zz = aa * HH + bb;
        }

        const float chroma = rt_lt_sqrt_f(atmp * atmp + btmp * btmp + 0.001f);
        const float ca = chroma * adjust;
        float Lc = rt_lt_lut_raw(lhskcurve, curve_size, ca) / ca;
        Lc = (Lc - 1.0f) * zz + 1.0f;
        Lprov1 *= Lc;
    }

    Chprov1 = rt_lt_sqrt_div(atmp * atmp + btmp * btmp);
    float out_l = Lprov1 * 327.68f;
    float out_a = 327.68f * Chprov1 * sincosval.y;
    float out_b = 327.68f * Chprov1 * sincosval.x;

    /*
     * The Munsell correction. The block guards it with the activity test; at
     * least one table is active whenever the method is MUN and the launcher
     * only sends the pass when it has one, but the test is kept.
     */
    if (method != 0 &&
        (l_active != 0 || a_active != 0 || b_active != 0 || cc_active != 0 ||
         lc_active != 0 || cl_active != 0)) {
        const float Lprov = out_l / 327.68f;
        const float Chprov = rt_lt_sqrt_div(out_a * out_a + out_b * out_b);
        float correctionHue = 0.f;
        float correctlum = 0.f;
        rt_lt_all_munsell_lch_lum(Lprov, LL, HH, Chprov, memChprov, munsell,
                                  munsell_sizes, &correctionHue, &correctlum);

        if (correctionHue != 0.f || correctlum != 0.f) {
            if (fabs(correctionHue) < 0.015f) {
                HH += correctlum;
            }
            sincosval = rt_lt_xsincosf(HH + correctionHue);
        }

        out_a = 327.68f * Chprov * sincosval.y;
        out_b = 327.68f * Chprov * sincosval.x;
    }

    lab[offset] = out_l;
    lab[lab_plane + offset] = out_a;
    lab[2 * lab_plane + offset] = out_b;
}
)CLC";
