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
 * Device kernels for the lab.sharpening impulse-denoise and defringe pair.
 *
 * The program is compiled together with RT_SHARPEN_CL, so the separable
 * Young-van Vliet line kernel `rt_sharpen_iir_line` is the Gaussian blur of
 * every pass here; the launcher derives its coefficients with
 * `sharpen_iir_factors()` exactly as the mask blur of output.sharpen does.
 *
 * Every arithmetic quirk of impulse_denoise.cc and PF_correct_RT.cc is kept:
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
 * impulse pass alone runs on any device.
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
