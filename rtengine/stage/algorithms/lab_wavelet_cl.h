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
#define RT_LW_W_EDG_DETECT 89
#define RT_LW_W_EDG_GRADW 90
#define RT_LW_W_EDG_TLOWW 91
#define RT_LW_W_EDG_AAMP 92
#define RT_LW_W_EDG_LIPINFL 93
#define RT_LW_W_EDG_LIPAMPL 94
#define RT_LW_W_EDG_SMOOTH 95
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
/* Mirrors the balance-chroma tail: [method][balan][ite] plus the mea block. */
#define RT_LW_W_BA_HEADER 4
#define RT_LW_W_BA_MEA_ENTRIES 10
/*
 * Mirrors RT_LAB_WAVELET_SOFTEND_*: the per-level scalars of the final soft
 * radius's coefficient-domain second pass and its level-count header.
 */
#define RT_LW_W_SOFTEND_LEVEL_SCALARS 8
#define RT_LW_W_SOFTEND_THREND_HEADER 1

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
 * ContAllL()/ContAllAB()'s balance-chroma arm. The L arm (`l_arm`) multiplies
 * each band by `1 + (kba - 1) * beta`, where beta is the sigmadir ramp the
 * host computes from the current coefficient; the a/b arm multiplies by the
 * raw kba. Both repeat the multiply `it` times, as the block's per-level
 * iteration loop does.
 *
 * `kba` follows the block's two variants: "sli" (method 1) uses the balance
 * over a luminance-dependent 600/300 divisor, and "cur" (method 2) reads the
 * per-pixel opacity curve. The curve lookup index is `6 * LL100`, clamped by
 * rt_lw_curve501() exactly as the 501-entry LUTf does. Directions 1 and 2 use
 * `1 + k` and direction 3 uses `1 - 2k`; the band array is direction-minor
 * here, so dir 0 and 1 are the "dir < 3" arm and dir 2 is the diagonal.
 */
__kernel void rt_lab_wavelet_w_ba(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* origL, int l_pitch,
    __global const float* params, int ba_offset,
    int level, int maxlvl, int levels, int w, int h, int l_arm)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }

    const int method = (int)params[ba_offset + 1];
    if (method != 1 && method != 2) {
        return;
    }
    const float bal = params[ba_offset + 2];
    const int ite = (int)params[ba_offset + 3];

    const int med = maxlvl / 2;
    int it;
    if (level < med) {
        it = 7 - ite;
    } else if (level == med) {
        it = 7;
    } else {
        it = 7 + ite;
    }

    const float LL100 = origL[(2 * y) * l_pitch + 2 * x] / 327.68f;

    __global const float* mea =
        params + ba_offset + RT_LW_W_BA_HEADER +
        level * RT_LW_W_BA_MEA_ENTRIES;
    __global const float* curve =
        params + ba_offset + RT_LW_W_BA_HEADER +
        levels * RT_LW_W_BA_MEA_ENTRIES;

    for (int dir = 0; dir < 3; ++dir) {
        __global float* band =
            dir == 0 ? band1 : (dir == 1 ? band2 : band3);
        const int pitch = dir == 0 ? p1 : (dir == 1 ? p2 : p3);
        const int at = y * pitch + x;
        float value = band[at];

        float kba;
        if (method == 2) {
            const float k1 =
                0.3f * (rt_lw_curve501(curve, 6.0f * LL100) - 0.5f);
            const float k2 = k1 * 2.0f;
            kba = dir < 2 ? 1.0f + k1 : 1.0f - k2;
        } else {
            const float aa = 4970.0f;
            const float bb = -397000.0f;
            const float b0 = 100000.0f;
            const float a0 = -4970.0f;
            float k1 = 600.0f;
            float k2 = 300.0f;
            if (LL100 > 80.0f) {
                k1 = aa * LL100 + bb;
                k2 = 0.5f * k1;
            }
            if (LL100 < 20.0f) {
                k1 = a0 * LL100 + b0;
                k2 = 0.5f * k1;
            }
            kba = dir < 2 ? 1.0f + bal / k1 : 1.0f - bal / k2;
        }

        if (l_arm != 0) {
            const float wavcl = fabs(value);
            float beta;
            if (wavcl < mea[0]) {
                beta = 0.05f;
            } else if (wavcl < mea[1]) {
                beta = 0.2f;
            } else if (wavcl < mea[2]) {
                beta = 0.7f;
            } else if (wavcl < mea[3]) {
                beta = 1.0f;
            } else if (wavcl < mea[4]) {
                beta = 1.0f;
            } else if (wavcl < mea[5]) {
                beta = 0.8f;
            } else if (wavcl < mea[6]) {
                beta = 0.6f;
            } else if (wavcl < mea[7]) {
                beta = 0.4f;
            } else if (wavcl < mea[8]) {
                beta = 0.2f;
            } else if (wavcl < mea[9]) {
                beta = 0.1f;
            } else {
                beta = 0.01f;
            }
            const float factor = 1.0f + (kba - 1.0f) * beta;
            for (int j = 0; j < it; ++j) {
                value *= factor;
            }
        } else {
            for (int j = 0; j < it; ++j) {
                value *= kba;
            }
        }
        band[at] = value;
    }
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

/*
 * calckoe() (ipwavelet.cc:3362) and the Lipschitz control
 * (ipwavelet.cc:2915-3035). The host computes the control only when
 * `cp.detectedge && cp.val > 0`; the launcher runs these kernels only for the
 * Medianlev arm.
 *
 * The raw pass reproduces calckoe()'s convolution, the `thr` clamp and the
 * min(thr2, |tmC / temp|) ratio. The running maximum that calckoe() folds into
 * each coefficient is a raster-order prefix maximum, so the resolve pass
 * replays it row by row instead of approximating it with the global maximum.
 * The control pass then rebuilds the block's direction-combination `interm`
 * and, when NPMethod and Lipst select the neighbour smoothing, replays the
 * in-place raster scan the host performs.
 */
inline float rt_lw_koe_tmc3(__global const float* band, int pitch, int i,
                            int j, float c0, float c1, float c2)
{
    return c0 * band[i * pitch + j] +
           c1 * ((band[(i - 1) * pitch + j] + band[(i + 1) * pitch + j]) +
                 (band[i * pitch + j + 1] + band[i * pitch + j - 1])) +
           c2 * ((band[(i - 1) * pitch + j - 1] +
                  band[(i - 1) * pitch + j + 1]) +
                 (band[(i + 1) * pitch + j - 1] +
                  band[(i + 1) * pitch + j + 1]));
}

