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
 * Device arithmetic for the working.trc node.
 *
 * The kernels are the second pass of ImProcFunctions::workingtrc() and its
 * neighbours for the default configuration: the optional gamut control, the
 * matrix-shaper transform, apsatur() and the return to Lab. The host prefix
 * (lab2rgb() and the fixed sRGB inverse) is not a kernel: it runs on the host
 * so the probe, the profile and the pass-2 input are byte-identical to the
 * CPU block. Everything here is single precision except the values the host
 * exports as float: the working matrices, the profile matrices and the
 * 16-bit tables. The host still owns the probe, the lcms profile and the
 * gamma inverse.
 *
 * The source is a namespace-scope constant rather than a macro because GCC 12
 * rejects a multi-line raw string inside a #define. The launcher prepends
 * RT_LAB_TO_RGB_CL for rt_gamma_lookup().
 */
inline constexpr const char* RT_WORKING_TRC_CL = R"CLC(
/* 16-bit sample scale of Imagefloat and LabImage (MAXVALF). */
#define RT_WT_MAXVAL 65535.0f
/* The block clamps the second-pass source to this epsilon. */
#define RT_WT_EPSILON 0.00001f
/* First-pass clamp: the epsilon workingtrc() applies before the sRGB inverse. */
/* Color::D50x / Color::D50z, the white point Lab2XYZ and XYZ2Lab use. */
#define RT_WT_D50X 0.9642f
#define RT_WT_D50Z 0.8249f
/* 116/16 and the CIE constants, the same float values Color uses. */
#define RT_WT_KAPPA 903.2962962962963f
/* MAXVALF * eps, eps = 216/24389: the end of the CIE linear segment. */
/* apsatur(): pow_F(2, -16) and the norm3() saturation ceiling. */
#define RT_WT_APSAT_NOISE 0.0000152587890625f
#define RT_WT_NORM3_MAX (3.402823466e+36f)

/*
 * Color::cachef[] / cachefy[] through LUT::operator[]. The host indexes the
 * table with a float in 0..65535 and interpolates; the device must do the same
 * rather than evaluate the cube root, because the table's samples are the
 * host's own quantisation and the difference is visible on near-zero XYZ.
 * `clip == LUT_CLIP_BELOW`, so a negative index clips to entry 0 (the caller
 * handles negative values before this point) and the last interval is 65534.
 */
inline float rt_wt_cache_lookup(__global const float* table, const float value)
{
    int index = (int)value;
    if (value < 0.0f) {
        index = 0;
    } else if (value > RT_WT_MAXVAL - 2.0f) {
        index = 65534;
    }
    const float fraction = value - (float)index;
    const float low = table[index];
    return low + (table[index + 1] - low) * fraction;
}

/* Color::computeXYZ2Lab(). */
inline float rt_wt_compute_f(const float value, __global const float* cache)
{
    if (value < 0.0f) {
        return 327.68f * ((RT_WT_KAPPA * value / RT_WT_MAXVAL + 16.0f) / 116.0f);
    }
    if (value > RT_WT_MAXVAL) {
        return 327.68f * cbrt(value / RT_WT_MAXVAL);
    }
    return rt_wt_cache_lookup(cache, value);
}

/* Color::computeXYZ2LabY(). */
inline float rt_wt_compute_fy(const float value, __global const float* cache)
{
    if (value < 0.0f) {
        return 327.68f * (RT_WT_KAPPA * value / RT_WT_MAXVAL);
    }
    if (value > RT_WT_MAXVAL) {
        return 327.68f * (116.0f * cbrt(value / RT_WT_MAXVAL) - 16.0f);
    }
    return rt_wt_cache_lookup(cache, value);
}

/* Color::XYZ2Lab(). */
inline void rt_wt_xyz2lab(const float X, const float Y, const float Z,
                          __global const float* cachef,
                          __global const float* cachefy,
                          float* L, float* a, float* b)
{
    const float fx = rt_wt_compute_f(X / RT_WT_D50X, cachef);
    const float fy = rt_wt_compute_f(Y, cachef);
    const float fz = rt_wt_compute_f(Z / RT_WT_D50Z, cachef);
    *L = rt_wt_compute_fy(Y, cachefy);
    *a = 500.0f * (fx - fy);
    *b = 200.0f * (fy - fz);
}

