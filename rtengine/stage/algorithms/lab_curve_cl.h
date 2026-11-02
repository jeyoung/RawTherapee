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
 * Supported: the L, a, b, C=f(C), L=f(C) and C=f(L) tables, the C=f(H),
 * L=f(H) and H=f(H) hue flat curves (host-sampled piecewise-linear tables),
 * the chromaticity slider, the NONE, LAB, XYZ, XYZREL and MUN Munsell methods,
 * the red and skin protection, `lcredsk`, the block's chroma scale for the
 * working profile, and black-and-white output through
 * `chromiLuminanceCurve()`'s `bwToning` arm. Under `bwToning` the hue and
 * chroma come from the original pixel before the a and b tables, the
 * chromaticity gate, the C=f(L) and C=f(C) tables and the red/skin gate are
 * skipped, L=f(C) is skipped, and the closing write is the a/b tables'
 * deviation from the original chroma (exactly zero when neither table is
 * active). The three hue flat curves still run under `bwToning`, exactly as
 * the block orders them. Residual (the launcher never reaches this kernel): a
 * non-zero contrast (it owns the hist16 refill), a hue flat curve the host
 * table cannot follow, a chromaticity slider outside -100..100 and a denoised
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

/* chromiLuminanceCurve()'s gamutmunselmethod numbering. */
#define RT_LT_METHOD_NONE 0
#define RT_LT_METHOD_LAB 1
#define RT_LT_METHOD_XYZ 2
#define RT_LT_METHOD_XYZREL 3

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
 * Color::huelab_to_huehsv2(): the block's Lab hue in radians to the HSV-like
 * hue in [0,1] that indexes the three hue flat curves. The arithmetic is the
 * host's double expression; the input is the kernel's float HH.
 */
inline double rt_lt_huehsv2(const float HH)
{
    double hr = 0.0;
    if (HH >= 0.0f && HH < 0.6f) {
        hr = 0.11666 * (double)HH + 0.93;
    } else if (HH >= 0.6f && HH < 1.4f) {
        hr = 0.1125 * (double)HH - 0.0675;
    } else if (HH >= 1.4f && HH < 2.0f) {
        hr = 0.2666 * (double)HH - 0.2833;
    } else if (HH >= 2.0f && HH < 3.14159f) {
        hr = 0.1489 * (double)HH - 0.04785;
    } else if (HH >= -3.14159f && HH < -2.8f) {
        hr = 0.23419 * (double)HH + 1.1557;
    } else if (HH >= -2.8f && HH < -2.3f) {
        hr = 0.16 * (double)HH + 0.948;
    } else if (HH >= -2.3f && HH < -0.9f) {
        hr = 0.12143 * (double)HH + 0.85928;
    } else if (HH >= -0.9f && HH < -0.1f) {
        hr = 0.2125 * (double)HH + 0.94125;
    } else if (HH >= -0.1f && HH < 0.0f) {
        hr = 0.1 * (double)HH + 0.93;
    }
    if (hr < 0.0) {
        hr += 1.0;
    } else if (hr > 1.0) {
        hr -= 1.0;
    }
    return hr;
}

/*
 * One host-sampled hue flat curve. slot[0] is the node count, then that many
 * x nodes, then that many y nodes. The lookup matches the host's validator:
 * linear inside the table and extrapolated with the first/last segment slope
 * outside it.
 */
inline double rt_lt_hue_lut(__global const float* slot, const double t)
{
    const int count = (int)slot[0];
    if (count < 2) {
        return 0.0;
    }
    __global const float* xs = slot + 1;
    __global const float* ys = slot + 1 + count;
    if (t <= (double)xs[0]) {
        const double slope = ((double)ys[1] - (double)ys[0]) /
                             ((double)xs[1] - (double)xs[0]);
        return (double)ys[0] + (t - (double)xs[0]) * slope;
    }
    if (t >= (double)xs[count - 1]) {
        const double slope = ((double)ys[count - 1] - (double)ys[count - 2]) /
                             ((double)xs[count - 1] - (double)xs[count - 2]);
        return (double)ys[count - 1] +
               (t - (double)xs[count - 1]) * slope;
    }
    int low = 0;
    int high = count - 1;
    while (high - low > 1) {
        const int middle = (low + high) / 2;
        if ((double)xs[middle] <= t) {
            low = middle;
        } else {
            high = middle;
        }
    }
    const double fraction = (t - (double)xs[low]) /
                            ((double)xs[high] - (double)xs[low]);
    return (double)ys[low] +
           ((double)ys[high] - (double)ys[low]) * fraction;
}