inline float rt_lw_koe_tmc5(__global const float* band, int pitch, int i,
                            int j, float c0, float c1, float c2, float c3,
                            float c4, float c5)
{
    return c0 * band[i * pitch + j] +
           c1 * ((band[(i - 1) * pitch + j] + band[(i + 1) * pitch + j]) +
                 (band[i * pitch + j + 1] + band[i * pitch + j - 1])) +
           c2 * ((band[(i - 1) * pitch + j - 1] +
                  band[(i - 1) * pitch + j + 1]) +
                 (band[(i + 1) * pitch + j - 1] +
                  band[(i + 1) * pitch + j + 1])) +
           c3 * ((band[(i - 2) * pitch + j] + band[(i + 2) * pitch + j]) +
                 (band[i * pitch + j - 2] + band[i * pitch + j + 2])) +
           c4 * ((band[(i - 2) * pitch + j - 1] +
                  band[(i - 2) * pitch + j + 1]) +
                 (band[(i + 2) * pitch + j + 1] +
                  band[(i + 2) * pitch + j - 1]) +
                 (band[(i - 1) * pitch + j - 2] +
                  band[(i - 1) * pitch + j + 2]) +
                 (band[(i + 1) * pitch + j + 2] +
                  band[(i + 1) * pitch + j - 2])) +
           c5 * ((band[(i - 2) * pitch + j - 2] +
                  band[(i - 2) * pitch + j + 2]) +
                 (band[(i + 2) * pitch + j - 2] +
                  band[(i + 2) * pitch + j + 2]));
}

/* One work item per band pixel; the three directions in one pass. */
__kernel void rt_lab_wavelet_w_koe_raw(
    __global const float* band1, int p1,
    __global const float* band2, int p2,
    __global const float* band3, int p3,
    __global float* koe1, __global float* koe2, __global float* koe3,
    int kp, int w, int h, int level, int tloww, int gradw)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int at = y * kp + x;
    const int borderL = tloww < 75 ? 1 : 2;
    /* calckoe() zeroes the coefficients when the 5x5 arm meets level 0/1. */
    if (tloww >= 75 && level <= 1) {
        koe1[at] = 0.0f;
        koe2[at] = 0.0f;
        koe3[at] = 0.0f;
        return;
    }
    /* calckoe() fills the border ring with 1.f. */
    if (x < borderL || y < borderL || x >= w - borderL ||
        y >= h - borderL) {
        koe1[at] = 1.0f;
        koe2[at] = 1.0f;
        koe3[at] = 1.0f;
        return;
    }

    const float edd = 3.0f;
    const float thr = 40.0f;
    const float thr2 = 1.5f * edd + (float)gradw / 30.0f;
    float t1;
    float t2;
    float t3;

    if (tloww < 75) {
        float c0;
        float c1;
        float c2;
        float mult;
        if (tloww < 30) {
            c0 = 8.94f;
            c1 = 1.71f;
            c2 = 0.33f;
            mult = 0.0584795f;
        } else if (tloww < 50) {
            c0 = 4.0091f;
            c1 = 2.0068f;
            c2 = 1.0045f;
            mult = 0.062288f;
        } else {
            c0 = 3.025f;
            c1 = 2.001f;
            c2 = 1.323f;
            mult = 0.06127f;
        }
        c0 *= mult;
        c1 *= mult;
        c2 *= mult;
        t1 = rt_lw_koe_tmc3(band1, p1, y, x, c0, c1, c2);
        t2 = rt_lw_koe_tmc3(band2, p2, y, x, c0, c1, c2);
        t3 = rt_lw_koe_tmc3(band3, p3, y, x, c0, c1, c2);
    } else {
        float c0;
        float c1;
        float c2;
        float c3;
        float c4;
        float c5;
        float mult;
        if (tloww < 85) {
            c0 = 15.0f;
            c1 = 10.0f;
            c2 = 7.0f;
            c3 = 3.0f;
            c4 = 2.0f;
            c5 = 0.5f;
            mult = 0.0088495f;
        } else {
            c0 = 15.0f;
            c1 = 12.0f;
            c2 = 9.0f;
            c3 = 5.0f;
            c4 = 4.0f;
            c5 = 2.0f;
            mult = 0.0062893f;
        }
        c0 *= mult;
        c1 *= mult;
        c2 *= mult;
        c3 *= mult;
        c4 *= mult;
        c5 *= mult;
        t1 = rt_lw_koe_tmc5(band1, p1, y, x, c0, c1, c2, c3, c4, c5);
        t2 = rt_lw_koe_tmc5(band2, p2, y, x, c0, c1, c2, c3, c4, c5);
        t3 = rt_lw_koe_tmc5(band3, p3, y, x, c0, c1, c2, c3, c4, c5);
    }

    const float v1 = band1[y * p1 + x];
    const float v2 = band2[y * p2 + x];
    const float v3 = band3[y * p3 + x];
    koe1[at] = min(thr2, fabs(t1 / max(fabs(v1), thr)));
    koe2[at] = min(thr2, fabs(t2 / max(fabs(v2), thr)));
    koe3[at] = min(thr2, fabs(t3 / max(fabs(v3), thr)));
}

/* One work item per (direction, row): the row's interior maximum. */
__kernel void rt_lab_wavelet_w_koe_rowmax(
    __global float* koe1, __global float* koe2, __global float* koe3,
    int kp, __global float* stats, int row_offset, int w, int h,
    int borderL)
{
    const int dir = get_global_id(0);
    const int y = get_global_id(1);
    if (dir >= 3 || y >= h) {
        return;
    }
    __global float* koe = dir == 0 ? koe1 : (dir == 1 ? koe2 : koe3);
    float m = 0.0f;
    if (y >= borderL && y < h - borderL) {
        for (int j = borderL; j < w - borderL; ++j) {
            m = max(m, koe[y * kp + j]);
        }
    }
    stats[row_offset + dir * h + y] = m;
}

/*
 * One work item per direction: the prefix maximum over the rows above, and
 * the global maximum calckoe() returns. `max` starts at zero exactly as the
 * host's maxkoeLi does.
 */
__kernel void rt_lab_wavelet_w_koe_carry(
    __global float* stats, int row_offset, int carry_offset,
    int max_offset, int h)
{
    const int dir = get_global_id(0);
    if (dir >= 3) {
        return;
    }
    float m = 0.0f;
    for (int i = 0; i < h; ++i) {
        stats[carry_offset + dir * h + i] = m;
        const float r = stats[row_offset + dir * h + i];
        if (r > m) {
            m = r;
        }
    }
    stats[max_offset + dir] = m;
}

