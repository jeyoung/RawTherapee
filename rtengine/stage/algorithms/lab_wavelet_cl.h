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

/* ------------------------------------------------------------------------
 * The wavelet tool's contrast-only path.
 *
 * Parameter-table indices; they mirror the RT_LW_PARAM_* defines in
 * lab_wavelet_context.h and must be changed together.
 * --------------------------------------------------------------------- */
#define RT_LW_W_LEVELS 0
#define RT_LW_W_HSMET 1
#define RT_LW_W_NUMLEVH 2
#define RT_LW_W_NUMLEVS 3
#define RT_LW_W_C 4
#define RT_LW_W_LOWTHR 18
#define RT_LW_W_B_LHL 20
#define RT_LW_W_T_LHL 21
#define RT_LW_W_B_RHL 22
#define RT_LW_W_T_RHL 23
#define RT_LW_W_B_LSL 24
#define RT_LW_W_T_LSL 25
#define RT_LW_W_B_RSL 26
#define RT_LW_W_T_RSL 27
#define RT_LW_W_TAPS 28
#define RT_LW_W_CONTENA 29
#define RT_LW_W_CHRO 30
#define RT_LW_W_CHMET 31
#define RT_LW_W_CHROMA 32
#define RT_LW_W_SIGMACOL 33
#define RT_LW_W_SKIN 34
#define RT_LW_W_B_L 35
#define RT_LW_W_T_L 36
#define RT_LW_W_T_R 37
#define RT_LW_W_B_R 38
#define RT_LW_W_EDGEENA 39
#define RT_LW_W_EDG_VAL 40
#define RT_LW_W_EDG_RAD 41
#define RT_LW_W_EDG_TIL 42
#define RT_LW_W_EDG_EFF 43
#define RT_LW_W_EDG_REINFORCE 44
#define RT_LW_W_EDG_CURVE 45
#define RT_LW_W_EDG_HIG 46
#define RT_LW_W_EDG_LOW 47
#define RT_LW_W_FILTER_ANAL_LO 48
#define RT_LW_W_FILTER_ANAL_HI 56
#define RT_LW_W_FILTER_SYNTH_LO 64
#define RT_LW_W_FILTER_SYNTH_HI 72
/* Mirrors RT_LAB_WAVELET_EDGE_ENTRIES and RT_LAB_WAVELET_CURVE_ENTRIES. */
#define RT_LW_W_EDGE_ENTRIES 13
#define RT_LW_W_EDGE_CURVE 501
/* Mirrors RT_LAB_WAVELET_FINAL_STATS_ENTRIES and FINAL_LUT_ENTRIES. */
#define RT_LW_W_FINAL_STATS 3
#define RT_LW_W_FINAL_LUT 65536
/* Mirrors the toning tail constants. */
#define RT_LW_W_TONING_MULS 9
#define RT_LW_W_TONING_HEADER 19
#define RT_LW_W_TONING_MEA_ENTRIES 10

/*
 * Level 0 analysis, vertical half. One work item per output column and output
 * row; both taps run over the source rows with the host's clamped border and
 * the same ascending tap order.
 */
__kernel void rt_lab_wavelet_w_analysis_v(
    __global const float* src, int src_pitch, int src_h,
    __global float* lo, int lo_pitch,
    __global float* hi, int hi_pitch,
    __global const float* filters, int taps, int offset,
    int width, int rows)
{
    const int x = get_global_id(0);
    const int r = get_global_id(1);
    if (x >= width || r >= rows) {
        return;
    }

    const int row = 2 * r;
    float low = 0.0f;
    float high = 0.0f;
    for (int j = 0; j < taps; ++j) {
        int arg = row + (offset - j);
        arg = max(0, min(arg, src_h - 1));
        const float v = src[arg * src_pitch + x];
        low += filters[RT_LW_W_FILTER_ANAL_LO + j] * v;
        high += filters[RT_LW_W_FILTER_ANAL_HI + j] * v;
    }
    lo[r * lo_pitch + x] = low;
    hi[r * hi_pitch + x] = high;
}

/* Level 0 analysis, horizontal half: one work item per output column. */
__kernel void rt_lab_wavelet_w_analysis_h(
    __global const float* lo, int lo_pitch,
    __global const float* hi, int hi_pitch,
    __global float* lowpass, int low_pitch,
    __global float* band1, int b1_pitch,
    __global float* band2, int b2_pitch,
    __global float* band3, int b3_pitch,
    __global const float* filters, int taps, int offset,
    int src_w, int dst_w, int rows)
{
    const int x = get_global_id(0);
    const int r = get_global_id(1);
    if (x >= dst_w || r >= rows) {
        return;
    }

    const int i = 2 * x;
    float ll = 0.0f;
    float lh = 0.0f;
    float hl = 0.0f;
    float hh = 0.0f;
    for (int j = 0; j < taps; ++j) {
        int arg = i + (offset - j);
        arg = max(0, min(arg, src_w - 1));
        const float a = lo[r * lo_pitch + arg];
        const float b = hi[r * hi_pitch + arg];
        ll += filters[RT_LW_W_FILTER_ANAL_LO + j] * a;
        lh += filters[RT_LW_W_FILTER_ANAL_HI + j] * a;
        hl += filters[RT_LW_W_FILTER_ANAL_LO + j] * b;
        hh += filters[RT_LW_W_FILTER_ANAL_HI + j] * b;
    }
    lowpass[r * low_pitch + x] = ll;
    band1[r * b1_pitch + x] = lh;
    band2[r * b2_pitch + x] = hl;
    band3[r * b3_pitch + x] = hh;
}

/*
 * One undecimated Haar analysis level, vertical half. skip is the level's
 * stride; the host's middle rows are the second branch with a negative tap.
 */
__kernel void rt_lab_wavelet_w_haar_v(
    __global const float* src, int src_pitch, int w, int h, int skip,
    __global float* lo, int lo_pitch,
    __global float* hi, int hi_pitch)
{
    const int x = get_global_id(0);
    const int r = get_global_id(1);
    if (x >= w || r >= h) {
        return;
    }

    float low;
    float high;
    if (r < h - skip) {
        const float a = src[r * src_pitch + x];
        const float b = src[(r + skip) * src_pitch + x];
        low = 0.25f * (a + b);
        high = 0.25f * (a - b);
    } else {
        const float a = src[r * src_pitch + x];
        const float b = src[(r - skip) * src_pitch + x];
        low = 0.25f * (a + b);
        high = 0.25f * (a - b);
    }
    lo[r * lo_pitch + x] = low;
    hi[r * hi_pitch + x] = high;
}

/* One undecimated Haar analysis level, horizontal half. */
__kernel void rt_lab_wavelet_w_haar_h(
    __global const float* lo, int lo_pitch,
    __global const float* hi, int hi_pitch,
    __global float* lowpass, int low_pitch,
    __global float* band1, int b1_pitch,
    __global float* band2, int b2_pitch,
    __global float* band3, int b3_pitch,
    int w, int h, int skip)
{
    const int i = get_global_id(0);
    const int r = get_global_id(1);
    if (i >= w || r >= h) {
        return;
    }

    int other;
    if (i < w - skip) {
        other = i + skip;
    } else if (i >= max(w - skip, skip)) {
        other = i - skip;
    } else {
        return;
    }

    const float a = lo[r * lo_pitch + i];
    const float b = lo[r * lo_pitch + other];
    const float c = hi[r * hi_pitch + i];
    const float d = hi[r * hi_pitch + other];
    lowpass[r * low_pitch + i] = a + b;
    band1[r * b1_pitch + i] = a - b;
    band2[r * b2_pitch + i] = c + d;
    band3[r * b3_pitch + i] = c - d;
}

