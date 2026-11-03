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

#include "stage/algorithms/sharpen_cl.h"

#include <string>

/*
 * Device kernels for the lab.sharpening block: impulse denoise, defringe,
 * ML sharpen edges and ML microcontrast.
 *
 * The program is compiled together with RT_SHARPEN_CL, so the Gaussian blur of
 * every pass here is one of the shared kernels: `rt_sharpen_iir_line` for the
 * separable Young-van Vliet pair (impulse denoise and a defringe radius of 0.6
 * or more), `rt_sharpen_fir3` for the 3x3 convolution a defringe radius from
 * 0.25 to below 0.6 selects, and `rt_sharpen_mask` for buildBlendMask()'s
 * contrast mask; the launcher derives the coefficients with
 * `sharpen_iir_factors()` and `sharpen_fir_factors()` exactly as
 * `gaussianBlur()` does in the mask blur of output.sharpen.
 *
 * Every arithmetic quirk of impulse_denoise.cc, PF_correct_RT.cc,
 * ipsharpenedges.cc and ipsharpen.cc is kept:
 * the 5x5 mark and correction windows shorten at both horizontal edges, the
 * correction skips marked neighbours and divides by the unclamped weight sum,
 * and the defringe windows are asymmetric at the left edge. Both passes are
 * single-thread results on the host as well, because a marked source sample is
 * never read after it is written and the defringe weighted mean always reads
 * the pre-defringe planes.
 *
 * The defringe fringe sum accumulates in double on the host, so the device
 * reduction keeps double through the row sums and the total. The program is
 * only used for the defringe pass on a device that advertises fp64; the
 * impulse and ML passes alone run on any device.
 *
 * MLsharpen()'s `f1 * SQR(f2 * f3) * f4` is evaluated as
 * `f1 * ((f2 * f3) * (f2 * f3)) * f4`. Regrouping the product changes the
 * last bit, and a second pass then amplifies it into a branch flip, so the
 * grouping is part of the port.
 *
 * The hue table is a uniform sample of the defringe FlatCurve with linear
 * interpolation. FlatCurve::getVal() is a dense polyline, so the host can
 * bound the interpolation error before it uploads; a curve it cannot follow is
 * refused rather than approximated. A sampled table at a too-coarse
 * resolution would move pixels across the `fringe < threshfactor` decision.
 */
inline constexpr const char* RT_LAB_SHARPENING_CL = R"CLC(
#ifdef cl_khr_fp64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
typedef double rt_ls_real;
#else
typedef float rt_ls_real;
#endif

/* The correction's range weight denominator floor: `SQR(dL) + 1.0f`. */
#define RT_LS_RANGE_EPS 1.0f

/*
 * atan2kf() and xatan2f() from rtengine/sleef.h, in the host's operation order
 * and float literals. PF_correct_RT() calls xatan2f(), so a device library
 * atan2 that is one ulp away would move a fringe across its threshold. The
 * port makes HH bit-identical to the host value.
 */
float rt_ls_atan2k(const float y_in, const float x_in)
{
    float x = x_in;
    float y = y_in;
    float q = 0.0f;
    if (x < 0.0f) {
        x = -x;
        q = -2.0f;
    }
    if (y > x) {
        const float swap = x;
        x = y;
        y = -swap;
        q += 1.0f;
    }
    const float s = y / x;
    const float t = s * s;
    float u = 0.00282363896258175373077393f;
    u = u * t + -0.0159569028764963150024414f;
    u = u * t + 0.0425049886107444763183594f;
    u = u * t + -0.0748900920152664184570312f;
    u = u * t + 0.106347933411598205566406f;
    u = u * t + -0.142027363181114196777344f;
    u = u * t + 0.199926957488059997558594f;
    u = u * t + -0.333331018686294555664062f;
    u = u * t;
    u = u * s + s;
    return q * (float)1.57079632679489661923 + u;
}

float rt_ls_atan2f(const float y, const float x)
{
    const float pi = (float)3.14159265358979323846;
    float r = rt_ls_atan2k(fabs(y), x);
    r = copysign(r, x);
    if (isinf(x) || x == 0.0f) {
        r = pi / 2.0f -
            (isinf(x) ? copysign(1.0f, x) * (pi * 0.5f) : 0.0f);
    }
    if (isinf(y)) {
        r = pi / 2.0f -
            (isinf(x) ? copysign(1.0f, x) * (pi * 0.25f) : 0.0f);
    }
    if (y == 0.0f) {
        r = copysign(1.0f, x) == -1.0f ? pi : 0.0f;
    }
    return (isnan(x) || isnan(y)) ? (float)NAN : copysign(r, y);
}