/* One work item per (direction, row): replay calckoe()'s diff fold. */
__kernel void rt_lab_wavelet_w_koe_resolve(
    __global float* koe1, __global float* koe2, __global float* koe3,
    int kp, __global const float* stats, int carry_offset, int w, int h,
    int borderL, float diff_factor)
{
    const int dir = get_global_id(0);
    const int y = get_global_id(1);
    if (dir >= 3 || y >= h || y < borderL || y >= h - borderL) {
        return;
    }
    __global float* koe = dir == 0 ? koe1 : (dir == 1 ? koe2 : koe3);
    float m = stats[carry_offset + dir * h + y];
    for (int j = borderL; j < w - borderL; ++j) {
        const int at = y * kp + j;
        const float k = koe[at];
        if (k > m) {
            m = k;
        }
        float diff = m - k;
        diff *= diff_factor;
        koe[at] = m - diff;
    }
}

inline float rt_lw_koe_interm(float k1, float k2, float k3,
                              float eddetthr, float eddlow,
                              float eddlipinfl, float eddlipampl,
                              float aamp)
{
    float sum = k1 * k1;
    sum += k2 * k2;
    sum += k3 * k3;
    float interm = sqrt(sum);
    interm *= 0.57736721f;

    const float eps = 0.0001f;
    float alph = k1 / (k2 + eps);
    float beta = k3 / (k2 + eps);
    const float alipinfl = (eddlipampl - 1.0f) / (1.0f - eddlipinfl);
    const float blipinfl = eddlipampl - alipinfl;
    if (alph > 1.0f) {
        alph = 1.0f / alph;
    }
    if (beta > 1.0f) {
        beta = 1.0f / beta;
    }
    float bet = 1.0f;
    if (alph > eddlipinfl && beta < 0.85f * eddlipinfl) {
        bet = beta;
    }
    float kampli;
    if (alph > eddlipinfl) {
        const float amplip = alipinfl * alph + blipinfl;
        kampli = (bet * bet) * amplip * aamp;
    } else {
        const float t = alph * bet;
        const float sq = t * t;
        const float amplip = (1.0f / eddlipinfl) * (sq * sq);
        kampli = amplip / aamp;
    }
    interm *= kampli;
    if (interm < eddetthr / eddlow) {
        interm = 0.01f;
    }
    return interm;
}

/* The control's direction combination, one work item per pixel. */
__kernel void rt_lab_wavelet_w_koe_control(
    __global float* koe1, __global float* koe2, __global float* koe3,
    int kp, int w, int h, float eddetthr, float eddlow, float eddlipinfl,
    float eddlipampl, float aamp)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) {
        return;
    }
    const int at = y * kp + x;
    const float interm = rt_lw_koe_interm(
        koe1[at], koe2[at], koe3[at], eddetthr, eddlow, eddlipinfl,
        eddlipampl, aamp);
    koe1[at] = interm;
    koe2[at] = interm;
    koe3[at] = interm;
}

/*
 * The same control with the NPMethod neighbour smoothing. The host's
 * smoothing is an in-place raster scan: a pixel reads the already-updated
 * upper and left neighbours and the still-raw lower and right ones, so it
 * cannot be parallelised per pixel and is replayed by one work item.
 */
__kernel void rt_lab_wavelet_w_koe_control_smooth(
    __global float* koe1, __global float* koe2, __global float* koe3,
    int kp, int w, int h, float eddetthr, float eddlow, float eddlipinfl,
    float eddlipampl, float aamp, int smoothing)
{
    if (get_global_id(0) != 0) {
        return;
    }
    const float kneigh = smoothing == 2 ? 28.0f : 38.0f;
    const float somm = smoothing == 2 ? 40.0f : 50.0f;
    for (int i = 1; i < h - 1; ++i) {
        for (int j = 1; j < w - 1; ++j) {
            const int at = i * kp + j;
            __global float* koe[3] = {koe1, koe2, koe3};
            for (int dir = 0; dir < 3; ++dir) {
                __global float* b = koe[dir];
                b[at] = (kneigh * b[at] +
                         2.0f * b[(i - 1) * kp + j] +
                         2.0f * b[(i + 1) * kp + j] +
                         2.0f * b[i * kp + j + 1] +
                         2.0f * b[i * kp + j - 1] +
                         b[(i - 1) * kp + j - 1] +
                         b[(i - 1) * kp + j + 1] +
                         b[(i + 1) * kp + j - 1] +
                         b[(i + 1) * kp + j + 1]) / somm;
            }
            const float interm = rt_lw_koe_interm(
                koe1[at], koe2[at], koe3[at], eddetthr, eddlow, eddlipinfl,
                eddlipampl, aamp);
            koe1[at] = interm;
            koe2[at] = interm;
            koe3[at] = interm;
        }
    }
}

__kernel void rt_lab_wavelet_w_edge(
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global float* band3, int p3,
    __global const float* params, int edge_offset,
    int level, int levels, int w, int h,
    __global const float* koe, int kp,
    __global const float* koe_stats, int stats_max_offset,
    int detectedge)
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
        /*
         * ContAllL()'s Lipschitz branch: with cp.detectedge set and level < 4
         * the precomputed `interm` (koeLi[level * 3]) scales the strength.
         * Levels 4 and above keep the plain edgePrecalc.
         */
        if (detectedge != 0) {
            if (level < 4) {
                const float maxkoe = koe_stats[stats_max_offset + dir];
                edge = 1.0f +
                       (edgePrecalc - 1.0f) * koe[y * kp + x] /
                           (1.0f + 0.9f * maxkoe);
            } else {
                edge = edgePrecalc;
            }
        }
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
 * kernel reproduces those polynomials (rt_lw_xlogf()/rt_lw_xexpf()) so the
 * reconstructed L, which the balance-chroma a/b arm reads, stays within one
 * LSB16. The scalars use double log() as the host does.
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
            const float vald = (rt_lw_xlogf(magnitude) - logmax) * rap;
            absciss = rt_lw_xexpf(vald);
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
#define RT_LW_W_REFINE_LEVELS 6
#define RT_LW_W_REFINE_LEVELS_STANDARD 4
#define RT_LW_W_REFINE_SCALARS 11
#define RT_LW_W_REFINE_HEADER 1
#define RT_LW_W_REFINE_LUT 501
#define RT_LW_W_REFINE_CURVE_HEADER 2

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
 * The FlatCurve hue helpers are defined with the final soft radius's cross
 * terms further down; the L denoise pass samples the same wavhuecurve
 * polyline, so it needs them in scope here.
 */
