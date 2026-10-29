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
 * Device arithmetic for the lab.wavelet node's directional pyramid equaliser.
 *
 * This is a direct port of dirpyr_equalizer.cc's dirpyr_channel() and
 * idirpyr_eq_channel()/Color::SkinSatCbdl(). The per-level range tables and
 * the hue/chroma masks are built on the host through the block's own code and
 * uploaded; the kernels never re-derive a curve or a threshold.
 *
 * The arithmetic stays single precision, which is what the host uses. The
 * only host rounding the kernel does not reproduce is the vectorised
 * dirpyr_channel() weight: the SSE path computes 1000*x/(diff+1000) where the
 * scalar path multiplies x by 1/(diff+1000). The two differ by at most one
 * ulp of the weight, and the parity test measures the resulting Lab
 * difference. Division and square root must be correctly rounded; the
 * launcher passes -cl-fp32-correctly-rounded-divide-sqrt.
 */
inline constexpr const char* RT_LAB_WAVELET_CL = R"CLC(
/* One range table per level, matching RT_LAB_WAVELET_LUT_ENTRIES. */
#define RT_LW_LUT_ENTRIES 0x20000
/* The block adds 0x10000 to a signed hipass to index the table. */
#define RT_LW_LUT_BIAS 0x10000
/* rangeFn(): the directional weight's scale. */
#define RT_LW_RANGE_FLOOR 1000.0f

/* LUTf::operator[](float) with LUT_CLIP_BELOW | LUT_CLIP_ABOVE, the lookup
 * idirpyr_eq_channel() uses: indexes are interpolated between neighbours. */
inline float rt_lw_lut(__global const float* table, float index)
{
    const int last = RT_LW_LUT_ENTRIES - 1;
    if (index < 0.0f) {
        return table[0];
    }
    if (index > (float)(RT_LW_LUT_ENTRIES - 2)) {
        return table[last];
    }
    const int base = (int)index;
    const float fraction = index - (float)base;
    const float low = table[base];
    const float high = table[base + 1] - low;
    return low + high * fraction;
}

/* rangeFn() from dirpyr_equalizer.cc. */
inline float rt_lw_range(float difference)
{
    return 1.0f / (fabs(difference) + RT_LW_RANGE_FLOOR);
}

/*
 * The directional weight. The host has two forms and uses both on the same
 * image: the scalar border and tail lines multiply the domain kernel by
 * rangeFn(), while the SSE interior divides (1000 * kernel) by the same
 * denominator. The two differ by an ulp of the weight. `sse` reproduces the
 * host's vectorised columns so the whole image rounds as the block does.
 */
inline float rt_lw_weight(float weight, float difference, int sse)
{
    const float magnitude = fabs(difference) + RT_LW_RANGE_FLOOR;
    if (sse != 0) {
        return (RT_LW_RANGE_FLOOR * weight) / magnitude;
    }
    return weight * (1.0f / magnitude);
}

/*
 * Color::SkinSatCbdl(), ported verbatim. The caller initialises scale to 1
 * and passes the positive or negative magnitude of skinprotect.
 */