/* huelab_to_huehsv2(): the Lab hue in radians mapped to an HSV hue in [0,1]. */
rt_ls_real rt_ls_huelab_to_huehsv2(const float hh)
{
    const rt_ls_real h = (rt_ls_real)hh;
    rt_ls_real hr = (rt_ls_real)0.0;

    if (h >= 0.0f && h < 0.6f) {
        hr = (rt_ls_real)0.11666 * h + (rt_ls_real)0.93;
    } else if (h >= 0.6f && h < 1.4f) {
        hr = (rt_ls_real)0.1125 * h - (rt_ls_real)0.0675;
    } else if (h >= 1.4f && h < 2.0f) {
        hr = (rt_ls_real)0.2666 * h - (rt_ls_real)0.2833;
    } else if (h >= 2.0f && h < 3.14159f) {
        hr = (rt_ls_real)0.1489 * h - (rt_ls_real)0.04785;
    } else if (h >= -3.14159f && h < -2.8f) {
        hr = (rt_ls_real)0.23419 * h + (rt_ls_real)1.1557;
    } else if (h >= -2.8f && h < -2.3f) {
        hr = (rt_ls_real)0.16 * h + (rt_ls_real)0.948;
    } else if (h >= -2.3f && h < -0.9f) {
        hr = (rt_ls_real)0.12143 * h + (rt_ls_real)0.85928;
    } else if (h >= -0.9f && h < -0.1f) {
        hr = (rt_ls_real)0.2125 * h + (rt_ls_real)0.94125;
    } else if (h >= -0.1f && h < 0.0f) {
        hr = (rt_ls_real)0.1 * h + (rt_ls_real)0.93;
    }

    if (hr < (rt_ls_real)0.0) {
        hr += (rt_ls_real)1.0;
    } else if (hr > (rt_ls_real)1.0) {
        hr -= (rt_ls_real)1.0;
    }
    return hr;
}

/* Evaluate the uniform hue table at t, in [0,1], with linear interpolation. */
float rt_ls_hue_table(__global const float* table, const int entries,
                      const rt_ls_real t)
{
    const rt_ls_real pos = t * (rt_ls_real)(entries - 1);
    int index = (int)pos;
    if (index < 0) {
        index = 0;
    }
    if (index > entries - 2) {
        index = entries - 2;
    }
    const rt_ls_real fraction = pos - (rt_ls_real)index;
    const rt_ls_real y0 = (rt_ls_real)table[index];
    const rt_ls_real y1 = (rt_ls_real)table[index + 1];
    return (float)(y0 + (y1 - y0) * fraction);
}

/*
 * markImpulse()'s 5x5 high-pass sum and threshold. The horizontal window is
 * 0..x+2 for x < 2 and x-2..width-1 for x >= width-2, exactly as the host
 * shortens both tails.
 */
__kernel void rt_lab_sharpening_impulse_mark(__global const float* luminance,
                                             __global const float* lowpass,
                                             __global unsigned char* mask,
                                             const int width,
                                             const int height,
                                             const int pitch,
                                             const int mask_pitch,
                                             const float impthr_div24)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float hpfabs = fabs(luminance[index] - lowpass[index]);

    int x0;
    int x1;
    if (x < 2) {
        x0 = 0;
        x1 = x + 2;
    } else if (x < width - 2) {
        x0 = x - 2;
        x1 = x + 2;
    } else {
        x0 = x - 2;
        x1 = width - 1;
    }
    int y0 = y - 2;
    if (y0 < 0) {
        y0 = 0;
    }
    int y1 = y + 2;
    if (y1 > height - 1) {
        y1 = height - 1;
    }

    float hfnbrave = 0.0f;
    for (int i = y0; i <= y1; ++i) {
        for (int j = x0; j <= x1; ++j) {
            const int k = i * pitch + j;
            hfnbrave += fabs(luminance[k] - lowpass[k]);
        }
    }
    mask[y * mask_pitch + x] =
        (hpfabs > ((hfnbrave - hpfabs) * impthr_div24)) ? 1u : 0u;
}

/*
 * impulse_nr()'s correction: for a marked pixel only, the range-weighted mean
 * of L, a and b over the same 5x5 window, skipping marked neighbours. The
 * centre's own (unmodified) samples supply the range weight, and a marked
 * source is never read after it is written because it is skipped.
 */