inline float rt_lw_softend_xatan2f(float y, float x);
inline double rt_lw_softend_huehsv2(float hh);
inline double rt_lw_softend_polyline(__global const float* source, double t);

/*
 * ShrinkAllL()'s local shrinkage, before boxblur(). `vari` is vari[level] and
 * `mad` is the level's squared madL for this direction; `ballum` selects the
 * noisevarlum table entry. The host's vector and scalar forms differ only by
 * the grouping of the same product, which this form reproduces. When
 * `hue_active` is set the block has multiplied noisevarlum by
 * `1 + 2 * (wavhuecurve(huelab_to_huehsv2(xatan2f(b, a))) - 0.5)` at the
 * last full-resolution pixel of each half-grid cell, so the same sample is
 * modulated here before the square.
 */
__kernel void rt_lab_wavelet_w_shrink_l(
    __global const float* band, int band_pitch,
    __global const float* orig_l, int l_pitch,
    __global const float* plane_a, int a_pitch,
    __global const float* plane_b, int b_pitch,
    __global const float* params, int hue_offset, int hue_active,
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
    if (hue_active != 0) {
        /*
         * The host's two full-resolution loops both write the half grid.
         * The ramp loop overwrites, so the value left in a cell is the
         * clamped (2y+1, 2x+1) corner sample; the hue loop then multiplies
         * that value once per full-resolution pixel in the cell, in row-major
         * order. The product therefore spans the whole 1x1..2x2 cell, not
         * just the corner. Reproduce the sequential float multiplies.
         */
        const int y_end = min(2 * y + 1, height - 1);
        const int x_end = min(2 * x + 1, width - 1);
        for (int iy = 2 * y; iy <= y_end; ++iy) {
            for (int ix = 2 * x; ix <= x_end; ++ix) {
                const float a = plane_a[iy * a_pitch + ix];
                const float b = plane_b[iy * b_pitch + ix];
                const float hue = rt_lw_softend_xatan2f(b, a);
                const double hsv = rt_lw_softend_huehsv2(hue);
                const float curve =
                    (float)rt_lw_softend_polyline(params + hue_offset, hsv);
                nv *= 1.0f + 2.0f * (curve - 0.5f);
            }
        }
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
 * WaveletDenoiseAll_BiShrinkAB()'s local shrink for a level below the
 * coarsest. Unlike ShrinkAllAB() it applies `SQR(1 - exp(...))` directly,
 * with no box blur, and the chroma noise variance is `noisevarchrom *
 * (noisevar_ab * madab)`. `mad_ab` is the block's squared MAD and `mad_l` the
 * L band's for the same level and direction; the L term uses the host's
 * vectorised grouping, `mag_L * (1 / (9 * mad_L))`.
 */
__kernel void rt_lab_wavelet_w_bishrink_ab(
    __global float* band_ab, int ab_pitch,
    __global const float* band_l, int l_pitch,
    __global const float* nvc, int nvc_pitch,
    float mad_l, float mad_ab, float noisevar,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    /* The block runs the whole multiply only above this noise variance. */
    if (noisevar <= 0.001f) {
        return;
    }
    const int at = y * ab_pitch + x;
    const float value_ab = band_ab[at];
    const float value_l = band_l[y * l_pitch + x];
    const float mag_ab = value_ab * value_ab;
    const float mag_l = value_l * value_l;
    const float mad_abr = noisevar * mad_ab;
    const float noise = nvc[y * nvc_pitch + x] * mad_abr;
    const float rmad_l = 1.0f / (mad_l * 9.0f);
    const float factor =
        1.0f - exp(-(mag_ab / noise) - (mag_l * rmad_l));
    band_ab[at] = value_ab * (factor * factor);
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
 * FlatCurve::getVal() over an encoded polyline block: [mode][count] followed
 * by count x values, count y values and count-1 slopes. The walk copies the
 * host: periodic wrap below the first knot, a binary search, then linear
 * interpolation in double.
 */
inline float rt_lw_refine_curve(__global const float* block, float absciss)
{
    const int curve_mode = (int)block[0];
    const int curve_count = (int)block[1];
    if (curve_mode != 1 || curve_count <= 1) {
        return 1.0f;
    }
    __global const float* px = block + 2;
    __global const float* py = block + 2 + curve_count;
    __global const float* ps = block + 2 + 2 * curve_count;
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
    return (float)((double)py[lo] + (t - (double)px[lo]) * (double)ps[lo]);
}

/*
 * ip_wavelet()'s denoise-curve refinement for one (direction, level). The
 * host launches it direction-major, level-minor. The standard pairing
 * (`expert` zero) writes bands 0..3 on every level iteration, so the four
 * post bands are both read and written and the current level's values are
 * the ones this iteration reads. The expert pairing writes bands 0..3 on the
 * first four iterations and bands 4 and 5 on all six, and applies the wavmed
 * factor between the two. `cur`/`cur_pre` are the current level's post/pre
 * bands. The per-level scalars, the 501-entry wavdenoise LUT and the three
 * curve blocks live in the refine tail.
 */
__kernel void rt_lab_wavelet_w_refine(
    __global const float* cur, int cur_pitch,
    __global const float* cur_pre, int cur_pre_pitch,
    __global float* post0, int p0,
    __global float* post1, int p1,
    __global float* post2, int p2,
    __global float* post3, int p3,
    __global float* post4, int p4,
    __global float* post5, int p5,
    __global const float* pre0, int q0,
    __global const float* pre1, int q1,
    __global const float* pre2, int q2,
    __global const float* pre3, int q3,
    __global const float* pre4, int q4,
    __global const float* pre5, int q5,
    __global const float* params, int offset,
    int level, int w, int h, int expert)
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
     * `sli`+Thrden feeds the wavlow FlatCurve polyline instead of the
     * wavdenoise LUT.
     */
    __global const float* low = lut + RT_LW_W_REFINE_LUT;
    const int curve_mode = (int)low[0];
    const int curve_count = (int)low[1];
    float curve;
    if (curve_mode == 1 && curve_count > 1) {
        curve = rt_lw_refine_curve(low, absciss);
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

    if (level < RT_LW_W_REFINE_LEVELS_STANDARD) {
        const int i0 = y * p0 + x;
        const int i1 = y * p1 + x;
        const int i2 = y * p2 + x;
        const int i3 = y * p3 + x;
        post0[i0] = pre0[y * q0 + x] + (post0[i0] - pre0[y * q0 + x]) * kinterm;
        post1[i1] = pre1[y * q1 + x] + (post1[i1] - pre1[y * q1 + x]) * kinterm;
        post2[i2] = pre2[y * q2 + x] + (post2[i2] - pre2[y * q2 + x]) * kinterm;
        post3[i3] = pre3[y * q3 + x] + (post3[i3] - pre3[y * q3 + x]) * kinterm;
    }
    if (expert == 0) {
        return;
    }
    /*
     * The expert pairing: the wavmed factor on bands 0..3 when Limden is
     * positive (its block's mode is zero otherwise), then the wavhigh factor
     * on bands 4 and 5. Both walk their own polyline at the same abscissa.
     */
    __global const float* med =
        low + RT_LW_W_REFINE_CURVE_HEADER + 3 * curve_count;
    if ((int)med[0] == 1) {
        float kcmed = rt_lw_refine_curve(med, absciss) - 1.0f;
        if (kcmed < 0.0f) {
            kcmed = -(kcmed * kcmed);
        }
        /* The block derives this factor from its own curve value, without
         * the low curve's kcFactor; only `reduceeffect` is shared. */
        float kintermed = 1.0f + reduceeffect * kcmed;
        kintermed = kintermed <= 0.0f ? 0.01f : kintermed;
        const int i0 = y * p0 + x;
        const int i1 = y * p1 + x;
        const int i2 = y * p2 + x;
        const int i3 = y * p3 + x;
        post0[i0] = pre0[y * q0 + x] +
                    (post0[i0] - pre0[y * q0 + x]) * kintermed;
        post1[i1] = pre1[y * q1 + x] +
                    (post1[i1] - pre1[y * q1 + x]) * kintermed;
        post2[i2] = pre2[y * q2 + x] +
                    (post2[i2] - pre2[y * q2 + x]) * kintermed;
        post3[i3] = pre3[y * q3 + x] +
                    (post3[i3] - pre3[y * q3 + x]) * kintermed;
    }
    __global const float* high =
        med + RT_LW_W_REFINE_CURVE_HEADER + 3 * (int)med[1];
    if ((int)high[0] == 1) {
        float kchigh = rt_lw_refine_curve(high, absciss) - 1.0f;
        if (kchigh < 0.0f) {
            kchigh = -(kchigh * kchigh);
        }
        float kintermhigh = 1.0f + reduceeffect * kchigh;
        kintermhigh = kintermhigh <= 0.0f ? 0.01f : kintermhigh;
        const int i4 = y * p4 + x;
        const int i5 = y * p5 + x;
        post4[i4] = pre4[y * q4 + x] +
                    (post4[i4] - pre4[y * q4 + x]) * kintermhigh;
        post5[i5] = pre5[y * q5 + x] +
                    (post5[i5] - pre5[y * q5 + x]) * kintermhigh;
    }
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
 * The clarity arm's guided soft radius: guidedfilter.cc's guidedFilter() plus
 * the export block's provradius guide and L write-back. The host computes
 * every derived scalar with the block's own arithmetic (the epsilon, the
 * subsampled extent, the box-blur radius and the two bilinear scale pairs), so
 * the kernels below only evaluate the per-pixel steps. The box blur is
 * rt_lw_box_h()/_v() above; the block's six f_mean() calls run through it with
 * the radius the host derived.
 */

/* The block's own Color::cachefy table, uploaded at every integer index. */
#define RT_LW_SOFT_LUT_ENTRIES 65536
#define RT_LW_SOFT_LUT_MAXS 65534.0f
/* Color::Lab2XYZ()'s constants, in the host's types. */
#define RT_LW_SOFT_KAPPA (24389.0 / 27.0)
#define RT_LW_SOFT_KAPPAF (float)(24389.0 / 27.0)
#define RT_LW_SOFT_C1BY116 (float)(1.0 / 116.0)
#define RT_LW_SOFT_C16BY116 (float)(16.0 / 116.0)
#define RT_LW_SOFT_EPSCAP 8.0f

/*
 * Color::Lab2XYZ()'s Y. Y depends on L alone, so the guide and the source
 * plane read one value per pixel; the block's a and b arguments only reach X
 * and Z, which the L write-back never reads.
 */
inline float rt_lw_soft_lab_y(float l)
{
    const float ll = l / 327.68f;
    const float fy = (RT_LW_SOFT_C1BY116 * ll) + RT_LW_SOFT_C16BY116;
    return (ll > RT_LW_SOFT_EPSCAP) ? 65535.0f * fy * fy * fy
                                    : 65535.0f * ll / RT_LW_SOFT_KAPPAF;
}

/*
 * LUTf::operator[](float) with LUT_CLIP_BELOW | LUT_CLIP_ABOVE, the lookup
 * Color::XYZ2Lab() uses for f in [0, 65535]. The table's size is
 * RT_LW_SOFT_LUT_ENTRIES, so its last interpolable index is two below it.
 */
inline float rt_lw_soft_lut(__global const float* table, float index)
{
    if (index < 0.0f) {
        return table[0];
    }
    if (index > RT_LW_SOFT_LUT_MAXS) {
        return table[RT_LW_SOFT_LUT_ENTRIES - 1];
    }
    const int base = (int)index;
    const float low = table[base];
    return low + (table[base + 1] - low) * (index - (float)base);
}

/* sleef.h's mlaf(), left uncontracted as the host builds it. */
inline float rt_lw_soft_mlaf(float x, float y, float z)
{
    const float product = x * y;
    return product + z;
}

/* sleef.h's ilogbp1f(), ldexpkf(), mulsignf() and xcbrtf(), copied verbatim
 * so Color::computeXYZ2LabY()'s out-of-range branch rounds as the host does. */
inline int rt_lw_soft_ilogbp1(float d)
{
    const int m = d < 5.421010862427522E-20f;
    d = m ? 1.8446744073709552E19f * d : d;
    int q = (as_int(d) >> 23) & 0xff;
    q = m ? q - (64 + 0x7e) : q - 0x7e;
    return q;
}

inline float rt_lw_soft_ldexpk(float x, int q)
{
    int m = q >> 31;
    m = (((m + q) >> 6) - m) << 4;
    q = q - (m << 2);
    float u = as_float((int)(m + 0x7f) << 23);
    u = u * u;
    x = x * u * u;
    u = as_float((int)(q + 0x7f) << 23);
    return x * u;
}

inline float rt_lw_soft_mulsign(float x, float y)
{
    return as_float(as_int(x) ^ (as_int(y) & (int)0x80000000u));
}

inline float rt_lw_soft_cbrtf(float d)
{
    float x;
    float q = 1.0f;
    const int e = rt_lw_soft_ilogbp1(d);
    d = rt_lw_soft_ldexpk(d, -e);
    const int r = (e + 6144) % 3;
    q = (r == 1) ? 1.2599210498948731647672106f : q;
    q = (r == 2) ? 1.5874010519681994747517056f : q;
    q = rt_lw_soft_ldexpk(q, (e + 6144) / 3 - 2048);
    q = rt_lw_soft_mulsign(q, d);
    d = fabs(d);

    x = -0.601564466953277587890625f;
    x = rt_lw_soft_mlaf(x, d, 2.8208892345428466796875f);
    x = rt_lw_soft_mlaf(x, d, -5.532182216644287109375f);
    x = rt_lw_soft_mlaf(x, d, 5.898262500762939453125f);
    x = rt_lw_soft_mlaf(x, d, -3.8095417022705078125f);
    x = rt_lw_soft_mlaf(x, d, 2.2241256237030029296875f);

    const float y = d * x * x;
    return (y - (2.0f / 3.0f) * y * (y * x - 1.0f)) * q;
}

/* Color::computeXYZ2LabY(), the L write-back's only conversion. */
inline float rt_lw_soft_xyz_l(float f, __global const float* table)
{
    if (f < 0.0f) {
        return (float)(327.68f * (RT_LW_SOFT_KAPPA * (double)f / 65535.0));
    }
    if (f > 65535.0f) {
        return 327.68f * (116.0f * rt_lw_soft_cbrtf(f / 65535.0f) - 16.0f);
    }
    return rt_lw_soft_lut(table, f);
}

/*
 * rescaleBilinear()'s getBilinearValue(): integer and fractional parts by
 * truncation, the neighbour clamped to the plane's last row or column.
 */
/* rt_math.h's intp(), left uncontracted as the host builds it. */
inline float rt_lw_soft_intp(float a, float b, float c)
{
    const float difference = b - c;
    const float product = a * difference;
    return product + c;
}

inline float rt_lw_soft_bilinear(__global const float* src, int pitch, int width,
                                 int height, float x, float y)
{
    const int xi = (int)x;
    const int yi = (int)y;
    const float xf = x - (float)xi;
    const float yf = y - (float)yi;
    const int xi1 = min(xi + 1, width - 1);
    const int yi1 = min(yi + 1, height - 1);
    const float bl = src[yi * pitch + xi];
    const float br = src[yi * pitch + xi1];
    const float tl = src[yi1 * pitch + xi];
    const float tr = src[yi1 * pitch + xi1];
    const float bottom = rt_lw_soft_intp(xf, br, bl);
    const float top = rt_lw_soft_intp(xf, tr, tl);
    return rt_lw_soft_intp(yf, top, bottom);
}

/* The clarity arm's provradius guide: the pre-pass Y / 32768. */
__kernel void rt_lab_wavelet_w_soft_guide(
    __global const float* l, int l_pitch,
    __global float* guide, int guide_pitch,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    guide[y * guide_pitch + x] = rt_lw_soft_lab_y(l[y * l_pitch + x]) / 32768.0f;
}

/*
 * The second pass' Y / 32768 into `src`, with ip_wavelet()'s final tile copy
 * applied to L first: the export block reads the clamped pass, so the guided
 * filter and the merge must see the same value.
 */
__kernel void rt_lab_wavelet_w_soft_src(
    __global float* l, int l_pitch,
    __global float* src, int src_pitch,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const float value = l[y * l_pitch + x];
    const float clamped = value <= 0.0f ? 1.0f : value;
    src[y * src_pitch + x] = rt_lw_soft_lab_y(clamped) / 32768.0f;
}

/* f_subsample(): both guidances at once, since they share the scales. */
__kernel void rt_lab_wavelet_w_soft_rescale(
    __global const float* guide, int guide_pitch,
    __global float* guide_out, int guide_out_pitch,
    __global const float* src, int src_pitch,
    __global float* src_out, int src_out_pitch,
    int width, int height, int out_w, int out_h,
    float col_scale, float row_scale)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= out_w || y >= out_h) {
        return;
    }
    const float sx = (float)x * col_scale;
    const float sy = (float)y * row_scale;
    guide_out[y * guide_out_pitch + x] =
        rt_lw_soft_bilinear(guide, guide_pitch, width, height, sx, sy);
    src_out[y * src_out_pitch + x] =
        rt_lw_soft_bilinear(src, src_pitch, width, height, sx, sy);
}

/* apply(MUL, corrIp, I1, p1) and apply(MUL, corrI, I1, I1) together. */
__kernel void rt_lab_wavelet_w_soft_mul(
    __global const float* guide, int guide_pitch,
    __global const float* src, int src_pitch,
    __global float* corr_p, int corr_p_pitch,
    __global float* corr_i, int corr_i_pitch,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const float i = guide[y * guide_pitch + x];
    corr_p[y * corr_p_pitch + x] = i * src[y * src_pitch + x];
    corr_i[y * corr_i_pitch + x] = i * i;
}

/*
 * The per-pixel reduction: varI, covIp, a and b. Each is the block's own
 * single operation, in the block's order.
 */
__kernel void rt_lab_wavelet_w_soft_reduce(
    __global const float* mean_i, int mean_i_pitch,
    __global const float* mean_p, int mean_p_pitch,
    __global const float* corr_i, int corr_i_pitch,
    __global const float* corr_p, int corr_p_pitch,
    __global float* a_out, int a_pitch,
    __global float* b_out, int b_pitch,
    int w, int h, float epsilon)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const float mi = mean_i[y * mean_i_pitch + x];
    const float mp = mean_p[y * mean_p_pitch + x];
    const float var_i = corr_i[y * corr_i_pitch + x] - mi * mi;
    const float cov_ip = corr_p[y * corr_p_pitch + x] - mi * mp;
    const float a = cov_ip / (var_i + epsilon);
    a_out[y * a_pitch + x] = a;
    b_out[y * b_pitch + x] = mp - a * mi;
}

