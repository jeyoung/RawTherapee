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

#include "procparams.h"
#include "stage/stage_abi.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

/*
 * Device kernels for the unsupported-mask path. They are compiled as one
 * program and entered separately, so the node costs one build.
 *
 * The threshold evaluation uses double when the device advertises
 * cl_khr_fp64, mirroring Threshold::multiply(), and degrades to float
 * otherwise. The latter still only affects the final rounding of one sample.
 */
inline constexpr const char* RT_SHARPEN_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF
#ifdef cl_khr_fp64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
typedef double rt_sharpen_real;
#else
typedef float rt_sharpen_real;
#endif

/* The CPU truncates the difference at this magnitude. */
#define RT_SHARPEN_THRESHOLD_UPPER_BOUND 2000.0f
/* The 5x5/7x7 division kernels clamp the denominator to this floor. */
#define RT_SHARPEN_DIV_FLOOR 0.00001f

/* buildBlendMask() with autoContrast off, including its border replication. */
__kernel void rt_sharpen_mask(__global const float* src,
                              __global float* mask,
                              const int width,
                              const int height,
                              const int pitch,
                              const float threshold,
                              const float scale)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    if (threshold == 0.0f) {
        mask[y * pitch + x] = 1.0f;
        return;
    }

    const int sx = x < 2 ? 2 : (x >= width - 2 ? width - 3 : x);
    const int sy = y < 2 ? 2 : (y >= height - 2 ? height - 3 : y);
    const int base = sy * pitch + sx;
    const float dx1 = src[base + 1] - src[base - 1];
    const float dy1 = src[base + pitch] - src[base - pitch];
    const float dx2 = src[base + 2] - src[base - 2];
    const float dy2 = src[base + 2 * pitch] - src[base - 2 * pitch];
    const float gradient = sqrt(dx1 * dx1 + dy1 * dy1 + dx2 * dx2 +
                                dy2 * dy2) * scale;
    const float xv = -16.0f + (16.0f / threshold) * gradient;
    mask[y * pitch + x] = 0.5f * (1.0f + xv / sqrt(1.0f + xv * xv));
}

/*
 * One 1-D Young-van Vliet IIR line in place at the destination. Horizontal
 * lines advance by one sample and start `pitch` samples apart; vertical lines
 * advance by `pitch` and start one sample apart. The coefficients arrive as
 * floats that the host derived in double.
 */
__kernel void rt_sharpen_iir_line(__global const float* src,
                                  __global float* dst,
                                  const int length,
                                  const int src_base_step,
                                  const int dst_base_step,
                                  const int src_stride,
                                  const int dst_stride,
                                  const float B,
                                  const float b1,
                                  const float b2,
                                  const float b3,
                                  const float m00,
                                  const float m01,
                                  const float m02,
                                  const float m10,
                                  const float m11,
                                  const float m12,
                                  const float m20,
                                  const float m21,
                                  const float m22)
{
    const int line = (int)get_global_id(0);
    const int sb = line * src_base_step;
    const int db = line * dst_base_step;

    const float s0 = src[sb];
    const float t0 = s0 * (B + b1 + b2 + b3);
    dst[db] = t0;
    const float t1 = B * src[sb + src_stride] + b1 * t0 + s0 * (b2 + b3);
    dst[db + dst_stride] = t1;
    const float t2 = B * src[sb + 2 * src_stride] + b1 * t1 + b2 * t0 +
                     b3 * s0;
    dst[db + 2 * dst_stride] = t2;

    /*
     * The tail's boundary sample is the *input* last sample. Reading it
     * before the forward pass lets the caller run the vertical pass in place:
     * afterwards dst[last] holds the forward result, not the input.
     */
    const int last = length - 1;
    const float sW = src[sb + last * src_stride];

    for (int j = 3; j < length; ++j) {
        dst[db + j * dst_stride] =
            B * src[sb + j * src_stride] +
            b1 * dst[db + (j - 1) * dst_stride] +
            b2 * dst[db + (j - 2) * dst_stride] +
            b3 * dst[db + (j - 3) * dst_stride];
    }

    const float f1 = dst[db + last * dst_stride];
    const float f2 = dst[db + (last - 1) * dst_stride];
    const float f3 = dst[db + (last - 2) * dst_stride];
    const float c0 = sW + m00 * (f1 - sW) + m01 * (f2 - sW) + m02 * (f3 - sW);
    const float c1 = sW + m10 * (f1 - sW) + m11 * (f2 - sW) + m12 * (f3 - sW);
    const float c2 = sW + m20 * (f1 - sW) + m21 * (f2 - sW) + m22 * (f3 - sW);

    dst[db + last * dst_stride] = c0;
    dst[db + (last - 1) * dst_stride] =
        B * f2 + b1 * dst[db + last * dst_stride] + b2 * c1 + b3 * c2;
    dst[db + (last - 2) * dst_stride] =
        B * f3 + b1 * dst[db + (last - 1) * dst_stride] +
        b2 * dst[db + last * dst_stride] + b3 * c1;

    for (int j = last - 3; j >= 0; --j) {
        dst[db + j * dst_stride] =
            B * dst[db + j * dst_stride] +
            b1 * dst[db + (j + 1) * dst_stride] +
            b2 * dst[db + (j + 2) * dst_stride] +
            b3 * dst[db + (j + 3) * dst_stride];
    }
}

/* gauss3x3() for distinct source and destination, including its borders. */
__kernel void rt_sharpen_fir3(__global const float* src,
                              __global float* dst,
                              const int width,
                              const int height,
                              const int src_pitch,
                              const int dst_pitch,
                              const float c0,
                              const float c1,
                              const float c2,
                              const float b0,
                              const float b1)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int src_index = y * src_pitch + x;
    const int dst_index = y * dst_pitch + x;
    float value;
    if (y == 0 || y == height - 1) {
        if (x == 0 || x == width - 1) {
            value = src[src_index];
        } else {
            value = b1 * (src[src_index - 1] + src[src_index + 1]) +
                    b0 * src[src_index];
        }
    } else if (x == 0 || x == width - 1) {
        value = b1 * (src[src_index - src_pitch] +
                      src[src_index + src_pitch]) +
                b0 * src[src_index];
    } else {
        value = c2 * (src[src_index - src_pitch - 1] +
                      src[src_index - src_pitch + 1] +
                      src[src_index + src_pitch - 1] +
                      src[src_index + src_pitch + 1]) +
                c1 * (src[src_index - src_pitch] +
                      src[src_index - 1] +
                      src[src_index + 1] +
                      src[src_index + src_pitch]) +
                c0 * src[src_index];
    }
    dst[dst_index] = value;
}

/* Threshold::multiply() with its branch order and double evaluation. */
float rt_sharpen_threshold(const float x,
                           const float y_max,
                           const int bl,
                           const int tl,
                           const int br,
                           const int tr,
                           const int is_double,
                           const int start_at_one)
{
    const rt_sharpen_real val = (rt_sharpen_real)x;
    const rt_sharpen_real y = (rt_sharpen_real)y_max;

    if (start_at_one) {
        if (is_double) {
            if (val == (rt_sharpen_real)br &&
                (rt_sharpen_real)br == (rt_sharpen_real)tr) {
                return 0.0f;
            }
            if (val >= (rt_sharpen_real)tr) {
                return y_max;
            }
            if (val > (rt_sharpen_real)br) {
                return (float)(y * (val - (rt_sharpen_real)br) /
                               ((rt_sharpen_real)tr - (rt_sharpen_real)br));
            }
        }
        if (val >= (rt_sharpen_real)bl) {
            return 0.0f;
        }
        if (val > (rt_sharpen_real)tl) {
            return (float)(y * (1.0 - (val - (rt_sharpen_real)bl) /
                                      ((rt_sharpen_real)tl -
                                       (rt_sharpen_real)bl)));
        }
        return y_max;
    }

    if (is_double) {
        if (val == (rt_sharpen_real)br &&
            (rt_sharpen_real)br == (rt_sharpen_real)tr) {
            return y_max;
        }
        if (val >= (rt_sharpen_real)br) {
            return 0.0f;
        }
        if (val > (rt_sharpen_real)tr) {
            return (float)(y * (1.0 - (val - (rt_sharpen_real)tr) /
                                      ((rt_sharpen_real)br -
                                       (rt_sharpen_real)tr)));
        }
    }
    if (val >= (rt_sharpen_real)tl) {
        return y_max;
    }
    if (val > (rt_sharpen_real)bl) {
        return (float)(y * (val - (rt_sharpen_real)bl) /
                       ((rt_sharpen_real)tl - (rt_sharpen_real)bl));
    }
    return 0.0f;
}