inline void rt_lw_skin_sat(float lum, float hue, float chrom, float skinprot,
                           float* scale, int neg, float b_l, float t_l,
                           float t_r)
{
    const float C9 = 8.f, C8 = 15.f, C7 = 12.f, C4 = 7.f, C3 = 5.f,
                C2 = 5.f, C1 = 5.f;
    const float H9 = 0.05f, H8 = 0.25f, H7 = 0.1f, H4 = 0.02f, H3 = 0.02f,
                H2 = 0.1f, H1 = 0.1f, H10 = -0.2f, H11 = -0.2f;

    if (lum >= 85.0f) {
        if ((hue > (t_l + 0.53f - H9) && hue < (t_r + H9)) &&
            (chrom > 8.0f && chrom < (14.0f + C9))) {
            *scale = (100.f - skinprot) / 100.1f;
        } else if (lum >= 92.0f) {
            if ((hue > t_l + 0.4f && hue < t_r) &&
                (chrom > 7.0f && chrom < (15.0f))) {
                *scale = (100.f - skinprot * 0.6f) / 100.1f;
            } else if ((hue > b_l && hue < t_r) &&
                       (chrom > 7.0f && chrom < (18.0f))) {
                *scale = (100.f - skinprot * 0.4f) / 100.1f;
            }
        } else if ((hue > t_l + 0.4f && hue < t_r - 0.3f) &&
                   (chrom > 7.0f && chrom < (26.0f + C9))) {
            *scale = (100.f - skinprot * 0.6f) / 100.1f;
        } else if ((hue > b_l + 0.05f && hue < t_r) &&
                   (chrom > 7.0f && chrom < (35.0f + C9))) {
            *scale = (100.f - skinprot * 0.4f) / 100.1f;
        }
    } else if (lum >= 70.0f) {
        if ((hue > t_l + 0.15f && hue < (t_r - 0.2f + H8)) &&
            (chrom > 8.0f && chrom < (35.0f + C8))) {
            *scale = (100.f - skinprot) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.2f) &&
                   (chrom > 7.0f && chrom < (48.0f + C9))) {
            *scale = (100.f - skinprot * 0.6f) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r) &&
                   (chrom > 7.0f && chrom < (55.0f + C9))) {
            *scale = (100.f - skinprot * 0.4f) / 100.1f;
        }
    } else if (lum >= 52.0f) {
        if ((hue > t_l && hue < (t_r + H7)) &&
            (chrom > 11.0f && chrom < (35.0f + C7))) {
            *scale = (100.f - skinprot) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.2f) &&
                   (chrom > 7.0f && chrom < (48.0f + C9))) {
            *scale = (100.f - skinprot * 0.6f) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r) &&
                   (chrom > 7.0f && chrom < (55.0f + C9))) {
            *scale = (100.f - skinprot * 0.4f) / 100.1f;
        }
    } else if (lum >= 35.0f) {
        if ((hue > t_l && hue < (t_r + H4)) &&
            (chrom > 13.0f && chrom < (37.0f + C4))) {
            *scale = (100.f - skinprot) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.2f) &&
                   (chrom > 7.0f && chrom < (48.0f + C9))) {
            *scale = (100.f - skinprot * 0.6f) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r) &&
                   (chrom > 7.0f && chrom < (55.0f + C9))) {
            *scale = (100.f - skinprot * 0.4f) / 100.1f;
        }
    } else if (lum >= 20.0f) {
        if ((hue > t_l && hue < (t_r + H3)) &&
            (chrom > 7.0f && chrom < (35.0f + C3))) {
            *scale = (100.f - skinprot) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.2f) &&
                   (chrom > 7.0f && chrom < (48.0f + C9))) {
            *scale = (100.f - skinprot * 0.6f) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r) &&
                   (chrom > 7.0f && chrom < (55.0f + C9))) {
            *scale = (100.f - skinprot * 0.4f) / 100.1f;
        }
    } else if (lum >= 10.0f) {
        if ((hue > (t_l - 0.25f + H10) && hue < (t_r - 0.3f + H2)) &&
            (chrom > 8.0f && chrom < (23.0f + C2))) {
            *scale = (100.f - skinprot) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.2f) &&
                   (chrom > 7.0f && chrom < (35.0f + C1))) {
            *scale = (100.f - skinprot * 0.6f) / 100.1f;
        } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.1f) &&
                   (chrom > 7.0f && chrom < (45.0f + C1))) {
            *scale = (100.f - skinprot * 0.4f) / 100.1f;
        }
    } else if ((hue > (t_l - 0.2f + H10) && hue < (t_r - 0.3f + H1)) &&
               (chrom > 8.0f && chrom < (23.0f + C1))) {
        *scale = (100.f - skinprot) / 100.1f;
    } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.2f) &&
               (chrom > 7.0f && chrom < (35.0f + C1))) {
        *scale = (100.f - skinprot * 0.6f) / 100.1f;
    } else if ((hue > (b_l + 0.07f + H11) && hue < t_r - 0.1f) &&
               (chrom > 7.0f && chrom < (45.0f + C1))) {
        *scale = (100.f - skinprot * 0.4f) / 100.1f;
    }

    /* extended zone for hair, beard and high skin protection */
    if (skinprot > 85.f && chrom < 20.f && neg) {
        const float modula = -0.0666f * skinprot + 6.66f;
        *scale *= modula;
    }
}

/*
 * One level of dirpyr_channel(): a directionally weighted low-pass. One work
 * item per pixel, with the same neighbour order and the same boundary clamps
 * as the scalar loop. `level` selects the 5x5 domain kernel; levels 0 and 1
 * run the kernel-free branch.
 */