/* q = bilinear(meana) * I + bilinear(meanb), the block's final loop. */
__kernel void rt_lab_wavelet_w_soft_upsample(
    __global const float* mean_a, int mean_a_pitch,
    __global const float* mean_b, int mean_b_pitch,
    __global const float* guide, int guide_pitch,
    __global float* out, int out_pitch,
    int width, int height, int small_w, int small_h,
    float col_scale, float row_scale)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const float sx = (float)x * col_scale;
    const float sy = (float)y * row_scale;
    const float a = rt_lw_soft_bilinear(mean_a, mean_a_pitch, small_w, small_h,
                                        sx, sy);
    const float b = rt_lw_soft_bilinear(mean_b, mean_b_pitch, small_w, small_h,
                                        sx, sy);
    out[y * out_pitch + x] = a * guide[y * guide_pitch + x] + b;
}

/*
 * ip_wavelet()'s final tile copy: an L at or below zero becomes one. The
 * clarity second pass zeroes bands, so its reconstruction can undershoot.
 */
__kernel void rt_lab_wavelet_w_clamp_l(
    __global float* l, int l_pitch, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int at = y * l_pitch + x;
    if (l[at] <= 0.0f) {
        l[at] = 1.0f;
    }
}

/* The block's L write-back: L = XYZ2Lab(X, 32768 * ble, Z).L. */
__kernel void rt_lab_wavelet_w_soft_write(
    __global float* l, int l_pitch,
    __global const float* src, int src_pitch,
    __global const float* params, int lut_offset,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const float y_value = 32768.0f * src[y * src_pitch + x];
    l[y * l_pitch + x] =
        rt_lw_soft_xyz_l(y_value, params + lut_offset);
}