/*
 * Threshold, delta and the intp(a, b, c) = a * (b - c) + c blend, in place.
 * `base` is the plane the difference is taken from: the luminance plane, or
 * the edge-limited plane when edges only is on. The result always replaces
 * the luminance plane.
 */
__kernel void rt_sharpen_apply(__global float* luminance,
                               __global const float* base_plane,
                               __global const float* blur,
                               __global const float* mask,
                               const int width,
                               const int height,
                               const int pitch,
                               const int use_blur,
                               const int amount,
                               const int bl,
                               const int tl,
                               const int br,
                               const int tr,
                               const int is_double,
                               const int start_at_one)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float base = base_plane[index];
    const float current = luminance[index];
    const float diff = use_blur != 0 ? base - blur[index] : 0.0f;
    const float y_max = (float)amount * diff * 0.01f;
    const float magnitude = fabs(diff) < RT_SHARPEN_THRESHOLD_UPPER_BOUND
                                ? fabs(diff)
                                : RT_SHARPEN_THRESHOLD_UPPER_BOUND;
    const float delta = rt_sharpen_threshold(magnitude, y_max, bl, tl, br, tr,
                                             is_double, start_at_one);
    const float updated = current + delta;
    luminance[index] = mask[index] * (updated - current) + current;
}

/* Richardson-Lucy initialisation: clamp L and seed the running estimate. */
__kernel void rt_sharpen_rld_init(__global float* luminance,
                                  __global float* estimate,
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
    const float value = luminance[index] < 0.0f ? 0.0f : luminance[index];
    luminance[index] = value;
    estimate[index] = value;
}

/* gauss3x3()'s denominator, including its first/last row and column rules. */
float rt_sharpen_blur3(__global const float* src,
                       const int index,
                       const int pitch,
                       const int x,
                       const int y,
                       const int width,
                       const int height,
                       const float c0,
                       const float c1,
                       const float c2,
                       const float b0,
                       const float b1)
{
    if (y == 0 || y == height - 1) {
        if (x == 0 || x == width - 1) {
            return src[index];
        }
        return b1 * (src[index - 1] + src[index + 1]) + b0 * src[index];
    }
    if (x == 0 || x == width - 1) {
        return b1 * (src[index - pitch] + src[index + pitch]) +
               b0 * src[index];
    }
    return c2 * (src[index - pitch - 1] + src[index - pitch + 1] +
                 src[index + pitch - 1] + src[index + pitch + 1]) +
           c1 * (src[index - pitch] + src[index - 1] + src[index + 1] +
                 src[index + pitch]) +
           c0 * src[index];
}

__kernel void rt_sharpen_div3(__global const float* estimate,
                              __global float* tmp,
                              __global const float* luminance,
                              const int width,
                              const int height,
                              const int pitch,
                              const float c0,
                              const float c1,
                              const float c2,
                              const float b0,
                              const float b1)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float denom = rt_sharpen_blur3(estimate, index, pitch, x, y, width,
                                         height, c0, c1, c2, b0, b1);
    const float divisor = denom > 0.0f ? denom : 1.0f;
    const float value = luminance[index] / divisor;
    tmp[index] = value < 0.0f ? 0.0f : value;
}

__kernel void rt_sharpen_mult3(__global const float* tmp,
                               __global float* estimate,
                               const int width,
                               const int height,
                               const int pitch,
                               const float c0,
                               const float c1,
                               const float c2,
                               const float b0,
                               const float b1)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    estimate[index] *= rt_sharpen_blur3(tmp, index, pitch, x, y, width,
                                        height, c0, c1, c2, b0, b1);
}

/* gauss7x7div()'s weighted sum, including the c21 double-use quirk. */
float rt_sharpen_blur7(__global const float* src,
                       const int index,
                       const int pitch,
                       const float c00,
                       const float c10,
                       const float c11,
                       const float c20,
                       const float c21,
                       const float c22,
                       const float c30,
                       const float c31)
{
    const int p = pitch;
    return c31 * (src[index - 3 * p - 1] + src[index - 3 * p + 1] +
                  src[index - p - 3] + src[index - p + 3] +
                  src[index + p - 3] + src[index + p + 3] +
                  src[index + 3 * p - 1] + src[index + 3 * p + 1]) +
           c30 * (src[index - 3 * p] + src[index - 3] +
                  src[index + 3] + src[index + 3 * p]) +
           c22 * (src[index - 2 * p - 2] + src[index - 2 * p + 2] +
                  src[index + 2 * p - 2] + src[index + 2 * p + 2]) +
           c21 * (src[index - 2 * p - 1] +
                  src[index - 2 * p + 1] * c21 +
                  src[index - p - 2] + src[index - p + 2] +
                  src[index + p - 2] + src[index + p + 2] +
                  src[index + 2 * p - 1] + src[index + 2 * p + 1]) +
           c20 * (src[index - 2 * p] + src[index - 2] +
                  src[index + 2] + src[index + 2 * p]) +
           c11 * (src[index - p - 1] + src[index - p + 1] +
                  src[index + p - 1] + src[index + p + 1]) +
           c10 * (src[index - p] + src[index - 1] + src[index + 1] +
                  src[index + p]) +
           c00 * src[index];
}

/* gauss5x5div()'s weighted sum. */
float rt_sharpen_blur5(__global const float* src,
                       const int index,
                       const int pitch,
                       const float c00,
                       const float c10,
                       const float c11,
                       const float c20,
                       const float c21)
{
    const int p = pitch;
    return c21 * (src[index - 2 * p - 1] + src[index - 2 * p + 1] +
                  src[index - p - 2] + src[index - p + 2] +
                  src[index + p - 2] + src[index + p + 2] +
                  src[index + 2 * p - 1] + src[index + 2 * p + 1]) +
           c20 * (src[index - 2 * p] + src[index - 2] +
                  src[index + 2] + src[index + 2 * p]) +
           c11 * (src[index - p - 1] + src[index - p + 1] +
                  src[index + p - 1] + src[index + p + 1]) +
           c10 * (src[index - p] + src[index - 1] + src[index + 1] +
                  src[index + p]) +
           c00 * src[index];
}

__kernel void rt_sharpen_div5(__global const float* estimate,
                              __global float* tmp,
                              __global const float* luminance,
                              const int width,
                              const int height,
                              const int pitch,
                              const float c00,
                              const float c10,
                              const float c11,
                              const float c20,
                              const float c21)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    if (x < 2 || x >= width - 2 || y < 2 || y >= height - 2) {
        tmp[index] = 1.0f;
        return;
    }
    const float denom = rt_sharpen_blur5(estimate, index, pitch, c00, c10,
                                         c11, c20, c21);
    tmp[index] = luminance[index] /
                 (denom < RT_SHARPEN_DIV_FLOOR ? RT_SHARPEN_DIV_FLOOR : denom);
}

__kernel void rt_sharpen_mult5(__global const float* tmp,
                               __global float* estimate,
                               const int width,
                               const int height,
                               const int pitch,
                               const float c00,
                               const float c10,
                               const float c11,
                               const float c20,
                               const float c21)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    if (x < 2 || x >= width - 2 || y < 2 || y >= height - 2) {
        return;
    }
    const int index = y * pitch + x;
    estimate[index] *= rt_sharpen_blur5(tmp, index, pitch, c00, c10, c11, c20,
                                        c21);
}

