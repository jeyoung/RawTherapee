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
 * Shared Lab-to-RGB arithmetic for the classic OpenCL backend and the stage
 * device. Both paths apply the same matrix and the same 16-bit gamma LUT, so
 * the arithmetic lives once; only the addressing differs, because the classic
 * kernel takes one buffer per plane while the device kernel takes one
 * three-plane buffer with row and plane pitches.
 *
 * This is a namespace-scope constant rather than a macro: GCC 12, the portable
 * package baseline, rejects a multi-line raw string inside a #define, while the
 * host compiler accepts it. Compose kernel sources at run time.
 */
inline constexpr const char* RT_LAB_TO_RGB_CL = R"CLC(
inline float rt_f2xyz(const float f)
{
    return f > 0.20689655172413793f
        ? f * f * f
        : (116.0f * f - 16.0f) * (27.0f / 24389.0f);
}

inline float rt_gamma_lookup(__global const float* lut, const float value)
{
    const float clamped = clamp(value, 0.0f, 65535.0f);
    const int index = (int)clamped;
    const int upper = min(index + 1, 65535);
    const float fraction = clamped - (float)index;
    return lut[index] + (lut[upper] - lut[index]) * fraction;
}

inline unsigned char rt_to_u8(const float value)
{
    const unsigned int v = (unsigned int)clamp(value, 0.0f, 65535.0f);
    return (unsigned char)(((v + 128u) - ((v + 128u) >> 8)) >> 8);
}

/*
 * The caller passes the offset of the pixel rather than the coordinates, so the
 * same helper serves a compact plane (offset = y * width + x) and a pitched
 * three-plane buffer (offset = y * row_pitch + x).
 */
inline float3 rt_lab_to_linear(__global const float* labL,
                               __global const float* labA,
                               __global const float* labB,
                               __global const float* matrix,
                               const int offset)
{
    const float L = labL[offset];
    const float a = labA[offset];
    const float b = labB[offset];
    const float LL = L / 327.68f;
    const float fy = (0.008620689655172414f * L) / 327.68f
        + 0.13793103448275862f;
    const float fx = (0.002f * a) / 327.68f + fy;
    const float fz = fy - (0.005f * b) / 327.68f;
    const float xyzX = 65535.0f * rt_f2xyz(fx) * 0.9642f;
    const float xyzZ = 65535.0f * rt_f2xyz(fz) * 0.8249f;
    const float xyzY = LL > 8.0f
        ? 65535.0f * fy * fy * fy
        : 65535.0f * LL / 903.2962962962963f;

    return (float3)(
        matrix[0] * xyzX + matrix[1] * xyzY + matrix[2] * xyzZ,
        matrix[3] * xyzX + matrix[4] * xyzY + matrix[5] * xyzZ,
        matrix[6] * xyzX + matrix[7] * xyzY + matrix[8] * xyzZ);
}
)CLC";