/*
 * The final panel's guided soft radius. The export block runs it after the
 * final pixel curve on the reconstructed L. The guide is xlin2log(L / 32768)
 * and the source is xlin2log(L), both at base ten; guidedFilterLog() converts
 * the source into the log domain, guidedFilter() filters it, and the block
 * blends the converted result with the pre-filter L. The xlin2log/xlog2lin
 * pair is sleef.h's, so the device rounds as the host does.
 */
#define RT_LW_SOFTEND_BASE 10.0f
#define RT_LW_SOFTEND_BASE_MINUS_ONE 9.0f
#define RT_LW_SOFTEND_L_SCALE (1.0f / 32768.0f)

inline float rt_lw_xlin2log10(float x)
{
    return rt_lw_xlogf(x * RT_LW_SOFTEND_BASE_MINUS_ONE + 1.0f) /
           rt_lw_xlogf(RT_LW_SOFTEND_BASE);
}

inline float rt_lw_xlog2lin10(float x)
{
    return (rt_lw_pow(RT_LW_SOFTEND_BASE, x) - 1.0f) /
           RT_LW_SOFTEND_BASE_MINUS_ONE;
}

/*
 * The guide, the source and the pre-filter L. `iL` is the block's copy of L
 * before guidedFilterLog() converts it, and it is what the final blend uses.
 */
