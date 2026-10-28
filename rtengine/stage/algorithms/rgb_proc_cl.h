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
 * The RGB tone and Lab conversion kernel, shared by the classic OpenCL
 * backend and the stage device. The kernel indexes single-plane buffers by
 * explicit row pitch, because the stage device allocates rows padded to the
 * stage alignment while the classic backend is compact. The two backends
 * upload one device buffer per plane and reuse the same text; only the upload,
 * the pitch and the readback differ.
 *
 * A namespace-scope constant rather than a macro: GCC 12, the portable
 * package baseline, rejects a multi-line raw string inside a #define.
 */
inline constexpr const char* RT_RGB_PROC_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

inline float rt_lut(__global const float* lut, const float value)
{
    const float clamped = clamp(value, 0.0f, 65535.0f);
    const int index = (int)clamped;
    const int upper = min(index + 1, 65535);
    const float fraction = clamped - (float)index;
    return lut[index] + (lut[upper] - lut[index]) * fraction;
}

inline float rt_hlcurve(const float expScale, const float comp,
                        const float hlRange, const float level)
{
    if (comp > 0.0f) {
        float val = level + (hlRange - 65536.0f);

        if (val == 0.0f) {
            val = 0.000001f;
        }

        float Y = val * expScale / hlRange;
        Y *= comp;

        if (Y <= -1.0f) {
            Y = -0.999999f;
        }

        const float R = hlRange / (val * comp);
        return log1p(Y) * R;
    }

    return expScale;
}

inline void rt_filmlike_clip_tone(float* r, float* g, float* b, const float L)
{
    const float rr = *r > L ? L : *r;
    const float bb = *b > L ? L : *b;
    const float gg = bb + ((rr - bb) * (*g - *b) / (*r - *b));
    *r = rr;
    *g = gg;
    *b = bb;
}

inline void rt_filmlike_clip(float* r, float* g, float* b)
{
    const float L = 65535.0f;

    if (*r >= *g) {
        if (*g > *b) {
            rt_filmlike_clip_tone(r, g, b, L);
        } else if (*b > *r) {
            rt_filmlike_clip_tone(b, r, g, L);
        } else if (*b > *g) {
            rt_filmlike_clip_tone(r, b, g, L);
        } else {
            *r = *r > L ? L : *r;
            *g = *g > L ? L : *g;
            *b = *g;
        }
    } else {
        if (*r >= *b) {
            rt_filmlike_clip_tone(g, r, b, L);
        } else if (*b > *g) {
            rt_filmlike_clip_tone(b, g, r, L);
        } else {
            rt_filmlike_clip_tone(g, b, r, L);
        }
    }
}

inline float rt_compute_f(__global const float* cachef, const float value)
{
    if (value < 0.0f) {
        return 327.68f * ((903.2962962962963f * value / 65535.0f + 16.0f)
                          / 116.0f);
    }
    if (value > 65535.0f) {
        return 327.68f * cbrt(value / 65535.0f);
    }
    return rt_lut(cachef, value);
}

inline float rt_compute_fy(__global const float* cachefy, const float value)
{
    if (value < 0.0f) {
        return 327.68f * (903.2962962962963f * value / 65535.0f);
    }
    if (value > 65535.0f) {
        return 327.68f * (116.0f * cbrt(value / 65535.0f) - 16.0f);
    }
    return rt_lut(cachefy, value);
}