__kernel void rt_lab_sharpening_impulse_correct(__global float* luminance,
                                                __global float* a,
                                                __global float* b,
                                                __global const unsigned char* mask,
                                                const int width,
                                                const int height,
                                                const int pitch,
                                                const int mask_pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    if (mask[y * mask_pitch + x] == 0u) {
        return;
    }

    const float centre = luminance[index];

    int x0;
    int x1;
    if (x < 2) {
        x0 = 0;
        x1 = x + 2;
    } else if (x < width - 2) {
        x0 = x - 2;
        x1 = x + 2;
    } else {
        x0 = x - 2;
        x1 = width - 1;
    }
    int y0 = y - 2;
    if (y0 < 0) {
        y0 = 0;
    }
    int y1 = y + 2;
    if (y1 > height - 1) {
        y1 = height - 1;
    }

    float norm = 0.0f;
    float lsum = 0.0f;
    float asum = 0.0f;
    float bsum = 0.0f;
    for (int i = y0; i <= y1; ++i) {
        for (int j = x0; j <= x1; ++j) {
            const int k = i * pitch + j;
            if (mask[i * mask_pitch + j] != 0u) {
                continue;
            }
            const float d = luminance[k] - centre;
            const float weight = 1.0f / (d * d + RT_LS_RANGE_EPS);
            lsum += weight * luminance[k];
            asum += weight * a[k];
            bsum += weight * b[k];
            norm += weight;
        }
    }
    if (norm != 0.0f) {
        luminance[index] = lsum / norm;
        a[index] = asum / norm;
        b[index] = bsum / norm;
    }
}

/* One plane to another, both at the same pitch. */
__kernel void rt_lab_sharpening_copy(__global const float* src,
                                     __global float* dst,
                                     const int width,
                                     const int height,
                                     const int pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    dst[index] = src[index];
}

/*
 * PF_correct_RT()'s chromaticity: the blurred a/b difference modulated by the
 * hue curve. `curve` is zero when the profile has no active hue curve, in
 * which case chromaChfactor stays 1.
 */
__kernel void rt_lab_sharpening_defringe_fringe(
    __global const float* a,
    __global const float* b,
    __global const float* blura,
    __global const float* blurb,
    __global float* fringe,
    __global const float* hue_table,
    const int hue_entries,
    const int curve,
    const int width,
    const int height,
    const int pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;

    float chroma_chfactor = 1.0f;
    if (curve != 0) {
        const float hh = rt_ls_atan2f(b[index], a[index]);
        const rt_ls_real t = rt_ls_huelab_to_huehsv2(hh);
        float chparam = rt_ls_hue_table(hue_table, hue_entries, t) - 0.5f;
        if (chparam < 0.0f) {
            chparam *= 2.0f;
        }
        chroma_chfactor = (1.0f + chparam) * (1.0f + chparam);
    }

    const float da = a[index] - blura[index];
    const float db = b[index] - blurb[index];
    fringe[index] = chroma_chfactor * (da * da + db * db);
}

/*
 * One double row sum of the float chromaticity. The host accumulates the same
 * values, cast from float to double, in one double variable.
 */
__kernel void rt_lab_sharpening_defringe_rows(__global const float* fringe,
                                              __global rt_ls_real* rows,
                                              const int width,
                                              const int height,
                                              const int pitch)
{
    const int y = (int)get_global_id(0);
    if (y >= height) {
        return;
    }
    rt_ls_real sum = (rt_ls_real)0.0;
    for (int x = 0; x < width; ++x) {
        sum += (rt_ls_real)fringe[y * pitch + x];
    }
    rows[y] = sum;
}

/*
 * The single-thread double total and mean. One work item is enough and keeps
 * the order deterministic: ascending rows, then the division by width*height.
 */
__kernel void rt_lab_sharpening_defringe_total(
    __global const rt_ls_real* rows,
    __global rt_ls_real* total,
    const int width,
    const int height)
{
    if (get_global_id(0) != 0) {
        return;
    }
    rt_ls_real sum = (rt_ls_real)0.0;
    for (int y = 0; y < height; ++y) {
        sum += rows[y];
    }
    total[0] = sum / ((rt_ls_real)width * (rt_ls_real)height);
}

/*
 * PF_correct_RT()'s weighted mean of the original a and b. The window is the
 * host's three-way split, including the asymmetric left tail: x < halfwin-1
 * reads 0..x+halfwin-1, the middle reads x-halfwin+1..x+halfwin-1 and the
 * right tail reads x-halfwin+1..width-1. The weight is the reciprocal fringe
 * value, evaluated here exactly as the host stores it.
 */
