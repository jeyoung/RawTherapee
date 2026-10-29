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
 * ImProcFunctions::labColorCorrectionRegions() (rtengine/iplabregions.cc) as
 * device kernels.
 *
 * The fragment is concatenated after lab_tools_cl.h, so it reuses the block's
 * Lab2XYZ, xyz2rgb, XYZ2Lab and cache helpers. It needs fp64: the host
 * multiplies the working matrices in double, its hue mapping and the
 * rgbLuminance() overload it calls are double expressions, and the mask
 * product is a double expression narrowed to float. The program is built with
 * cl_khr_fp64 and the launcher refuses a device without fp64.
 *
 * The mask curve is a host-sampled piecewise-linear table, not a re-derived
 * FlatCurve. Its lookup, including the linear extrapolation outside the table
 * domain, is the same expression the host uses to validate the samples.
 *
 * xlogf/xexpf/atan2kf are the sleef implementations the host calls. The host
 * build uses -ffp-contract=off and this program sets FP_CONTRACT OFF, so the
 * polynomial operation order is the same.
 */
inline constexpr const char* RT_LAB_REGIONS_CL = R"CLC(

/* sleef.h's L2Uf/L2Lf, R_LN2f and the float Pi constants. */
#define RT_LR_L2Uf 0.693145751953125f
#define RT_LR_L2Lf 1.428606765330187045e-06f
#define RT_LR_RLN2 1.442695040888963407359924681001892137426645954152985934135449406931f
#define RT_LR_PI 3.1415927f
#define RT_LR_PI_2 (RT_LR_PI / 2.0f)
#define RT_LR_PI_4 (RT_LR_PI * 0.25f)

inline float rt_lr_mulsignf(const float x, const float y)
{
    return as_float(as_int(x) ^ (as_int(y) & (int)0x80000000));
}

inline float rt_lr_signf(const float d)
{
    return copysign(1.0f, d);
}

inline float rt_lr_xfabsf(const float x)
{
    return as_float(0x7fffffff & as_int(x));
}

/* sleef.h ilogbp1f(). */
inline int rt_lr_ilogbp1f(float d)
{
    const int m = d < 5.421010862427522E-20f;
    d = m ? 1.8446744073709552E19f * d : d;
    int q = (as_int(d) >> 23) & 0xff;
    q = m ? q - (64 + 0x7e) : q - 0x7e;
    return q;
}

/* sleef.h ldexpkf(). */
inline float rt_lr_ldexpkf(float x, int q)
{
    int m = q >> 31;
    m = (((m + q) >> 6) - m) << 4;
    q = q - (m << 2);
    float u = as_float(((int)(m + 0x7f)) << 23);
    u = u * u;
    x = x * u * u;
    u = as_float(((int)(q + 0x7f)) << 23);
    return x * u;
}

/* sleef.h xlogf(). */
inline float rt_lr_xlogf(float d)
{
    const int e = rt_lr_ilogbp1f(d * 0.7071f);
    const float m = rt_lr_ldexpkf(d, -e);
    float x = (m - 1.0f) / (m + 1.0f);
    const float x2 = x * x;

    float t = 0.2371599674224853515625f;
    t = t * x2 + 0.285279005765914916992188f;
    t = t * x2 + 0.400005519390106201171875f;
    t = t * x2 + 0.666666567325592041015625f;
    t = t * x2 + 2.0f;

    x = x * t + 0.693147180559945286226764f * (float)e;
    if (isinf(d) && d > 0.0f) {
        x = INFINITY;
    }
    if (d < 0.0f) {
        x = NAN;
    }
    if (d == 0.0f) {
        x = -INFINITY;
    }
    return x;
}

/* sleef.h xrintf(): _mm_cvt_ss2si rounds to nearest even. */
inline int rt_lr_xrintf(float x)
{
    return (int)rint(x);
}

/* sleef.h xexpf(). */
inline float rt_lr_xexpf(float d)
{
    if (d <= -104.0f) {
        return 0.0f;
    }
    const int q = rt_lr_xrintf(d * RT_LR_RLN2);
    float s = (float)q * -RT_LR_L2Uf + d;
    s = (float)q * -RT_LR_L2Lf + s;

    float u = 0.00136324646882712841033936f;
    u = u * s + 0.00836596917361021041870117f;
    u = u * s + 0.0416710823774337768554688f;
    u = u * s + 0.166665524244308471679688f;
    u = u * s + 0.499999850988388061523438f;

    u = s * (s * u + 1.0f) + 1.0f;
    return rt_lr_ldexpkf(u, q);
}