__kernel void rt_sharpen_div7(__global const float* estimate,
                              __global float* tmp,
                              __global const float* luminance,
                              const int width,
                              const int height,
                              const int pitch,
                              const float c00,
                              const float c10,
                              const float c11,
                              const float c20,
                              const float c21,
                              const float c22,
                              const float c30,
                              const float c31)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    if (x < 3 || x >= width - 3 || y < 3 || y >= height - 3) {
        tmp[index] = 1.0f;
        return;
    }
    const float denom = rt_sharpen_blur7(estimate, index, pitch, c00, c10,
                                         c11, c20, c21, c22, c30, c31);
    tmp[index] = luminance[index] /
                 (denom < RT_SHARPEN_DIV_FLOOR ? RT_SHARPEN_DIV_FLOOR : denom);
}

__kernel void rt_sharpen_mult7(__global const float* tmp,
                               __global float* estimate,
                               const int width,
                               const int height,
                               const int pitch,
                               const float c00,
                               const float c10,
                               const float c11,
                               const float c20,
                               const float c21,
                               const float c22,
                               const float c30,
                               const float c31)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    if (x < 3 || x >= width - 3 || y < 3 || y >= height - 3) {
        return;
    }
    const int index = y * pitch + x;
    estimate[index] *= rt_sharpen_blur7(tmp, index, pitch, c00, c10, c11, c20,
                                        c21, c22, c30, c31);
}

/* Final intp(blend * amount, max(estimate, 0), L), in place. */
__kernel void rt_sharpen_rld_final(__global float* luminance,
                                   __global const float* estimate,
                                   __global const float* mask,
                                   const int width,
                                   const int height,
                                   const int pitch,
                                   const float amount)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float current = luminance[index];
    const float positive = estimate[index] < 0.0f ? 0.0f : estimate[index];
    const float blend = mask[index] * amount;
    luminance[index] = blend * (positive - current) + current;
}

/* A plane copy, used where gaussianBlur() skips below GAUSS_SKIP. */
__kernel void rt_sharpen_copy(__global const float* src,
                              __global float* dst,
                              const int width,
                              const int height,
                              const int src_pitch,
                              const int dst_pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    dst[y * dst_pitch + x] = src[y * src_pitch + x];
}

/*
 * The blur-radius merge of both arms:
 *   intp(blend, luminance, max(clamped, 0))
 * with `out` and `luminance` possibly the same plane.
 */
__kernel void rt_sharpen_blend_max(__global float* out,
                                   __global const float* clamped,
                                   __global const float* luminance,
                                   __global const float* mask,
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
    const float value = clamped[index] < 0.0f ? 0.0f : clamped[index];
    const float base = luminance[index];
    out[index] = mask[index] * (base - value) + value;
}

/* sleef.h ilogbp1f(), the xlogf() exponent probe. */
inline int rt_sharpen_ilogbp1f(float d)
{
    const int m = d < 5.421010862427522E-20f;
    d = m ? 1.8446744073709552E19f * d : d;
    int q = (as_int(d) >> 23) & 0xff;
    q = m ? q - (64 + 0x7e) : q - 0x7e;
    return q;
}

/* sleef.h ldexpkf(). */
inline float rt_sharpen_ldexpkf(float x, int q)
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

/* sleef.h xlogf(), the logarithm dcdamping() uses on both CPU arms. */
inline float rt_sharpen_xlogf(float d)
{
    const int e = rt_sharpen_ilogbp1f(d * 0.7071f);
    const float m = rt_sharpen_ldexpkf(d, -e);
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

/*
 * dcdamping() of the deconvolution, including its SIMD/scalar split: the
 * vector arm covers the columns below `width - width % 4` and groups the
 * polynomial and the log sum differently from the scalar tail.
 */
__kernel void rt_sharpen_damp(__global float* value,
                              __global const float* luminance,
                              const int width,
                              const int height,
                              const int pitch,
                              const float damping_fac)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float image = value[index];
    const float original = luminance[index];
    if (image <= 0.0f || original <= 0.0f) {
        value[index] = 0.0f;
        return;
    }
    const float w = original - image;
    const float log_ratio = rt_sharpen_xlogf(image / original);
    float u;
    if (x < width - (width % 4)) {
        u = (original * log_ratio + w) * damping_fac;
        u = u < 1.0f ? u : 1.0f;
        const float u2 = u * u;
        const float u4 = u2 * u2;
        u = u4 * (5.0f - u * 4.0f);
    } else {
        u = (original * log_ratio - image + original) * damping_fac;
        u = u < 1.0f ? u : 1.0f;
        u = u * u * u * u * (5.0f - u * 4.0f);
    }
    value[index] = (w / image) * u + 1.0f;
}

/*
 * sharpenHaloCtrl(): one work item per interior row, because the sliding
 * three-row maximum and minimum carry across the row.
 */
__kernel void rt_sharpen_halo(__global float* luminance,
                              __global const float* blur,
                              __global const float* base,
                              __global const float* mask,
                              const int width,
                              const int height,
                              const int pitch,
                              const float scale,
                              const float sharp_fac,
                              const int bl,
                              const int tl,
                              const int br,
                              const int tr,
                              const int is_double,
                              const int start_at_one)
{
    const int i = (int)get_global_id(0);
    if (i < 2 || i >= height - 2) {
        return;
    }
    float max1 = 0.0f;
    float max2 = 0.0f;
    float min1 = 0.0f;
    float min2 = 0.0f;
    for (int j = 2; j < width - 2; ++j) {
        const int a = (i - 2) * pitch + j;
        const int b = (i - 1) * pitch + j;
        const int c = i * pitch + j;
        const int d = (i + 1) * pitch + j;
        const int e = (i + 2) * pitch + j;
        const float np1 =
            2.0f * (base[a] + base[a + 1] + base[a + 2] +
                    base[b] + base[b + 1] + base[b + 2] +
                    base[c] + base[c + 1] + base[c + 2]) / 27.0f +
            base[b + 1] / 3.0f;
        const float np2 =
            2.0f * (base[b] + base[b + 1] + base[b + 2] +
                    base[c] + base[c + 1] + base[c + 2] +
                    base[d] + base[d + 1] + base[d + 2]) / 27.0f +
            base[c + 1] / 3.0f;
        const float np3 =
            2.0f * (base[c] + base[c + 1] + base[c + 2] +
                    base[d] + base[d + 1] + base[d + 2] +
                    base[e] + base[e + 1] + base[e + 2]) / 27.0f +
            base[d + 1] / 3.0f;

        const float maxn = np1 < np2 ? (np2 < np3 ? np3 : np2)
                                     : (np1 < np3 ? np3 : np1);
        const float minn = np1 < np2 ? (np1 < np3 ? np1 : np3)
                                     : (np2 < np3 ? np2 : np3);
        float max_all = max1 < max2 ? max2 : max1;
        max_all = max_all < maxn ? maxn : max_all;
        float min_all = min1 < min2 ? min1 : min2;
        min_all = min_all < minn ? min_all : minn;

        max1 = max2;
        max2 = maxn;
        min1 = min2;
        min2 = minn;

        const float current = luminance[c];
        if (max_all < current) {
            max_all = current;
        }
        if (min_all > current) {
            min_all = current;
        }

        const float diff = base[c] - blur[c];
        const float magnitude = fabs(diff) < RT_SHARPEN_THRESHOLD_UPPER_BOUND
                                    ? fabs(diff)
                                    : RT_SHARPEN_THRESHOLD_UPPER_BOUND;
        const float delta = rt_sharpen_threshold(
            magnitude, sharp_fac * diff, bl, tl, br, tr, is_double,
            start_at_one);
        float new_l = current + delta;

        if (new_l > max_all) {
            new_l = max_all + (new_l - max_all) * scale;
        } else if (new_l < min_all) {
            new_l = min_all - (min_all - new_l) * scale;
        }
        luminance[c] = mask[c] * (new_l - current) + current;
    }
}