/* Level 0 synthesis, horizontal half: the two reconstruction rows. */
__kernel void rt_lab_wavelet_w_synth_h(
    __global const float* lowpass, int low_pitch,
    __global const float* band1, int b1_pitch,
    __global const float* band2, int b2_pitch,
    __global const float* band3, int b3_pitch,
    __global float* out_lo, int out_lo_pitch,
    __global float* out_hi, int out_hi_pitch,
    __global const float* filters, int taps, int offset,
    int src_w, int dst_w, int rows)
{
    const int i = get_global_id(0);
    const int r = get_global_id(1);
    if (i >= dst_w || r >= rows) {
        return;
    }

    const int shift = taps - offset - 1;
    const int i_src = (i + shift) / 2;
    const int begin = (i + shift) % 2;
    float tot_lo = 0.0f;
    float tot_hi = 0.0f;
    int l = 0;
    for (int j = begin; j < taps; j += 2, ++l) {
        int arg = i_src - l;
        arg = max(0, min(arg, src_w - 1));
        tot_lo += filters[RT_LW_W_FILTER_SYNTH_LO + j] *
                      lowpass[r * low_pitch + arg] +
                  filters[RT_LW_W_FILTER_SYNTH_HI + j] *
                      band1[r * b1_pitch + arg];
        tot_hi += filters[RT_LW_W_FILTER_SYNTH_LO + j] *
                      band2[r * b2_pitch + arg] +
                  filters[RT_LW_W_FILTER_SYNTH_HI + j] *
                      band3[r * b3_pitch + arg];
    }
    out_lo[r * out_lo_pitch + i] = tot_lo;
    out_hi[r * out_hi_pitch + i] = tot_hi;
}

/*
 * Level 0 synthesis, vertical half. Writes the source plane in place, mixes
 * the original plane by the strength blend and, for L, applies the block's
 * final L <= 0 -> 1 clamp, which is the last operation the host performs on
 * L. The chroma planes do not clamp.
 */
__kernel void rt_lab_wavelet_w_synth_v(
    __global const float* lo, int lo_pitch,
    __global const float* hi, int hi_pitch,
    __global float* dst, int dst_pitch,
    __global const float* filters, int taps, int offset,
    int width, int src_h, int dst_h, float blend, int clamp)
{
    const int x = get_global_id(0);
    const int i = get_global_id(1);
    if (x >= width || i >= dst_h) {
        return;
    }

    const int shift = taps - offset - 1;
    const int i_src = (i + shift) / 2;
    const int begin = (i + shift) % 2;
    float tot = 0.0f;
    int l = 0;
    for (int j = begin; j < taps; j += 2, ++l) {
        int arg = i_src - l;
        arg = max(0, min(arg, src_h - 1));
        tot += filters[RT_LW_W_FILTER_SYNTH_LO + j] *
                   lo[arg * lo_pitch + x] +
               filters[RT_LW_W_FILTER_SYNTH_HI + j] *
                   hi[arg * hi_pitch + x];
    }

    const float src_factor = 1.0f - blend;
    float result = dst[i * dst_pitch + x] * src_factor + blend * 4.0f * tot;
    if (clamp != 0 && result <= 0.0f) {
        result = 1.0f;
    }
    dst[i * dst_pitch + x] = result;
}

/* One undecimated Haar synthesis level, horizontal half. */
__kernel void rt_lab_wavelet_w_haar_synth_h(
    __global const float* lowpass, int low_pitch,
    __global const float* band1, int b1_pitch,
    __global const float* band2, int b2_pitch,
    __global const float* band3, int b3_pitch,
    __global float* out_lo, int out_lo_pitch,
    __global float* out_hi, int out_hi_pitch,
    int w, int h, int skip)
{
    const int i = get_global_id(0);
    const int r = get_global_id(1);
    if (i >= w || r >= h) {
        return;
    }

    float low;
    float high;
    if (i < skip) {
        low = lowpass[r * low_pitch + i] + band1[r * b1_pitch + i];
        high = band2[r * b2_pitch + i] + band3[r * b3_pitch + i];
    } else {
        low = 0.5f * (lowpass[r * low_pitch + i] +
                      band1[r * b1_pitch + i] +
                      lowpass[r * low_pitch + i - skip] -
                      band1[r * b1_pitch + i - skip]);
        high = 0.5f * (band2[r * b2_pitch + i] +
                       band3[r * b3_pitch + i] +
                       band2[r * b2_pitch + i - skip] -
                       band3[r * b3_pitch + i - skip]);
    }
    out_lo[r * out_lo_pitch + i] = low;
    out_hi[r * out_hi_pitch + i] = high;
}

/* One undecimated Haar synthesis level, vertical half. */
__kernel void rt_lab_wavelet_w_haar_synth_v(
    __global const float* lo, int lo_pitch,
    __global const float* hi, int hi_pitch,
    __global float* dst, int dst_pitch,
    int w, int h, int skip)
{
    const int x = get_global_id(0);
    const int i = get_global_id(1);
    if (x >= w || i >= h) {
        return;
    }

    float value;
    if (i < skip) {
        value = lo[i * lo_pitch + x] + hi[i * hi_pitch + x];
    } else {
        value = 0.5f * (lo[i * lo_pitch + x] + hi[i * hi_pitch + x] +
                        lo[(i - skip) * lo_pitch + x] -
                        hi[(i - skip) * hi_pitch + x]);
    }
    dst[i * dst_pitch + x] = value;
}

/*
 * Band statistics. One work item per (level, direction) band, scanning the
 * band in ascending row-major order and accumulating in double, which is the
 * single-threaded OpenMP reduction the host performs at OMP_NUM_THREADS=1.
 * Aver() and Sigma() are reproduced back to back so the second pass sees the
 * same float mean the host uses.
 */
__kernel void rt_lab_wavelet_w_stats(
    __global const float* band1, int p1,
    __global const float* band2, int p2,
    __global const float* band3, int p3,
    __global float* stats, int stats_offset, int w, int h, int bands)
{
    const int band = get_global_id(0);
    if (band >= bands) {
        return;
    }

    const int dir = band % 3;
    __global const float* data = dir == 0 ? band1 : (dir == 1 ? band2 : band3);
    const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);

    const float thres = 32.7f;
    double averaP = 0.0;
    double averaN = 0.0;
    int countP = 0;
    int countN = 0;
    float lmax = 0.0f;
    float lmin = 0.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = data[y * pitch + x];
            if (v >= thres) {
                averaP += (double)v;
                if (v > lmax) {
                    lmax = v;
                }
                ++countP;
            } else if (v < -thres) {
                averaN += (double)v;
                if (v < lmin) {
                    lmin = v;
                }
                ++countN;
            }
        }
    }
    const float averagePlus =
        countP > 0 ? (float)(averaP / (double)countP) : 0.0f;
    const float averageNeg =
        countN > 0 ? (float)(averaN / (double)countN) : 0.0f;

    double variP = 0.0;
    double variN = 0.0;
    int sigmaCountP = 0;
    int sigmaCountN = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = data[y * pitch + x];
            if (v >= thres) {
                const float d = v - averagePlus;
                variP += (double)(d * d);
                ++sigmaCountP;
            } else if (v <= -thres) {
                const float d = v - averageNeg;
                variN += (double)(d * d);
                ++sigmaCountN;
            }
        }
    }
    const float sigmaPlus =
        sigmaCountP > 0 ? (float)sqrt(variP / (double)sigmaCountP) : 0.0f;
    const float sigmaNeg =
        sigmaCountN > 0 ? (float)sqrt(variN / (double)sigmaCountN) : 0.0f;

    __global float* out = stats + stats_offset + band * 6;
    out[0] = averagePlus;
    out[1] = averageNeg;
    out[2] = lmax;
    out[3] = lmin;
    out[4] = sigmaPlus;
    out[5] = sigmaNeg;
}

