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
 * Device arithmetic for the lab.tools node.
 *
 * The node runs three of the Lab block's pixel tools: the LabGrid colour-toning
 * pass, shadows/highlights in both its Lab and RGB modes, and vibrance. The
 * Lab-curve pass is a separate fp64 program in lab_curve_cl.h. Everything the
 * block does with pow_F(), the sh_contrast NURBS curve, the diagonal curves or
 * lcms is either not part of this node or is built on the host.
 *
 * The shadows/highlights round is the block's own sequence:
 *
 *   mask/L from the L plane, guidedFilter(L, mask, mask, r, 0.075, m, 4),
 *   a host-built tone table, then the blend over the block's region.
 *
 * guidedFilter() is ported here exactly as guidedfilter.cc implements it,
 * including the fast-guided-filter subsampling of 4, the box radius r/4, the
 * bilinear rescale and readout and boxblur()'s own running sums. The box blur
 * is a pair of serial kernels: one work item per row for the horizontal pass
 * and one per column for the vertical pass, following boxblur()'s float
 * operations in the same order and reading the departing sample from the
 * read-only source plane instead of a private ring buffer. That is the same
 * arithmetic, so it is not an approximation.
 *
 * The source is self-contained: the host prepends no other kernel fragment.
 */
inline constexpr const char* RT_LAB_TOOLS_CL = R"CLC(
/* Imagefloat/LabImage sample scale (MAXVALF). */
#define RT_LT_MAXVAL 65535.0f
/* Color::Lab2XYZ()'s constants, the same float values the block uses. */
#define RT_LT_D50X 0.9642f
#define RT_LT_D50Z 0.8249f
#define RT_LT_KAPPA 903.2962962962963f
#define RT_LT_EPSKAP 8.0f
#define RT_LT_C1BY116 (1.0f / 116.0f)
#define RT_LT_C16BY116 (16.0f / 116.0f)
#define RT_LT_EPSINV3 (6.0f / 29.0f)
#define RT_LT_KAPPAINV (27.0f / 24389.0f)
/* rt_math.h pow4() = SQR(SQR(x)). */
inline float rt_lt_pow4(const float x)
{
    const float x2 = x * x;
    return x2 * x2;
}

/* rt_math.h LIM01(), with the same comparison order. */
inline float rt_lt_lim01(const float a)
{
    const float low = (a < 1.0f) ? a : 1.0f;
    return (0.0f < low) ? low : 0.0f;
}

/* rt_math.h intp(a, b, c) = a * (b - c) + c. */
inline float rt_lt_intp(const float a, const float b, const float c)
{
    return a * (b - c) + c;
}

/* LUTf::operator[](float) with LUT_CLIP_BELOW | LUT_CLIP_ABOVE. */
inline float rt_lt_lut(__global const float* lut, const int lut_size,
                       const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        return lut[0];
    }
    if (index > (float)(lut_size - 2)) {
        return lut[lut_size - 1];
    }
    const float low = lut[idx];
    const float high = lut[idx + 1] - low;
    return low + high * (index - (float)idx);
}

/* getBilinearValue() from rescale.h. */
inline float rt_lt_bilinear(__global const float* src, const int width,
                            const int height, const int pitch,
                            const float x, const float y)
{
    const int xi = (int)x;
    const int yi = (int)y;
    const float xf = x - (float)xi;
    const float yf = y - (float)yi;
    const int xi1 = min(xi + 1, width - 1);
    const int yi1 = min(yi + 1, height - 1);
    const int row = yi * pitch;
    const int row1 = yi1 * pitch;
    const float bottom = rt_lt_intp(xf, src[row + xi1], src[row + xi]);
    const float top = rt_lt_intp(xf, src[row1 + xi1], src[row1 + xi]);
    return rt_lt_intp(yf, top, bottom);
}