/* bilateral2.h's exponential table, built once per edges-only pass. */
__kernel void rt_sharpen_bilateral_lut(__global float* lut,
                                       const int entries,
                                       const int centre,
                                       const float sens,
                                       const float scale)
{
    const int i = (int)get_global_id(0);
    if (i >= entries) {
        return;
    }
    const int offset = i - centre;
#ifdef cl_khr_fp64
    const double exponent =
        -((double)offset * (double)offset) /
        (2.0 * (double)sens * (double)sens);
    lut[i] = (float)(exp(exponent) * (double)scale);
#else
    const float exponent =
        -((float)offset * (float)offset) / (2.0f * sens * sens);
    lut[i] = exp(exponent) * scale;
#endif
}

/*
 * bilateral2.h's filter: `weights` holds the upper triangle of the symmetric
 * kernel, rows and columns ordered from the outermost offset inwards.
 */
__kernel void rt_sharpen_bilateral(__global const float* src,
                                   __global float* dst,
                                   __global const float* lut,
                                   __global const int* weights,
                                   const int width,
                                   const int height,
                                   const int pitch,
                                   const int radius,
                                   const int centre,
                                   const int entries)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    if (x < radius || x >= width - radius || y < radius ||
        y >= height - radius) {
        dst[index] = src[index];
        return;
    }
    const float self = src[index];
    float numerator = 0.0f;
    float denominator = 0.0f;
    for (int di = -radius; di <= radius; ++di) {
        const int row = radius - (di < 0 ? -di : di);
        for (int dj = -radius; dj <= radius; ++dj) {
            const int column = radius - (dj < 0 ? -dj : dj);
            const float weight = (float)weights[row * (radius + 1) + column];
            const float other = src[index + di * pitch + dj];
            const float probe = other - self + (float)centre;
            int lookup = (int)probe;
            if (probe < 0.0f) {
                lookup = 0;
            } else if (probe > (float)(entries - 1)) {
                lookup = entries - 1;
            }
            const float factor = lut[lookup];
            numerator += weight * (other * factor);
            denominator += weight * factor;
        }
    }
    dst[index] = numerator / denominator;
}
)CLC";

/*
 * The output sharpening node, shared by its call site, its host builtin, its
 * device launcher and its test.
 *
 * Argument order:
 *   [0]      one-plane F32 L buffer, read and written in place
 *   [1]      method, SharpenMethod as an int
 *   [2]      amount
 *   [3]      contrast
 *   [4]      radius
 *   [5]      blurradius
 *   [6..9]   threshold bottom-left, top-left, bottom-right, top-right
 *   [10]     threshold uses the four-value form
 *   [11]     threshold starts at one
 *   [12]     edges only
 *   [13]     edge detection radius
 *   [14]     edge tolerance
 *   [15]     halo control
 *   [16]     halo control amount
 *   [17]     deconvolution amount
 *   [18]     deconvolution radius
 *   [19]     deconvolution iterations
 *   [20]     deconvolution damping
 *   [21]     automatic deconvolution radius
 *   [22]     deconvolution corner boost
 *   [23]     deconvolution corner latitude
 *   [24]     scale
 *   [25]     show mask
 *
 * Every parameter the core reads is encoded, including the residuals, so the
 * host builtin reproduces the CPU path for the whole parameter set.
 */

namespace rtengine::stage
{

enum SharpenMethod : int {
    SHARPEN_METHOD_USM = 0,
    SHARPEN_METHOD_RLD = 1
};

enum SharpenArg : std::size_t {
    SHARPEN_ARG_BUFFER = 0,
    SHARPEN_ARG_METHOD,
    SHARPEN_ARG_AMOUNT,
    SHARPEN_ARG_CONTRAST,
    SHARPEN_ARG_RADIUS,
    SHARPEN_ARG_BLUR_RADIUS,
    SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT,
    SHARPEN_ARG_THRESHOLD_TOP_LEFT,
    SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT,
    SHARPEN_ARG_THRESHOLD_TOP_RIGHT,
    SHARPEN_ARG_THRESHOLD_IS_DOUBLE,
    SHARPEN_ARG_THRESHOLD_START_AT_ONE,
    SHARPEN_ARG_EDGES_ONLY,
    SHARPEN_ARG_EDGES_RADIUS,
    SHARPEN_ARG_EDGES_TOLERANCE,
    SHARPEN_ARG_HALO_CONTROL,
    SHARPEN_ARG_HALO_CONTROL_AMOUNT,
    SHARPEN_ARG_DECONV_AMOUNT,
    SHARPEN_ARG_DECONV_RADIUS,
    SHARPEN_ARG_DECONV_ITERATIONS,
    SHARPEN_ARG_DECONV_DAMPING,
    SHARPEN_ARG_DECONV_AUTO_RADIUS,
    SHARPEN_ARG_DECONV_CORNER_BOOST,
    SHARPEN_ARG_DECONV_CORNER_LATITUDE,
    SHARPEN_ARG_SCALE,
    SHARPEN_ARG_SHOW_MASK,
    SHARPEN_NODE_ARGS
};

/*
 * Everything the core reads, in a form both the ABI encoder and the device
 * launcher can inspect without touching the production parameter struct.
 */
struct SharpenNodeParams {
    SharpenMethod method = SHARPEN_METHOD_USM;
    int amount = 0;
    double contrast = 0.0;
    double radius = 0.0;
    double blurradius = 0.0;
    int threshold_bottom_left = 0;
    int threshold_top_left = 0;
    int threshold_bottom_right = 0;
    int threshold_top_right = 0;
    bool threshold_is_double = false;
    bool threshold_start_at_one = false;
    bool edgesonly = false;
    double edges_radius = 0.0;
    int edges_tolerance = 0;
    bool halocontrol = false;
    int halocontrol_amount = 0;
    int deconvamount = 0;
    double deconvradius = 0.0;
    int deconviter = 0;
    int deconvdamping = 0;
    bool deconvAutoRadius = false;
    double deconvCornerBoost = 0.0;
    int deconvCornerLatitude = 0;
    double scale = 1.0;
    bool showMask = false;
};

constexpr double SHARPEN_BLUR_RADIUS_LIMIT = 0.25;
/* Mirrors GAUSS_SKIP and GAUSS_3X3_LIMIT in gauss.cc. */
constexpr double SHARPEN_GAUSS_SKIP = 0.25;
constexpr double SHARPEN_FIR_LIMIT = 0.6;
constexpr double SHARPEN_SIGMA_LIMIT = 25.0;
constexpr double SHARPEN_MASK_SIGMA = 2.0;
constexpr double SHARPEN_MASK_GRADIENT_SCALE = 0.0625 / 327.68;
/* Mirrors the gaussianBlur() support-window breakpoints. */
constexpr double SHARPEN_5X5_LIMIT = 0.84;
constexpr double SHARPEN_7X7_LIMIT = 1.15;
constexpr int SHARPEN_DECONV_ITERATION_LIMIT = 1000;
/* bilateral2.h's range-weight table and its sigma buckets. */
constexpr int SHARPEN_BILATERAL_LUT_ENTRIES = 0x20000;
constexpr int SHARPEN_BILATERAL_LUT_CENTRE = 0x10000;
/* Below this sigma the host copies the plane instead of filtering it. */
constexpr double SHARPEN_BILATERAL_COPY_SIGMA = 0.45;
constexpr int SHARPEN_BILATERAL_MAX_WEIGHTS = 36;

/*
 * Coefficients of the 1-D Young-van Vliet recurrence, computed in double on
 * the host from the same formulae as calculateYvVFactors() in gauss.cc. The
 * M scaling matches the SSE implementation that ships on x86-64, because that
 * is the production CPU path the device is compared against.
 */
struct SharpenIirFactors {
    float B = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float b3 = 0.0f;
    float m[3][3] = {};
};

inline void sharpen_iir_factors(double sigma, SharpenIirFactors& out)
{
    double q;
    if (sigma < 2.5) {
        q = 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sigma);
    } else {
        q = 0.98711 * sigma - 0.96330;
    }