/*
 * The contrast multiply of ContAllL() for the contrast-only configuration.
 * lbeta and alpha are the host's; the highlight/shadow ramp reproduces the
 * block's per-level kH, its four highlight and four shadow corners, and the
 * two level gates.
 */
__kernel void rt_lab_wavelet_w_contrast(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* coeff0, int coeff_pitch,
    __global const float* origL, int l_pitch,
    __global const float* params, int mea_offset,
    int level, int w, int h, int maxlvl)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    const int i = y * w + x;
    const float cpMul = params[RT_LW_W_C + level];
    const float lowthr = params[RT_LW_W_LOWTHR];
    const int half_levels = maxlvl / 2;

    /* ContAllL() skips the whole contrast multiply when Expcontrast is off;
     * the level stays untouched. */
    if (params[RT_LW_W_CONTENA] == 0.0f) {
        return;
    }

    const float LL100 = origL[(2 * y) * l_pitch + 2 * x] / 327.68f;
    const float LL100res = coeff0[y * coeff_pitch + x] / 327.68f;
    const float delta = fabs(LL100 - LL100res) / (float)half_levels;

    float kH[10];
    for (int ml = 0; ml < maxlvl; ++ml) {
        if (ml < half_levels) {
            kH[ml] = (LL100res + (float)ml * delta) / LL100res;
        } else {
            kH[ml] = (LL100 - (float)ml * delta) / LL100res;
        }
    }

    const int numlevH = (int)params[RT_LW_W_NUMLEVH];
    const int numlevS = (int)params[RT_LW_W_NUMLEVS];
    const int hsmet = params[RT_LW_W_HSMET] != 0.0f;

    const float t_lhl = params[RT_LW_W_T_LHL];
    const float b_lhl = params[RT_LW_W_B_LHL];
    const float t_rhl = params[RT_LW_W_T_RHL];
    const float b_rhl = params[RT_LW_W_B_RHL];
    const float t_lsl = params[RT_LW_W_T_LSL];
    const float b_lsl = params[RT_LW_W_B_LSL];
    const float t_rsl = params[RT_LW_W_T_RSL];
    const float b_rsl = params[RT_LW_W_B_RSL];

    const float red0 = 0.005f * (110.0f - lowthr);
    const float red1 = 0.008f * (110.0f - lowthr);
    const float red2 = 0.011f * (110.0f - lowthr);

    const int base = mea_offset + level * 10;
    const float mea0 = params[base + 0];
    const float mea1 = params[base + 1];
    const float mea2 = params[base + 2];
    const float mea3 = params[base + 3];
    const float mea4 = params[base + 4];
    const float mea5 = params[base + 5];
    const float mea6 = params[base + 6];
    const float mea7 = params[base + 7];
    const float mea8 = params[base + 8];
    const float mea9 = params[base + 9];

    for (int dir = 0; dir < 3; ++dir) {
        /* ContAllL() skips the whole multiply when the level's contrast
         * multiplier is zero; the bands stay untouched. */
        if (cpMul == 0.0f) {
            continue;
        }
        __global float* band =
            dir == 0 ? band1 : (dir == 1 ? band2 : band3);
        const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
        const int at = y * pitch + x;
        const float value = band[at];

        float lbeta;
        if (cpMul < 0.0f) {
            lbeta = 1.0f;
        } else {
            const float wavcl = fabs(value);
            if (wavcl < mea0) {
                lbeta = 0.4f * red0;
            } else if (wavcl < mea1) {
                lbeta = 0.5f * red1;
            } else if (wavcl < mea2) {
                lbeta = 0.7f * red2;
            } else if (wavcl < mea3) {
                lbeta = 1.0f;
            } else if (wavcl < mea4) {
                lbeta = 1.0f;
            } else if (wavcl < mea5) {
                lbeta = 0.8f;
            } else if (wavcl < mea6) {
                lbeta = 0.6f;
            } else if (wavcl < mea7) {
                lbeta = 0.4f;
            } else if (wavcl < mea8) {
                lbeta = 0.2f;
            } else if (wavcl < mea9) {
                lbeta = 0.1f;
            } else {
                lbeta = 0.0f;
            }
        }

        const float alpha =
            (1024.0f + 15.0f * cpMul * lbeta) / 1024.0f;
        float kLlev = 1.0f;

        if (hsmet) {
            const float aaal =
                (1.0f - alpha) / ((b_lhl - t_lhl) * kH[level]);
            const float bbal = 1.0f - aaal * b_lhl * kH[level];
            const float aaar =
                (alpha - 1.0f) / (t_rhl - b_rhl) * kH[level];
            const float bbbr = 1.0f - b_rhl * aaar * kH[level];
            const float aaalS = (1.0f - alpha) / (b_lsl - t_lsl);
            const float bbalS = 1.0f - aaalS * b_lsl;
            const float aaarS = (alpha - 1.0f) / (t_rsl - b_rsl);
            const float bbbrS = 1.0f - b_rsl * aaarS;

            if (level <= numlevH) {
                if (LL100 > t_lhl * kH[level] &&
                    LL100 < t_rhl * kH[level]) {
                    kLlev = alpha;
                } else if (LL100 > b_lhl * kH[level] &&
                           LL100 <= t_lhl * kH[level]) {
                    kLlev = aaal * LL100 + bbal;
                } else if (LL100 > t_rhl * kH[level] &&
                           LL100 <= b_rhl * kH[level]) {
                    kLlev = aaar * LL100 + bbbr;
                } else {
                    kLlev = 1.0f;
                }
            }
            if (level >= numlevS - 1) {
                if (LL100 > t_lsl && LL100 < t_rsl) {
                    kLlev = alpha;
                } else if (LL100 > b_lsl && LL100 <= t_lsl) {
                    kLlev = aaalS * LL100 + bbalS;
                } else if (LL100 > t_rsl && LL100 <= b_rsl) {
                    kLlev = aaarS * LL100 + bbbrS;
                } else {
                    kLlev = 1.0f;
                }
            }
        } else {
            kLlev = alpha;
        }

        band[at] = value * kLlev;
    }
}

/*
 * The chroma multiply of ContAllAB() for CHmethod "link" (CHmet == 2). The
 * arm multiplies the a/b coefficients by alphaC, gated by the level's contrast
 * multiplier, the chroma strength and Expchroma. The betaab ramp is the same
 * ten-step ramp as ContAllL(), fed by the a/b mea block built with sigmacol
 * and an offset of 1. Skin protection reads the hue/chroma masks and the
 * post-L-reconstruction L plane, exactly as the block does.
 */
