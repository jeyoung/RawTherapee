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
#define RT_LW_W_FILTER_ANAL_LO 32
#define RT_LW_W_FILTER_ANAL_HI 40
#define RT_LW_W_FILTER_SYNTH_LO 48
#define RT_LW_W_FILTER_SYNTH_HI 56

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
 * Level 0 synthesis, vertical half. Writes the source L plane in place, mixes
 * the original L by the strength blend and applies the block's final
 * L <= 0 -> 1 clamp, which is the last operation the host performs on L.
 */
__kernel void rt_lab_wavelet_w_synth_v(
    __global const float* lo, int lo_pitch,
    __global const float* hi, int hi_pitch,
    __global float* dst, int dst_pitch,
    __global const float* filters, int taps, int offset,
    int width, int src_h, int dst_h, float blend)
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
    if (result <= 0.0f) {
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
)CLC";