/* Color::Lab2XYZ(). */
inline void rt_lt_lab2xyz(const float L, const float a, const float b,
                          float* x, float* y, float* z)
{
    const float LL = L / 327.68f;
    const float aa = a / 327.68f;
    const float bb = b / 327.68f;
    const float fy = (RT_LT_C1BY116 * LL) + RT_LT_C16BY116;
    const float fx = (0.002f * aa) + fy;
    const float fz = fy - (0.005f * bb);
    const float fx3 = fx * fx * fx;
    const float fz3 = fz * fz * fz;
    const float fxl = (116.0f * fx - 16.0f) * RT_LT_KAPPAINV;
    const float fzl = (116.0f * fz - 16.0f) * RT_LT_KAPPAINV;
    *x = 65535.0f * ((fx > RT_LT_EPSINV3) ? fx3 : fxl) * RT_LT_D50X;
    *z = 65535.0f * ((fz > RT_LT_EPSINV3) ? fz3 : fzl) * RT_LT_D50Z;
    *y = (LL > RT_LT_EPSKAP) ? 65535.0f * fy * fy * fy
                             : 65535.0f * LL / RT_LT_KAPPA;
}

/* Color::xyz2rgb() with a working-space inverse matrix. */
inline void rt_lt_xyz2rgb(const float x, const float y, const float z,
                          __global const float* m,
                          float* r, float* g, float* b)
{
    *r = m[0] * x + m[1] * y + m[2] * z;
    *g = m[3] * x + m[4] * y + m[5] * z;
    *b = m[6] * x + m[7] * y + m[8] * z;
}

/*
 * The round's mask and guiding image: ImProcFunctions::shadowsHighlights()'s
 * first loop. `scratch` plane 0 receives the mask, plane 1 the guiding L.
 * thresh and scale are shadowsHighlightsThreshold()/Scale() on the host.
 */
__kernel void rt_lab_tools_mask(__global const float* lab,
                                __global float* scratch,
                                const int width, const int height,
                                const int lab_pitch, const int lab_plane,
                                const int scratch_pitch,
                                const int scratch_plane,
                                const int hl, const float thresh,
                                const float scale)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const float l = lab[y * lab_pitch + x];
    const float l1 = l / 32768.0f;
    float mask = 0.0f;
    float guided = 0.0f;
    if (hl != 0) {
        mask = (l > thresh) ? 1.0f : rt_lt_pow4(l * scale);
        guided = 1.0f - l1;
    } else {
        mask = (l <= thresh) ? 1.0f : rt_lt_pow4(scale / l);
        guided = l1;
    }

    const int out = y * scratch_pitch + x;
    scratch[out] = mask;
    scratch[scratch_plane + out] = guided;
}

/*
 * guidedFilter()'s f_subsample() for the two arrays: rescaleBilinear() from
 * the full frame to the subsampled grid. `scratch` plane 0 holds the mask and
 * plane 1 the L guide; `i1` receives the guide's rescale and `p1` the mask's.
 */
__kernel void rt_lab_tools_subsample(__global const float* scratch,
                                     __global float* i1,
                                     __global float* p1,
                                     const int sw, const int sh,
                                     const int width, const int height,
                                     const int scratch_pitch,
                                     const int scratch_plane,
                                     const int sub_pitch,
                                     const float col_scale,
                                     const float row_scale)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= sw || y >= sh) {
        return;
    }

    const float fx = x * col_scale;
    const float fy = y * row_scale;
    const int out = y * sub_pitch + x;
    i1[out] = rt_lt_bilinear(scratch + scratch_plane, width, height,
                             scratch_pitch, fx, fy);
    p1[out] = rt_lt_bilinear(scratch, width, height, scratch_pitch, fx, fy);
}

/*
 * boxblur()'s horizontal pass, one work item per row.
 *
 * The host keeps the departing samples of the running sum in a ring buffer;
 * here they are read from the read-only source plane, which holds the same
 * values because the destination is a different buffer. radius 0 is the
 * host's copy.
 */
__kernel void rt_lab_tools_box_h(__global const float* src,
                                 __global float* dst,
                                 const int radius,
                                 const int width, const int height,
                                 const int pitch)
{
    const int row = get_global_id(1);
    if (row >= height) {
        return;
    }

    __global const float* s = src + row * pitch;
    __global float* d = dst + row * pitch;
    if (radius == 0) {
        for (int col = 0; col < width; ++col) {
            d[col] = s[col];
        }
        return;
    }

    float len = (float)radius + 1.0f;
    float tempval = s[0];
    for (int j = 1; j <= radius; ++j) {
        tempval += s[j];
    }
    tempval /= len;
    d[0] = tempval;

    for (int col = 1; col <= radius; ++col) {
        tempval = (tempval * len + s[col + radius]) / (len + 1.0f);
        d[col] = tempval;
        len += 1.0f;
    }

    for (int col = radius + 1; col < width - radius; ++col) {
        const float departing = s[col - radius - 1];
        tempval = tempval + (s[col + radius] - departing) / len;
        d[col] = tempval;
    }

    for (int col = width - radius; col < width; ++col) {
        tempval = (tempval * len - s[col - radius - 1]) / (len - 1.0f);
        d[col] = tempval;
        len -= 1.0f;
    }
}