__kernel void rt_lab_wavelet_w_chroma(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* origL, int l_pitch,
    __global const float* hue, int hue_pitch,
    __global const float* chrom, int chrom_pitch,
    __global const float* params, int mea_offset,
    int level, int w, int h, int skin)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    const float cpMul = params[RT_LW_W_C + level];
    const float cpChrom = params[RT_LW_W_CHRO];
    if (cpMul == 0.0f || cpChrom == 0.0f) {
        return;
    }

    const int base = mea_offset + level * 10;
    const float mea0 = params[base + 0];
    const float mea1 = params[base + 1];
    const float mea2 = params[base + 2];
    const float mea3 = params[base + 3];
    const float mea4 = params[base + 4];
    const float mea5 = params[base + 5];
    const float mea6 = params[base + 6];
    const float mea7 = params[base + 7];
    const float mea8 = params[base + 8];
    const float mea9 = params[base + 9];

    const int i = y * w + x;

    for (int dir = 0; dir < 3; ++dir) {
        __global float* band =
            dir == 0 ? band1 : (dir == 1 ? band2 : band3);
        const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
        const int at = y * pitch + x;
        const float value = band[at];
        const float wavcab = fabs(value);

        float betaab;
        if (wavcab < mea0) {
            betaab = 0.05f;
        } else if (wavcab < mea1) {
            betaab = 0.2f;
        } else if (wavcab < mea2) {
            betaab = 0.7f;
        } else if (wavcab < mea3) {
            betaab = 1.0f;
        } else if (wavcab < mea4) {
            betaab = 1.0f;
        } else if (wavcab < mea5) {
            betaab = 0.8f;
        } else if (wavcab < mea6) {
            betaab = 0.6f;
        } else if (wavcab < mea7) {
            betaab = 0.4f;
        } else if (wavcab < mea8) {
            betaab = 0.2f;
        } else if (wavcab < mea9) {
            betaab = 0.1f;
        } else {
            betaab = 0.0f;
        }

        float scale = 1.0f;

        if (skin != 0) {
            const float skinprot = params[RT_LW_W_SKIN];
            const float skinprotneg = -skinprot;
            const float factorHard = 1.0f - skinprotneg / 100.0f;
            const float LL100 =
                origL[(2 * y) * l_pitch + 2 * x] / 327.68f;
            const float modhue = hue[(2 * y) * hue_pitch + 2 * x];
            const float modchro = chrom[(2 * y) * chrom_pitch + 2 * x];

            if (skinprot > 0.0f) {
                rt_lw_skin_sat(LL100, modhue, modchro, skinprot, &scale, 1,
                               params[RT_LW_W_B_L], params[RT_LW_W_T_L],
                               params[RT_LW_W_T_R]);
            } else if (skinprot < 0.0f) {
                rt_lw_skin_sat(LL100, modhue, modchro, skinprotneg, &scale,
                               0, params[RT_LW_W_B_L],
                               params[RT_LW_W_T_L], params[RT_LW_W_T_R]);
                scale = (scale == 1.0f) ? factorHard : 1.0f;
            }
        }

        const float alphaC =
            (1024.0f + 15.0f * cpMul * cpChrom * betaab * scale / 50.0f) /
            1024.0f;
        band[at] = value * alphaC;
    }
}

/*
 * The edge panel's plain arm: ContAllL()'s EDmet == 2 block with the host's
 * lipschitz flag set and `cp.detectedge` clear, so koeLi plays no part. One
 * work item per pixel; the block's border ring (borderL == 1) is untouched.
 *
 * `level` selects the level's edge mea block; the tail also carries MaxP,
 * mean and sigma and the sampled ccwcurve. `edge_offset` is the start of the
 * tail (limrad at [0], then levels * 13 entries, then the curve).
 */
inline float rt_lw_curve501(__global const float* table, float index)
{
    /* LUTf::operator[](float) for the 501-entry WavCurve: maxsf is size - 2. */
    const float maxsf = (float)(RT_LW_W_EDGE_CURVE - 2);
    if (index < 0.0f) {
        return table[0];
    }
    if (index > maxsf) {
        return table[RT_LW_W_EDGE_CURVE - 1];
    }
    const int base = (int)index;
    const float fraction = index - (float)base;
    const float low = table[base];
    return low + (table[base + 1] - low) * fraction;
}

/*
 * rtengine/sleef.h's xlogf()/xexpf()/pow_F() as the denoise-curve refinement
 * uses them. The host substitutes these approximations for log()/exp(); a
 * one-ulp difference in the curve abscissa can move the refinement factor,
 * so the refinement reproduces the polynomials rather than substituting the
 * platform's log()/exp(). `mlaf(x, y, z)` in the host is a plain multiply-add
 * with both roundings, which is what the expressions below do.
 */
inline int rt_lw_ilogbp1(float d)
{
    const int m = d < 5.421010862427522E-20f;
    d = m ? 1.8446744073709552E19f * d : d;
    int q = (as_int(d) >> 23) & 0xff;
    q = m ? q - (64 + 0x7e) : q - 0x7e;
    return q;
}

inline float rt_lw_ldexpk(float x, int q)
{
    int m = q >> 31;
    m = (((m + q) >> 6) - m) << 4;
    q = q - (m << 2);
    float u = as_float((m + 0x7f) << 23);
    u = u * u;
    x = x * u;
    x = x * u;
    u = as_float((q + 0x7f) << 23);
    return x * u;
}