/* Color::xyz2rgb() on the block's double matrix: double products, narrowed on
 * store like the host's float out-parameters. */
inline void rt_lt_xyz2rgb_d(const float x, const float y, const float z,
                            __global const double* m, float* r, float* g,
                            float* b)
{
    *r = (float)(m[0] * (double)x + m[1] * (double)y + m[2] * (double)z);
    *g = (float)(m[3] * (double)x + m[4] * (double)y + m[5] * (double)z);
    *b = (float)(m[6] * (double)x + m[7] * (double)y + m[8] * (double)z);
}

/*
 * Color::gamutLchonly(HH, sincosval, Lprov1, Chprov1, R, G, B, wip,
 * isHLEnabled, lowerCoef, higherCoef): the RGB overload chromiLuminanceCurve()'s
 * LAB method calls, with ClipLevel 65535 and the working-space inverse matrix
 * as a flat nine-double array. It writes the corrected R, G, B and the
 * corrected L and C back. `HH` is finite at the host call because the block
 * derives it before this arm; the host's `isnan(HH)` recovery is kept so the
 * helper still follows the source if that ever changes.
 */
inline void rt_lt_gamut_lchonly_rgb(
    const float HH, const float2 sincosval, float* Lprov1_out,
    float* Chprov1_out, float* R_out, float* G_out, float* B_out,
    __global const double* wip, const int isHLEnabled, const float lowerCoef,
    const float higherCoef)
{
    /* Color::gamutLchonly()'s RGB overload: ClipLevel 65535, not vibrance's
     * saturation overload's 1.0. */
    const float clip_level = 65535.0f;
    float Lprov1 = *Lprov1_out;
    float Chprov1 = *Chprov1_out;
    const float ChprovSave = Chprov1;
    float hh = HH;
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    int inGamut;

    do {
        inGamut = 1;

        const float aprov1 = Chprov1 * sincosval.y;
        const float bprov1 = Chprov1 * sincosval.x;

        const float fy = RT_LT_C1BY116 * Lprov1 + RT_LT_C16BY116;
        const float fx = 0.002f * aprov1 + fy;
        const float fz = fy - 0.005f * bprov1;

        const float x_ = 65535.0f * rt_lt_f2xyz(fx) * RT_LT_D50X;
        const float z_ = 65535.0f * rt_lt_f2xyz(fz) * RT_LT_D50Z;
        const float y_ = (Lprov1 > RT_LT_EPSKAP)
                             ? 65535.0f * fy * fy * fy
                             : 65535.0f * Lprov1 / RT_LT_KAPPA;

        rt_lt_xyz2rgb_d(x_, y_, z_, wip, &R, &G, &B);

        if (R < 0.0f || G < 0.0f || B < 0.0f) {
            if (isnan(HH)) {
                const float atemp = ChprovSave * sincosval.y * 327.68;
                const float btemp = ChprovSave * sincosval.x * 327.68;
                hh = rt_lt_xatan2f(btemp, atemp);
            }

            if (Lprov1 < 0.1f) {
                Lprov1 = 0.1f;
            }

            /* Gamut for L with ultra blue. */
            if (hh < -0.9f && hh > -1.55f) {
                if (Chprov1 > 160.0f && Lprov1 < 5.0f) {
                    Lprov1 = 5.0f;
                }
                if (Chprov1 > 140.0f && Lprov1 < 3.5f) {
                    Lprov1 = 3.5f;
                }
                if (Chprov1 > 120.0f && Lprov1 < 2.0f) {
                    Lprov1 = 2.0f;
                }
                if (Chprov1 > 105.0f && Lprov1 < 1.0f) {
                    Lprov1 = 1.0f;
                }
                if (Chprov1 > 90.0f && Lprov1 < 0.7f) {
                    Lprov1 = 0.7f;
                }
                if (Chprov1 > 50.0f && Lprov1 < 0.5f) {
                    Lprov1 = 0.5f;
                }
                if (Chprov1 > 20.0f && Lprov1 < 0.4f) {
                    Lprov1 = 0.4f;
                }
            }

            Chprov1 *= higherCoef;

            if (Chprov1 <= 3.0f) {
                Lprov1 += lowerCoef;
            }

            inGamut = 0;
        } else if (!isHLEnabled &&
                   rt_lt_max(rt_lt_max(R, G), B) > clip_level &&
                   rt_lt_min(rt_lt_min(R, G), B) <= clip_level) {
            /* A clipped, coloured highlight: only with highlight
             * reconstruction off, as in the host. */
            if (Lprov1 > 99.999f) {
                Lprov1 = 99.98f;
            }

            Chprov1 *= higherCoef;

            if (Chprov1 <= 3.0f) {
                Lprov1 -= lowerCoef;
            }

            inGamut = 0;
        }
    } while (!inGamut);

    *Lprov1_out = Lprov1;
    *Chprov1_out = Chprov1;
    *R_out = R;
    *G_out = G;
    *B_out = B;
}