/*
 * boxblur()'s vertical pass, one work item per column. The interior uses the
 * reciprocal multiply boxblur()'s SSE and remaining-column paths both use; the
 * head and tail use division, as the host does.
 */
__kernel void rt_lab_tools_box_v(__global const float* src,
                                 __global float* dst,
                                 const int radius,
                                 const int width, const int height,
                                 const int pitch)
{
    const int col = get_global_id(0);
    if (col >= width) {
        return;
    }

    if (radius == 0) {
        for (int row = 0; row < height; ++row) {
            dst[row * pitch + col] = src[row * pitch + col];
        }
        return;
    }

    float len = (float)radius + 1.0f;
    float tempval = src[col];
    for (int i = 1; i <= radius; ++i) {
        tempval += src[i * pitch + col];
    }
    tempval /= len;
    dst[col] = tempval;

    for (int row = 1; row <= radius; ++row) {
        tempval = (tempval * len + src[(row + radius) * pitch + col]) /
                  (len + 1.0f);
        dst[row * pitch + col] = tempval;
        len += 1.0f;
    }

    const float rlen = 1.0f / len;
    for (int row = radius + 1; row < height - radius; ++row) {
        const float departing = src[(row - radius - 1) * pitch + col];
        tempval = tempval +
                  (src[(row + radius) * pitch + col] - departing) * rlen;
        dst[row * pitch + col] = tempval;
    }

    for (int row = height - radius; row < height; ++row) {
        const float lenm1 = len - 1.0f;
        tempval = (tempval * len -
                   src[(row - radius - 1) * pitch + col]) / lenm1;
        dst[row * pitch + col] = tempval;
        len = lenm1;
    }
}

/*
 * guidedFilter()'s element-wise operations on the subsampled planes.
 *   0: dst = a * b
 *   1: dst = c - a * b
 *   2: dst = a / (b + epsilon)
 */
__kernel void rt_lab_tools_combine(__global float* dst,
                                   __global const float* a,
                                   __global const float* b,
                                   __global const float* c,
                                   const int op, const float epsilon,
                                   const int pitch, const int width,
                                   const int height)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int o = y * pitch + x;
    const float av = a[o];
    const float bv = b[o];
    float result = 0.0f;
    if (op == 0) {
        result = av * bv;
    } else if (op == 1) {
        result = c[o] - (av * bv);
    } else {
        result = av / (bv + epsilon);
    }
    dst[o] = result;
}

/*
 * guidedFilter()'s readout: the bilinear upsample of meana and meanb, then
 * q = meana * I + meanb over the full frame. `scratch` plane 0 is the mask
 * (the destination) and plane 1 the guiding L.
 */
__kernel void rt_lab_tools_upsample(__global const float* meana,
                                    __global const float* meanb,
                                    __global float* scratch,
                                    const int sw, const int sh,
                                    const int width, const int height,
                                    const int sub_pitch,
                                    const int scratch_pitch,
                                    const int scratch_plane,
                                    const float col_scale,
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
    const int o = y * scratch_pitch + x;
    scratch[o] = upper * scratch[scratch_plane + o] + lower;
}

/*
 * The blend of the Lab-mode round: intp(blend, f[l], l) and, when pushing
 * shadows, the chromaticity scaling. `lut` is the host-built tone table.
 */
__kernel void rt_lab_tools_apply(__global float* lab,
                                 __global const float* mask,
                                 __global const float* lut,
                                 const int lut_size,
                                 const int width, const int height,
                                 const int lab_pitch, const int lab_plane,
                                 const int mask_pitch, const int hl)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float l = lab[offset];
    const float blend = rt_lt_lim01(mask[y * mask_pitch + x]);
    const float orig = 1.0f - blend;
    if (l >= 0.0f && l < 32768.0f) {
        const float value = rt_lt_intp(blend, rt_lt_lut(lut, lut_size, l), l);
        lab[offset] = value;
        if (hl == 0 && l > 1.0f) {
            /* when pushing shadows, scale also the chromaticity */
            const float ratio = value / l * 0.5f;
            const float s = ((ratio < 1.0f) ? 1.0f : ratio) * blend;
            const float a = lab[lab_plane + offset];
            const float b = lab[2 * lab_plane + offset];
            lab[lab_plane + offset] = a * s + a * orig;
            lab[2 * lab_plane + offset] = b * s + b * orig;
        }
    }
}

