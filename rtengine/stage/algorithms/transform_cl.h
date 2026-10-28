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
 * Sampling helpers for the rotation/scale transform, shared by the classic
 * OpenCL backend (`rtengine/opencl/openclengine.cc`) and the stage device
 * (`rtengine/stage/stage_opencl_device.cc`).
 *
 * Both backends interpolate the same way, so the arithmetic lives here once.
 * The entry points differ only in how they reach the pixels: the classic
 * backend passes one buffer per plane and a stride equal to the width, while
 * the stage device passes one three-plane buffer and a row plus plane pitch.
 * `stride` is the row stride in floats, and `width`/`height` are the image
 * bounds used for clamping.
 *
 * This is a macro rather than a constant so each kernel source can concatenate
 * it with its own entry point at compile time.
 */
#define RT_TRANSFORM_SAMPLING_CL R"CLC(
inline float rt_transform_finish(const float value, const int useLog)
{
    return useLog ? exp(value) : value;
}

inline float rt_transform_cubic(__global const float* src, const int stride,
                                const int xs, const int ys,
                                const float dx, const float dy)
{
    const float A = -0.85f;
    const float t1v = A * (dy - dy * dy);
    const float t2v = (3.0f - 2.0f * dy) * dy * dy;
    const float w0v = t1v - (t1v * dy);
    const float w1v = 1.0f - (t1v * dy) - t2v;
    const float w2v = t1v * dy - t1v + t2v;
    const float w3v = t1v * dy;
    float values[4];

    for (int i = 0; i < 4; ++i) {
        values[i] = (w0v * src[ys * stride + xs + i]
            + w1v * src[(ys + 1) * stride + xs + i])
            + (w2v * src[(ys + 2) * stride + xs + i]
            + w3v * src[(ys + 3) * stride + xs + i]);
    }

    const float t1h = A * (dx - dx * dx);
    const float t2h = (3.0f - 2.0f * dx) * dx * dx;
    const float w0h = t1h - (t1h * dx);
    const float w1h = 1.0f - (t1h * dx) - t2h;
    const float w2h = t1h * dx - t1h + t2h;
    const float w3h = t1h * dx;

    return (values[0] * w0h + values[2] * w2h)
        + (values[1] * w1h + values[3] * w3h);
}

inline float rt_transform_bilinear(__global const float* src, const int stride,
                                   const int width, const int height,
                                   const int xc, const int yc,
                                   const float dx, const float dy)
{
    const int x1 = clamp(xc, 0, width - 1);
    const int x2 = clamp(xc + 1, 0, width - 1);
    const int y1 = clamp(yc, 0, height - 1);
    const int y2 = clamp(yc + 1, 0, height - 1);

    return src[y1 * stride + x1] * (1.0f - dx) * (1.0f - dy)
        + src[y1 * stride + x2] * dx * (1.0f - dy)
        + src[y2 * stride + x1] * (1.0f - dx) * dy
        + src[y2 * stride + x2] * dx * dy;
}
)CLC"
