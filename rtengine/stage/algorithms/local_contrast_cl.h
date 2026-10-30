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
 * Device arithmetic for ImProcFunctions::localContrast().
 *
 * The block blurs the L plane with gaussianBlur() and blends the difference
 * back:
 *
 *   bufval = (L - blur) * amount;
 *   if (darkness != 1 || lightness != 1)
 *       bufval *= (bufval > 0) ? lightness : darkness;
 *   L = LIM(L + bufval, 0.0001, 32767);
 *
 * gaussianBlur() runs a separable Young-van Vliet IIR filter. Above
 * GAUSS_DOUBLE (sigma 25) the host takes the double-precision path
 * (gaussHorizontal<double>() and gaussVertical<double>()): every recurrence
 * step is double and only the finished line narrows to float. Below that
 * limit the host takes the SSE float path (gaussHorizontalSse() and
 * gaussVerticalSse()): rows in groups of four and columns in groups of eight
 * run the recurrence in single precision with float coefficients, and the
 * remaining tail lines run the same recurrence in double with the same
 * coefficients and narrow every store. Both paths must be reproduced exactly,
 * so this program carries both and the host exports both coefficient sets
 * through RtLabToolsIir.
 *
 * The double path cannot keep its forward state in private memory: a line is
 * as long as the image is wide, and the boundary correction and the backward
 * recurrence read the whole forward state. The kernel therefore writes the
 * forward state to a device double buffer and reads it back in the same work
 * item, which is the host's `double temp2[]` array. The float path keeps its
 * state in the destination plane, as the host keeps it in its float `tmp[]`.
 *
 * The source is self-contained; the host prepends the fp64 extension.
 */
inline constexpr const char* RT_LOCAL_CONTRAST_CL = R"CLC(
/* ImProcFunctions::localContrast()'s LIM() bounds. */
#define RT_LC_LOW 0.0001f
#define RT_LC_HIGH 32767.0f

/*
 * One 1-D IIR line.
 *
 * Horizontal lines advance by one sample and start `src_base_step` samples
 * apart; vertical lines advance by `src_stride` and start one sample apart.
 * `src_base`/`dst_base` offset the whole pass, which is how a plane of a
 * three-plane ROI buffer is addressed without a sub-buffer.
 * `mode` 1 is the double path; `mode` 0 is the SSE path, where the first
 * `vector_lines` lines take the float lane recurrence and the rest take the
 * double recurrence with float stores. `state` is the double forward-state
 * buffer, line-major, used by the double path only.
 */