inline float rt_lw_xlogf(float d)
{
    const int e = rt_lw_ilogbp1(d * 0.7071f);
    const float m = rt_lw_ldexpk(d, -e);
    float x = (m - 1.0f) / (m + 1.0f);
    const float x2 = x * x;

    float t = 0.2371599674224853515625f;
    t = t * x2 + 0.285279005765914916992188f;
    t = t * x2 + 0.400005519390106201171875f;
    t = t * x2 + 0.666666567325592041015625f;
    t = t * x2 + 2.0f;

    x = x * t + 0.693147180559945286226764f * (float)e;

    if (d == INFINITY) {
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

inline float rt_lw_xexpf(float d)
{
    if (d <= -104.0f) {
        return 0.0f;
    }

    /* xrintf() rounds the double product to float first, then to int. */
    const int q = convert_int_rte(
        (float)((double)d * 1.442695040888963407359924681001892137426645954152985934135449406931));
    const float qf = (float)q;
    float s = qf * (-0.693145751953125f) + d;
    s = qf * (-1.428606765330187045e-06f) + s;

    float u = 0.00136324646882712841033936f;
    u = u * s + 0.00836596917361021041870117f;
    u = u * s + 0.0416710823774337768554688f;
    u = u * s + 0.166665524244308471679688f;
    u = u * s + 0.499999850988388061523438f;

    u = s * (s * u + 1.0f) + 1.0f;
    return rt_lw_ldexpk(u, q);
}

inline float rt_lw_pow(float a, float b)
{
    return rt_lw_xexpf(b * rt_lw_xlogf(a));
}

__kernel void rt_lab_wavelet_w_edge(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* params, int edge_offset,
    int level, int levels, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    /* ContAllL()'s EDmet == 2 loop starts at borderL == 1 on every side. */
    if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) {
        return;
    }

    const int entry = edge_offset + 1 + level * RT_LW_W_EDGE_ENTRIES;
    const float maxp = params[entry + 10];
    /* The host only enters the multiply when MaxP[level] > 0. */
    if (maxp <= 0.0f) {
        return;
    }
    const float mean = params[entry + 11];
    const float sigma = params[entry + 12];

    const float limrad = params[edge_offset];
    const float rad = params[RT_LW_W_EDG_RAD] / 60.0f;
    const float strength = params[RT_LW_W_EDG_VAL] / 8.0f;
    const float eff = params[RT_LW_W_EDG_EFF];
    const int reinforce = (int)params[RT_LW_W_EDG_REINFORCE];

    float repart = params[RT_LW_W_EDG_TIL];
    const float limit = limrad / 60.0f;
    if (reinforce != 2) {
        const float brepart =
            reinforce == 1 ? params[RT_LW_W_EDG_HIG]
                           : params[RT_LW_W_EDG_LOW];
        const float arepart = -(brepart - 1.0f) / limit;
        if (rad < limit) {
            repart *= (arepart * rad + brepart);
        }
    }
    const float al0 = 1.0f + repart / 50.0f;
    const float al10 = 1.0f;
    const float ak = -(al0 - al10) / 10.0f;
    const float bk = al0;
    const float koef = ak * (float)level + bk;
    const float lev = (float)level;
    float expkoef = -pow(fabs(rad - lev), koef);
    if (reinforce == 3) {
        if (rad < limit && level == 0) {
            expkoef *= fabs(repart);
        }
    }
    if (reinforce == 1) {
        if (rad < limit && level == 1) {
            expkoef /= repart;
        }
    }
    const float refin = strength * exp(expkoef);
    const float edgePrecalc = 1.0f + refin;

    const int edgcurv = params[RT_LW_W_EDG_CURVE] != 0.0f;
    __global const float* curve =
        params + edge_offset + 1 + levels * RT_LW_W_EDGE_ENTRIES;
    const float logmax = log(maxp);
    const float rapX = (mean + sigma) / maxp;
    const float inx = log(0.666f);
    const float iny = log(rapX);
    const float rap = inx / iny;
    const float asig = 0.166f / sigma;
    const float bsig = 0.5f - asig * mean;
    const float amean = 0.5f / mean;

    const float mea0 = params[entry + 0];
    const float mea1 = params[entry + 1];
    const float mea2 = params[entry + 2];
    const float mea3 = params[entry + 3];
    const float mea4 = params[entry + 4];
    const float mea5 = params[entry + 5];
    const float mea6 = params[entry + 6];
    const float mea7 = params[entry + 7];
    const float mea8 = params[entry + 8];
    const float mea9 = params[entry + 9];

    for (int dir = 0; dir < 3; ++dir) {
        __global float* band =
            dir == 0 ? band1 : (dir == 1 ? band2 : band3);
        const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
        const int at = y * pitch + x;
        const float value = band[at];
        const float wavcl = fabs(value);

        float beta;
        if (eff < 2.5f) {
            if (wavcl < mea0) {
                beta = 0.05f;
            } else if (wavcl < mea1) {
                beta = 0.2f;
            } else if (wavcl < mea2) {
                beta = 0.7f;
            } else if (wavcl < mea3) {
                beta = 1.0f;
            } else if (wavcl < mea4) {
                beta = 1.0f;
            } else if (wavcl < mea5) {
                beta = 0.8f;
            } else if (wavcl < mea6) {
                beta = 0.6f;
            } else if (wavcl < mea7) {
                beta = 0.4f;
            } else if (wavcl < mea8) {
                beta = 0.2f;
            } else if (wavcl < mea9) {
                beta = 0.1f;
            } else {
                beta = 0.0f;
            }
        } else {
            beta = 1.0f;
        }

        float edge = edgePrecalc;
        if (edgcurv != 0) {
            float absciss = 0.0f;
            if (wavcl >= (mean + sigma)) {
                const float valc = log(wavcl) - logmax;
                absciss = exp(valc * rap);
            } else if (wavcl >= mean && wavcl < (mean + sigma)) {
                absciss = asig * wavcl + bsig;
            } else if (wavcl < mean) {
                absciss = amean * wavcl;
            }

            const float abssd = 4.0f;
            const float bbssd = 2.0f;
            float kmul;
            float kmuld;
            if (absciss > 0.666f && absciss < 1.0f) {
                const float maxamp = 2.5f;
                const float maxampd = 10.0f;
                const float a_abssd = (maxamp - abssd) / 0.333f;
                const float b_abssd = maxamp - a_abssd;
                const float da_abssd = (maxampd - abssd) / 0.333f;
                const float db_abssd = maxampd - da_abssd;
                kmul = a_abssd * absciss + b_abssd;
                kmuld = da_abssd * absciss + db_abssd;
            } else {
                const float am = (abssd - bbssd) / 0.666f;
                kmul = absciss * am + bbssd;
                kmuld = kmul;
            }

            const float curvev = rt_lw_curve501(curve, absciss * 500.0f);
            const float kc = kmul * (curvev - 0.5f);
            float kinterm;
            if (kc >= 0.0f) {
                kinterm = 1.0f + 0.6f * kmul * (curvev - 0.5f);
            } else {
                const float kcd = kmuld * (curvev - 0.5f);
                kinterm = 1.0f - (kcd * kcd) / 10.0f;
            }
            if (kinterm < 0.0f) {
                kinterm = 0.01f;
            }
            edge *= kinterm;
            edge = max(edge, 1.0f);
        }

        band[at] = value * (1.0f + (edge - 1.0f) * beta);
    }
}

/*
 * The final panel's coefficient curve: finalContAllL() with finena set and
 * the block's own WavOpacityCurveWL. One work item per band pixel; `level`
 * selects the level's MaxP/mean/sigma in the final tail. The tail starts at
 * `final_offset`: sigmafin at [0], levels * 3 statistics entries, then the
 * 501-point opacity curve.
 *
 * The host uses xlogf()/xexpf() for the per-coefficient exponential; this
 * kernel uses log()/exp(), the same substitution the edge arm documents. The
 * scalars use double log() as the host does.
 */
__kernel void rt_lab_wavelet_w_final(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* params, int final_offset,
    int level, int levels, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    const int entry = final_offset + 1 + level * RT_LW_W_FINAL_STATS;
    const float maxp = params[entry + 0];
    const float mean = params[entry + 1];
    const float sigma = params[entry + 2];
    if (maxp <= 0.0f || mean == 0.0f || sigma == 0.0f) {
        return;
    }

    const float sigmafin = params[final_offset];
    const __global float* curve =
        params + final_offset + 1 + levels * RT_LW_W_FINAL_STATS;

    const float logmax = (float)log((double)maxp);
    const float rapX = (mean + sigmafin * sigma) / maxp;
    /* The host's insigma is the float 0.666f promoted to double. */
    const float inx = (float)log((double)0.666f);
    const float iny = (float)log((double)rapX);
    const float rap = inx / iny;
    const float asig = 0.166f / (sigma * sigmafin);
    const float bsig = 0.5f - asig * mean;
    const float amean = 0.5f / mean;
    const float threshold = mean + sigmafin * sigma;

    for (int dir = 0; dir < 3; ++dir) {
        __global float* band =
            dir == 0 ? band1 : (dir == 1 ? band2 : band3);
        const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
        const int at = y * pitch + x;
        const float value = band[at];
        const float magnitude = fabs(value);

        float absciss;
        if (magnitude >= threshold) {
            const float vald = (log(magnitude) - logmax) * rap;
            absciss = exp(vald);
        } else if (magnitude >= mean) {
            absciss = asig * magnitude + bsig;
        } else {
            absciss = amean * magnitude;
        }

        const float kc = rt_lw_curve501(curve, absciss * 500.0f) - 0.5f;
        const float reduce = kc <= 0.0f ? 1.0f : 1.5f;
        float kinterm = 1.0f + reduce * kc;
        if (kinterm <= 0.0f) {
            kinterm = 0.01f;
        }
        band[at] = value * kinterm;
    }
}

/*
 * The final panel's pixel curve: ip_wavelet()'s tile finalize applies
 * wavclCurve to L before the block's `L <= 0 -> 1` clamp. The 65536-entry
 * LUT is the block's LUTf built with no clipping flags, so the float lookup
 * extrapolates through the last interval instead of clamping there.
 */
inline float rt_lw_curve65536(__global const float* table, float index)
{
    const int maxs = RT_LW_W_FINAL_LUT - 2;
    int base = (int)index;
    if (index < 0.0f) {
        base = 0;
    } else if (index > (float)maxs) {
        base = maxs;
    }
    const float fraction = index - (float)base;
    const float low = table[base];
    return low + (table[base + 1] - low) * fraction;
}

__kernel void rt_lab_wavelet_w_final_pixel(
    __global float* l, int pitch,
    __global const float* params, int final_offset,
    int levels, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    const __global float* curve = params + final_offset + 1 +
                                  levels * RT_LW_W_FINAL_STATS +
                                  RT_LW_W_EDGE_CURVE;
    const int at = y * pitch + x;
    const float value = l[at];
    const float lin = max(value, 0.0f);
    const float curved = rt_lw_curve65536(curve, lin);
    float out = (0.5f * lin + 1.5f * curved) / 2.0f;
    if (out <= 0.0f) {
        out = 1.0f;
    }
    l[at] = out;
}

/*
 * The toning panel's zero-protection arm: ContAllAB()'s opacity-curve
 * multiply on the a/b bands. `channel` selects the a (0) or b (1) plane.
 * The protection range (protab > 0) is a host residual, so kreduc1/kreduc2
 * are one and the host's full-resolution reference is not read.
 */
__kernel void rt_lab_wavelet_w_toning(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* params, int toning_offset,
    int level, int levels, int channel, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h || level >= RT_LW_W_TONING_MULS) {
        return;
    }

    const float mul =
        params[toning_offset + 1 + channel * RT_LW_W_TONING_MULS + level];
    if (mul == 0.0f) {
        return;
    }

    const __global float* mea =
        params + toning_offset + RT_LW_W_TONING_HEADER +
        (channel * levels + level) * RT_LW_W_TONING_MEA_ENTRIES;

    for (int dir = 0; dir < 3; ++dir) {
        __global float* band =
            dir == 0 ? band1 : (dir == 1 ? band2 : band3);
        const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
        const int at = y * pitch + x;
        const float magnitude = fabs(band[at]);

        float betaab;
        if (magnitude < mea[0]) {
            betaab = 0.05f;
        } else if (magnitude < mea[1]) {
            betaab = 0.2f;
        } else if (magnitude < mea[2]) {
            betaab = 0.7f;
        } else if (magnitude < mea[3]) {
            betaab = 1.0f;
        } else if (magnitude < mea[4]) {
            betaab = 1.0f;
        } else if (magnitude < mea[5]) {
            betaab = 0.8f;
        } else if (magnitude < mea[6]) {
            betaab = 0.6f;
        } else if (magnitude < mea[7]) {
            betaab = 0.4f;
        } else if (magnitude < mea[8]) {
            betaab = 0.2f;
        } else if (magnitude < mea[9]) {
            betaab = 0.1f;
        } else {
            betaab = 0.0f;
        }

        const float beta = (1024.0f + 50.0f * mul * betaab) / 1024.0f;
        band[at] *= beta;
    }
}