/* Color::rgbxyz() with the working-space matrix, as rgb2lab() builds it. */
inline void rt_wt_rgbxyz(const float r, const float g, const float b,
                         __global const float* m, float* X, float* Y,
                         float* Z)
{
    *X = m[0] * r + m[1] * g + m[2] * b;
    *Y = m[3] * r + m[4] * g + m[5] * b;
    *Z = m[6] * r + m[7] * g + m[8] * b;
}

/* power_norm2() from iplab2rgb.cc. */
inline float rt_wt_power_norm2(float r, float g, float b)
{
    r = fabs(r);
    g = fabs(g);
    b = fabs(b);
    const float r2 = r * r;
    const float g2 = g * g;
    const float b2 = b * b;
    const float d = r2 + g2 + b2;
    const float n = r * r2 + g * g2 + b * b2;
    return n / fmax(d, 1e-12f);
}

/* norm3() from iplab2rgb.cc. The host evaluates the luminance in double from
 * the float matrix; the single-precision form is the documented device
 * contract and its difference is below the parity tolerance. */
inline float rt_wt_norm3(const float r, const float g, const float b,
                         __global const float* wp)
{
    return fmin(RT_WT_NORM3_MAX,
                rt_wt_power_norm2(r, g, b) / 2.0f
                    + (r * wp[3] + g * wp[4] + b * wp[5]) / 2.0f);
}

/* apsatur()'s sfcie() and apply_satcie() pair. */
inline float rt_wt_sfcie(const float s, const float c)
{
    return c > RT_WT_APSAT_NOISE
        ? 1.0f - fmin(fabs(s) / c, 1.0f)
        : 0.0f;
}

inline void rt_wt_apply_satcie(float* r, float* g, float* b, const float f,
                               const float ll, const float satu)
{
    const float rl = *r - ll;
    const float gl = *g - ll;
    const float bl = *b - ll;
    const float sf = fmax(rt_wt_sfcie(rl, *r),
                          fmax(rt_wt_sfcie(gl, *g), rt_wt_sfcie(bl, *b)));
    const float target = pow(f, 0.3f * satu) * 0.6f + 0.4f;
    /* rt_math.h intp(a, b, c) = a * (b - c) + c. */
    const float s = sf * (target - 1.0f) + 1.0f;
    *r = ll + s * rl;
    *g = ll + s * gl;
    *b = ll + s * bl;
}

/*
 * Encode a linear value with the inverse of the profile TRC, outside the 0..1
 * table range.
 *
 * The profile's TRC tag maps encoded -> linear, so the transform's
 * linear -> encoded direction needs its inverse. buildGpuProfile() already
 * inverts the curve for the table; the analytic branch must use the same
 * direction. For a type-5 tag that inverse is lcms's type -5 evaluator; for a
 * type -5 tag (the block builds one when the requested gamma is below 1) it is
 * the forward type-5 evaluator. curve = {type, g, a, b, c, d, 0, 0} with the
 * tag's own parameter order.
 */
inline float rt_wt_trc_inverse(__global const float* gamma,
                               __global const float* curve,
                               const float linear)
{
    const int type = (int)curve[0];
    const float g = curve[1];
    const float a = curve[2];
    const float b = curve[3];
    const float c = curve[4];
    const float d = curve[5];

    /* The table's inverse is exact in the TRC's linear segment, but its
     * 65536 samples cannot resolve the first few encoded codes: lcms evaluates
     * the line directly there and the two differ by more than one LSB16 on
     * dark pixels. Below the breakpoint the inverse is exactly linear / c. */
    if (type == 5 && linear >= 0.0f && linear < c * d) {
        return (linear / c) * RT_WT_MAXVAL;
    }
    if (linear >= 0.0f && linear <= 1.0f) {
        return rt_gamma_lookup(gamma, linear * RT_WT_MAXVAL);
    }

    if (type == 5) {
        /* lcms type -5: X = (Y^(1/g) - b)/a above the breakpoint, Y/c below.
         * The forward breakpoint value is c * d. */
        const float threshold = c * d;
        return (linear >= threshold ? (pow(linear, 1.0f / g) - b) / a
                                    : linear / c)
            * RT_WT_MAXVAL;
    }
    /* lcms type 5: Y = (aX + b)^g above d, cX below. */
    return (linear >= d ? pow(a * linear + b, g) : c * linear)
        * RT_WT_MAXVAL;
}