/*
 * MLsharpen() and MLmicrocontrast() both work on a Chanel/327.68f working
 * copy. This is that copy, shared by the edge and microcontrast passes.
 */
__kernel void rt_lab_sharpening_scale(__global const float* src,
                                      __global float* dst,
                                      const int width,
                                      const int height,
                                      const int pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    dst[index] = src[index] / 327.68f;
}

/*
 * MLmicrocontrast()'s luminance tables. They are algorithm invariants of
 * ipsharpen.cc, indexed by the uniformity slider 0..10.
 */
__constant float rt_ls_L98[11] = {0.001f, 0.0015f, 0.002f, 0.004f, 0.006f,
                                  0.008f, 0.01f, 0.03f, 0.05f, 0.1f, 0.1f};
__constant float rt_ls_L95[11] = {0.0012f, 0.002f, 0.005f, 0.01f, 0.02f,
                                  0.05f, 0.1f, 0.12f, 0.15f, 0.2f, 0.25f};
__constant float rt_ls_L92[11] = {0.01f, 0.015f, 0.02f, 0.06f, 0.10f, 0.13f,
                                  0.17f, 0.25f, 0.3f, 0.32f, 0.35f};
__constant float rt_ls_L90[11] = {0.015f, 0.02f, 0.04f, 0.08f, 0.12f, 0.15f,
                                  0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
__constant float rt_ls_L87[11] = {0.025f, 0.03f, 0.05f, 0.1f, 0.15f, 0.25f,
                                  0.3f, 0.4f, 0.5f, 0.63f, 0.75f};
__constant float rt_ls_L83[11] = {0.055f, 0.08f, 0.1f, 0.15f, 0.2f, 0.3f,
                                  0.4f, 0.5f, 0.6f, 0.75f, 0.85f};
__constant float rt_ls_L80[11] = {0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f, 0.5f,
                                  0.6f, 0.7f, 0.8f, 0.9f};
__constant float rt_ls_L75[11] = {0.22f, 0.25f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f,
                                  0.8f, 0.85f, 0.9f, 0.95f};
__constant float rt_ls_L70[11] = {0.35f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.97f,
                                  1.0f, 1.0f, 1.0f, 1.0f};
__constant float rt_ls_L63[11] = {0.55f, 0.6f, 0.7f, 0.8f, 0.85f, 0.9f, 1.0f,
                                  1.0f, 1.0f, 1.0f, 1.0f};
__constant float rt_ls_L58[11] = {0.75f, 0.77f, 0.8f, 0.9f, 1.0f, 1.0f, 1.0f,
                                  1.0f, 1.0f, 1.0f, 1.0f};
__constant float rt_ls_Cont0[11] = {0.05f, 0.1f, 0.2f, 0.25f, 0.3f, 0.4f,
                                    0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
__constant float rt_ls_Cont1[11] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f,
                                    0.8f, 0.9f, 0.95f, 1.0f};
__constant float rt_ls_Cont2[11] = {0.2f, 0.40f, 0.6f, 0.7f, 0.8f, 0.85f,
                                    0.90f, 0.95f, 1.0f, 1.05f, 1.10f};
__constant float rt_ls_Cont3[11] = {0.5f, 0.6f, 0.7f, 0.8f, 0.85f, 0.9f, 1.0f,
                                    1.0f, 1.05f, 1.10f, 1.20f};
__constant float rt_ls_Cont4[11] = {0.8f, 0.85f, 0.9f, 0.95f, 1.0f, 1.05f,
                                    1.10f, 1.150f, 1.2f, 1.25f, 1.40f};
__constant float rt_ls_Cont5[11] = {1.0f, 1.1f, 1.2f, 1.25f, 1.3f, 1.4f,
                                    1.45f, 1.50f, 1.6f, 1.65f, 1.80f};

inline float rt_ls_intp(const float a, const float b, const float c)
{
    return a * (b - c) + c;
}

inline float rt_ls_in_interval(const float a, const float b, const float c)
{
    return (a < fmax(b, c)) && (a > fmin(b, c));
}

inline float rt_ls_select_weight(const float a, const float b, const float low,
                                 const float high)
{
    const float min_val = fmin(a, b);
    const float max_val = fmax(a, b);
    const float res = (min_val < 0.45f * max_val) ? low : high;
    return (min_val > 0.05f * max_val) ? res : high;
}

/*
 * MLsharpen()'s single pass for one channel. `l` is the pre-pass working copy
 * of `channel`, so every neighbour read is the pass's input. The host writes
 * only the interior 2..W-3 x 2..H-3 and, for L, only where v < 92.
 */
__kernel void rt_lab_sharpening_ml_edges(__global const float* l,
                                         __global float* channel,
                                         const int width,
                                         const int height,
                                         const int pitch,
                                         const int channel_index,
                                         const float amount,
                                         const float amountby3)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x < 2 || y < 2 || x >= width - 2 || y >= height - 2) {
        return;
    }
    const int offset = y * pitch + x;
    const float eps2 = 0.001f;
    const float chmax[3] = {0.125f, 0.33333334f, 0.33333334f};
    const int width2 = 2 * pitch;

    const float wH = eps2 + fabs(l[offset + 1] - l[offset - 1]);
    const float wV = eps2 + fabs(l[offset + pitch] - l[offset - pitch]);

    float s = 2.f / (2.f + fabs(wH - wV));
    float wD1 =
        eps2 + fabs(l[offset + pitch + 1] - l[offset - pitch - 1]) * s;
    float wD2 =
        eps2 + fabs(l[offset + pitch - 1] - l[offset - pitch + 1]) * s;
    s = wD1;
    wD1 /= wD2;
    wD2 /= s;

    const float v = l[offset];
    float lumH = v;
    float lumV = v;
    float lumD1 = v;
    float lumD2 = v;

    const float contrast =
        fmin(sqrt((l[offset + 1] - l[offset - 1]) *
                      (l[offset + 1] - l[offset - 1]) +
                  (l[offset + pitch] - l[offset - pitch]) *
                      (l[offset + pitch] - l[offset - pitch])) *
                 chmax[channel_index],
             1.f);

    if (rt_ls_in_interval(v, l[offset - 1], l[offset + 1])) {
        float f1 = fabs(l[offset - 2] - l[offset - 1]);
        float f2 = l[offset - 1] - v;
        float f3 = (l[offset - 1] - l[offset - pitch]) *
                   (l[offset - 1] - l[offset + pitch]);
        float f4 = sqrt(fabs((l[offset - 1] - l[offset - width2]) *
                             (l[offset - 1] - l[offset + width2])));
        const float difL = f1 * ((f2 * f3) * (f2 * f3)) * f4;
        if (difL > 0.f) {
            f1 = fabs(l[offset + 2] - l[offset + 1]);
            f2 = l[offset + 1] - v;
            f3 = (l[offset + 1] - l[offset - pitch]) *
                 (l[offset + 1] - l[offset + pitch]);
            f4 = sqrt(fabs((l[offset + 1] - l[offset - width2]) *
                           (l[offset + 1] - l[offset + width2])));
            const float difR = f1 * ((f2 * f3) * (f2 * f3)) * f4;
            if (difR > 0.f) {
                lumH = (l[offset - 1] * difR + l[offset + 1] * difL) /
                       (difL + difR);
                lumH = rt_ls_intp(contrast, lumH, v);
            }
        }
    }

    if (rt_ls_in_interval(v, l[offset - pitch], l[offset + pitch])) {
        float f1 = fabs(l[offset - width2] - l[offset - pitch]);
        float f2 = l[offset - pitch] - v;
        float f3 = (l[offset - pitch] - l[offset - 1]) *
                   (l[offset - pitch] - l[offset + 1]);
        float f4 = sqrt(fabs((l[offset - pitch] - l[offset - 2]) *
                             (l[offset - pitch] - l[offset + 2])));
        const float difT = f1 * ((f2 * f3) * (f2 * f3)) * f4;
        if (difT > 0.f) {
            f1 = fabs(l[offset + width2] - l[offset + pitch]);
            f2 = l[offset + pitch] - v;
            f3 = (l[offset + pitch] - l[offset - 1]) *
                 (l[offset + pitch] - l[offset + 1]);
            f4 = sqrt(fabs((l[offset + pitch] - l[offset - 2]) *
                           (l[offset + pitch] - l[offset + 2])));
            const float difB = f1 * ((f2 * f3) * (f2 * f3)) * f4;
            if (difB > 0.f) {
                lumV = (l[offset - pitch] * difB + l[offset + pitch] * difT) /
                       (difT + difB);
                lumV = rt_ls_intp(contrast, lumV, v);
            }
        }
    }

    if (rt_ls_in_interval(v, l[offset - 1 - pitch],
                          l[offset + 1 + pitch])) {
        float f1 =
            fabs(l[offset - 2 - width2] - l[offset - 1 - pitch]);
        float f2 = l[offset - 1 - pitch] - v;
        float f3 = (l[offset - 1 - pitch] - l[offset - pitch + 1]) *
                   (l[offset - 1 - pitch] - l[offset + pitch - 1]);
        float f4 =
            sqrt(fabs((l[offset - 1 - pitch] - l[offset - width2 + 2]) *
                      (l[offset - 1 - pitch] - l[offset + width2 - 2])));
        const float difLT = f1 * ((f2 * f3) * (f2 * f3)) * f4;
        if (difLT > 0.f) {
            f1 = fabs(l[offset + 2 + width2] - l[offset + 1 + pitch]);
            f2 = l[offset + 1 + pitch] - v;
            f3 = (l[offset + 1 + pitch] - l[offset - pitch + 1]) *
                 (l[offset + 1 + pitch] - l[offset + pitch - 1]);
            f4 = sqrt(
                fabs((l[offset + 1 + pitch] - l[offset - width2 + 2]) *
                     (l[offset + 1 + pitch] - l[offset + width2 - 2])));
            const float difRB = f1 * ((f2 * f3) * (f2 * f3)) * f4;
            if (difRB > 0.f) {
                lumD1 = (l[offset - 1 - pitch] * difRB +
                         l[offset + 1 + pitch] * difLT) /
                        (difLT + difRB);
                lumD1 = rt_ls_intp(contrast, lumD1, v);
            }
        }
    }

    if (rt_ls_in_interval(v, l[offset + 1 - pitch],
                          l[offset - 1 + pitch])) {
        float f1 =
            fabs(l[offset - 2 + width2] - l[offset - 1 + pitch]);
        float f2 = l[offset - 1 + pitch] - v;
        float f3 = (l[offset - 1 + pitch] - l[offset - pitch - 1]) *
                   (l[offset - 1 + pitch] - l[offset + pitch + 1]);
        float f4 =
            sqrt(fabs((l[offset - 1 + pitch] - l[offset - width2 - 2]) *
                      (l[offset - 1 + pitch] - l[offset + width2 + 2])));
        const float difLB = f1 * ((f2 * f3) * (f2 * f3)) * f4;
        if (difLB > 0.f) {
            f1 = fabs(l[offset + 2 - width2] - l[offset + 1 - pitch]);
            f2 = l[offset + 1 - pitch] - v;
            f3 = (l[offset + 1 - pitch] - l[offset + pitch + 1]) *
                 (l[offset + 1 - pitch] - l[offset - pitch - 1]);
            f4 = sqrt(
                fabs((l[offset + 1 - pitch] - l[offset + width2 + 2]) *
                     (l[offset + 1 - pitch] - l[offset - width2 - 2])));
            const float difRT = f1 * ((f2 * f3) * (f2 * f3)) * f4;
            if (difRT > 0.f) {
                lumD2 = (l[offset + 1 - pitch] * difLB +
                         l[offset - 1 + pitch] * difRT) /
                        (difLB + difRT);
                lumD2 = rt_ls_intp(contrast, lumD2, v);
            }
        }
    }

    const float weight = rt_ls_select_weight(wH, wV, amountby3, amount);
    const float combined = (lumH * wH + lumV * wV + lumD1 * wD1 +
                            lumD2 * wD2) /
                           (wH + wV + wD1 + wD2);
    const float mixed = rt_ls_intp(weight, combined, v);
    if (channel_index == 0) {
        if (v < 92.f) {
            channel[offset] = fabs(327.68f * mixed);
        }
    } else {
        channel[offset] = 327.68f * mixed;
    }
}