/* ------------------------------------------------------------------------
 * The standard denoise arm: WaveletDenoiseAllL(), the chroma arm the block
 * forces at six levels (WaveletDenoiseAllAB() with useNoiseCCurve and the
 * plain noise-variance plane), and the MAD histogram behind both.
 *
 * Every coefficient and table value stays single precision. The device uses
 * exp() where the host uses the scalar or vector xexpf(); the measured
 * difference is recorded in PARITY.md.
 * --------------------------------------------------------------------- */
/* Mirrors RT_LAB_WAVELET_DENOISE_HEADER and the histogram's bin count. */
#define RT_LW_W_DENOISE_HEADER 7
#define RT_LW_W_MAD_BINS 32768
#define RT_LW_W_REFINE_LEVELS 4
#define RT_LW_W_REFINE_SCALARS 11
#define RT_LW_W_REFINE_HEADER 1
#define RT_LW_W_REFINE_LUT 501

/*
 * ImProcFunctions::Mad()'s histogram, one work item per (level, direction)
 * band. The band scans in ascending row-major order and truncates the
 * absolute value to a bin, clamping at the top; each band owns its own
 * 32768-bin range, so no atomics are needed.
 */
__kernel void rt_lab_wavelet_w_mad(
    __global const float* band1, int p1,
    __global const float* band2, int p2,
    __global const float* band3, int p3,
    __global uint* histogram, int histogram_offset,
    int w, int h, int bands)
{
    const int band = get_global_id(0);
    if (band >= bands) {
        return;
    }
    const int dir = band % 3;
    __global const float* data = dir == 0 ? band1 : (dir == 1 ? band2 : band3);
    const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
    __global uint* bins = histogram + histogram_offset + band * RT_LW_W_MAD_BINS;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float magnitude = fabs(data[y * pitch + x]);
            const int bin = (int)min(32767.0f, magnitude);
            bins[bin] += 1u;
        }
    }
}

/*
 * noisevarchrom: ip_wavelet()'s chroma-magnitude ramp, written on the half
 * grid. The full-resolution loop writes each half-grid cell four times; the
 * last write wins, which is the sample at the clamped (2y+1, 2x+1) corner.
 */
__kernel void rt_lab_wavelet_w_noisevar(
    __global const float* plane_a, int a_pitch,
    __global const float* plane_b, int b_pitch,
    __global float* nvc, int nvc_pitch,
    int w, int h, int width, int height)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int px = min(2 * x + 1, width - 1);
    const int py = min(2 * y + 1, height - 1);
    const float a = plane_a[py * a_pitch + px];
    const float b = plane_b[py * b_pitch + px];
    const float chroma = sqrt(a * a + b * b);
    const float nvch = 0.6f;
    const float nvcl = 0.1f;
    const float seuil = 4000.0f;
    const float seuil2 = 15000.0f;
    const float ac = (nvch - nvcl) / (seuil - seuil2);
    const float bc = nvch - seuil * ac;
    float value;
    if (chroma < seuil) {
        value = nvch;
    } else if (chroma < seuil2) {
        value = ac * chroma + bc;
    } else {
        value = nvcl;
    }
    nvc[y * nvc_pitch + x] = value;
}

/*
 * ShrinkAllL()'s local shrinkage, before boxblur(). `vari` is vari[level] and
 * `mad` is the level's squared madL for this direction; `ballum` selects the
 * noisevarlum table entry. The host's vector and scalar forms differ only by
 * the grouping of the same product, which this form reproduces.
 */