    const double b0 =
        1.57825 + 2.44413 * q + 1.4281 * q * q + 0.422205 * q * q * q;
    double b1 = 2.44413 * q + 2.85619 * q * q + 1.26661 * q * q * q;
    double b2 = -1.4281 * q * q - 1.26661 * q * q * q;
    double b3 = 0.422205 * q * q * q;
    const double B = 1.0 - (b1 + b2 + b3) / b0;

    b1 /= b0;
    b2 /= b0;
    b3 /= b0;

    double M[3][3];
    M[0][0] = -b3 * b1 + 1.0 - b3 * b3 - b2;
    M[0][1] = (b3 + b1) * (b2 + b3 * b1);
    M[0][2] = b3 * (b1 + b3 * b2);
    M[1][0] = b1 + b3 * b2;
    M[1][1] = -(b2 - 1.0) * (b2 + b3 * b1);
    M[1][2] = -(b3 * b1 + b3 * b3 + b2 - 1.0) * b3;
    M[2][0] = b3 * b1 + b2 + b1 * b1 - b2 * b2;
    M[2][1] = b1 * b2 + b3 * b2 * b2 - b1 * b3 * b3 - b3 * b3 * b3 -
              b3 * b2 + b3;
    M[2][2] = b3 * (b1 + b3 * b2);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            M[i][j] *= (1.0 + b2 + (b1 - b3) * b3);
            M[i][j] /=
                (1.0 + b1 - b2 + b3) * (1.0 - b1 - b2 - b3);
        }
    }

    out.B = static_cast<float>(B);
    out.b1 = static_cast<float>(b1);
    out.b2 = static_cast<float>(b2);
    out.b3 = static_cast<float>(b3);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.m[i][j] = static_cast<float>(M[i][j]);
        }
    }
}

/* The normalized 3x3 kernel gaussianBlur() uses below sigma 0.6. */
struct SharpenFirFactors {
    float c0 = 0.0f;
    float c1 = 0.0f;
    float c2 = 0.0f;
    float b0 = 0.0f;
    float b1 = 0.0f;
};

inline void sharpen_fir_factors(double sigma, SharpenFirFactors& out)
{
    const double inv_sigma = 1.0 / sigma;
    const double c = std::exp(-0.5 * (inv_sigma * inv_sigma));
    const double c2 = std::exp(-(inv_sigma * inv_sigma));
    const double sum = 1.0 + 4.0 * (c + c2);
    out.c0 = static_cast<float>(1.0 / sum);
    out.c1 = static_cast<float>(c / sum);
    out.c2 = static_cast<float>(c2 / sum);

    const double edge = std::exp(-1.0 / (2.0 * sigma * sigma));
    const double edge_sum = 2.0 * edge + 1.0;
    out.b1 = static_cast<float>(edge / edge_sum);
    out.b0 = static_cast<float>(1.0 / edge_sum);
}

/* The five named entries of compute5x5kernel(), in its float arithmetic. */
struct SharpenKernel5 {
    float c00 = 0.0f;
    float c10 = 0.0f;
    float c11 = 0.0f;
    float c20 = 0.0f;
    float c21 = 0.0f;
};

inline void sharpen_kernel5(double sigma_value, SharpenKernel5& out)
{
    const float sigma = static_cast<float>(sigma_value);
    const double temp = static_cast<double>(-2.0f * (sigma * sigma));
    const double radius2 = (3.0 * SHARPEN_5X5_LIMIT) *
                           (3.0 * SHARPEN_5X5_LIMIT);
    float kernel[5][5];
    float sum = 0.0f;
    for (int i = -2; i <= 2; ++i) {
        for (int j = -2; j <= 2; ++j) {
            if (i * i + j * j <= radius2) {
                kernel[i + 2][j + 2] =
                    static_cast<float>(std::exp((i * i + j * j) / temp));
                sum += kernel[i + 2][j + 2];
            } else {
                kernel[i + 2][j + 2] = 0.0f;
            }
        }
    }
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            kernel[i][j] /= sum;
        }
    }
    out.c21 = kernel[0][1];
    out.c20 = kernel[0][2];
    out.c11 = kernel[1][1];
    out.c10 = kernel[1][2];
    out.c00 = kernel[2][2];
}

/* The eight named entries of compute7x7kernel(), in its float arithmetic. */
struct SharpenKernel7 {
    float c00 = 0.0f;
    float c10 = 0.0f;
    float c11 = 0.0f;
    float c20 = 0.0f;
    float c21 = 0.0f;
    float c22 = 0.0f;
    float c30 = 0.0f;
    float c31 = 0.0f;
};

inline void sharpen_kernel7(double sigma_value, SharpenKernel7& out)
{
    const float sigma = static_cast<float>(sigma_value);
    const double temp = static_cast<double>(-2.0f * (sigma * sigma));
    const double radius2 = (3.0 * SHARPEN_7X7_LIMIT) *
                           (3.0 * SHARPEN_7X7_LIMIT);
    float kernel[7][7];
    float sum = 0.0f;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            if (i * i + j * j <= radius2) {
                kernel[i + 3][j + 3] =
                    static_cast<float>(std::exp((i * i + j * j) / temp));
                sum += kernel[i + 3][j + 3];
            } else {
                kernel[i + 3][j + 3] = 0.0f;
            }
        }
    }
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 7; ++j) {
            kernel[i][j] /= sum;
        }
    }
    out.c31 = kernel[0][2];
    out.c30 = kernel[0][3];
    out.c22 = kernel[1][1];
    out.c21 = kernel[1][2];
    out.c20 = kernel[1][3];
    out.c11 = kernel[2][2];
    out.c10 = kernel[2][3];
    out.c00 = kernel[3][3];
}

/*
 * bilateral2.h's per-sigma kernel coefficients, in its bucket order. Each row
 * lists the upper sigma bound, the support radius, the table scale, the
 * upper-triangle weights and their count. The final bucket has no upper bound.
 */
struct SharpenBilateralGrade {
    double upper;
    int radius;
    int scale;
    const int* weights;
    int count;
};

inline const int* sharpen_bilateral_05()
{
    static const int w[] = {1, 7, 7, 55};
    return w;
}

inline const int* sharpen_bilateral_06()
{
    static const int w[] = {1, 4, 4, 16};
    return w;
}

inline const int* sharpen_bilateral_07()
{
    static const int w[] = {0, 0, 1, 0, 8, 21, 1, 21, 59};
    return w;
}

inline const int* sharpen_bilateral_08()
{
    static const int w[] = {0, 0, 1, 0, 5, 10, 1, 10, 23};
    return w;
}

inline const int* sharpen_bilateral_09()
{
    static const int w[] = {0, 1, 2, 1, 6, 12, 2, 12, 22};
    return w;
}

inline const int* sharpen_bilateral_10()
{
    static const int w[] = {0, 1, 2, 1, 4, 7, 2, 7, 12};
    return w;
}

inline const int* sharpen_bilateral_11()
{
    static const int w[] = {0, 0, 1, 1, 0, 2, 5, 8,
                            1, 5, 18, 27, 1, 8, 27, 41};
    return w;
}

inline const int* sharpen_bilateral_12()
{
    static const int w[] = {0, 0, 1, 1, 0, 1, 4, 6,
                            1, 4, 11, 16, 1, 6, 16, 23};
    return w;
}

inline const int* sharpen_bilateral_13()
{
    static const int w[] = {0, 0, 1, 1, 0, 2, 4, 6,
                            1, 4, 11, 14, 1, 6, 14, 19};
    return w;
}

inline const int* sharpen_bilateral_14()
{
    static const int w[] = {0, 1, 2, 3, 1, 4, 8, 10,
                            2, 8, 17, 21, 3, 10, 21, 28};
    return w;
}