__kernel void rt_lab_wavelet_w_softend_prepare(
    __global const float* l, int l_pitch,
    __global float* guide, int guide_pitch,
    __global float* src, int src_pitch,
    __global float* il, int il_pitch,
    int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const float value = l[y * l_pitch + x];
    il[y * il_pitch + x] = value;
    guide[y * guide_pitch + x] =
        rt_lw_xlin2log10(max(value * RT_LW_SOFTEND_L_SCALE, 0.0f));
    src[y * src_pitch + x] = rt_lw_xlin2log10(max(value, 0.0f));
}

/* q -> xlog2lin -> intp(blend, q, iL) -> L, the block's final loop. */
__kernel void rt_lab_wavelet_w_softend_finish(
    __global const float* q, int q_pitch,
    __global float* l, int l_pitch,
    __global const float* il, int il_pitch,
    int w, int h, float blend)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const float linear = rt_lw_xlog2lin10(max(q[y * q_pitch + x], 0.0f));
    l[y * l_pitch + x] =
        rt_lw_soft_intp(blend, linear, il[y * il_pitch + x]);
}

/*
 * The block's cross terms. `Thrend > 0` and the hue modulation both need the
 * guided filter's output in the linear domain before the block's final blend,
 * and the hue arm then pulls it towards the pre-filter L by
 * `1 + (wavguidCurve(hue) - 0.5)` where the hue is xatan2f(b, a) put through
 * Color::huelab_to_huehsv2(). This kernel is the same q -> xlog2lin step the
 * finish kernel does, optionally followed by that modulation, and it leaves
 * the result in `src` for the decomposition and the final blend.
 *
 * `curve_offset` is the hue polyline's index in the parameter table, in the
 * tail encoding: the knot count, the abscissae, the ordinates and the slopes.
 */
