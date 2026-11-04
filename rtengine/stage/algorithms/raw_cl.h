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

#include <cstdint>

/*
 * The raw.preprocess device node: the Bayer arm of
 * RawImageSource::scaleColors().
 *
 * The host keeps the prelude that derives the black offsets and the channel
 * multipliers: the black-level selection, c_white, calculate_scale_mul() and
 * the clip levels all depend on metadata and on double-precision helpers, so
 * they stay where the CPU path computes them. The node only performs the
 * per-pixel affine step, which is a subtraction, a clamp to zero and a
 * multiplication, and therefore rounds identically in both paths.
 *
 * One work item owns one row. It writes the scaled pixels in place and the
 * per-row channel maximum to a small row-max buffer; the host reduces those
 * rows into RawImageSource::chmax, exactly as the CPU loop accumulates its
 * per-thread maxima. Keeping the reduction on a row boundary avoids an
 * atomic per pixel and a second full-image scan.
 *
 * Argument layout (raw.preprocess):
 *   [0] F32, one plane, read/write: the mosaic, a rect view whose row pitch is
 *       the parent width in floats
 *   [1] F32, one plane: eight coefficients, cblacksom[0..3] then
 *       scale_mul[0..3]
 *   [2] F32, one plane, width 3*height and height 1: the per-row maxima of
 *       colour 0, 1 and 2, row after row (a one-row buffer so the residency's
 *       row alignment cannot split a row triple)
 *   [3] INT: the raw filters word, the same value RawImage::FC() reads
 *   [4] INT: the origin x of the rect in the parent mosaic
 *   [5] INT: the origin y of the rect in the parent mosaic
 *
 * A namespace-scope constant rather than a macro: GCC 12, the portable
 * package baseline, rejects a multi-line raw string inside a #define.
 */
inline constexpr const char* RT_RAW_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

__kernel void rt_raw_preprocess_scale(
    __global float* image,
    const int width,
    const int height,
    const int pitch,
    __global const float* coeffs,
    __global float* row_max,
    const uint filters,
    const int origin_x,
    const int origin_y)
{
    const int row = get_global_id(0);
    if (row >= height) {
        return;
    }

    const int y = origin_y + row;
    const uint tile_row = ((uint)y << 1) & 14u;
    const uint even_row = ((uint)y & 1u) == 0u;

    float max_r = 0.0f;
    float max_g = 0.0f;
    float max_b = 0.0f;
    __global float* line = image + (long)row * pitch;

    for (int col = 0; col < width; ++col) {
        const uint tile_col = (uint)(origin_x + col) & 1u;
        const uint c = (filters >> ((tile_row + tile_col) << 1)) & 3u;
        const uint c4 = (c == 1u && even_row) ? 3u : c;
        const float val =
            fmax(0.0f, line[col] - coeffs[c4]) * coeffs[4u + c4];
        line[col] = val;

        if (c == 0u) {
            max_r = fmax(max_r, val);
        } else if (c == 1u) {
            max_g = fmax(max_g, val);
        } else if (c == 2u) {
            max_b = fmax(max_b, val);
        }
    }

    row_max[row * 3 + 0] = max_r;
    row_max[row * 3 + 1] = max_g;
    row_max[row * 3 + 2] = max_b;
}
)CLC";

/* The node accepts a three-colour Bayer pattern only; a value of three in any
 * of the eight-by-two filter positions belongs to a four-colour sensor, whose
 * CPU loop indexes chmax with the fourth colour and is not reproduced here. */
inline constexpr bool raw_scale_filters_supported(std::uint32_t filters)
{
    for (int y = 0; y < 8; ++y) {
        const std::uint32_t tile_row =
            (static_cast<std::uint32_t>(y) << 1) & 14u;
        for (int x = 0; x < 2; ++x) {
            const std::uint32_t shift =
                (tile_row + static_cast<std::uint32_t>(x)) << 1;
            if (((filters >> shift) & 3u) == 3u) {
                return false;
            }
        }
    }
    return true;
}

/* Argument indices for the node. */
inline constexpr int RAW_PREPROCESS_ARG_IMAGE = 0;
inline constexpr int RAW_PREPROCESS_ARG_COEFFS = 1;
inline constexpr int RAW_PREPROCESS_ARG_ROW_MAX = 2;
inline constexpr int RAW_PREPROCESS_ARG_FILTERS = 3;
inline constexpr int RAW_PREPROCESS_ARG_ORIGIN_X = 4;
inline constexpr int RAW_PREPROCESS_ARG_ORIGIN_Y = 5;
inline constexpr int RAW_PREPROCESS_ARG_COUNT = 6;
inline constexpr int RAW_PREPROCESS_COEFF_COUNT = 8;
