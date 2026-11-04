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
 * The raw.demosaic device node: the MONO and NONE arms of
 * RawImageSource::nodemosaic().
 *
 * Every interpolating demosaic arm stays on the host. The fast arm, the
 * simplest of them, derives its gradient weights with the SSE reciprocal
 * approximation (`_mm_rcp_ps` in fast_demo.cc's INVGRADV), which has no
 * portable OpenCL equivalent, and the remaining arms (AHD, EAHD, HPHD, VNG4,
 * DCB, IGV, LMMSE, AMaZE, RCD, pixel shift and dual demosaic) are full
 * algorithms with their own multi-pass state. `nodemosaic()` is the one arm
 * that is a pure per-pixel selection: for a monochrome sensor or the MONO
 * method it copies the mosaic into the three planes, and for NONE it routes
 * each pixel to the plane named by the CFA. Both are assignments, so the
 * device and the host agree bit for bit by construction.
 *
 * One work item owns one row. The four buffers are separate one-plane F32
 * images so the residency owns each host array by its own address; the
 * mosaic is read once and each output pixel is written exactly once.
 *
 * Argument layout (raw.demosaic):
 *   [0] F32, one plane, read: the mosaic
 *   [1] F32, one plane, write: red
 *   [2] F32, one plane, write: green
 *   [3] F32, one plane, write: blue
 *   [4] INT: 1 for the monochrome copy, 0 for the CFA split
 *   [5] INT: the raw filters word, the same value RawImage::FC() reads; it is
 *       unused when [4] is 1 and is still validated
 */
inline constexpr const char* RT_RAW_DEMOSAIC_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

__kernel void rt_raw_demosaic_nodemosaic(
    __global const float* raw,
    __global float* red,
    __global float* green,
    __global float* blue,
    const int width,
    const int height,
    const int pitch,
    const uint filters,
    const int mono)
{
    const int row = get_global_id(0);
    if (row >= height) {
        return;
    }

    const long base = (long)row * pitch;
    const uint tile_row = ((uint)row << 1) & 14u;

    for (int col = 0; col < width; ++col) {
        const long index = base + col;
        const float value = raw[index];

        if (mono != 0) {
            red[index] = value;
            green[index] = value;
            blue[index] = value;
        } else {
            const uint tile_col = (uint)col & 1u;
            const uint c = (filters >> ((tile_row + tile_col) << 1)) & 3u;
            red[index] = c == 0u ? value : 0.0f;
            green[index] = c == 1u ? value : 0.0f;
            blue[index] = c == 2u ? value : 0.0f;
        }
    }
}
)CLC";

/* The CFA split reproduces RawImageSource::nodemosaic()'s switch, which has
 * no case for colour 3: a four-colour pattern would leave those pixels
 * unwritten, so the node refuses it and the host runs the arm. */
inline constexpr bool raw_demosaic_filters_supported(std::uint32_t filters)
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
inline constexpr int RAW_DEMOSAIC_ARG_RAW = 0;
inline constexpr int RAW_DEMOSAIC_ARG_RED = 1;
inline constexpr int RAW_DEMOSAIC_ARG_GREEN = 2;
inline constexpr int RAW_DEMOSAIC_ARG_BLUE = 3;
inline constexpr int RAW_DEMOSAIC_ARG_MONO = 4;
inline constexpr int RAW_DEMOSAIC_ARG_FILTERS = 5;
inline constexpr int RAW_DEMOSAIC_ARG_COUNT = 6;