__kernel void rt_lab_wavelet_w_shrink_l(
    __global const float* band, int band_pitch,
    __global const float* orig_l, int l_pitch,
    __global float* sfave, int sfave_pitch,
    float vari, float mad, float ballum,
    int level, int w, int h, int width, int height)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int px = min(2 * x + 1, width - 1);
    const int py = min(2 * y + 1, height - 1);
    const float l = orig_l[py * l_pitch + px];
    const float nvlh[13] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
                            1.f, 1.f, 1.f, 1.f, 0.7f, 0.5f};
    const float nvll[13] = {0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f,
                            0.45f, 0.7f, 0.8f, 1.f, 1.f, 1.f};
    const float seuillow = 3000.0f;
    const float seuilhigh = 18000.0f;
    const int index = (int)(10.0f - ballum);
    const float ac = (nvlh[index] - nvll[index]) / (seuillow - seuilhigh);
    const float bc = nvlh[index] - seuillow * ac;
    float nv;
    if (l < seuillow) {
        nv = nvlh[index];
    } else if (l < seuilhigh) {
        nv = ac * l + bc;
    } else {
        nv = nvll[index];
    }
    const float nvl = vari * nv * nv;
    const float level_factor = mad * 5.0f / (float)(level + 1);
    const float m = level_factor * nvl;
    const float value = band[y * band_pitch + x];
    const float mag = value * value;
    sfave[y * sfave_pitch + x] =
        mag / (mag + m * exp(-mag / (9.0f * m)) + 0.01f);
}

/*
 * ShrinkAllAB()'s local shrinkage. The chroma noise variance is the
 * half-grid `nvc` plane, `madab` is the band's own squared MAD and `mad_l`
 * the L band's squared MAD for the same level and direction.
 */
__kernel void rt_lab_wavelet_w_shrink_ab(
    __global const float* band_ab, int ab_pitch,
    __global const float* band_l, int l_pitch,
    __global const float* nvc, int nvc_pitch,
    __global float* sfave, int sfave_pitch,
    float mad_l, float mad_ab,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int at = y * ab_pitch + x;
    const float value_ab = band_ab[at];
    const float value_l = band_l[y * l_pitch + x];
    const float mag_ab = value_ab * value_ab;
    const float mag_l = value_l * value_l;
    const float noise = nvc[y * nvc_pitch + x] * mad_ab;
    sfave[y * sfave_pitch + x] =
        1.0f - exp(-(mag_ab / noise) - (mag_l / (9.0f * mad_l)));
}

/*
 * ShrinkAllL()/ShrinkAllAB()'s blend of the local and box-blurred shrinkage.
 */
__kernel void rt_lab_wavelet_w_shrink_apply(
    __global float* band, int band_pitch,
    __global const float* sfave, int sfave_pitch,
    __global const float* sfaved, int sfaved_pitch,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int at = y * band_pitch + x;
    const float sf = sfave[y * sfave_pitch + x];
    const float sfd = sfaved[y * sfaved_pitch + x];
    band[at] *= (sfd * sfd + sf * sf) / (sfd + sf + 0.01f);
}

/* A band snapshot: the "cur" refinement keeps the pre-denoise L set. */
__kernel void rt_lab_wavelet_w_copy(
    __global const float* src, int src_pitch,
    __global float* dst, int dst_pitch,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    dst[y * dst_pitch + x] = src[y * src_pitch + x];
}

/*
 * ip_wavelet()'s denoise-curve refinement for one (direction, level). The
 * host launches it direction-major, level-minor, and the block writes levels
 * 0..3 on every level iteration, so the four post bands are both read and
 * written and the current level's values are the ones this iteration reads.
 * `cur`/`cur_pre` are the current level's post/pre bands. The per-level
 * scalars and the 501-entry wavdenoise LUT live in the refine tail.
 */