__kernel void rt_lab_wavelet_decompose(
    __global const float* fine, int fine_pitch,
    __global float* coarse, int coarse_pitch,
    int width, int height, int level, int scale, int vectorised)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int halfwin = 2;
    const float centre = fine[y * fine_pitch + x];
    float value = 0.0f;
    float norm = 0.0f;

    const int window = level > 1 ? halfwin * scale : scale;

    /*
     * The host's SSE loop starts at `window` and advances four output columns
     * per iteration while the group start is below width - window - 3. Each
     * lane k of a group loads the four consecutive samples at jnbr + k, not
     * the scale-spaced samples a scalar pass would use, so the device must
     * reproduce the group and lane, not just the weight form.
     */
    int sse = 0;
    int group = x;
    int lane = 0;
    if (vectorised != 0 && x >= window) {
        const int candidate = window + ((x - window) / 4) * 4;
        if (candidate < width - window - 3) {
            sse = 1;
            group = candidate;
            lane = x - candidate;
        }
    }

    if (sse != 0) {
        if (level > 1) {
            const int domker[5][5] = {
                {1000, 1000, 1000, 1000, 1000},
                {1000, 2000, 2000, 2000, 1000},
                {1000, 2000, 2000, 2000, 1000},
                {1000, 2000, 2000, 2000, 1000},
                {1000, 1000, 1000, 1000, 1000}
            };
            for (int inbr = max(0, y - window);
                 inbr <= min(height - 1, y + window); inbr += scale) {
                const int row = (inbr - y) / scale + halfwin;
                int column = 0;
                for (int jnbr = group - window; jnbr <= group + window;
                     jnbr += scale, ++column) {
                    const float sample =
                        fine[inbr * fine_pitch + jnbr + lane];
                    const float dirwt = rt_lw_weight(
                        (float)domker[row][column], sample - centre, 1);
                    value += dirwt * sample;
                    norm += dirwt;
                }
            }
        } else {
            for (int inbr = max(0, y - scale);
                 inbr <= min(height - 1, y + scale); inbr += scale) {
                for (int jnbr = group - scale; jnbr <= group + scale;
                     jnbr += scale) {
                    const float sample =
                        fine[inbr * fine_pitch + jnbr + lane];
                    const float dirwt =
                        rt_lw_weight(1.0f, sample - centre, 1);
                    value += dirwt * sample;
                    norm += dirwt;
                }
            }
        }
    } else if (level > 1) {
        const int domker[5][5] = {
            {1000, 1000, 1000, 1000, 1000},
            {1000, 2000, 2000, 2000, 1000},
            {1000, 2000, 2000, 2000, 1000},
            {1000, 2000, 2000, 2000, 1000},
            {1000, 1000, 1000, 1000, 1000}
        };
        const int jmin = max(0, x - window);
        const int jmax = min(width - 1, x + window);
        for (int inbr = max(0, y - window);
             inbr <= min(height - 1, y + window); inbr += scale) {
            const int row = (inbr - y) / scale + halfwin;
            for (int jnbr = jmin; jnbr <= jmax; jnbr += scale) {
                const int column = (jnbr - x) / scale + halfwin;
                const float sample = fine[inbr * fine_pitch + jnbr];
                const float dirwt = rt_lw_weight(
                    (float)domker[row][column], sample - centre, 0);
                value += dirwt * sample;
                norm += dirwt;
            }
        }
    } else {
        const int jmin = max(0, x - scale);
        const int jmax = min(width - 1, x + scale);
        for (int inbr = max(0, y - scale);
             inbr <= min(height - 1, y + scale); inbr += scale) {
            for (int jnbr = jmin; jnbr <= jmax; jnbr += scale) {
                const float sample = fine[inbr * fine_pitch + jnbr];
                const float dirwt =
                    rt_lw_weight(1.0f, sample - centre, 0);
                value += dirwt * sample;
                norm += dirwt;
            }
        }
    }

    coarse[y * coarse_pitch + x] = value / norm;
}

/*
 * One level of idirpyr_eq_channel(): add the weighted high-pass to the
 * reconstruction buffer. `to_dst` is set for level 0, whose result the block
 * copies into the source plane; every other level accumulates in `buffer`.
 * The first reconstruction level aliases `buffer` and `coarse`, exactly as
 * the block reuses the last pyramid level.
 */
__kernel void rt_lab_wavelet_equalize(
    __global const float* fine, int fine_pitch,
    __global const float* coarse, int coarse_pitch,
    __global float* buffer, int buffer_pitch,
    __global float* dst, int dst_pitch,
    __global const float* luts, int lut_level,
    __global const float* hue, int hue_pitch,
    __global const float* chrom, int chrom_pitch,
    int width, int height, int skin,
    float skinprot, float b_l, float t_l, float t_r, int to_dst)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * fine_pitch + x;
    const float fine_value = fine[offset];
    const float coarse_value = coarse[y * coarse_pitch + x];
    const float hipass = fine_value - coarse_value;
    const float correct = rt_lw_lut(
        luts + lut_level * RT_LW_LUT_ENTRIES,
        hipass + (float)RT_LW_LUT_BIAS);

    const float old = buffer[y * buffer_pitch + x];
    float result;
    if (skin == 0) {
        result = old + correct * hipass;
    } else if (skinprot > 0.0f) {
        float scale = 1.0f;
        rt_lw_skin_sat(fine_value / 327.68f, hue[y * hue_pitch + x],
                       chrom[y * chrom_pitch + x], skinprot, &scale, 1,
                       b_l, t_l, t_r);
        result = old + (1.0f + correct * scale) * hipass;
    } else {
        const float skinprotneg = -skinprot;
        const float factorHard = 1.0f - skinprotneg / 100.0f;
        float scale = 1.0f;
        rt_lw_skin_sat(fine_value / 327.68f, hue[y * hue_pitch + x],
                       chrom[y * chrom_pitch + x], skinprotneg, &scale, 0,
                       b_l, t_l, t_r);
        if (scale == 1.0f) {
            result = old + (1.0f + correct * factorHard) * hipass;
        } else {
            result = old + (1.0f + correct) * hipass;
        }
    }

    if (to_dst != 0) {
        dst[y * dst_pitch + x] = result;
    } else {
        buffer[y * buffer_pitch + x] = result;
    }
}
)CLC";