/* opthelper.h pow_F(a, b) = xexpf(b * xlogf(a)). */
inline float rt_lr_pow_F(const float a, const float b)
{
    return rt_lr_xexpf(b * rt_lr_xlogf(a));
}

/* sleef.h xlin2log(). */
inline float rt_lr_xlin2log(float x, float base)
{
    const float one = 1.0f;
    return rt_lr_xlogf(x * (base - one) + one) / rt_lr_xlogf(base);
}

/* iplabregions.cc fastlin2log() for base 10. */
inline float rt_lr_fastlin2log(float x, float factor, float base)
{
    const float base_log = 1.0f / rt_lr_xlogf(base);
    factor = factor * (base - 1.0f);
    return rt_lr_xlogf(x * factor + 1.0f) * base_log;
}

/* sleef.h atan2kf(). */
inline float rt_lr_atan2kf(float y, float x)
{
    float s, t, u;
    float q = 0.0f;

    if (x < 0.0f) {
        x = -x;
        q = -2.0f;
    }
    if (y > x) {
        t = x;
        x = y;
        y = -t;
        q += 1.0f;
    }

    s = y / x;
    t = s * s;

    u = 0.00282363896258175373077393f;
    u = u * t + -0.0159569028764963150024414f;
    u = u * t + 0.0425049886107444763183594f;
    u = u * t + -0.0748900920152664184570312f;
    u = u * t + 0.106347933411598205566406f;
    u = u * t + -0.142027363181114196777344f;
    u = u * t + 0.199926957488059997558594f;
    u = u * t + -0.333331018686294555664062f;

    t = u * t;
    t = t * s + s;
    return q * RT_LR_PI_2 + t;
}

/* sleef.h xatan2f(). */
inline float rt_lr_xatan2f(const float y, const float x)
{
    float r = rt_lr_atan2kf(rt_lr_xfabsf(y), x);

    r = rt_lr_mulsignf(r, x);
    if (isinf(x) || x == 0.0f) {
        r = RT_LR_PI_2 -
            (isinf(x) ? (rt_lr_signf(x) * RT_LR_PI_2) : 0.0f);
    }
    if (isinf(y)) {
        r = RT_LR_PI_2 -
            (isinf(x) ? (rt_lr_signf(x) * RT_LR_PI_4) : 0.0f);
    }
    if (y == 0.0f) {
        r = (rt_lr_signf(x) == -1.0f) ? RT_LR_PI : 0.0f;
    }

    return (isnan(x) || isnan(y)) ? NAN : rt_lr_mulsignf(r, y);
}

/* Color::huelab_to_huehsv2(): double arithmetic, narrowed by the caller. */
inline double rt_lr_huelab_to_huehsv2(const float HH)
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

/* rt_math.h LIM01() on the host's double mask product. */
inline double rt_lr_lim01d(const double a)
{
    const double low = (a < 1.0) ? a : 1.0;
    return (0.0 < low) ? low : 0.0;
}

/*
 * One host-sampled mask curve. slot[0] is the node count; then that many x
 * nodes, then that many y nodes. The lookup extrapolates both sides with the
 * first/last segment slope, exactly as the host validates it.
 */