/*
 * Color::computeXYZ2Lab()'s helpers, for the RGB-mode blend. The host
 * evaluates its negative branches and its two colour-matrix products in
 * double; the kernel keeps single precision and the differences are one ulp of
 * a 16-bit channel, which the node's table-slope bound keeps below one LSB16.
 */

/*
 * Color::computeXYZ2Lab()'s cube root, ported from sleef.h's xcbrtf().
 *
 * The host does not call the libm cube root above the 16-bit range: it runs
 * this polynomial, which is a few ulps away from a correctly rounded root.
 * The difference is small but the RGB-mode blend reads it, so the kernel runs
 * the same polynomial on the same float operations instead of cbrt().
 */
inline float rt_lt_cbrt(float d)
{
    float x, y, q = 1.0f;
    int e, r;

    /* ilogbp1f() */
    const int m = d < 5.421010862427522E-20f;
    d = m ? 1.8446744073709552E19f * d : d;
    int exponent = (as_int(d) >> 23) & 0xff;
    e = m ? exponent - (64 + 0x7e) : exponent - 0x7e;

    /* ldexpkf(d, -e) */
    int shift = (-e) >> 31;
    shift = (((shift + (-e)) >> 6) - shift) << 4;
    int rest = (-e) - (shift << 2);
    float u = as_float(((int)(shift + 0x7f)) << 23);
    u = u * u;
    d = d * u * u;
    u = as_float(((int)(rest + 0x7f)) << 23);
    d = d * u;

    r = (e + 6144) % 3;
    q = (r == 1) ? 1.2599210498948731647672106f : q;
    q = (r == 2) ? 1.5874010519681994747517056f : q;

    /* ldexpkf(q, (e + 6144) / 3 - 2048) */
    const int qexp = (e + 6144) / 3 - 2048;
    shift = qexp >> 31;
    shift = (((shift + qexp) >> 6) - shift) << 4;
    rest = qexp - (shift << 2);
    u = as_float(((int)(shift + 0x7f)) << 23);
    u = u * u;
    q = q * u * u;
    u = as_float(((int)(rest + 0x7f)) << 23);
    q = q * u;

    /* mulsignf(q, d) */
    q = as_float(as_int(q) ^ (as_int(d) & (1 << 31)));
    /* xfabsf(d) */
    d = as_float(0x7fffffff & as_int(d));

    x = -0.601564466953277587890625f;
    x = x * d + 2.8208892345428466796875f;
    x = x * d + -5.532182216644287109375f;
    x = x * d + 5.898262500762939453125f;
    x = x * d + -3.8095417022705078125f;
    x = x * d + 2.2241256237030029296875f;

    y = d * x * x;
    y = (y - (2.0f / 3.0f) * y * (y * x - 1.0f)) * q;
    return y;
}

/* LUTf::operator[](float) with LUT_CLIP_BELOW: Color::cachef[]'s access. */
inline float rt_lt_cache_lookup(__global const float* table, const float value)
{
    int index = (int)value;
    if (value < 0.0f) {
        index = 0;
    } else if (value > RT_LT_MAXVAL - 2.0f) {
        index = 65534;
    }
    const float fraction = value - (float)index;
    const float low = table[index];
    return low + (table[index + 1] - low) * fraction;
}

/* Color::computeXYZ2Lab(): the host's own cube root, as above. */
inline float rt_lt_compute_f(const float value, __global const float* cache)
{
    if (value < 0.0f) {
        return 327.68f * ((RT_LT_KAPPA * value / RT_LT_MAXVAL + 16.0f) /
                          116.0f);
    }
    if (value > RT_LT_MAXVAL) {
        return 327.68f * rt_lt_cbrt(value / RT_LT_MAXVAL);
    }
    return rt_lt_cache_lookup(cache, value);
}