/*
 * Color::gamutmap(), arithmetic for arithmetic. `p` is chromiLuminanceCurve()'s
 * `wp`, ICCStore::workingSpaceMatrix(), row-major: columns are the RGB
 * tristimulus primaries, which is how the host indexes `p[0][c]`, `p[1][c]`
 * and `p[2][c]`. The lam ratios are double because `p` is; u, v, lam_min and
 * the final XYZ stay float, as in the host.
 */
inline void rt_lt_gamutmap(float* X, const float Y, float* Z,
                           __global const double* p)
{
    const float epsil = 0.0001f;
    float intermXYZ = *X + 15.f * Y + 3.f * *Z;
    if (intermXYZ <= 0.f) {
        intermXYZ = epsil;
    }

    /* Color::u0 and Color::v0, from the same float D50 constants. */
    const double u0 = 4.0 * (double)RT_LT_D50X /
                      ((double)RT_LT_D50X + 15.0 + 3.0 * (double)RT_LT_D50Z);
    const double v0 = 9.0 /
                      ((double)RT_LT_D50X + 15.0 + 3.0 * (double)RT_LT_D50Z);

    /*
     * The host's `4 * X / intermXYZ - u0`: the quotient is float and the
     * subtraction uses the double u0, narrowed on assignment.
     */
    const float uq = 4.f * *X / intermXYZ;
    const float vq = 9.f * Y / intermXYZ;
    float u = (float)((double)uq - u0);
    float v = (float)((double)vq - v0);
    float lam[6];
    float lam_min = 1.0f;

    const double p0[3] = {p[0], p[1], p[2]};
    const double p1[3] = {p[3], p[4], p[5]};
    const double p2[3] = {p[6], p[7], p[8]};

    for (int c = 0; c < 3; ++c) {
        const int c1 = (c + 1) % 3;
        const int c2 = (c + 2) % 3;
        for (int m = 0; m < 2; ++m) {
            double value =
                -(p0[c1] * p1[c] *
                  ((-12.0 + 3.0 * u0 + 20.0 * v0) * (double)Y +
                   4.0 * (double)(m * 65535) * v0 * p2[c2])) +
                p0[c] * p1[c1] *
                    ((-12.0 + 3.0 * u0 + 20.0 * v0) * (double)Y +
                     4.0 * (double)(m * 65535) * v0 * p2[c2]) -
                4.0 * v0 * p0[c1] *
                    ((double)Y - (double)(m * 65535) * p1[c2]) * p2[c] +
                4.0 * v0 * p0[c] *
                    ((double)Y - (double)(m * 65535) * p1[c2]) * p2[c1] -
                (4.0 * (double)(m * 65535) * v0 * p0[c2] -
                 9.0 * u0 * (double)Y) *
                    (p1[c1] * p2[c] - p1[c] * p2[c1]);
            const double denominator =
                (3.f * u * Y) *
                    (p0[c1] * p1[c] - p1[c1] * (p0[c] + 3.0 * p2[c]) +
                     3.0 * p1[c] * p2[c1]) +
                (4.f * v) *
                    (p0[c1] * ((5.f * Y) * p1[c] +
                               (double)(m * 65535) * p1[c] * p2[c2] +
                               (double)Y * p2[c] -
                               (double)(m * 65535) * p1[c2] * p2[c]) -
                     p0[c] * ((5.f * Y) * p1[c1] +
                              (double)(m * 65535) * p1[c1] * p2[c2] +
                              (double)Y * p2[c1] -
                              (double)(m * 65535) * p1[c2] * p2[c1]) +
                     (double)(m * 65535) * p0[c2] *
                         (p1[c1] * p2[c] - p1[c] * p2[c1]));
            lam[c * 2 + m] = (float)value;
            lam[c * 2 + m] = (float)((double)lam[c * 2 + m] / denominator);
            if (lam[c * 2 + m] < lam_min && lam[c * 2 + m] > 0.f) {
                lam_min = lam[c * 2 + m];
            }
        }
    }

    u = (float)((double)u * (double)lam_min + u0);
    v = (float)((double)v * (double)lam_min + v0);

    *X = (9.f * u * Y) / (4.f * v);
    float intermuv = 12.f - 3.f * u - 20.f * v;
    if (intermuv < 0.f) {
        intermuv = 0.f;
    }
    *Z = intermuv * Y / (4.f * v);
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
 * `method` is RT_LAB_TOOLS_MUNSELL_NONE, _LAB, _XYZ, _XYZREL or _MUN.
 * `rst_protection` is 100 - rstprotection, `protect_red`/`protect_red_h` are
 * the raw settings->protectred and settings->protectredh values, and
 * `adjust` is the working-profile chroma scale for the C=f(C) and C=f(L)
 * table index.
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
    const float adjust, const int lcredsk, const int bw_toning,
    __global const float* munsell, __global const float* munsell_sizes,
    const int width, const int height,
    const int lab_pitch, const int lab_plane,
    __global const float* hue_luts, const int hue_stride,
    const int ch_active, const int lh_active, const int hh_active,
    const int chromaticity, const float amount_chroma,
    __global const double* wip, const int highlight,
    __global const double* wp, __global const float* cachef,
    __global const float* cachefy)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float Lin = lab[offset];
    const float LL = Lin / 327.68f;
    const float a_orig = lab[lab_plane + offset];
    const float b_orig = lab[2 * lab_plane + offset];
    float atmp = a_orig;
    float btmp = b_orig;

    float Lprov1 = rt_lt_lut_raw(lcurve, lcurve_size, Lin) / 327.68f;

    /* Both arms apply the a and b tables; `bwToning` keeps the curved values
     * for its closing deviation instead of re-deriving the hue from them. */
    if (a_active != 0) {
        atmp = rt_lt_lut(acurve, curve_size, atmp + 32768.0f) - 32768.0f;
    }
    if (b_active != 0) {
        btmp = rt_lt_lut(bcurve, curve_size, btmp + 32768.0f) - 32768.0f;
    }

    /*
     * chromiLuminanceCurve()'s SSE2 precompute covers the columns its
     * `for (k = 0; k < W - 3; k += 4)` loop covers: 4 * (floor((W-4)/4) + 1)
     * columns, or none when W < 4. Its tail loop, the last up to three
     * columns, uses the scalar double sqrt instead, and so does the whole
     * frame when the a or b table is active and `bwToning` is off (the
     * precompute's guard is `bwToning || (!autili && !butili)`). Reproduce
     * both.
     */
    const int simd_columns = width >= 4 ? 4 * (((width - 4) / 4) + 1) : 0;
    const bool host_precompute =
        bw_toning != 0 || (a_active == 0 && b_active == 0);
    /*
     * The hue and chroma source. `!bwToning` takes them from the curved a and
     * b values; `bwToning` takes them from the original pixel, before the
     * tables, and keeps them for the closing write.
     */
    const float hue_a = bw_toning != 0 ? a_orig : atmp;
    const float hue_b = bw_toning != 0 ? b_orig : btmp;
    float CC = rt_lt_sqrt_div(hue_a * hue_a + hue_b * hue_b);
    if (host_precompute && x < simd_columns) {
        CC = rt_lt_sqrt_div_f32(hue_a * hue_a + hue_b * hue_b);
    }
    float HH = rt_lt_xatan2f(hue_b, hue_a);
    float2 sincosval;
    if (CC == 0.0f) {
        sincosval.y = 1.0f;
        sincosval.x = 0.0f;
    } else {
        sincosval.y = hue_a / (CC * 327.68f);
        sincosval.x = hue_b / (CC * 327.68f);
    }
    const float memChprov = CC;
    float Chprov1 = CC;

    /*
     * L=f(H). The block builds this curve only when `chromaticity > -100`,
     * which the host setup enforces, and reads the hue HH as it stands after
     * the a and b tables. The lightness travels through the block's chroma
     * weight `kcc = CC / amchroma` and then a gamut loop that scales the
     * chroma down until the pixel fits the working space.
     */
    if (lh_active != 0) {
        const float ClipLevel = 65535.f;
        const float valparam_raw =
            (float)(rt_lt_hue_lut(hue_luts + hue_stride, rt_lt_huehsv2(HH)) -
                    0.5);
        float valparam = valparam_raw;
        float valparamneg = valparam_raw;
        const float kcc = CC / amount_chroma;
        valparam *= 2.f * kcc;
        valparamneg *= kcc;
        float l_r = Lprov1 / 100.f;
        if (valparam > 0.f) {
            l_r = (1.f - valparam) * l_r +
                  valparam *
                      (1.f - rt_lt_pow4(1.f - rt_lt_min(l_r, 1.0f)));
        } else {
            const float khue = 1.9f;
            l_r *= (1.f + khue * valparamneg);
        }
        Lprov1 = l_r * 100.f;

        float Chprov2 = rt_lt_sqrt_div(atmp * atmp + btmp * btmp);
        bool in_rgb;
        do {
            in_rgb = true;
            const float aprov1 = Chprov2 * sincosval.y;
            const float bprov1 = Chprov2 * sincosval.x;
            const float fy = (RT_LT_C1BY116 * Lprov1) + RT_LT_C16BY116;
            const float fx = (0.002f * aprov1) + fy;
            const float fz = fy - (0.005f * bprov1);
            const float x_ = 65535.f *
                             ((fx > RT_LT_EPSINV3) ? fx * fx * fx
                                                   : (116.f * fx - 16.f) *
                                                         RT_LT_KAPPAINV) *
                             RT_LT_D50X;
            const float z_ = 65535.f *
                             ((fz > RT_LT_EPSINV3) ? fz * fz * fz
                                                   : (116.f * fz - 16.f) *
                                                         RT_LT_KAPPAINV) *
                             RT_LT_D50Z;
            const float y_ = Lprov1 > RT_LT_EPSKAP
                                 ? 65535.f * fy * fy * fy
                                 : 65535.f * Lprov1 / RT_LT_KAPPA;
            float R, G, B;
            rt_lt_xyz2rgb_d(x_, y_, z_, wip, &R, &G, &B);
            if (R < 0.0f || G < 0.0f || B < 0.0f) {
                if (Lprov1 < 0.1f) {
                    Lprov1 = 0.1f;
                }
                Chprov2 *= 0.95f;
                in_rgb = false;
            } else if (highlight == 0 &&
                       (R > ClipLevel || G > ClipLevel || B > ClipLevel)) {
                if (Lprov1 > 99.98f) {
                    Lprov1 = 99.98f;
                }
                Chprov2 *= 0.95f;
                in_rgb = false;
            }
        } while (!in_rgb);

        atmp = 327.68f * Chprov2 * sincosval.y;
        btmp = 327.68f * Chprov2 * sincosval.x;
    }

    /* C=f(H): the block's `atmp *= 1 + (getVal(hr) - 0.5) * 2`. */
    if (ch_active != 0) {
        const float chparam =
            (float)((rt_lt_hue_lut(hue_luts, rt_lt_huehsv2(HH)) - 0.5) *
                    2.0);
        const float chromaChfactor = 1.0f + chparam;
        atmp *= chromaChfactor;
        btmp *= chromaChfactor;
    }

    /* H=f(H): rotate the hue and rebuild the direction, magnitude unchanged. */
    if (hh_active != 0) {
        const float valparam =
            (float)((rt_lt_hue_lut(hue_luts + 2 * hue_stride,
                                   rt_lt_huehsv2(HH)) -
                     0.5) *
                        1.7 +
                    (double)HH);
        HH = valparam;
        sincosval = rt_lt_xsincosf(HH);
    }

    /*
     * The block's first red/skin gate, which only the `!bwToning` arm runs.
     * It carries the chromaticity slider: chromapro scales a and b about the
     * grey axis, with the red/skin transition when it exceeds one. The
     * denoise factor is one because the supported set refuses the denoise
     * tool.
     */
    const float protect_red = rt_lt_lim(protect_red_raw, 20.f, 180.f);
    const float protect_red_h = rt_lt_lim(protect_red_h_raw, 0.1f, 1.f);
    if (bw_toning == 0) {
        const float chromapro =
            ((float)chromaticity + 100.0f) / 100.0f;
        float factorskinc;
        float factorskinextc;
        if (chromapro > 1.f) {
            float scale = 100.0f / 100.1f;
            float scaleext = 1.f;
            rt_lt_scalered(rst_protection, chromapro, 0.0f, HH,
                           protect_red_h, &scale, &scaleext);
            const float interm = chromapro - 1.f;
            factorskinc = 1.f + (interm * scale);
            factorskinextc = 1.f + (interm * scaleext);
        } else {
            factorskinc = chromapro;
            factorskinextc = chromapro;
        }
        /*
         * The block passes `factorsatc` as both the saturation factor and the
         * reference `factor` argument, so the red/skin gate may overwrite it
         * before the product.
         */
        float factorsatc = chromapro;

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

        factorsatc = rt_lt_transitred(HH, Chprov1, dred, factorskinc,
                                      protect_red, factorskinextc,
                                      protect_red_h, factorsatc, factorsatc);
        atmp *= factorsatc;
        btmp *= factorsatc;

        /* C=f(L), skipped under `bwToning`. */
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
                rt_lt_scalered(rst_protection, chromaCfactor, 0.0f, HH,
                               deltaHH, &scale, &scaleext);
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
            const float factor = rt_lt_transitred(
                HH, Chprov1, dred, factorskin, protect_redcur, factorskinext,
                deltaHH, factorsat, factorsat);
            atmp = rt_lt_lim(atmp * factor, rt_lt_min(-42000.f, atmp),
                             rt_lt_max(42000.f, atmp));
            btmp = rt_lt_lim(btmp * factor, rt_lt_min(-42000.f, btmp),
                             rt_lt_max(42000.f, btmp));
        }

        /* C=f(C), skipped under `bwToning`. */
        if (cc_active != 0) {
            const float curf = 0.7f;
            float scale = 100.0f / 100.1f;
            float scaleext = 1.0f;
            const float chroma =
                rt_lt_sqrt_f(atmp * atmp + btmp * btmp + 0.001f);
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
                rt_lt_scalered(rst_protection, chromaCfactor, 0.0f, HH,
                               deltaHH, &scale, &scaleext);
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
            const float factor = rt_lt_transitred(
                HH, Chprov1, dred, factorskin, protect_redcur, factorskinext,
                deltaHH, factorsat, factorsat);
            atmp *= factor;
            btmp *= factor;
        }
    }

    /* L=f(C), skipped under `bwToning` (the host's `cclutili && !bwToning`). */
    if (bw_toning == 0 && lc_active != 0) {
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

    if (bw_toning != 0) {
        /*
         * chromiLuminanceCurve()'s `bwToning` closing block: the hue and
         * chroma come from the a/b tables' deviation from the original
         * chroma, not from the curved absolute values.
         */
        atmp -= a_orig;
        btmp -= b_orig;
        Chprov1 = rt_lt_sqrt_div(atmp * atmp + btmp * btmp);
        if (Chprov1 == 0.0f) {
            sincosval.x = 0.0f;
            sincosval.y = 1.0f;
        } else {
            sincosval.x = btmp / (327.68f * Chprov1);
            sincosval.y = atmp / (327.68f * Chprov1);
        }
    } else {
        Chprov1 = rt_lt_sqrt_div(atmp * atmp + btmp * btmp);
    }
    float out_l = Lprov1 * 327.68f;
    float out_a = 327.68f * Chprov1 * sincosval.y;
    float out_b = 327.68f * Chprov1 * sincosval.x;

    /*
     * gamutmuns 2/3: the block's XYZ correction. It takes lnew's L and the
     * a/b it has just written, projects them onto the working-space gamut
     * through Color::gamutmap(), blends the projection halfway back to the
     * original for XYZREL, and rebuilds L, HH, the chroma and the direction
     * from the projected XYZ. `wp` is the same working matrix the host
     * passes. The host's XYZREL blend is 0.5 here, not the local
     * adjustments' 0.3.
     */
    if (method == RT_LT_METHOD_XYZ ||
        method == RT_LT_METHOD_XYZREL) {
        float xg;
        float yg;
        float zg;
        rt_lt_lab2xyz(out_l, atmp, btmp, &xg, &yg, &zg);
        const float x0 = xg;
        const float y0 = yg;
        const float z0 = zg;

        rt_lt_gamutmap(&xg, yg, &zg, wp);

        if (method == RT_LT_METHOD_XYZREL) {
            xg = xg + 0.5f * (x0 - xg);
            yg = yg + 0.5f * (y0 - yg);
            zg = zg + 0.5f * (z0 - zg);
        }

        float lag;
        float aag2;
        float bbg2;
        rt_lt_xyz2lab(xg, yg, zg, cachef, cachefy, &lag, &aag2, &bbg2);
        Lprov1 = lag / 327.68f;
        HH = rt_lt_xatan2f(bbg2, aag2);
        Chprov1 = sqrt(aag2 * aag2 + bbg2 * bbg2) / 327.68f;

        if (Chprov1 == 0.0f) {
            sincosval.y = 1.f;
            sincosval.x = 0.f;
        } else {
            sincosval.y = aag2 / (Chprov1 * 327.68f);
            sincosval.x = bbg2 / (Chprov1 * 327.68f);
        }

        out_l = Lprov1 * 327.68f;
        out_a = 327.68f * Chprov1 * sincosval.y;
        out_b = 327.68f * Chprov1 * sincosval.x;
    }

    /*
     * gamutmuns 1: the block's LAB correction. Color::gamutLchonly()'s RGB
     * overload projects the pixel through the double working-space inverse and
     * iterates on the RGB clip tests with ClipLevel 65535, writing L and C
     * back; the arm then rebuilds lnew's L, a and b from them. It runs before
     * the Munsell arm, exactly as the host orders it, so the Munsell block
     * below sees the corrected L and a/b.
     */
    if (method == RT_LT_METHOD_LAB) {
        float R;
        float G;
        float B;
        rt_lt_gamut_lchonly_rgb(HH, sincosval, &Lprov1, &Chprov1, &R, &G, &B,
                                wip, highlight, 0.15f, 0.96f);

        out_l = Lprov1 * 327.68f;
        out_a = 327.68f * Chprov1 * sincosval.y;
        out_b = 327.68f * Chprov1 * sincosval.x;
    }

    /*
     * The Munsell correction. The guard is the block's own: a non-NONE method
     * followed by its activity list. The activity test is load-bearing, not
     * defensive: a configured XYZ or XYZREL method can reach here with
     * identity tables and the chromaticity slider at zero, and then every
     * flag is clear and the correction is skipped, exactly as the block
     * skips it.
     *
     * The activity list excludes `bwToning`; under black-and-white the six
     * tables can still be active, so the correction runs on the grey or
     * deviated chroma exactly as the block does.
     */
    if (method != RT_LT_METHOD_NONE &&
        (l_active != 0 || a_active != 0 || b_active != 0 || cc_active != 0 ||
         lc_active != 0 || cl_active != 0 || ch_active != 0 ||
         lh_active != 0 || hh_active != 0 || chromaticity != 0)) {
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

    /*
     * The block's closing `gamutmuns == 0` branch. Without `bwToning` it
     * repeats the write above; under `bwToning` it writes the a/b tables'
     * deviation directly, which is zero when no a or b table is active.
     */
    if (method == RT_LT_METHOD_NONE && bw_toning != 0) {
        out_a = atmp;
        out_b = btmp;
    }

    lab[offset] = out_l;
    lab[lab_plane + offset] = out_a;
    lab[2 * lab_plane + offset] = out_b;
}
)CLC";