/*
 * MLmicrocontrast()'s final mix. `luminance` is read and written in place; `lm`
 * is its pre-pass working copy and `blend` the contrast mask produced by
 * buildBlendMask(). The pyramid tables and the uniformity index follow
 * ipsharpen.cc exactly.
 */
__kernel void rt_lab_sharpening_ml_micro(__global float* luminance,
                                         __global const float* lm,
                                         __global const float* blend,
                                         const int width,
                                         const int height,
                                         const int pitch,
                                         const int k,
                                         const float amount,
                                         const int uniformity)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x < k || y < k || x >= width - k || y >= height - k) {
        return;
    }
    const int offset = y * pitch + x;
    const int unif = uniformity;
    const float v = lm[offset];
    const float sqrt2 = sqrt(2.0f);
    const float sqrt1d25 = sqrt(1.25f);

    float contrast;
    if (k == 1) {
        contrast = sqrt((lm[offset + 1] - lm[offset - 1]) *
                            (lm[offset + 1] - lm[offset - 1]) +
                        (lm[offset + pitch] - lm[offset - pitch]) *
                            (lm[offset + pitch] - lm[offset - pitch])) *
                   0.125f;
    } else {
        contrast =
            sqrt((lm[offset + 1] - lm[offset - 1]) *
                     (lm[offset + 1] - lm[offset - 1]) +
                 (lm[offset + pitch] - lm[offset - pitch]) *
                     (lm[offset + pitch] - lm[offset - pitch]) +
                 (lm[offset + 2] - lm[offset - 2]) *
                     (lm[offset + 2] - lm[offset - 2]) +
                 (lm[offset + 2 * pitch] - lm[offset - 2 * pitch]) *
                     (lm[offset + 2 * pitch] - lm[offset - 2 * pitch])) *
            0.0625f;
    }
    contrast = fmin(contrast, 1.f);

    float temp = v + 4.f * (v * (amount + sqrt2 * amount));
    float temp1 =
        sqrt2 * amount *
        (lm[offset - pitch - 1] + lm[offset - pitch + 1] +
         lm[offset + pitch - 1] + lm[offset + pitch + 1]);
    temp1 += amount * (lm[offset - pitch] + lm[offset - 1] +
                       lm[offset + 1] + lm[offset + pitch]);
    temp -= temp1;

    if (k == 2) {
        float temp2 = -(lm[offset + 2 * pitch] + lm[offset - 2 * pitch] +
                        lm[offset - 2] + lm[offset + 2]);
        temp2 -= sqrt1d25 *
                 (lm[offset + 2 * pitch - 1] + lm[offset + 2 * pitch + 1] +
                  lm[offset + pitch + 2] + lm[offset + pitch - 2] +
                  lm[offset - 2 * pitch - 1] + lm[offset - 2 * pitch + 1] +
                  lm[offset - pitch + 2] + lm[offset - pitch - 2]);
        temp2 -= sqrt2 * (lm[offset + 2 * pitch - 2] +
                          lm[offset + 2 * pitch + 2] +
                          lm[offset - 2 * pitch - 2] +
                          lm[offset - 2 * pitch + 2]);
        temp2 += 18.601126159f * v;
        temp2 *= 2.f * amount;
        temp += temp2;
    }

    temp = fmax(temp, 0.f);

    int stop = 0;
    for (int row = y - k; row <= y + k && stop == 0; ++row) {
        for (int index2 = row * pitch + x - k;
             index2 <= row * pitch + x + k; ++index2) {
            if ((lm[index2] - temp) * (v - lm[index2]) > 0.f) {
                temp = rt_ls_intp(0.75f, temp, lm[index2]);
                stop = 1;
                break;
            }
        }
    }

    if (v > 95.0f || v < 5.0f) {
        contrast *= rt_ls_Cont0[unif];
    } else if (v > 90.0f || v < 10.0f) {
        contrast *= rt_ls_Cont1[unif];
    } else if (v > 80.0f || v < 20.0f) {
        contrast *= rt_ls_Cont2[unif];
    } else if (v > 70.0f || v < 30.0f) {
        contrast *= rt_ls_Cont3[unif];
    } else if (v > 60.0f || v < 40.0f) {
        contrast *= rt_ls_Cont4[unif];
    } else {
        contrast *= rt_ls_Cont5[unif];
    }
    contrast = fmin(contrast, 1.f);

    const float tempL = rt_ls_intp(contrast, v, temp);
    if (tempL > v) {
        float t2 = tempL / v;
        t2 = fmin(t2, 1.7f);
        t2 -= 1.f;
        float lum_mod;
        if (v > 98.0f) {
            lum_mod = 0.f;
        } else if (v > 95.0f) {
            lum_mod = rt_ls_L95[unif];
        } else if (v > 92.0f) {
            lum_mod = rt_ls_L92[unif];
        } else if (v > 90.0f) {
            lum_mod = rt_ls_L90[unif];
        } else if (v > 87.0f) {
            lum_mod = rt_ls_L87[unif];
        } else if (v > 83.0f) {
            lum_mod = rt_ls_L83[unif];
        } else if (v > 80.0f) {
            lum_mod = rt_ls_L80[unif];
        } else if (v > 75.0f) {
            lum_mod = rt_ls_L75[unif];
        } else if (v > 70.0f) {
            lum_mod = rt_ls_L70[unif];
        } else if (v > 63.0f) {
            lum_mod = rt_ls_L63[unif];
        } else if (v > 58.0f) {
            lum_mod = rt_ls_L58[unif];
        } else if (v > 42.0f) {
            lum_mod = rt_ls_L58[unif];
        } else if (v > 37.0f) {
            lum_mod = rt_ls_L63[unif];
        } else if (v > 30.0f) {
            lum_mod = rt_ls_L70[unif];
        } else if (v > 25.0f) {
            lum_mod = rt_ls_L75[unif];
        } else if (v > 20.0f) {
            lum_mod = rt_ls_L80[unif];
        } else if (v > 17.0f) {
            lum_mod = rt_ls_L83[unif];
        } else if (v > 13.0f) {
            lum_mod = rt_ls_L87[unif];
        } else if (v > 10.0f) {
            lum_mod = rt_ls_L90[unif];
        } else if (v > 5.0f) {
            lum_mod = rt_ls_L95[unif];
        } else {
            lum_mod = 0.f;
        }
        luminance[offset] = rt_ls_intp(
            blend[offset], luminance[offset] * (lum_mod * t2 + 1.f),
            luminance[offset]);
    } else {
        float t4 = v / tempL;
        if (t4 > 1.0f) {
            t4 = fmin(t4, 1.7f);
            t4 -= 1.f;
            float lum_mod;
            if (v < 2.0f) {
                lum_mod = rt_ls_L98[unif];
            } else if (v < 5.0f) {
                lum_mod = rt_ls_L95[unif];
            } else if (v < 8.0f) {
                lum_mod = rt_ls_L92[unif];
            } else if (v < 10.0f) {
                lum_mod = rt_ls_L90[unif];
            } else if (v < 13.0f) {
                lum_mod = rt_ls_L87[unif];
            } else if (v < 17.0f) {
                lum_mod = rt_ls_L83[unif];
            } else if (v < 20.0f) {
                lum_mod = rt_ls_L80[unif];
            } else if (v < 25.0f) {
                lum_mod = rt_ls_L75[unif];
            } else if (v < 30.0f) {
                lum_mod = rt_ls_L70[unif];
            } else if (v < 37.0f) {
                lum_mod = rt_ls_L63[unif];
            } else if (v < 42.0f) {
                lum_mod = rt_ls_L58[unif];
            } else if (v < 58.0f) {
                lum_mod = rt_ls_L58[unif];
            } else if (v < 63.0f) {
                lum_mod = rt_ls_L63[unif];
            } else if (v < 70.0f) {
                lum_mod = rt_ls_L70[unif];
            } else if (v < 75.0f) {
                lum_mod = rt_ls_L75[unif];
            } else if (v < 80.0f) {
                lum_mod = rt_ls_L80[unif];
            } else if (v < 83.0f) {
                lum_mod = rt_ls_L83[unif];
            } else if (v < 87.0f) {
                lum_mod = rt_ls_L87[unif];
            } else if (v < 90.0f) {
                lum_mod = rt_ls_L90[unif];
            } else if (v < 95.0f) {
                lum_mod = rt_ls_L95[unif];
            } else {
                lum_mod = 0.f;
            }
            luminance[offset] = rt_ls_intp(
                blend[offset], luminance[offset] / (lum_mod * t4 + 1.f),
                luminance[offset]);
        }
    }
}