__kernel void rt_lab_wavelet_w_refine(
    __global const float* cur, int cur_pitch,
    __global const float* cur_pre, int cur_pre_pitch,
    __global float* post0, int p0,
    __global float* post1, int p1,
    __global float* post2, int p2,
    __global float* post3, int p3,
    __global const float* pre0, int q0,
    __global const float* pre1, int q1,
    __global const float* pre2, int q2,
    __global const float* pre3, int q3,
    __global const float* params, int offset,
    int level, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    __global const float* entry =
        params + offset + RT_LW_W_REFINE_HEADER +
        level * RT_LW_W_REFINE_SCALARS;
    if (entry[0] == 0.0f) {
        return;
    }
    const float weight = entry[1];
    const float logmax = entry[2];
    const float rap = entry[3];
    const float asig = entry[4];
    const float bsig = entry[5];
    const float amean = entry[6];
    const float k = entry[7];
    const float maxval = entry[8];
    const float tempmean = entry[9];
    const float kc_factor = entry[10];
    __global const float* lut =
        params + offset + RT_LW_W_REFINE_HEADER +
        RT_LW_W_REFINE_LEVELS * RT_LW_W_REFINE_SCALARS;

    const float wav = cur[y * cur_pitch + x];
    const float prew = cur_pre[y * cur_pre_pitch + x];
    /* intp(weightL, post, pre) and fabs(). */
    const float tempwav = fabs(weight * (wav - prew) + prew);

    float absciss;
    if (tempwav >= maxval) {
        absciss = rt_lw_xexpf((rt_lw_xlogf(tempwav) - logmax) * rap);
    } else if (tempwav >= tempmean) {
        absciss = asig * tempwav + bsig;
    } else {
        absciss = 0.5f * rt_lw_pow(amean * tempwav, k);
    }

    /*
     * `sli`+Thrden feeds a FlatCurve polyline instead of the wavdenoise LUT.
     * The walk copies FlatCurve::getVal(): periodic wrap below the first knot,
     * a binary search, then linear interpolation in double.
     */
    __global const float* source = lut + RT_LW_W_REFINE_LUT;
    const int curve_mode = (int)source[0];
    const int curve_count = (int)source[1];
    float curve;
    if (curve_mode == 1 && curve_count > 1) {
        __global const float* px = source + 2;
        __global const float* py = source + 2 + curve_count;
        __global const float* ps = source + 2 + 2 * curve_count;
        double t = (double)absciss;
        if (t < (double)px[0]) {
            t += 1.0;
        }
        int lo = 0;
        int hi = curve_count - 1;
        while (hi > 1 + lo) {
            const int mid = (hi + lo) / 2;
            if ((double)px[mid] > t) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        curve = (float)((double)py[lo] +
                        (t - (double)px[lo]) * (double)ps[lo]);
    } else {
        curve = rt_lw_curve501(lut, absciss * 500.0f);
    }
    float kc = curve - 1.0f;
    if (kc < 0.0f) {
        kc = -(kc * kc);
    }
    kc *= kc_factor;
    const float reduceeffect = kc <= 0.0f ? 1.0f : 1.2f;
    float kinterm = 1.0f + reduceeffect * kc;
    kinterm = kinterm <= 0.0f ? 0.01f : kinterm;

    const int i0 = y * p0 + x;
    const int i1 = y * p1 + x;
    const int i2 = y * p2 + x;
    const int i3 = y * p3 + x;
    post0[i0] = pre0[y * q0 + x] + (post0[i0] - pre0[y * q0 + x]) * kinterm;
    post1[i1] = pre1[y * q1 + x] + (post1[i1] - pre1[y * q1 + x]) * kinterm;
    post2[i2] = pre2[y * q2 + x] + (post2[i2] - pre2[y * q2 + x]) * kinterm;
    post3[i3] = pre3[y * q3 + x] + (post3[i3] - pre3[y * q3 + x]) * kinterm;
}

/*
 * boxblur()'s horizontal pass, one work item per row. Copied from
 * lab.tools' rt_lab_tools_box_h so the running-sum arithmetic is identical.
 */
__kernel void rt_lw_box_h(__global const float* src,
                          __global float* dst,
                          const int radius,
                          const int width, const int height,
                          const int pitch)
{
    const int row = get_global_id(1);
    if (row >= height) {
        return;
    }

    __global const float* s = src + row * pitch;
    __global float* d = dst + row * pitch;
    if (radius == 0) {
        for (int col = 0; col < width; ++col) {
            d[col] = s[col];
        }
        return;
    }

    float len = (float)radius + 1.0f;
    float tempval = s[0];
    for (int j = 1; j <= radius; ++j) {
        tempval += s[j];
    }
    tempval /= len;
    d[0] = tempval;

    for (int col = 1; col <= radius; ++col) {
        tempval = (tempval * len + s[col + radius]) / (len + 1.0f);
        d[col] = tempval;
        len += 1.0f;
    }

    for (int col = radius + 1; col < width - radius; ++col) {
        const float departing = s[col - radius - 1];
        tempval = tempval + (s[col + radius] - departing) / len;
        d[col] = tempval;
    }

    for (int col = width - radius; col < width; ++col) {
        tempval = (tempval * len - s[col - radius - 1]) / (len - 1.0f);
        d[col] = tempval;
        len -= 1.0f;
    }
}

/* boxblur()'s vertical pass, one work item per column. */
__kernel void rt_lw_box_v(__global const float* src,
                          __global float* dst,
                          const int radius,
                          const int width, const int height,
                          const int pitch)
{
    const int col = get_global_id(0);
    if (col >= width) {
        return;
    }

    if (radius == 0) {
        for (int row = 0; row < height; ++row) {
            dst[row * pitch + col] = src[row * pitch + col];
        }
        return;
    }

    float len = (float)radius + 1.0f;
    float tempval = src[col];
    for (int i = 1; i <= radius; ++i) {
        tempval += src[i * pitch + col];
    }
    tempval /= len;
    dst[col] = tempval;

    for (int row = 1; row <= radius; ++row) {
        tempval = (tempval * len + src[(row + radius) * pitch + col]) /
                  (len + 1.0f);
        dst[row * pitch + col] = tempval;
        len += 1.0f;
    }

    const float rlen = 1.0f / len;
    for (int row = radius + 1; row < height - radius; ++row) {
        const float departing = src[(row - radius - 1) * pitch + col];
        tempval = tempval +
                  (src[(row + radius) * pitch + col] - departing) * rlen;
        dst[row * pitch + col] = tempval;
    }

    for (int row = height - radius; row < height; ++row) {
        const float lenm1 = len - 1.0f;
        tempval = (tempval * len -
                   src[(row - radius - 1) * pitch + col]) / lenm1;
        dst[row * pitch + col] = tempval;
        len = lenm1;
    }
}

/*
 * finalContAllL()/ContAllAB()'s CLmethod selection. `method` is 0 one,
 * 1 inf and 2 sup; `direction` is 0 all, 1 one, 2 two and 3 thr. The kernel
 * zeroes the bands the block zeroes and, at level 0, paints the coefficient-0
 * plane: L uses 12000 for the grey background and 0 for black, the a/b planes
 * always use 0, and the residual background leaves the plane untouched.
 */
__kernel void rt_lab_wavelet_w_select(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global float* coeff0, int coeff_pitch,
    int level, int w, int h,
    int method, int back_method,
    int choice_level, int direction,
    int dir1, int dir2, int max_ilev, int ab)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    if (level == 0 && back_method != 2) {
        const float background =
            (ab == 0 && back_method == 1) ? 12000.0f : 0.0f;
        coeff0[y * coeff_pitch + x] = background;
    }

    int zero_all = 0;
    int zero_pair = 0;
    if (method == 0) {
        if (direction == 0) {
            if (level != choice_level) {
                zero_all = 1;
            }
        } else if (choice_level >= max_ilev) {
            zero_all = 1;
        } else if (level != choice_level) {
            zero_pair = 1;
        }
    } else if (method == 1) {
        if (direction == 0) {
            if (level > choice_level) {
                zero_all = 1;
            }
        } else if (level > choice_level) {
            zero_pair = 1;
        }
    } else if (method == 2) {
        if (direction == 0) {
            if (level <= choice_level) {
                zero_all = 1;
            }
        } else if (choice_level >= max_ilev) {
            zero_all = 1;
        } else if (level <= choice_level) {
            zero_pair = 1;
        }
    }

    if (zero_all) {
        band1[y * p1 + x] = 0.0f;
        band2[y * p2 + x] = 0.0f;
        band3[y * p3 + x] = 0.0f;
    } else if (zero_pair) {
        if (dir1 == 1) {
            band1[y * p1 + x] = 0.0f;
        } else if (dir1 == 2) {
            band2[y * p2 + x] = 0.0f;
        } else {
            band3[y * p3 + x] = 0.0f;
        }
        if (dir2 == 1) {
            band1[y * p1 + x] = 0.0f;
        } else if (dir2 == 2) {
            band2[y * p2 + x] = 0.0f;
        } else {
            band3[y * p3 + x] = 0.0f;
        }
    }
}

/*
 * The clarity arm's merge, from the export block's final loop:
 *   L = LIM((1 + mL0) * unshar_L - mL * L, 0, 32768)
 *   a = (1 + mC0) * unshar_a - mC * a
 *   b = (1 + mC0) * unshar_b - mC * b
 * The second pass left L <= 0 raised to 1 by its final tile copy, so the
 * kernel reproduces that before the merge. When the block's merge is the
 * zero-factor arm the unshar buffers alias the working planes, which collapses
 * the arithmetic to the clamped first pass.
 */
__kernel void rt_lab_wavelet_w_clarity_merge(
    __global float* l, int l_pitch,
    __global const float* unshar_l, int unshar_l_pitch,
    __global float* a, int a_pitch,
    __global const float* unshar_a, int unshar_a_pitch,
    __global float* b, int b_pitch,
    __global const float* unshar_b, int unshar_b_pitch,
    int w, int h,
    float mL0, float mL, float mC0, float mC)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    const int at = y * l_pitch + x;
    float second = l[at];
    if (second <= 0.0f) {
        second = 1.0f;
    }
    float value =
        (1.0f + mL0) * unshar_l[y * unshar_l_pitch + x] - mL * second;
    value = fmin(fmax(value, 0.0f), 32768.0f);
    l[at] = value;

    a[y * a_pitch + x] = (1.0f + mC0) * unshar_a[y * unshar_a_pitch + x] -
                         mC * a[y * a_pitch + x];
    const int b_at = y * b_pitch + x;
    b[b_at] = (1.0f + mC0) * unshar_b[y * unshar_b_pitch + x] -
              mC * b[b_at];
}
)CLC";