__kernel void rt_rgb_proc(__global const float* srcR,
                          __global const float* srcG,
                          __global const float* srcB,
                          __global float* dstL,
                          __global float* dstA,
                          __global float* dstB,
                          __global const float* hlLut,
                          __global const float* shLut,
                          __global const float* toneLut,
                          __global const float* rLut,
                          __global const float* gLut,
                          __global const float* bLut,
                          __global const float* cachef,
                          __global const float* cachefy,
                          __global const float* channelMixer,
                          __global const float* toxyz,
                          const int width,
                          const int height,
                          const int srcPitch,
                          const int dstPitch,
                          const float expScale,
                          const float comp,
                          const float hlRange,
                          const int toneCurveBlack,
                          const int clampOog,
                          const int mixChannels,
                          const int rgbCurveMask)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);

    if (x >= width || y >= height) {
        return;
    }

    const int srcIndex = y * srcPitch + x;
    const int dstIndex = y * dstPitch + x;
    float r = srcR[srcIndex];
    float g = srcG[srcIndex];
    float b = srcB[srcIndex];

    if (mixChannels) {
        const float rmix = (r * channelMixer[0] + g * channelMixer[1]
                            + b * channelMixer[2]) / 100.0f;
        const float gmix = (r * channelMixer[3] + g * channelMixer[4]
                            + b * channelMixer[5]) / 100.0f;
        const float bmix = (r * channelMixer[6] + g * channelMixer[7]
                            + b * channelMixer[8]) / 100.0f;
        r = rmix;
        g = gmix;
        b = bmix;
    }

    float toneFactor;

    if (fmax(r, fmax(g, b)) >= 65535.0f) {
        toneFactor = ((r < 65535.0f ? rt_lut(hlLut, r)
                                    : rt_hlcurve(expScale, comp, hlRange, r))
                      + (g < 65535.0f ? rt_lut(hlLut, g)
                                      : rt_hlcurve(expScale, comp, hlRange, g))
                      + (b < 65535.0f ? rt_lut(hlLut, b)
                                      : rt_hlcurve(expScale, comp, hlRange, b)))
            / 3.0f;
    } else {
        toneFactor = (rt_lut(hlLut, r) + rt_lut(hlLut, g)
                      + rt_lut(hlLut, b)) / 3.0f;
    }

    r *= toneFactor;
    g *= toneFactor;
    b *= toneFactor;

    if (toneCurveBlack != 0) {
        const float Y = 0.299f * r + 0.587f * g + 0.114f * b;
        toneFactor = rt_lut(shLut, Y);
        r *= toneFactor;
        g *= toneFactor;
        b *= toneFactor;
    }

    if (clampOog != 0) {
        r = fmax(r, 0.0f);
        g = fmax(g, 0.0f);
        b = fmax(b, 0.0f);

        if (r > 65535.0f || g > 65535.0f || b > 65535.0f) {
            rt_filmlike_clip(&r, &g, &b);
        }
    }

    if (r >= 0.0f && r <= 65535.0f) {
        r = rt_lut(toneLut, r);
    }
    if (g >= 0.0f && g <= 65535.0f) {
        g = rt_lut(toneLut, g);
    }
    if (b >= 0.0f && b <= 65535.0f) {
        b = rt_lut(toneLut, b);
    }

    if ((rgbCurveMask & 1) != 0 && r >= 0.0f && r <= 65535.0f) {
        r = rt_lut(rLut, r);
    }
    if ((rgbCurveMask & 2) != 0 && g >= 0.0f && g <= 65535.0f) {
        g = rt_lut(gLut, g);
    }
    if ((rgbCurveMask & 4) != 0 && b >= 0.0f && b <= 65535.0f) {
        b = rt_lut(bLut, b);
    }

    const float X = toxyz[0] * r + toxyz[1] * g + toxyz[2] * b;
    const float Y = toxyz[3] * r + toxyz[4] * g + toxyz[5] * b;
    const float Z = toxyz[6] * r + toxyz[7] * g + toxyz[8] * b;
    const float fx = rt_compute_f(cachef, X);
    const float fy = rt_compute_f(cachef, Y);
    const float fz = rt_compute_f(cachef, Z);

    dstL[dstIndex] = rt_compute_fy(cachefy, Y);
    dstA[dstIndex] = 500.0f * (fx - fy);
    dstB[dstIndex] = 200.0f * (fy - fz);
}
)CLC";
