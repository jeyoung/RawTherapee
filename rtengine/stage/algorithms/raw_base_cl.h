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
 * Two raw.base device sub-steps share one program source.
 *
 * The matrix kernel is the camera-to-working-space multiply in
 * RawImageSource::colorSpaceConversion_() when neither an input ICC profile
 * nor a DCP profile is supplied. The host computes the 3x3 matrix itself:
 * ICCStore's working-space inverse multiplied by the camera matrix, in double
 * precision. The result is passed unchanged, and the kernel repeats the host
 * loop's arithmetic exactly: every product is the product of a double matrix
 * entry and a float pixel promoted to double, the additions are
 * left-associative doubles, and the three results are rounded to float only
 * when they are written back. The kernel therefore needs an fp64 device and
 * refuses one without it.
 *
 * The white-balance kernel is the `rtot *= rm` (and optional CLIP) step of
 * RawImageSource::getImage(). The host derives rm, gm and bm as floats and
 * passes them unchanged; each kernel product is therefore the host's float
 * times float, rounded once, and the clamp repeats CLIP()'s two ordered
 * ternaries rather than fmin/fmax so a NaN becomes zero as it does on the
 * host.
 *
 * One work item owns one row. The image is a three-plane F32 buffer read and
 * written in place; the host already owns the three planes, so the node reads
 * them back into the same memory. Every other colorSpaceConversion_() arm --
 * an input ICC profile, a DCP profile, the LUT and Lab paths -- stays on the
 * host and the caller's loop runs untouched.
 *
 * Argument layout (raw.base, matrix mode):
 *   [0] F32, three planes, read/write: the image
 *   [1..9] DOUBLE: the row-major camera-to-working-space matrix m00..m22
 *
 * Argument layout (raw.base.wb):
 *   [0] F32, three planes, read/write: the image
 *   [1] INT: 1 when the caller clamps to [0, 65535] after the multiply
 *   [2..4] DOUBLE: rm, gm and bm, each exactly a host float
 *
 * A namespace-scope constant rather than a macro: GCC 12, the portable
 * package baseline, rejects a multi-line raw string inside a #define.
 */
inline constexpr const char* RT_RAW_BASE_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

__kernel void rt_raw_base_matrix(
    __global float* image,
    const double m00,
    const double m01,
    const double m02,
    const double m10,
    const double m11,
    const double m12,
    const double m20,
    const double m21,
    const double m22,
    const int width,
    const int height,
    const int pitch,
    const int plane_pitch)
{
    const int row = get_global_id(0);
    if (row >= height) {
        return;
    }

    const long offset = (long)row * pitch;
    __global float* red = image + offset;
    __global float* green = red + plane_pitch;
    __global float* blue = green + plane_pitch;

    for (int col = 0; col < width; ++col) {
        const double r = (double)red[col];
        const double g = (double)green[col];
        const double b = (double)blue[col];

        double nr = m00 * r;
        nr = nr + m01 * g;
        nr = nr + m02 * b;
        double ng = m10 * r;
        ng = ng + m11 * g;
        ng = ng + m12 * b;
        double nb = m20 * r;
        nb = nb + m21 * g;
        nb = nb + m22 * b;

        red[col] = (float)nr;
        green[col] = (float)ng;
        blue[col] = (float)nb;
    }
}

__kernel void rt_raw_base_wb(
    __global float* image,
    const int clip,
    const float rm,
    const float gm,
    const float bm,
    const int width,
    const int height,
    const int pitch,
    const int plane_pitch)
{
    const int row = get_global_id(0);
    if (row >= height) {
        return;
    }

    const long offset = (long)row * pitch;
    __global float* red = image + offset;
    __global float* green = red + plane_pitch;
    __global float* blue = green + plane_pitch;

    /* The host's coefficients are floats; the ABI carries them as doubles,
     * so the launcher narrows them back to the exact host value. */

    for (int col = 0; col < width; ++col) {
        float r = red[col] * rm;
        float g = green[col] * gm;
        float b = blue[col] * bm;

        if (clip != 0) {
            /* CLIP() is max(0, min(value, 65535)) built from ordered
             * ternaries, so a NaN becomes zero rather than 65535. */
            r = (65535.0f < r) ? 65535.0f : r;
            g = (65535.0f < g) ? 65535.0f : g;
            b = (65535.0f < b) ? 65535.0f : b;
            r = (0.0f < r) ? r : 0.0f;
            g = (0.0f < g) ? g : 0.0f;
            b = (0.0f < b) ? b : 0.0f;
        }

        red[col] = r;
        green[col] = g;
        blue[col] = b;
    }
}
)CLC";

/* Argument indices for the node. */
inline constexpr int RAW_BASE_ARG_IMAGE = 0;
inline constexpr int RAW_BASE_ARG_MATRIX = 1;
inline constexpr int RAW_BASE_MATRIX_COUNT = 9;
inline constexpr int RAW_BASE_ARG_COUNT = RAW_BASE_ARG_MATRIX + RAW_BASE_MATRIX_COUNT;

/* White-balance mode of the same node. */
inline constexpr int RAW_BASE_WB_ARG_IMAGE = 0;
inline constexpr int RAW_BASE_WB_ARG_CLIP = 1;
inline constexpr int RAW_BASE_WB_ARG_MULTIPLIERS = 2;
inline constexpr int RAW_BASE_WB_MULTIPLIER_COUNT = 3;
inline constexpr int RAW_BASE_WB_ARG_COUNT =
    RAW_BASE_WB_ARG_MULTIPLIERS + RAW_BASE_WB_MULTIPLIER_COUNT;