inline const int* sharpen_bilateral_15()
{
    static const int w[] = {0, 0, 0, 1, 1, 0, 1, 2, 4, 5, 0, 2, 6,
                            12, 14, 1, 4, 12, 22, 28, 1, 5, 14, 28, 35};
    return w;
}

inline const int* sharpen_bilateral_16()
{
    static const int w[] = {0, 0, 0, 1, 1, 0, 1, 2, 3, 4, 0, 2, 5,
                            9, 10, 1, 3, 9, 15, 19, 1, 4, 10, 19, 23};
    return w;
}

inline const int* sharpen_bilateral_17()
{
    static const int w[] = {0, 0, 1, 1, 1, 0, 1, 2, 3, 4, 1, 2, 5,
                            8, 9, 1, 3, 8, 13, 16, 1, 4, 9, 16, 19};
    return w;
}

inline const int* sharpen_bilateral_18()
{
    static const int w[] = {0, 0, 1, 2, 2, 0, 1, 3, 5, 5, 1, 3, 6,
                            10, 12, 2, 5, 10, 16, 19, 2, 5, 12, 19, 22};
    return w;
}

inline const int* sharpen_bilateral_19()
{
    static const int w[] = {0, 0, 1, 2, 2, 0, 1, 3, 4, 5, 1, 3, 5,
                            8, 9, 2, 4, 8, 12, 14, 2, 5, 9, 14, 16};
    return w;
}

inline const int* sharpen_bilateral_20()
{
    static const int w[] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 2, 3, 3, 0, 1, 2, 4,
                            7, 7, 1, 2, 4, 8, 12, 14, 1, 3, 7, 12, 18, 20,
                            1, 3, 7, 14, 20, 23};
    return w;
}

inline const int* sharpen_bilateral_21()
{
    static const int w[] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 2, 3, 3, 0, 1, 2, 4,
                            6, 7, 1, 2, 4, 8, 11, 12, 1, 3, 6, 11, 15, 17,
                            1, 3, 7, 12, 17, 19};
    return w;
}

inline const int* sharpen_bilateral_22()
{
    static const int w[] = {0, 0, 0, 1, 1, 2, 0, 1, 2, 3, 3, 4, 1, 2, 3, 5,
                            7, 8, 1, 3, 5, 9, 12, 13, 1, 3, 7, 12, 16, 18,
                            2, 4, 8, 13, 18, 20};
    return w;
}

inline const int* sharpen_bilateral_23()
{
    static const int w[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 2, 3, 3, 1, 1, 3, 5,
                            6, 7, 1, 2, 5, 7, 10, 11, 1, 3, 6, 10, 13, 14,
                            1, 3, 7, 11, 14, 16};
    return w;
}

inline const int* sharpen_bilateral_24()
{
    static const int w[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 2, 3, 3, 1, 1, 3, 4,
                            5, 6, 1, 2, 4, 6, 8, 9, 1, 3, 5, 8, 10, 11,
                            1, 3, 6, 9, 11, 12};
    return w;
}

inline const int* sharpen_bilateral_25()
{
    static const int w[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 2, 3, 3, 1, 1, 2, 4,
                            5, 5, 1, 2, 4, 5, 7, 7, 1, 3, 5, 7, 9, 9,
                            1, 3, 5, 7, 9, 10};
    return w;
}

/*
 * Select bilateral2.h's bucket for `sigma`. A radius of zero means the host
 * copies the plane; false means the request cannot be reproduced.
 */
inline bool sharpen_bilateral_grade(double sigma, int& radius, int& scale,
                                    const int*& weights, int& count)
{
    static const SharpenBilateralGrade grades[] = {
        {0.55, 1, 318, sharpen_bilateral_05(), 4},
        {0.65, 1, 768, sharpen_bilateral_06(), 4},
        {0.75, 2, 366, sharpen_bilateral_07(), 9},
        {0.85, 2, 753, sharpen_bilateral_08(), 9},
        {0.95, 2, 595, sharpen_bilateral_09(), 9},
        {1.05, 2, 910, sharpen_bilateral_10(), 9},
        {1.15, 3, 209, sharpen_bilateral_11(), 16},
        {1.25, 3, 322, sharpen_bilateral_12(), 16},
        {1.35, 3, 336, sharpen_bilateral_13(), 16},
        {1.45, 3, 195, sharpen_bilateral_14(), 16},
        {1.55, 4, 132, sharpen_bilateral_15(), 25},
        {1.65, 4, 180, sharpen_bilateral_16(), 25},
        {1.75, 4, 195, sharpen_bilateral_17(), 25},
        {1.85, 4, 151, sharpen_bilateral_18(), 25},
        {1.95, 4, 151, sharpen_bilateral_19(), 25},
        {2.05, 5, 116, sharpen_bilateral_20(), 36},
        {2.15, 5, 127, sharpen_bilateral_21(), 36},
        {2.25, 5, 109, sharpen_bilateral_22(), 36},
        {2.35, 5, 132, sharpen_bilateral_23(), 36},
        {2.45, 5, 156, sharpen_bilateral_24(), 36},
    };
    if (sigma < SHARPEN_BILATERAL_COPY_SIGMA) {
        radius = 0;
        scale = 1;
        weights = nullptr;
        count = 0;
        return true;
    }
    for (const SharpenBilateralGrade& grade : grades) {
        if (sigma < grade.upper) {
            radius = grade.radius;
            scale = grade.scale;
            weights = grade.weights;
            count = grade.count;
            return true;
        }
    }
    radius = 5;
    scale = 173;
    weights = sharpen_bilateral_25();
    count = 36;
    return true;
}

/*
 * The device accepts the USM path and the RLD path with the kernels this
 * launcher implements, including the residual switches. Anything the kernels
 * do not reproduce returns false with a reason, so the host builtin runs and
 * the record says host.
 */
inline bool sharpen_device_supported(const SharpenNodeParams& p,
                                     std::string& error)
{
    if (!(p.scale > 0.0) || !std::isfinite(p.scale)) {
        error = "output.sharpen: invalid scale";
        return false;
    }
    /*
     * The show-mask output is the mask itself, and buildBlendMask()'s SIMD
     * arm evaluates the sigmoid with _mm_rsqrt_ps(). That approximation is
     * not reproducible by an exact kernel to the mask's full scale, so the
     * arm stays on the host.
     */
    if (p.showMask) {
        error = "output.sharpen: show mask needs the host's approximate "
                "reciprocal square root";
        return false;
    }
    if (p.blurradius >= SHARPEN_SIGMA_LIMIT) {
        error = "output.sharpen: the blur radius uses the double Gaussian";
        return false;
    }

    if (p.method == SHARPEN_METHOD_USM) {
        if (!std::isfinite(p.radius) || p.radius < 0.0) {
            error = "output.sharpen: invalid radius";
            return false;
        }
        const double sigma = p.radius / p.scale;
        if (!(sigma < SHARPEN_SIGMA_LIMIT)) {
            error = "output.sharpen: sigma is outside the device range";
            return false;
        }
        if (p.edgesonly) {
            if (!std::isfinite(p.edges_radius)) {
                error = "output.sharpen: invalid edge radius";
                return false;
            }
            /*
             * A non-positive tolerance makes the host's range-weight table
             * NaN at the centre sample, so the whole plane is NaN. There is
             * no meaningful result to reproduce.
             */
            if (p.edges_tolerance <= 0) {
                error = "output.sharpen: a non-positive edge tolerance is a "
                        "host residual";
                return false;
            }
            int radius = 0;
            int scale = 0;
            const int* weights = nullptr;
            int count = 0;
            if (!sharpen_bilateral_grade(p.edges_radius / p.scale, radius,
                                         scale, weights, count)) {
                error = "output.sharpen: unsupported edge radius";
                return false;
            }
            assert(radius == 0 || count <= SHARPEN_BILATERAL_MAX_WEIGHTS);
        }
        return true;
    }

    if (p.method == SHARPEN_METHOD_RLD) {
        if (p.deconviter < 0 ||
            p.deconviter > SHARPEN_DECONV_ITERATION_LIMIT) {
            error = "output.sharpen: deconvolution iteration count is outside "
                    "the device range";
            return false;
        }
        if (!std::isfinite(p.deconvradius) || p.deconvradius < 0.0) {
            error = "output.sharpen: invalid deconvolution radius";
            return false;
        }
        const double sigma = p.deconvradius / p.scale;
        if (!(sigma < SHARPEN_7X7_LIMIT)) {
            error = "output.sharpen: separable deconvolution is a host "
                    "residual";
            return false;
        }
        return true;
    }

    error = "output.sharpen: unknown method";
    return false;
}