__kernel void rt_lab_tools_lc_line(__global const float* src,
                                   __global float* dst,
                                   __global double* state,
                                   const int length,
                                   const int lines,
                                   const int src_base_step,
                                   const int dst_base_step,
                                   const int src_stride,
                                   const int dst_stride,
                                   const int src_base,
                                   const int dst_base,
                                   const int mode,
                                   const int vector_lines,
                                   const float lane_B,
                                   const float lane_b1,
                                   const float lane_b2,
                                   const float lane_b3,
                                   const float lane_m00,
                                   const float lane_m01,
                                   const float lane_m02,
                                   const float lane_m10,
                                   const float lane_m11,
                                   const float lane_m12,
                                   const float lane_m20,
                                   const float lane_m21,
                                   const float lane_m22,
                                   const double border_B,
                                   const double border_b1,
                                   const double border_b2,
                                   const double border_b3,
                                   const double border_m00,
                                   const double border_m01,
                                   const double border_m02,
                                   const double border_m10,
                                   const double border_m11,
                                   const double border_m12,
                                   const double border_m20,
                                   const double border_m21,
                                   const double border_m22,
                                   const double wide_B,
                                   const double wide_b1,
                                   const double wide_b2,
                                   const double wide_b3,
                                   const double wide_m00,
                                   const double wide_m01,
                                   const double wide_m02,
                                   const double wide_m10,
                                   const double wide_m11,
                                   const double wide_m12,
                                   const double wide_m20,
                                   const double wide_m21,
                                   const double wide_m22)
{
    const int line = (int)get_global_id(0);
    if (line >= lines || length < 4) {
        return;
    }

    const int sb = src_base + line * src_base_step;
    const int db = dst_base + line * dst_base_step;
    const int last = length - 1;

    if (mode != 0) {
        /* gaussHorizontal<double>()/gaussVertical<double>(). The forward and
         * backward states stay double; only the finished line narrows. */
        const int st = line * length;
        const double s0 = src[sb];
        state[st] = wide_B * s0 + wide_b1 * s0 + wide_b2 * s0 + wide_b3 * s0;
        const double s1 = src[sb + src_stride];
        state[st + 1] = wide_B * s1 + wide_b1 * state[st] +
                        wide_b2 * s0 + wide_b3 * s0;
        const double s2 = src[sb + 2 * src_stride];
        state[st + 2] = wide_B * s2 + wide_b1 * state[st + 1] +
                        wide_b2 * state[st] + wide_b3 * s0;
        for (int j = 3; j < length; ++j) {
            state[st + j] =
                wide_B * src[sb + j * src_stride] +
                wide_b1 * state[st + j - 1] +
                wide_b2 * state[st + j - 2] +
                wide_b3 * state[st + j - 3];
        }

        const double sW = src[sb + last * src_stride];
        const double f1 = state[st + last];
        const double f2 = state[st + last - 1];
        const double f3 = state[st + last - 2];
        const double m0 = sW + wide_m00 * (f1 - sW) + wide_m01 * (f2 - sW) +
                          wide_m02 * (f3 - sW);
        const double m1 = sW + wide_m10 * (f1 - sW) + wide_m11 * (f2 - sW) +
                          wide_m12 * (f3 - sW);
        const double m2 = sW + wide_m20 * (f1 - sW) + wide_m21 * (f2 - sW) +
                          wide_m22 * (f3 - sW);
        state[st + last] = m0;
        state[st + last - 1] = wide_B * f2 + wide_b1 * state[st + last] +
                               wide_b2 * m1 + wide_b3 * m2;
        state[st + last - 2] = wide_B * f3 +
                               wide_b1 * state[st + last - 1] +
                               wide_b2 * state[st + last] + wide_b3 * m1;
        for (int j = last - 3; j >= 0; --j) {
            state[st + j] = wide_B * state[st + j] +
                            wide_b1 * state[st + j + 1] +
                            wide_b2 * state[st + j + 2] +
                            wide_b3 * state[st + j + 3];
        }
        for (int j = 0; j < length; ++j) {
            dst[db + j * dst_stride] = (float)state[st + j];
        }
        return;
    }

    if (line < vector_lines) {
        /* One SSE lane: gaussHorizontalSse()/gaussVerticalSse()'s float
         * recurrence with the float coefficients the host narrows. */
        const float s0 = src[sb];
        const float t0 = s0 * (lane_B + lane_b1 + lane_b2 + lane_b3);
        dst[db] = t0;
        const float s1 = src[sb + src_stride];
        const float t1 = lane_B * s1 + lane_b1 * t0 + s0 * (lane_b2 + lane_b3);
        dst[db + dst_stride] = t1;
        const float s2 = src[sb + 2 * src_stride];
        const float t2 = lane_B * s2 + lane_b1 * t1 + lane_b2 * t0 +
                         lane_b3 * s0;
        dst[db + 2 * dst_stride] = t2;
        for (int j = 3; j < length; ++j) {
            dst[db + j * dst_stride] =
                lane_B * src[sb + j * src_stride] +
                lane_b1 * dst[db + (j - 1) * dst_stride] +
                lane_b2 * dst[db + (j - 2) * dst_stride] +
                lane_b3 * dst[db + (j - 3) * dst_stride];
        }

        const float sW = src[sb + last * src_stride];
        const float f1 = dst[db + last * dst_stride];
        const float f2 = dst[db + (last - 1) * dst_stride];
        const float f3 = dst[db + (last - 2) * dst_stride];
        const float m0 = sW + lane_m00 * (f1 - sW) + lane_m01 * (f2 - sW) +
                         lane_m02 * (f3 - sW);
        const float m1 = sW + lane_m10 * (f1 - sW) + lane_m11 * (f2 - sW) +
                         lane_m12 * (f3 - sW);
        const float m2 = sW + lane_m20 * (f1 - sW) + lane_m21 * (f2 - sW) +
                         lane_m22 * (f3 - sW);
        dst[db + last * dst_stride] = m0;
        dst[db + (last - 1) * dst_stride] =
            lane_B * f2 + lane_b1 * dst[db + last * dst_stride] +
            lane_b2 * m1 + lane_b3 * m2;
        dst[db + (last - 2) * dst_stride] =
            lane_B * f3 + lane_b1 * dst[db + (last - 1) * dst_stride] +
            lane_b2 * dst[db + last * dst_stride] + lane_b3 * m1;
        for (int j = last - 3; j >= 0; --j) {
            dst[db + j * dst_stride] =
                lane_B * dst[db + j * dst_stride] +
                lane_b1 * dst[db + (j + 1) * dst_stride] +
                lane_b2 * dst[db + (j + 2) * dst_stride] +
                lane_b3 * dst[db + (j + 3) * dst_stride];
        }
        return;
    }

    /* The SSE path's non-vectorised remainder: the recurrence is evaluated in
     * double and every store narrows to float, exactly as the host's tail
     * loop does. */
    const double b0 = src[sb];
    dst[db] = (float)(b0 * (border_B + border_b1 + border_b2 + border_b3));
    const double b1 = src[sb + src_stride];
    dst[db + dst_stride] =
        (float)(border_B * b1 + border_b1 * dst[db] +
                b0 * (border_b2 + border_b3));
    const double b2 = src[sb + 2 * src_stride];
    dst[db + 2 * dst_stride] =
        (float)(border_B * b2 + border_b1 * dst[db + dst_stride] +
                border_b2 * dst[db] + border_b3 * b0);
    for (int j = 3; j < length; ++j) {
        dst[db + j * dst_stride] =
            (float)(border_B * src[sb + j * src_stride] +
                    border_b1 * dst[db + (j - 1) * dst_stride] +
                    border_b2 * dst[db + (j - 2) * dst_stride] +
                    border_b3 * dst[db + (j - 3) * dst_stride]);
    }

    /* The host's tail lines keep the source and the forward state in float, so
     * each deviation is a float subtraction before the double boundary matrix
     * multiplies it. Keeping this subtraction in double would move the tail. */
    const float bW = src[sb + last * src_stride];
    const float g1 = dst[db + last * dst_stride];
    const float g2 = dst[db + (last - 1) * dst_stride];
    const float g3 = dst[db + (last - 2) * dst_stride];
    const float dev1 = g1 - bW;
    const float dev2 = g2 - bW;
    const float dev3 = g3 - bW;
    const float n0 = (float)((double)bW + border_m00 * (double)dev1 +
                             border_m01 * (double)dev2 +
                             border_m02 * (double)dev3);
    const float n1 = (float)((double)bW + border_m10 * (double)dev1 +
                             border_m11 * (double)dev2 +
                             border_m12 * (double)dev3);
    const float n2 = (float)((double)bW + border_m20 * (double)dev1 +
                             border_m21 * (double)dev2 +
                             border_m22 * (double)dev3);
    dst[db + last * dst_stride] = n0;
    dst[db + (last - 1) * dst_stride] =
        (float)(border_B * (double)g2 +
                border_b1 * (double)dst[db + last * dst_stride] +
                border_b2 * (double)n1 + border_b3 * (double)n2);
    dst[db + (last - 2) * dst_stride] =
        (float)(border_B * (double)g3 +
                border_b1 * (double)dst[db + (last - 1) * dst_stride] +
                border_b2 * (double)dst[db + last * dst_stride] +
                border_b3 * (double)n1);
    for (int j = last - 3; j >= 0; --j) {
        dst[db + j * dst_stride] =
            (float)(border_B * (double)dst[db + j * dst_stride] +
                    border_b1 * (double)dst[db + (j + 1) * dst_stride] +
                    border_b2 * (double)dst[db + (j + 2) * dst_stride] +
                    border_b3 * (double)dst[db + (j + 3) * dst_stride]);
    }
}

/*
 * The blend: ImProcFunctions::localContrast()'s per-pixel step. `lab` is the
 * L plane, read and written in place; `blur` is the finished Gaussian.
 */
__kernel void rt_lab_tools_lc_blend(__global float* lab,
                                    __global const float* blur,
                                    const int width, const int height,
                                    const int lab_pitch, const int lab_plane,
                                    const int blur_pitch,
                                    const float amount, const float darkness,
                                    const float lightness)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float l = lab[offset];
    float bufval = (l - blur[y * blur_pitch + x]) * amount;
    if (darkness != 1.0f || lightness != 1.0f) {
        bufval *= (bufval > 0.0f) ? lightness : darkness;
    }
    lab[offset] = fmax(RT_LC_LOW, fmin(l + bufval, RT_LC_HIGH));
}
)CLC";