/* Color::computeXYZ2LabY(). */
inline float rt_lt_compute_fy(const float value, __global const float* cache)
{
    if (value < 0.0f) {
        return 327.68f * (RT_LT_KAPPA * value / RT_LT_MAXVAL);
    }
    if (value > RT_LT_MAXVAL) {
        return 327.68f * (116.0f * rt_lt_cbrt(value / RT_LT_MAXVAL) - 16.0f);
    }
    return rt_lt_cache_lookup(cache, value);
}

/* Color::XYZ2Lab(). */
inline void rt_lt_xyz2lab(const float X, const float Y, const float Z,
                          __global const float* cachef,
                          __global const float* cachefy,
                          float* L, float* a, float* b)
{
    const float fx = rt_lt_compute_f(X / RT_LT_D50X, cachef);
    const float fy = rt_lt_compute_f(Y, cachef);
    const float fz = rt_lt_compute_f(Z / RT_LT_D50Z, cachef);
    *L = rt_lt_compute_fy(Y, cachefy);
    *a = 500.0f * (fx - fy);
    *b = 200.0f * (fy - fz);
}

/*
 * The blend of the RGB-mode round: lab2rgb(), the per-channel tone table
 * where the channel is not out of gamut, then rgb2lab() with the working
 * matrices and the host's own Lab caches.
 *
 * The host evaluates the two colour-matrix products in double; the kernel
 * keeps single precision, so the channel that indexes the tone table can
 * differ by an ulp of a 16-bit value. The context refuses a tone table whose
 * largest adjacent step can turn that ulp into more than one LSB16.
 */
__kernel void rt_lab_tools_apply_rgb(__global float* lab,
                                     __global const float* mask,
                                     __global const float* lut,
                                     const int lut_size,
                                     __global const float* ws,
                                     __global const float* iws,
                                     __global const float* cachef,
                                     __global const float* cachefy,
                                     const int width, const int height,
                                     const int lab_pitch,
                                     const int lab_plane,
                                     const int mask_pitch)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float l = lab[offset];
    const float blend = rt_lt_lim01(mask[y * mask_pitch + x]);
    if (l >= 0.0f && l < 32768.0f) {
        const float a = lab[lab_plane + offset];
        const float b = lab[2 * lab_plane + offset];

        float xyz[3] = {0.0f, 0.0f, 0.0f};
        rt_lt_lab2xyz(l, a, b, &xyz[0], &xyz[1], &xyz[2]);
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        rgb[0] = iws[0] * xyz[0] + iws[1] * xyz[1] + iws[2] * xyz[2];
        rgb[1] = iws[3] * xyz[0] + iws[4] * xyz[1] + iws[5] * xyz[2];
        rgb[2] = iws[6] * xyz[0] + iws[7] * xyz[1] + iws[8] * xyz[2];

        for (int i = 0; i < 3; ++i) {
            const float c = rgb[i];
            /* OOG(c) in rt_math.h: c < 0 || c > MAXVAL. */
            if (!(c < 0.0f || c > RT_LT_MAXVAL)) {
                rgb[i] = rt_lt_intp(blend, rt_lt_lut(lut, lut_size, c), c);
            }
        }

        const float out_x = ws[0] * rgb[0] + ws[1] * rgb[1] + ws[2] * rgb[2];
        const float out_y = ws[3] * rgb[0] + ws[4] * rgb[1] + ws[5] * rgb[2];
        const float out_z = ws[6] * rgb[0] + ws[7] * rgb[1] + ws[8] * rgb[2];

        float out_l = 0.0f;
        float out_a = 0.0f;
        float out_b = 0.0f;
        rt_lt_xyz2lab(out_x, out_y, out_z, cachef, cachefy, &out_l, &out_a,
                      &out_b);
        lab[offset] = out_l;
        lab[lab_plane + offset] = out_a;
        lab[2 * lab_plane + offset] = out_b;
    }
}
/* The LabGrid pass: a += L * a_scale + a_base, b likewise. */
__kernel void rt_lab_tools_labgrid(__global float* lab,
                                   const int width, const int height,
                                   const int lab_pitch, const int lab_plane,
                                   const float a_scale, const float a_base,
                                   const float b_scale, const float b_base)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float l = lab[offset];
    lab[lab_plane + offset] += l * a_scale + a_base;
    lab[2 * lab_plane + offset] += l * b_scale + b_base;
}
)CLC";