/* The node parameters a profile selects. Rejects a method outside the enum. */
inline bool sharpen_node_params(const procparams::SharpeningParams& p,
                                double scale, bool showMask,
                                SharpenNodeParams& out, std::string& error)
{
    if (p.method == "usm") {
        out.method = SHARPEN_METHOD_USM;
    } else if (p.method == "rld") {
        out.method = SHARPEN_METHOD_RLD;
    } else {
        error = "sharpen: unknown method '" + p.method + "'";
        return false;
    }

    out.amount = p.amount;
    out.contrast = p.contrast;
    out.radius = p.radius;
    out.blurradius = p.blurradius;
    out.threshold_bottom_left = p.threshold.getBottomLeft();
    out.threshold_top_left = p.threshold.getTopLeft();
    out.threshold_bottom_right = p.threshold.getBottomRight();
    out.threshold_top_right = p.threshold.getTopRight();
    out.threshold_is_double = p.threshold.isDouble();
    /*
     * Threshold keeps its start-at-one flag private and exposes no getter.
     * SharpeningParams constructs it with false and setValues() never changes
     * it, so every profile the editor writes uses the start-at-zero form. The
     * field is still encoded so the ABI is complete for reconstructed values.
     */
    out.threshold_start_at_one = false;
    out.edgesonly = p.edgesonly;
    out.edges_radius = p.edges_radius;
    out.edges_tolerance = p.edges_tolerance;
    out.halocontrol = p.halocontrol;
    out.halocontrol_amount = p.halocontrol_amount;
    out.deconvamount = p.deconvamount;
    out.deconvradius = p.deconvradius;
    out.deconviter = p.deconviter;
    out.deconvdamping = p.deconvdamping;
    out.deconvAutoRadius = p.deconvAutoRadius;
    out.deconvCornerBoost = p.deconvCornerBoost;
    out.deconvCornerLatitude = p.deconvCornerLatitude;
    out.scale = scale;
    out.showMask = showMask;
    return true;
}

/* Copy the encoded parameters back into the production parameter struct. */
inline void sharpen_apply_params(const SharpenNodeParams& node,
                                 procparams::SharpeningParams& p)
{
    p.enabled = true;
    p.method = node.method == SHARPEN_METHOD_RLD ? "rld" : "usm";
    p.amount = node.amount;
    p.contrast = node.contrast;
    p.radius = node.radius;
    p.blurradius = node.blurradius;
    if (node.threshold_is_double) {
        p.threshold = procparams::Threshold<int>(
            node.threshold_bottom_left, node.threshold_top_left,
            node.threshold_bottom_right, node.threshold_top_right,
            node.threshold_start_at_one);
    } else {
        p.threshold = procparams::Threshold<int>(
            node.threshold_bottom_left, node.threshold_top_left,
            node.threshold_start_at_one);
    }
    p.edgesonly = node.edgesonly;
    p.edges_radius = node.edges_radius;
    p.edges_tolerance = node.edges_tolerance;
    p.halocontrol = node.halocontrol;
    p.halocontrol_amount = node.halocontrol_amount;
    p.deconvamount = node.deconvamount;
    p.deconvradius = node.deconvradius;
    p.deconviter = node.deconviter;
    p.deconvdamping = node.deconvdamping;
    p.deconvAutoRadius = node.deconvAutoRadius;
    p.deconvCornerBoost = node.deconvCornerBoost;
    p.deconvCornerLatitude = node.deconvCornerLatitude;
}

inline RtStageBuffer sharpen_l_buffer(float* L, int width, int height)
{
    RtStageBuffer buffer{};
    buffer.size = sizeof(RtStageBuffer);
    buffer.data = L;
    buffer.width = width;
    buffer.height = height;
    buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
    buffer.plane_pitch = buffer.row_pitch * static_cast<std::size_t>(height);
    buffer.planes = 1;
    buffer.format = RT_STAGE_F32;
    buffer.flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;
    return buffer;
}

/*
 * The argument descriptors point into this struct for the scalar values, but
 * the scalars are copied into the union, so only the array itself must stay
 * alive until the node returns.
 */
struct SharpenNodeArgs {
    RtStageArg args[SHARPEN_NODE_ARGS];
};

inline void sharpen_node_args(float* L, int width, int height,
                              const SharpenNodeParams& p, SharpenNodeArgs& out)
{
    out.args[SHARPEN_ARG_BUFFER].kind = RT_STAGE_ARG_BUFFER;
    out.args[SHARPEN_ARG_BUFFER].u.buffer = sharpen_l_buffer(L, width, height);

    const auto set_int = [&out](SharpenArg index, std::int64_t value) {
        out.args[index].kind = RT_STAGE_ARG_INT;
        out.args[index].u.i = value;
    };
    const auto set_double = [&out](SharpenArg index, double value) {
        out.args[index].kind = RT_STAGE_ARG_DOUBLE;
        out.args[index].u.f = value;
    };

    set_int(SHARPEN_ARG_METHOD, static_cast<std::int64_t>(p.method));
    set_int(SHARPEN_ARG_AMOUNT, p.amount);
    set_double(SHARPEN_ARG_CONTRAST, p.contrast);
    set_double(SHARPEN_ARG_RADIUS, p.radius);
    set_double(SHARPEN_ARG_BLUR_RADIUS, p.blurradius);
    set_int(SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT, p.threshold_bottom_left);
    set_int(SHARPEN_ARG_THRESHOLD_TOP_LEFT, p.threshold_top_left);
    set_int(SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT, p.threshold_bottom_right);
    set_int(SHARPEN_ARG_THRESHOLD_TOP_RIGHT, p.threshold_top_right);
    set_int(SHARPEN_ARG_THRESHOLD_IS_DOUBLE, p.threshold_is_double ? 1 : 0);
    set_int(SHARPEN_ARG_THRESHOLD_START_AT_ONE,
            p.threshold_start_at_one ? 1 : 0);
    set_int(SHARPEN_ARG_EDGES_ONLY, p.edgesonly ? 1 : 0);
    set_double(SHARPEN_ARG_EDGES_RADIUS, p.edges_radius);
    set_int(SHARPEN_ARG_EDGES_TOLERANCE, p.edges_tolerance);
    set_int(SHARPEN_ARG_HALO_CONTROL, p.halocontrol ? 1 : 0);
    set_int(SHARPEN_ARG_HALO_CONTROL_AMOUNT, p.halocontrol_amount);
    set_int(SHARPEN_ARG_DECONV_AMOUNT, p.deconvamount);
    set_double(SHARPEN_ARG_DECONV_RADIUS, p.deconvradius);
    set_int(SHARPEN_ARG_DECONV_ITERATIONS, p.deconviter);
    set_int(SHARPEN_ARG_DECONV_DAMPING, p.deconvdamping);
    set_int(SHARPEN_ARG_DECONV_AUTO_RADIUS, p.deconvAutoRadius ? 1 : 0);
    set_double(SHARPEN_ARG_DECONV_CORNER_BOOST, p.deconvCornerBoost);
    set_int(SHARPEN_ARG_DECONV_CORNER_LATITUDE, p.deconvCornerLatitude);
    set_double(SHARPEN_ARG_SCALE, p.scale);
    set_int(SHARPEN_ARG_SHOW_MASK, p.showMask ? 1 : 0);
}