inline double rt_lw_softend_polyline(__global const float* source, double t)
{
    const int count = (int)source[0];
    if (count < 2) {
        return 1.0;
    }
    __global const float* px = source + 1;
    __global const float* py = px + count;
    __global const float* ps = py + count;
    /* FlatCurve::getVal()'s periodic wrap and binary search. */
    if (t < (double)px[0]) {
        t += 1.0;
    }
    int lo = 0;
    int hi = count - 1;
    while (hi > 1 + lo) {
        const int mid = (hi + lo) / 2;
        if ((double)px[mid] > t) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return (double)py[lo] + (t - (double)px[lo]) * (double)ps[lo];
}

/* sleef.h's atan2kf() and xatan2f(), as Color::huelab_to_huehsv2() expects. */
#define RT_LW_PI_F 3.1415927f
#define RT_LW_PI_F_2 (RT_LW_PI_F / 2.0f)

inline float rt_lw_softend_atan2kf(float y, float x)
{
    float s, t, u;
    float q = 0.0f;

    if (x < 0.0f) {
        x = -x;
        q = -2.0f;
    }
    if (y > x) {
        t = x;
        x = y;
        y = -t;
        q += 1.0f;
    }

    s = y / x;
    t = s * s;

    u = 0.00282363896258175373077393f;
    u = u * t + -0.0159569028764963150024414f;
    u = u * t + 0.0425049886107444763183594f;
    u = u * t + -0.0748900920152664184570312f;
    u = u * t + 0.106347933411598205566406f;
    u = u * t + -0.142027363181114196777344f;
    u = u * t + 0.199926957488059997558594f;
    u = u * t + -0.333331018686294555664062f;

    t = u * t;
    t = t * s + s;
    return q * RT_LW_PI_F_2 + t;
}

inline float rt_lw_softend_xatan2f(float y, float x)
{
    float r = rt_lw_softend_atan2kf(fabs(y), x);

    r = rt_lw_soft_mulsign(r, x);
    if (isinf(x) || x == 0.0f) {
        r = RT_LW_PI_F_2 -
            (isinf(x) ? (copysign(1.0f, x) * RT_LW_PI_F_2) : 0.0f);
    }
    if (isinf(y)) {
        r = RT_LW_PI_F_2 -
            (isinf(x) ? (copysign(1.0f, x) * (RT_LW_PI_F * 0.25f)) : 0.0f);
    }
    if (y == 0.0f) {
        r = (copysign(1.0f, x) == -1.0f) ? RT_LW_PI_F : 0.0f;
    }
    return (isnan(x) || isnan(y)) ? NAN : rt_lw_soft_mulsign(r, y);
}

/* Color::huelab_to_huehsv2(): the block's double arithmetic. */
inline double rt_lw_softend_huehsv2(float hh)
{
    double hr = 0.0;
    if (hh >= 0.0f && hh < 0.6f) {
        hr = 0.11666 * (double)hh + 0.93;
    } else if (hh >= 0.6f && hh < 1.4f) {
        hr = 0.1125 * (double)hh - 0.0675;
    } else if (hh >= 1.4f && hh < 2.0f) {
        hr = 0.2666 * (double)hh - 0.2833;
    } else if (hh >= 2.0f && hh < 3.14159f) {
        hr = 0.1489 * (double)hh - 0.04785;
    } else if (hh >= -3.14159f && hh < -2.8f) {
        hr = 0.23419 * (double)hh + 1.1557;
    } else if (hh >= -2.8f && hh < -2.3f) {
        hr = 0.16 * (double)hh + 0.948;
    } else if (hh >= -2.3f && hh < -0.9f) {
        hr = 0.12143 * (double)hh + 0.85928;
    } else if (hh >= -0.9f && hh < -0.1f) {
        hr = 0.2125 * (double)hh + 0.94125;
    } else if (hh >= -0.1f && hh < 0.0f) {
        hr = 0.1 * (double)hh + 0.93;
    }
    if (hr < 0.0) {
        hr += 1.0;
    } else if (hr > 1.0) {
        hr -= 1.0;
    }
    return hr;
}

__kernel void rt_lab_wavelet_w_softend_linear(
    __global float* src, int src_pitch,
    __global const float* il, int il_pitch,
    __global const float* a, int a_pitch,
    __global const float* b, int b_pitch,
    __global const float* params, int curve_offset,
    int w, int h, int hue_active)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    const int at = y * src_pitch + x;
    float value = rt_lw_xlog2lin10(max(src[at], 0.0f));
    if (hue_active) {
        const float hue =
            rt_lw_softend_xatan2f(b[y * b_pitch + x], a[y * a_pitch + x]);
        const double hsv = rt_lw_softend_huehsv2(hue);
        const float curve =
            (float)rt_lw_softend_polyline(params + curve_offset, hsv);
        const float valparam = 1.0f * (curve - 0.5f);
        const float base = il[y * il_pitch + x];
        value = base + (value - base) * (1.0f + valparam);
    }
    src[at] = value;
}

/*
 * The block's coefficient-domain second pass. For each (level, direction) it
 * mixes the guided filter's coefficients with the pre-filter L's, maps the
 * magnitude through the wavguid FlatCurve, and interpolates the guided
 * filter's coefficient towards the pre-filter one. `entry_offset` points at
 * the level-count header; `curve_offset` at the wavguid polyline.
 */
inline float rt_lw_softend_band(float wav, float ref, float tempmean,
                                float upper, float logmax, float rap,
                                float asig, float bsig, float amean,
                                __global const float* curve)
{
    const float tempwav = fabs(0.7f * wav + 0.3f * ref);
    float absciss;
    if (tempwav >= upper) {
        absciss = rt_lw_xexpf((rt_lw_xlogf(tempwav) - logmax) * rap);
    } else if (tempwav >= tempmean) {
        absciss = asig * tempwav + bsig;
    } else {
        absciss = 0.5f * sqrt(sqrt(amean * tempwav));
    }
    float kc = (float)(rt_lw_softend_polyline(curve, (double)absciss) - 1.0);
    kc = kc < 0.0f ? -(kc * kc) : kc;
    const float reduceeffect = kc <= 0.0f ? 1.0f : 1.2f;
    const float kinterm = fmax(1.0f + reduceeffect * kc, 0.0f);
    return rt_lw_soft_intp(kinterm, wav, ref);
}

__kernel void rt_lab_wavelet_w_softend_band(
    __global float* band0, int p0,
    __global float* band1, int p1,
    __global float* band2, int p2,
    __global const float* ref0, int q0,
    __global const float* ref1, int q1,
    __global const float* ref2, int q2,
    __global const float* params, int entry_offset, int curve_offset,
    int level, int w, int h)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    __global const float* entry =
        params + entry_offset + level * RT_LW_W_SOFTEND_LEVEL_SCALARS;
    if (entry[0] == 0.0f) {
        return;
    }
    __global const float* curve = params + curve_offset;
    const float tempmean = entry[1];
    const float upper = entry[2];
    const float logmax = entry[3];
    const float rap = entry[4];
    const float asig = entry[5];
    const float bsig = entry[6];
    const float amean = entry[7];

    const int at0 = y * p0 + x;
    band0[at0] = rt_lw_softend_band(
        band0[at0], ref0[y * q0 + x], tempmean, upper, logmax, rap, asig,
        bsig, amean, curve);
    const int at1 = y * p1 + x;
    band1[at1] = rt_lw_softend_band(
        band1[at1], ref1[y * q1 + x], tempmean, upper, logmax, rap, asig,
        bsig, amean, curve);
    const int at2 = y * p2 + x;
    band2[at2] = rt_lw_softend_band(
        band2[at2], ref2[y * q2 + x], tempmean, upper, logmax, rap, asig,
        bsig, amean, curve);
}

/* The block's final blend when the second pass or the hue arm already left
 * the guided filter's output in the linear domain. */
__kernel void rt_lab_wavelet_w_softend_blend(
    __global const float* q, int q_pitch,
    __global float* l, int l_pitch,
    __global const float* il, int il_pitch,
    int w, int h, float blend)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= w || y >= h) {
        return;
    }
    l[y * l_pitch + x] =
        rt_lw_soft_intp(blend, q[y * q_pitch + x], il[y * il_pitch + x]);
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
    const float second = l[at];
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