__kernel void rt_lab_sharpening_defringe_apply(
    __global float* a,
    __global float* b,
    __global const float* original_a,
    __global const float* original_b,
    __global const float* fringe,
    const float chromave,
    const float threshfactor,
    const int halfwin,
    const int width,
    const int height,
    const int pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float value = 1.0f / (fringe[index] + chromave);
    if (!(value < threshfactor)) {
        return;
    }

    int x0;
    int x1;
    if (x < halfwin - 1) {
        x0 = 0;
        x1 = x + halfwin - 1;
    } else if (x < width - halfwin + 1) {
        x0 = x - halfwin + 1;
        x1 = x + halfwin - 1;
    } else {
        x0 = x - halfwin + 1;
        x1 = width - 1;
    }
    if (x1 > width - 1) {
        x1 = width - 1;
    }
    int y0 = y - halfwin + 1;
    if (y0 < 0) {
        y0 = 0;
    }
    int y1 = y + halfwin - 1;
    if (y1 > height - 1) {
        y1 = height - 1;
    }

    float atot = 0.0f;
    float btot = 0.0f;
    float norm = 0.0f;
    for (int i = y0; i <= y1; ++i) {
        for (int j = x0; j <= x1; ++j) {
            const int k = i * pitch + j;
            const float weight = 1.0f / (fringe[k] + chromave);
            atot += weight * original_a[k];
            btot += weight * original_b[k];
            norm += weight;
        }
    }
    a[index] = atot / norm;
    b[index] = btot / norm;
}
)CLC";