/*
 * Pass 2: the abstract-profile matrix-shaper transform, per pixel. The input
 * is the host's toxyz + optional gamutmap image (three planes, float, the
 * block's own values); `matrix` is XYZ -> linear from buildGpuProfile() and
 * gamma is the 65536-entry linear -> encoded table.
 */
__kernel void rt_working_trc_second(__global const float* xyz,
                                    __global float* post,
                                    __global const float* matrix,
                                    __global const float* gamma,
                                    __global const float* curve,
                                    const int width, const int height,
                                    const int xyz_pitch,
                                    const int xyz_plane_pitch,
                                    const int rgb_pitch,
                                    const int rgb_plane_pitch)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int in = y * xyz_pitch + x;
    const float X = xyz[in];
    const float Y = xyz[xyz_plane_pitch + in];
    const float Z = xyz[2 * xyz_plane_pitch + in];

    const float lr = matrix[0] * X + matrix[1] * Y + matrix[2] * Z;
    const float lg = matrix[3] * X + matrix[4] * Y + matrix[5] * Z;
    const float lb = matrix[6] * X + matrix[7] * Y + matrix[8] * Z;

    const int out = y * rgb_pitch + x;
    post[out] = rt_wt_trc_inverse(gamma, curve, lr);
    post[rgb_plane_pitch + out] = rt_wt_trc_inverse(gamma, curve, lg);
    post[2 * rgb_plane_pitch + out] = rt_wt_trc_inverse(gamma, curve, lb);
}

/* apsatur(): the post-TRC image is modified in place. `reference` is
 * lab2rgb()'s linear output, the same pre-TRC image the host copies, and `wp`
 * the working RGB -> XYZ matrix. */
__kernel void rt_working_trc_apsatur(__global const float* reference,
                                     __global float* post,
                                     __global const float* wp,
                                     const float satu,
                                     const int width, const int height,
                                     const int rgb_pitch,
                                     const int rgb_plane_pitch)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * rgb_pitch + x;
    float R0 = post[offset] / RT_WT_MAXVAL;
    float G0 = post[rgb_plane_pitch + offset] / RT_WT_MAXVAL;
    float B0 = post[2 * rgb_plane_pitch + offset] / RT_WT_MAXVAL;
    const float R2 = reference[offset] / RT_WT_MAXVAL;
    const float G2 = reference[rgb_plane_pitch + offset] / RT_WT_MAXVAL;
    const float B2 = reference[2 * rgb_plane_pitch + offset] / RT_WT_MAXVAL;

    float fcie2 = rt_wt_norm3(R2, G2, B2, wp);
    float fcie0 = rt_wt_norm3(R0, G0, B0, wp);
    fcie0 = fmax(fcie0, RT_WT_APSAT_NOISE);
    fcie2 = fmax(fcie2, RT_WT_APSAT_NOISE);
    const float amp = fcie0 / fcie2;

    rt_wt_apply_satcie(&R0, &G0, &B0, amp, fcie2, satu);

    post[offset] = R0 * RT_WT_MAXVAL;
    post[rgb_plane_pitch + offset] = G0 * RT_WT_MAXVAL;
    post[2 * rgb_plane_pitch + offset] = B0 * RT_WT_MAXVAL;
}

/* Closing RGB -> Lab: Color::rgbxyz() then Color::XYZ2Lab(). */
__kernel void rt_working_trc_lab(__global const float* post,
                                 __global float* lab,
                                 __global const float* wp,
                                 __global const float* cachef,
                                 __global const float* cachefy,
                                 const int width, const int height,
                                 const int rgb_pitch,
                                 const int rgb_plane_pitch,
                                 const int lab_pitch,
                                 const int lab_plane_pitch)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int in = y * rgb_pitch + x;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    rt_wt_rgbxyz(post[in], post[rgb_plane_pitch + in],
                 post[2 * rgb_plane_pitch + in], wp, &X, &Y, &Z);

    float L = 0.0f;
    float a = 0.0f;
    float b = 0.0f;
    rt_wt_xyz2lab(X, Y, Z, cachef, cachefy, &L, &a, &b);

    const int out = y * lab_pitch + x;
    lab[out] = L;
    lab[lab_plane_pitch + out] = a;
    lab[2 * lab_plane_pitch + out] = b;
}
)CLC";