inline double rt_lr_lut(__global const float* slot, const double t)
{
    const int count = (int)slot[0];
    if (count < 2) {
        return 0.0;
    }
    __global const float* xs = slot + 1;
    __global const float* ys = slot + 1 + count;
    if (t <= (double)xs[0]) {
        const double slope =
            ((double)ys[1] - (double)ys[0]) /
            ((double)xs[1] - (double)xs[0]);
        return (double)ys[0] + (t - (double)xs[0]) * slope;
    }
    if (t >= (double)xs[count - 1]) {
        const double slope =
            ((double)ys[count - 1] - (double)ys[count - 2]) /
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
    const double fraction =
        (t - (double)xs[low]) /
        ((double)xs[high] - (double)xs[low]);
    return (double)ys[low] +
           ((double)ys[high] - (double)ys[low]) * fraction;
}

/*
 * The region mask: the block's first loop. `src` is the original Lab
 * snapshot. The kernel writes the mask to scratch plane 0 and the guide to
 * plane 1, which guidedFilter() reads.
 */
__kernel void rt_lab_tools_region_mask(
    __global const float* src, __global float* scratch,
    __global const float* luts, __global const float* params,
    const int region, const int lut_stride, const float c_factor,
    const int width, const int height,
    const int src_pitch, const int src_plane,
    const int scratch_pitch, const int scratch_plane)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * src_pitch + x;
    const float lightness = src[offset];
    const float a = src[src_plane + offset];
    const float b = src[2 * src_plane + offset];

    const float l = lightness / 32768.0f;
    const float guide = rt_lt_lim01(l);

    const float c = sqrt(a * a + b * b) / 327.68f;
    const float c_index = rt_lr_fastlin2log(c, c_factor, 10.0f);

    float h = rt_lr_xatan2f(b, a);
    h = (float)rt_lr_huelab_to_huehsv2(h);
    h += 1.0f / 6.0f;
    if (h > 1.0f) {
        h -= 1.0f;
    }
    h = rt_lr_xlin2log(h, 3.0f);

    __global const float* values = params + region * 14;
    __global const float* region_luts = luts + region * 3 * lut_stride;

    double mask = 1.0;
    if (values[10] != 0.0f) {
        mask *= rt_lr_lut(region_luts, (double)h);
    }
    if (values[11] != 0.0f) {
        mask *= rt_lr_lut(region_luts + lut_stride, (double)c_index);
    }
    if (values[12] != 0.0f) {
        /* FlatCurve::getVal() adds 1.0 below its first polyline node. */
        double light_index = (double)l;
        if (light_index < 0.0) {
            light_index += 1.0;
        }
        mask *= rt_lr_lut(region_luts + 2 * lut_stride, light_index);
    }

    const int out = y * scratch_pitch + x;
    scratch[out] = (float)rt_lr_lim01d(mask);
    scratch[scratch_plane + out] = guide;
}

/*
 * guidedFilter()'s readout for one region mask: q = meana * I + meanb, where
 * I is the guide in scratch plane 1. The destination is a plane of the
 * region-mask buffer, so the initial mask can stay in scratch for the second
 * radius.
 */
__kernel void rt_lab_tools_region_upsample(
    __global const float* meana, __global const float* meanb,
    __global const float* scratch, __global float* dst,
    const int sw, const int sh, const int width, const int height,
    const int sub_pitch, const int scratch_pitch, const int scratch_plane,
    const int dst_pitch, const int dst_plane, const float col_scale,
    const float row_scale)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const float fx = x * col_scale;
    const float fy = y * row_scale;
    const float upper = rt_lt_bilinear(meana, sw, sh, sub_pitch, fx, fy);
    const float lower = rt_lt_bilinear(meanb, sw, sh, sub_pitch, fx, fy);
    const int offset = y * scratch_pitch + x;
    const int out = y * dst_pitch + x;
    dst[dst_plane + out] = upper * scratch[scratch_plane + offset] + lower;
}

/* Color::xyz2rgb() with the host's double matrix. */
inline void rt_lr_xyz2rgb(const float x, const float y, const float z,
                          __global const double* m,
                          float* r, float* g, float* b)
{
    *r = (float)(m[0] * (double)x + m[1] * (double)y + m[2] * (double)z);
    *g = (float)(m[3] * (double)x + m[4] * (double)y + m[5] * (double)z);
    *b = (float)(m[6] * (double)x + m[7] * (double)y + m[8] * (double)z);
}

/* Color::rgbxyz() with the host's double working-space matrix. */
inline void rt_lr_rgbxyz(const float r, const float g, const float b,
                         __global const double* m,
                         float* x, float* y, float* z)
{
    *x = (float)(m[0] * (double)r + m[1] * (double)g + m[2] * (double)b);
    *y = (float)(m[3] * (double)r + m[4] * (double)g + m[5] * (double)b);
    *z = (float)(m[6] * (double)r + m[7] * (double)g + m[8] * (double)b);
}