/* Validate the buffer, the argument kinds and the method enum. */
inline bool sharpen_node_decode(const RtStageArg* args, std::uint32_t nargs,
                                SharpenNodeParams& out, RtStageBuffer& buffer,
                                std::string& error)
{
    if (args == nullptr || nargs != SHARPEN_NODE_ARGS) {
        error = "sharpen expects " + std::to_string(SHARPEN_NODE_ARGS) +
                " arguments";
        return false;
    }

    const auto is_int = [args](SharpenArg index) {
        return args[index].kind == RT_STAGE_ARG_INT;
    };
    const auto is_double = [args](SharpenArg index) {
        return args[index].kind == RT_STAGE_ARG_DOUBLE;
    };

    if (args[SHARPEN_ARG_BUFFER].kind != RT_STAGE_ARG_BUFFER) {
        error = "sharpen expects an L buffer first";
        return false;
    }
    buffer = args[SHARPEN_ARG_BUFFER].u.buffer;
    if (buffer.size != sizeof(RtStageBuffer) || buffer.data == nullptr ||
        buffer.format != RT_STAGE_F32 || buffer.planes != 1 ||
        buffer.width <= 0 || buffer.height <= 0) {
        error = "sharpen needs a tightly packed one-plane F32 L buffer";
        return false;
    }
    /*
     * The extracted CPU core addresses the plane with a W-sample stride and
     * the host builtin passes its raw pointer, so the host cannot honour a
     * padded row or plane. The in-tree producer hands over a tight LabImage
     * plane. Requiring exact pitches keeps the host and device interpretations
     * identical; a padded descriptor is rejected rather than silently misread
     * on the host.
     */
    const std::size_t tight_row =
        static_cast<std::size_t>(buffer.width) * sizeof(float);
    if (tight_row >
        std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(buffer.height)) {
        error = "sharpen plane size overflows";
        return false;
    }
    const std::size_t tight_plane =
        tight_row * static_cast<std::size_t>(buffer.height);
    if (buffer.row_pitch != tight_row || buffer.plane_pitch != tight_plane) {
        error = "sharpen needs a tightly packed one-plane F32 L buffer";
        return false;
    }

    const SharpenArg intArgs[] = {
        SHARPEN_ARG_METHOD,
        SHARPEN_ARG_AMOUNT,
        SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT,
        SHARPEN_ARG_THRESHOLD_TOP_LEFT,
        SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT,
        SHARPEN_ARG_THRESHOLD_TOP_RIGHT,
        SHARPEN_ARG_THRESHOLD_IS_DOUBLE,
        SHARPEN_ARG_THRESHOLD_START_AT_ONE,
        SHARPEN_ARG_EDGES_ONLY,
        SHARPEN_ARG_EDGES_TOLERANCE,
        SHARPEN_ARG_HALO_CONTROL,
        SHARPEN_ARG_HALO_CONTROL_AMOUNT,
        SHARPEN_ARG_DECONV_AMOUNT,
        SHARPEN_ARG_DECONV_ITERATIONS,
        SHARPEN_ARG_DECONV_DAMPING,
        SHARPEN_ARG_DECONV_AUTO_RADIUS,
        SHARPEN_ARG_DECONV_CORNER_LATITUDE,
        SHARPEN_ARG_SHOW_MASK};
    for (SharpenArg index : intArgs) {
        if (!is_int(index)) {
            error = "sharpen argument " + std::to_string(index) +
                    " is not an int";
            return false;
        }
    }

    const SharpenArg doubleArgs[] = {
        SHARPEN_ARG_CONTRAST, SHARPEN_ARG_RADIUS,
        SHARPEN_ARG_BLUR_RADIUS, SHARPEN_ARG_EDGES_RADIUS,
        SHARPEN_ARG_DECONV_RADIUS, SHARPEN_ARG_DECONV_CORNER_BOOST,
        SHARPEN_ARG_SCALE};
    for (SharpenArg index : doubleArgs) {
        if (!is_double(index)) {
            error = "sharpen argument " + std::to_string(index) +
                    " is not a double";
            return false;
        }
    }

    const std::int64_t method = args[SHARPEN_ARG_METHOD].u.i;
    if (method != SHARPEN_METHOD_USM && method != SHARPEN_METHOD_RLD) {
        error = "sharpen method must be usm or rld";
        return false;
    }
    out.method = static_cast<SharpenMethod>(method);

    const auto flag = [args](SharpenArg index) {
        return args[index].u.i != 0;
    };
    const auto flag_value = [args](SharpenArg index) {
        return args[index].u.i;
    };

    out.amount = static_cast<int>(args[SHARPEN_ARG_AMOUNT].u.i);
    out.contrast = args[SHARPEN_ARG_CONTRAST].u.f;
    out.radius = args[SHARPEN_ARG_RADIUS].u.f;
    out.blurradius = args[SHARPEN_ARG_BLUR_RADIUS].u.f;
    out.threshold_bottom_left =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT].u.i);
    out.threshold_top_left =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_TOP_LEFT].u.i);
    out.threshold_bottom_right =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT].u.i);
    out.threshold_top_right =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_TOP_RIGHT].u.i);
    out.threshold_is_double = flag(SHARPEN_ARG_THRESHOLD_IS_DOUBLE);
    out.threshold_start_at_one = flag(SHARPEN_ARG_THRESHOLD_START_AT_ONE);
    out.edgesonly = flag(SHARPEN_ARG_EDGES_ONLY);
    out.edges_radius = args[SHARPEN_ARG_EDGES_RADIUS].u.f;
    out.edges_tolerance =
        static_cast<int>(args[SHARPEN_ARG_EDGES_TOLERANCE].u.i);
    out.halocontrol = flag(SHARPEN_ARG_HALO_CONTROL);
    out.halocontrol_amount =
        static_cast<int>(args[SHARPEN_ARG_HALO_CONTROL_AMOUNT].u.i);
    out.deconvamount = static_cast<int>(args[SHARPEN_ARG_DECONV_AMOUNT].u.i);
    out.deconvradius = args[SHARPEN_ARG_DECONV_RADIUS].u.f;
    out.deconviter =
        static_cast<int>(args[SHARPEN_ARG_DECONV_ITERATIONS].u.i);
    out.deconvdamping =
        static_cast<int>(args[SHARPEN_ARG_DECONV_DAMPING].u.i);
    out.deconvAutoRadius = flag(SHARPEN_ARG_DECONV_AUTO_RADIUS);
    out.deconvCornerBoost = args[SHARPEN_ARG_DECONV_CORNER_BOOST].u.f;
    out.deconvCornerLatitude =
        static_cast<int>(args[SHARPEN_ARG_DECONV_CORNER_LATITUDE].u.i);
    out.scale = args[SHARPEN_ARG_SCALE].u.f;
    out.showMask = flag(SHARPEN_ARG_SHOW_MASK);

    /*
     * The boolean slots are ints on the wire. Keep the decoder strict so a
     * malformed descriptor cannot smuggle an ambiguous flag into the core.
     */
    const SharpenArg flagArgs[] = {
        SHARPEN_ARG_THRESHOLD_IS_DOUBLE,
        SHARPEN_ARG_THRESHOLD_START_AT_ONE,
        SHARPEN_ARG_EDGES_ONLY,
        SHARPEN_ARG_HALO_CONTROL,
        SHARPEN_ARG_DECONV_AUTO_RADIUS,
        SHARPEN_ARG_SHOW_MASK};
    for (SharpenArg index : flagArgs) {
        const std::int64_t value = flag_value(index);
        if (value != 0 && value != 1) {
            error = "sharpen flag " + std::to_string(index) +
                    " is not 0 or 1";
            return false;
        }
    }

    return true;
}

} // namespace rtengine::stage
