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
 * The raw.base device node: the camera-to-working-space matrix multiply in
 * RawImageSource::colorSpaceConversion_() when neither an input ICC profile
 * nor a DCP profile is supplied.
 *
 * The host computes the 3x3 matrix itself: ICCStore's working-space inverse
 * multiplied by the camera matrix, in double precision. The result is passed
 * unchanged, and the kernel repeats the host loop's arithmetic exactly:
 * every product is the product of a double matrix entry and a float pixel
 * promoted to double, the additions are left-associative doubles, and the
 * three results are rounded to float only when they are written back. The
 * kernel therefore needs an fp64 device and refuses one without it.
 *
 * One work item owns one row. The image is a three-plane F32 buffer read and
 * written in place; the host already owns the three planes, so the node reads
 * them back into the same memory. Every other colorSpaceConversion_() arm --
 * an input ICC profile, a DCP profile, the LUT and Lab paths -- stays on the
 * host and the caller's loop runs untouched.
 *
 * Argument layout (raw.base):
 *   [0] F32, three planes, read/write: the image
 *   [1..9] DOUBLE: the row-major camera-to-working-space matrix m00..m22
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
)CLC";

/* Argument indices for the node. */
inline constexpr int RAW_BASE_ARG_IMAGE = 0;
inline constexpr int RAW_BASE_ARG_MATRIX = 1;
inline constexpr int RAW_BASE_MATRIX_COUNT = 9;
inline constexpr int RAW_BASE_ARG_COUNT = RAW_BASE_ARG_MATRIX + RAW_BASE_MATRIX_COUNT;