/* iplabregions.cc's CDL lambda. */
inline void rt_lr_cdl(float* l, float* a, float* b, const float slope,
                      const float offset, const float power,
                      const float saturation, __global const double* ws,
                      __global const double* iws,
                      __global const float* cachef,
                      __global const float* cachefy)
{
    float rgb[3];
    float x, y, z;
    rt_lt_lab2xyz(*l, *a, *b, &x, &y, &z);
    rt_lr_xyz2rgb(x, y, z, iws, &rgb[0], &rgb[1], &rgb[2]);
    for (int i = 0; i < 3; ++i) {
        rgb[i] = rt_lr_pow_F(
                     fmax((rgb[i] / 65535.0f) * slope + offset, 0.0f),
                     power) *
                 65535.0f;
    }
    if (saturation != 1.0f) {
        /* Color::rgbLuminance(r, g, b, ws): a double expression narrowed to
         * float. */
        const float luminance = (float)((double)rgb[0] * ws[3] +
                                        (double)rgb[1] * ws[4] +
                                        (double)rgb[2] * ws[5]);
        for (int i = 0; i < 3; ++i) {
            rgb[i] = fmax(luminance + saturation * (rgb[i] - luminance),
                          0.0f);
        }
    }
    rt_lr_rgbxyz(rgb[0], rgb[1], rgb[2], ws, &x, &y, &z);
    rt_lt_xyz2lab(x, y, z, cachef, cachefy, l, a, b);
}

/* iplabregions.cc's chan lambda. */
inline void rt_lr_chan(const float prev_l, const float prev_a,
                       const float prev_b, float* l, float* a, float* b,
                       const int channel, __global const double* ws,
                       __global const double* iws,
                       __global const float* cachef,
                       __global const float* cachefy)
{
    float prev_rgb[3];
    float rgb[3];
    float x, y, z;
    rt_lt_lab2xyz(*l, *a, *b, &x, &y, &z);
    rt_lr_xyz2rgb(x, y, z, iws, &rgb[0], &rgb[1], &rgb[2]);
    rt_lt_lab2xyz(prev_l, prev_a, prev_b, &x, &y, &z);
    rt_lr_xyz2rgb(x, y, z, iws, &prev_rgb[0], &prev_rgb[1], &prev_rgb[2]);
    prev_rgb[channel] = rgb[channel];
    rt_lr_rgbxyz(prev_rgb[0], prev_rgb[1], prev_rgb[2], ws, &x, &y, &z);
    rt_lt_xyz2lab(x, y, z, cachef, cachefy, l, a, b);
}

/*
 * One region's application, the block's per-pixel region loop for a single
 * region. The region passes run in region order, each on the previous
 * region's result, which is the block's sequential order. `masks` plane 0 is
 * the ab mask and plane 1 the L mask.
 */
__kernel void rt_lab_tools_region_apply(
    __global float* lab, __global const float* masks,
    __global const float* params, __global const double* ws,
    __global const double* iws, __global const float* cachef,
    __global const float* cachefy, const int region, const int width,
    const int height, const int lab_pitch, const int lab_plane,
    const int mask_pitch, const int mask_plane)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const int mask_offset = y * mask_pitch + x;

    __global const float* values = params + region * 14;
    const float abca = values[0];
    const float abcb = values[1];
    const float rs = values[2];
    const float slope = values[3];
    const float offset_v = values[4];
    const float power = values[5];
    const int channel = (int)values[6];

    const float blend = masks[mask_offset];
    float l = lab[offset];
    float a = lab[lab_plane + offset];
    float b = lab[2 * lab_plane + offset];

    float l_new = l;
    float a_new = fmax(fmin(a + l * abca, 42000.0f), -42000.0f);
    float b_new = fmax(fmin(b + l * abcb, 42000.0f), -42000.0f);
    if (slope != 1.0f || offset_v != 0.0f || power != 1.0f ||
        rs != 1.0f) {
        rt_lr_cdl(&l_new, &a_new, &b_new, slope, offset_v, power, rs, ws,
                  iws, cachef, cachefy);
    }
    l_new = fmax(l_new, 0.0f);
    if (channel >= 0) {
        rt_lr_chan(l, a, b, &l_new, &a_new, &b_new, channel, ws, iws,
                   cachef, cachefy);
    }
    l = rt_lt_intp(masks[mask_plane + mask_offset], l_new, l);
    a = rt_lt_intp(blend, a_new, a);
    b = rt_lt_intp(blend, b_new, b);

    lab[offset] = l;
    lab[lab_plane + offset] = a;
    lab[2 * lab_plane + offset] = b;
}
)CLC";
