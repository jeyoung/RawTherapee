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

#include "stage_abi.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtengine
{
class ImProcFunctions;
class LabImage;
}

namespace rtengine::procparams
{
struct ProcParams;
}

/*
 * Host context of the lab.wavelet node.
 *
 * The device launcher receives this structure through RT_STAGE_ARG_PTR. It is
 * host-only: the pointer and its callbacks live in host memory and nothing
 * here is passed to a kernel. The context builds the equaliser's per-level
 * range tables through its own fillLut(), runs the hue/chroma precompute
 * through the equaliser's own SSE-aware code, and builds the wavelet tool's
 * parameter table: the level count, the scalars, the 8-tap filters and, after
 * the device statistics read back, one mea block per plane and level through
 * the block's own calceffect(). It can also run the original host block for a
 * residual.
 *
 * Gamutlab protection is a host pre-pass: the block calls BadpixelsLab()
 * before the equaliser when gamutlab is set and skin protection is non-zero.
 * That pass is a separable Gaussian analysis with host-specific SSE rounding,
 * so the device launcher runs it on a host copy and uploads the result rather
 * than approximating it. The prepared copy is what the hue/chroma masks are
 * built from, in the same order as the block. The same masks serve the
 * wavelet tool's own skin protection: the equaliser only changes L, so a/b
 * and their masks are identical at the start of both passes.
 */
#define RT_LAB_WAVELET_CONTEXT_VERSION 16u
/* "LWCT": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LAB_WAVELET_CONTEXT_MAGIC 0x4C574354u
/* The number of levels of the directional pyramid. */
#define RT_LAB_WAVELET_MAX_LEVELS 6u
/* The wavelet decomposition admits up to ten levels (wavelet_level's limit). */
#define RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS 10u
/*
 * The wavelet parameter table. The host builds one flat float block per
 * submission: the scalars below, then the 8-tap analysis and synthesis
 * filters, then one RT_LAB_WAVELET_MEA_ENTRIES-entry block per (plane, level)
 * filled by compute_mea() after the device statistics have been read back.
 * Plane 0 is L, planes 1 and 2 are the chroma pass's a and b. The device
 * never re-derives a filter, a threshold or a range.
 */
#define RT_LAB_WAVELET_WAVELET_PARAMS 96u
#define RT_LAB_WAVELET_MEA_ENTRIES 10u
/* One block of raw statistics per (level, direction) band. */
#define RT_LAB_WAVELET_STATS_PER_BAND 6u

/* Wavelet parameter table indices. C0..C9 occupy ten consecutive slots. */
#define RT_LW_PARAM_LEVELS 0
#define RT_LW_PARAM_HSMET 1
#define RT_LW_PARAM_NUMLEVH 2
#define RT_LW_PARAM_NUMLEVS 3
#define RT_LW_PARAM_C 4
#define RT_LW_PARAM_THRESHOLD 14
#define RT_LW_PARAM_THRESHOLD2 15
#define RT_LW_PARAM_SIGMA 16
#define RT_LW_PARAM_OFFSET 17
#define RT_LW_PARAM_LOWTHR 18
#define RT_LW_PARAM_STRENGTH 19
#define RT_LW_PARAM_B_LHL 20
#define RT_LW_PARAM_T_LHL 21
#define RT_LW_PARAM_B_RHL 22
#define RT_LW_PARAM_T_RHL 23
#define RT_LW_PARAM_B_LSL 24
#define RT_LW_PARAM_T_LSL 25
#define RT_LW_PARAM_B_RSL 26
#define RT_LW_PARAM_T_RSL 27
#define RT_LW_PARAM_TAPS 28
/* The wavelet tool's other panels and the chroma arm's scalars. */
#define RT_LW_PARAM_CONTENA 29
#define RT_LW_PARAM_CHRO 30
#define RT_LW_PARAM_CHMET 31
/* 1 when the a/b planes are present: the chroma arm and the edge panel both
 * keep the block's a/b decomposition and reconstruction alive. */
#define RT_LW_PARAM_CHROMA 32
#define RT_LW_PARAM_SIGMACOL 33
#define RT_LW_PARAM_W_SKIN 34
#define RT_LW_PARAM_W_B_L 35
#define RT_LW_PARAM_W_T_L 36
#define RT_LW_PARAM_W_T_R 37
#define RT_LW_PARAM_W_B_R 38
/*
 * The edge panel. Slots 39..47 are free in the scalar table; the edge mea,
 * the per-level mean/sigma/MaxP and the sampled edge curve live in the
 * parameter table's tail after the per-plane mea blocks.
 */
#define RT_LW_PARAM_EDGEENA 39
#define RT_LW_PARAM_EDG_VAL 40
#define RT_LW_PARAM_EDG_RAD 41
#define RT_LW_PARAM_EDG_TIL 42
#define RT_LW_PARAM_EDG_EFF 43
#define RT_LW_PARAM_EDG_REINFORCE 44
#define RT_LW_PARAM_EDG_CURVE 45
#define RT_LW_PARAM_EDG_HIG 46
#define RT_LW_PARAM_EDG_LOW 47
/*
 * The edge panel's Lipschitz control (calckoe() and the neighbour
 * smoothing). Slots 89..95 sit after the clarity soft-radius scalars:
 *   [89] 1 when medianlev() detection runs (cp.detectedge)
 *   [90] edgedetect (calckoe()'s gradw, 0..100)
 *   [91] edgedetectthr (calckoe()'s tloww, 0..100; 75 selects the 5x5 arm)
 *   [92] aamp = 1 + edgedetectthr2 / 100
 *   [93] eddlipinfl = 0.005 * edgsens + 0.4
 *   [94] eddlipampl = 1 + edgampl / 50
 *   [95] smoothing mode: 0 off, 1 NPmethod "low", 2 "high"
 */
#define RT_LW_PARAM_EDG_DETECT 89
#define RT_LW_PARAM_EDG_GRADW 90
#define RT_LW_PARAM_EDG_TLOWW 91
#define RT_LW_PARAM_EDG_AAMP 92
#define RT_LW_PARAM_EDG_LIPINFL 93
#define RT_LW_PARAM_EDG_LIPAMPL 94
#define RT_LW_PARAM_EDG_SMOOTH 95
/* Slots 48..79 hold Daubechies analysis lo/hi and synthesis lo/hi. */
#define RT_LW_PARAM_FILTER_ANAL_LO 48
#define RT_LW_PARAM_FILTER_ANAL_HI 56
#define RT_LW_PARAM_FILTER_SYNTH_LO 64
#define RT_LW_PARAM_FILTER_SYNTH_HI 72
/*
 * The clarity arm's guided soft radius, present in every table and zero when
 * the arm does not run. The host computes the guided filter's derived scalars
 * once, with the block's own float arithmetic, so the device never re-derives
 * the radius, the subsampling or the bilinear scales:
 *   [80] 1 when the soft radius runs (clarity arm with softrad > 0)
 *   [81] the block's epsilon (guidedFilter's `epsilon` float argument)
 *   [82] the subsampled width (guidedfilter.cc's W / subsampling)
 *   [83] the subsampled height
 *   [84] the box-blur radius: f_mean()'s LIM and boxblur()'s own clamp
 *   [85] the downscale column factor, float(W) / float(w)
 *   [86] the downscale row factor
 *   [87] the upscale column factor, float(w) / float(W)
 *   [88] the upscale row factor
 * Slots 89..95 are free. The final panel's guided soft radius is carried in
 * its own tail after the clarity soft LUT (see
 * RT_LAB_WAVELET_SOFTEND_ENTRIES), because it needs nine scalars.
 */
#define RT_LW_PARAM_SOFT_ACTIVE 80
#define RT_LW_PARAM_SOFT_EPSILON 81
#define RT_LW_PARAM_SOFT_W 82
#define RT_LW_PARAM_SOFT_H 83
#define RT_LW_PARAM_SOFT_BOXRAD 84
#define RT_LW_PARAM_SOFT_SCALE_DX 85
#define RT_LW_PARAM_SOFT_SCALE_DY 86
#define RT_LW_PARAM_SOFT_SCALE_UX 87
#define RT_LW_PARAM_SOFT_SCALE_UY 88
/*
 * The soft-radius tail, present only when the clarity arm's soft radius runs:
 * RT_LAB_WAVELET_SOFT_LUT_ENTRIES samples of the block's own Color::cachefy
 * table. The L write-back is XYZ2Lab()'s L output, which is a float lookup of
 * the new Y through that table; uploading the table reproduces the host
 * rounding exactly instead of approximating std::cbrt() on the device.
 */
#define RT_LAB_WAVELET_SOFT_LUT_ENTRIES 65536u
/*
 * The final panel's guided soft radius tail, present only when `softradend`
 * runs (the export block's second guided-filter block):
 *   [0] blend: LIM01(strend / 100)
 *   [1] epsilon: 0.001f * pow(2, -detend)
 *   [2] the subsampled width (guidedfilter.cc's W / subsampling)
 *   [3] the subsampled height
 *   [4] the box-blur radius
 *   [5] the downscale column factor, float(W) / float(w)
 *   [6] the downscale row factor
 *   [7] the upscale column factor, float(w) / float(W)
 *   [8] the upscale row factor
 * The tail always follows the clarity soft LUT, so the launcher can find it
 * at `entries - RT_LAB_WAVELET_SOFTEND_ENTRIES` when the two optional blocks
 * below are absent.
 *
 * Two optional blocks follow the nine scalars:
 *
 * The coefficient-domain second block (`Thrend > 0`), present only when the
 * block runs it:
 *   [0] the level count L (the transform's own count)
 *   [1 + l*8 + k] the per-level scalars built by compute_softend() for
 *                 level l = 0..L-2:
 *                   k = 0 the block's guard (MaxP > 0 && mean != 0 &&
 *                         sigma != 0) as 1 or 0
 *                   k = 1 tempmean = 0.3*mean + 0.7*meang
 *                   k = 2 the upper knot tempmean + sig*tempsig
 *                   k = 3 logmax = log(tempmax), the block's double log
 *                   k = 4 rap = log(insigma) / log(rapX)
 *                   k = 5 asig = 0.166 / (tempsig*sig)
 *                   k = 6 bsig = 0.5 - asig*tempmean
 *                   k = 7 amean = 1 / tempmean
 *   then the wavguid FlatCurve's polyline: count, count x, count y and
 *   count-1 slopes, the same encoding the refinement tail uses.
 *
 * The hue modulation (`wavguidcurve` non-identity), present only when the
 * curve is non-identity: the wavguidCurve polyline in the same encoding.
 */
#define RT_LAB_WAVELET_SOFTEND_ENTRIES 9u
#define RT_LAB_WAVELET_SOFTEND_BLEND 0u
#define RT_LAB_WAVELET_SOFTEND_EPSILON 1u
#define RT_LAB_WAVELET_SOFTEND_W 2u
#define RT_LAB_WAVELET_SOFTEND_H 3u
#define RT_LAB_WAVELET_SOFTEND_BOXRAD 4u
#define RT_LAB_WAVELET_SOFTEND_SCALE_DX 5u
#define RT_LAB_WAVELET_SOFTEND_SCALE_DY 6u
#define RT_LAB_WAVELET_SOFTEND_SCALE_UX 7u
#define RT_LAB_WAVELET_SOFTEND_SCALE_UY 8u
/* One per-level scalar block of the coefficient-domain second pass. */
#define RT_LAB_WAVELET_SOFTEND_LEVEL_SCALARS 8u
/* The second block's level-count header. */
#define RT_LAB_WAVELET_SOFTEND_THREND_HEADER 1u
/*
 * The edge tail, present only when the edge panel runs:
 *   [0]                 settings->limrad
 *   [1 + level*13 + 0]  the block's calceffect() mea for the edge beta ramp
 *   [1 + level*13 + 10] MaxP (the average of the three positive band maxima)
 *   [1 + level*13 + 11] mean (the positive band average)
 *   [1 + level*13 + 12] sigma (the positive band standard deviation)
 *   then 501 sampled points of the block's own WavCurve (ccwcurve).
 */
#define RT_LAB_WAVELET_EDGE_ENTRIES 13u
#define RT_LAB_WAVELET_CURVE_ENTRIES 501u
/*
 * The final-curve tail, present only when the final curves run:
 *   [0]                 sigmafin
 *   [1 + level*3 + 0]   MaxP (the average of the three positive band maxima)
 *   [1 + level*3 + 1]   mean (the positive band average)
 *   [1 + level*3 + 2]   sigma (the positive band standard deviation)
 *   then RT_LAB_WAVELET_CURVE_ENTRIES samples of the block's own
 *   WavOpacityCurveWL (opacityCurveWL), then RT_LAB_WAVELET_FINAL_LUT_ENTRIES
 *   samples of the block's wavclCurve LUT.
 */
#define RT_LAB_WAVELET_FINAL_STATS_ENTRIES 3u
#define RT_LAB_WAVELET_FINAL_LUT_ENTRIES 65536u
/*
 * The toning tail, present only when the toning panel runs:
 *   [0]                  sigmaton
 *   [1 + channel*9 + l]  mulopaRG/mulopaBY at level l (channel 0 = a, 1 = b)
 *   then 2 * levels * 10 mea entries: channel-major, level-major, the
 *   block's calceffect() with sigmaton and an offset of 1.
 */
#define RT_LAB_WAVELET_TONING_MULS 9u
#define RT_LAB_WAVELET_TONING_HEADER 19u
#define RT_LAB_WAVELET_TONING_MEA_ENTRIES 10u
/*
 * The denoise tail, present only when the denoise arm the block forces at six
 * levels is armed:
 *   [0..5] vari[0..5]
 *   [6]    ballum
 *   then one plane-major block per plane: levels * 3 squared MAD entries,
 *   direction-major inside each level. Plane 0 is L, planes 1 and 2 are the
 *   a and b bands the forced chroma arm denoises.
 *   then RT_LAB_WAVELET_DENOISE_HUE_ENTRIES floats: the wavhuecurve polyline
 *   the L pass samples per pixel to modulate the noise variance. A zero count
 *   means the identity curve, the admitted default.
 * `quaMethod` "cons" runs `WaveletDenoiseAllL()` and `WaveletDenoiseAllAB()`
 * once per plane; "agre" runs the BiShrink pair first and then those same
 * passes, reusing this tail. The MAD histogram is a separate internal device
 * buffer, not a node argument.
 */
#define RT_LAB_WAVELET_DENOISE_HEADER 7u
#define RT_LAB_WAVELET_DENOISE_MAD_ENTRIES 3u
/*
 * The chroma noise variance `noisevarab_r` that ip_wavelet() passes to
 * WaveletDenoiseAllAB() and WaveletDenoiseAll_BiShrinkAB(). The block
 * hard-codes 100.f; it is not a profile value, so the device takes it from
 * here rather than from the parameter table.
 */
#define RT_LAB_WAVELET_DENOISE_NOISEVAR_AB 100.0f
/*
 * The denoise refinement tail (`sliMethod == "cur"`, or `sliMethod == "sli"`
 * with a non-zero Thrden), present only when the refinement is admitted. It
 * follows the denoise tail:
 *   [0]                  1 when the block would run the refinement
 *   [1 + level*11 + k]   level 0..5, k = active, weightL, logmax, rap, asig,
 *                        bsig, amean, k, maxVal, tempmean, kcFactor
 *   then RT_LAB_WAVELET_REFINE_LUT samples of the block's own wavdenoise
 *   curve (the WavCurve 501-entry float LUT), used when mode is zero
 *   then [mode] and [count]; mode 1 carries the Thrden wavlow polyline as
 *   count x values, count y values and count slopes.
 *   then the expert arm's two extra curves in the same encoding: wavmed
 *   (`Limden > 0`) and wavhigh (always).
 * The refinement runs at levref == 4 levels when cp.complex is zero and six
 * when it is one; the expert pairing maps levels 0..3 to sigmm14 and 4..5 to
 * sigmm56, and rewrites bands 0..3 with the wavmed factor and bands 4..5 with
 * the wavhigh factor on every level iteration.
 */
#define RT_LAB_WAVELET_REFINE_LEVELS 6u
/* The standard pairing's levref, and the expert pairing's. */
#define RT_LAB_WAVELET_REFINE_LEVELS_STANDARD 4u
#define RT_LAB_WAVELET_REFINE_LEVELS_EXPERT 6u
#define RT_LAB_WAVELET_REFINE_SCALARS 11u
#define RT_LAB_WAVELET_REFINE_HEADER 1u
#define RT_LAB_WAVELET_REFINE_LUT 501u
/* The two expert curve blocks: [mode] and [count] then the encoded polyline. */
#define RT_LAB_WAVELET_REFINE_CURVE_HEADER 2u
/* FlatCurve's default polyline count is 1000 plus the closing point. */
#define RT_LAB_WAVELET_REFINE_POLY_MAX 1024u
/*
 * The denoise hue curve's block, present after the denoise tail's squared MAD
 * entries whenever the denoise arm runs. It carries the wavhuecurve FlatCurve
 * polyline in the same encoding as the refinement tail: the knot count, the
 * abscissae, the ordinates and count-1 slopes. A count below two means the
 * curve is the identity and the kernel leaves the noise variance alone. The
 * block is a fixed bound so the tail offset stays a compile-time derivation.
 */
#define RT_LAB_WAVELET_DENOISE_HUE_ENTRIES \
    (3u * RT_LAB_WAVELET_REFINE_POLY_MAX)
/*
 * The balance-chroma (BA) tail, present only when `BAmethod` is "sli" or
 * "cur". It reproduces ContAllL()/ContAllAB()'s BA arm:
 *   [0]                 reserved (one when the tail is present)
 *   [1]                 BAmet: 1 for "sli", 2 for "cur"
 *   [2]                 balan (the Balance slider as a float)
 *   [3]                 ite (the Iterations slider)
 *   [4 + level*10 + k]  ContAllL()'s calceffect() with sigmadir and an
 *                       offset of 1, one mea block per level
 *   then RT_LAB_WAVELET_CURVE_ENTRIES samples of WavOpacityCurveW, the
 *   "cur" arm's opacity curve.
 * The a/b arm reads no mea block; its multiplier is the raw kba.
 */
#define RT_LAB_WAVELET_BA_HEADER 4u
#define RT_LAB_WAVELET_BA_MEA_ENTRIES 10u

/*
 * The exact length of the wavelet parameter table. Both the host builder and
 * the launcher derive it from this one function so a table built without a
 * tail can never be read past its end.
 */
inline std::size_t labWaveletParamEntries(std::uint32_t planes,
                                          std::uint32_t levels, bool edge,
                                          bool final_curves, bool toning,
                                          bool denoise, bool refine,
                                          std::uint32_t refine_poly,
                                          std::uint32_t refine_med_poly,
                                          std::uint32_t refine_high_poly,
                                          bool soft, bool softend,
                                          bool softend_thrend,
                                          std::uint32_t softend_thrend_poly,
                                          std::uint32_t softend_hue_poly,
                                          bool ba = false)
{
    std::size_t entries = RT_LAB_WAVELET_WAVELET_PARAMS +
                          static_cast<std::size_t>(planes) *
                              static_cast<std::size_t>(levels) *
                              RT_LAB_WAVELET_MEA_ENTRIES;
    if (edge) {
        entries += 1 + static_cast<std::size_t>(levels) *
                           RT_LAB_WAVELET_EDGE_ENTRIES +
                   RT_LAB_WAVELET_CURVE_ENTRIES;
    }
    if (final_curves) {
        entries += 1 + static_cast<std::size_t>(levels) *
                           RT_LAB_WAVELET_FINAL_STATS_ENTRIES +
                   RT_LAB_WAVELET_CURVE_ENTRIES +
                   RT_LAB_WAVELET_FINAL_LUT_ENTRIES;
    }
    if (toning) {
        entries += RT_LAB_WAVELET_TONING_HEADER +
                   2 * static_cast<std::size_t>(levels) *
                       RT_LAB_WAVELET_TONING_MEA_ENTRIES;
    }
    if (denoise) {
        entries += RT_LAB_WAVELET_DENOISE_HEADER +
                   static_cast<std::size_t>(planes) *
                       static_cast<std::size_t>(levels) *
                       RT_LAB_WAVELET_DENOISE_MAD_ENTRIES +
                   RT_LAB_WAVELET_DENOISE_HUE_ENTRIES;
    }
    if (refine) {
        entries += RT_LAB_WAVELET_REFINE_HEADER +
                   static_cast<std::size_t>(RT_LAB_WAVELET_REFINE_LEVELS) *
                       RT_LAB_WAVELET_REFINE_SCALARS +
                   RT_LAB_WAVELET_REFINE_LUT +
                   RT_LAB_WAVELET_REFINE_CURVE_HEADER +
                   3 * static_cast<std::size_t>(refine_poly) +
                   RT_LAB_WAVELET_REFINE_CURVE_HEADER +
                   3 * static_cast<std::size_t>(refine_med_poly) +
                   RT_LAB_WAVELET_REFINE_CURVE_HEADER +
                   3 * static_cast<std::size_t>(refine_high_poly);
    }
    if (soft) {
        entries += RT_LAB_WAVELET_SOFT_LUT_ENTRIES;
    }
    if (softend) {
        entries += RT_LAB_WAVELET_SOFTEND_ENTRIES;
        if (softend_thrend) {
            /* One entry per transformed level above the last, which the
             * block's `level < levwavL - 1` loop skips. */
            entries += RT_LAB_WAVELET_SOFTEND_THREND_HEADER;
            if (levels > 1) {
                entries += static_cast<std::size_t>(levels - 1) *
                           RT_LAB_WAVELET_SOFTEND_LEVEL_SCALARS;
            }
            entries += static_cast<std::size_t>(softend_thrend_poly) * 3u;
        }
        entries += static_cast<std::size_t>(softend_hue_poly) * 3u;
    }
    if (ba) {
        /* The balance-chroma tail is the table's last block. */
        entries += RT_LAB_WAVELET_BA_HEADER +
                   static_cast<std::size_t>(levels) *
                       RT_LAB_WAVELET_BA_MEA_ENTRIES +
                   RT_LAB_WAVELET_CURVE_ENTRIES;
    }
    return entries;
}
/*
 * One range table per level: 0x20000 entries covering hipass + 0x10000, the
 * LUT size dirpyr_equalizer.cc's fillLut() writes and idirpyr_eq_channel()
 * indexes with a float lookup (interpolated between neighbouring entries).
 */
#define RT_LAB_WAVELET_LUT_ENTRIES 0x20000u
#define RT_LAB_WAVELET_LUT_BIAS 0x10000
/* The block's scale argument at the export call site. */
#define RT_LAB_WAVELET_EXPORT_SCALE 1

typedef struct RtLabWaveletResult {
    /* levels * RT_LAB_WAVELET_LUT_ENTRIES floats, level-major. */
    const float* luts;
    /* The host-built hue and chroma masks, width * height floats each. They
     * are null when skin protection is off. */
    const float* hue;
    const float* chrom;
    uint32_t lut_entries;
    uint32_t levels;
    uint32_t skinprotect;
    /*
     * The wavelet pass. `wavelet` is 1 when the device runs it. `params`
     * carries wavelet_entries floats: the scalar/filter table followed by the
     * per-(plane, level) mea blocks that compute_mea() fills after the
     * statistics readback.
     */
    uint32_t wavelet;
    uint32_t wavelet_levels;
    uint32_t wavelet_entries;
    float* params;
} RtLabWaveletResult;

/*
 * The wavelet sub-structure of the context. The launcher reads it to size the
 * transform, the statistics pass and the contrast pass; the host owns every
 * value. compute_mea() is called with one plane's device statistics block
 * (RT_LAB_WAVELET_STATS_PER_BAND floats per (level, direction), level-major,
 * direction 1..3) and fills that plane's mea tail of `params`. Plane 0 is L;
 * planes 1 and 2 are a and b and exist when the chroma arm, the edge panel or
 * the final curves keep the block's a/b round trip alive.
 */
typedef struct RtLabWaveletWavelet {
    /* 1 when the call site admits the wavelet pass on the device. */
    uint32_t active;
    /* L's transform level count. The a and b planes run ip_wavelet()'s base
     * decomposition level, which is lower whenever only the chroma panel or
     * the forced chroma denoise raises L to seven; the parameter table is
     * always laid out with this stride. */
    uint32_t levels;
    uint32_t taps;
    uint32_t entries;
    /* 1 for L only, 3 when the a/b planes also run (the chroma arm, the edge
     * panel, the final curves or the balance-chroma a/b arm, whose level-trim
     * change keeps the block's a/b round trip). */
    uint32_t planes;
    /* 1 when the final-curve tail is present in `params`. */
    uint32_t final;
    /* 1 when the toning tail is present in `params`. */
    uint32_t toning;
    /* 1 when the denoise tail is present in `params`. */
    uint32_t denoise;
    /*
     * 1 when `quaMethod == "agre"` selects the BiShrink pair. `ip_wavelet()`
     * runs `WaveletDenoiseAll_BiShrinkL()` and then `WaveletDenoiseAllL()`,
     * and each a/b plane gets `WaveletDenoiseAll_BiShrinkAB()` and then
     * `WaveletDenoiseAllAB()`. The pair reads the same `vari`, `ballum` and
     * squared MAD tail as the standard arm, so the table is unchanged; only
     * the number of coefficient passes differs. The forced chroma pair
     * recomputes its own squared MAD from the post-BiShrink bands, because
     * `ShrinkAllAB()` calls `Mad()` on the band it is about to shrink.
     */
    uint32_t bishrink;
    /* 1 when the final panel's guided soft radius tail is present. */
    uint32_t softend;
    /* 1 when the tail carries the coefficient-domain second block (Thrend >
     * 0). `softend_thrend_poly` is that block's wavguid polyline knot count
     * and `softend_hue_poly` the hue curve's, both zero when absent. */
    uint32_t softend_thrend;
    uint32_t softend_thrend_poly;
    uint32_t softend_hue_poly;
    /* 1 when the denoising refinement tail is present in `params`. */
    uint32_t refine;
    /* The Thrden wavlow polyline's knot count in that tail, or zero when the
     * refinement samples the wavdenoise LUT instead. */
    uint32_t refine_poly;
    /*
     * The refinement's levref: four for the standard pairing, six for the
     * expert one. `refine_expert` is one when the block pairs levels 0..3 and
     * 4..5 separately (`cp.complex == 1`); `refine_med_poly` is the wavmed
     * curve's knot count (zero when Limden is zero) and `refine_high_poly`
     * the wavhigh curve's, both zero when the expert arm is off.
     */
    uint32_t refine_levels;
    uint32_t refine_expert;
    uint32_t refine_med_poly;
    uint32_t refine_high_poly;
    /*
     * The balance-chroma arm. `ba` is one when the tail is present; `ba_l`
     * and `ba_ab` are the two block gates: the L arm runs with `expfinal`
     * (cp.finena) and the a/b arm with `TMr` (cp.diag). The method and its
     * scalars live in the tail itself, so the launcher only re-checks them.
     */
    uint32_t ba;
    uint32_t ba_l;
    uint32_t ba_ab;
    /*
     * 1 when ip_wavelet() re-evaluates the L statistics after the contrast,
     * edge and final multiplies: `cp.val > 0 || ref || contr || cp.diagcurv`
     * at ipwavelet.cc:1573. A zero value means the block passes the
     * statistics Evaluate2() produced before the denoise and the contrast
     * multiply to finalContAllL(), so the final tail must be built from that
     * earlier block. Evaluated by the host; the launcher never re-derives it.
     */
    uint32_t final_refresh;
    float* params;
    /*
     * Fills one plane's per-level mea blocks from the statistics the device
     * computed for it. `levels` is the number of levels that plane ran, which
     * can be smaller than the table's stride: the a and b planes keep
     * ip_wavelet()'s base decomposition level while L keeps seven. Entries
     * above `levels` are left untouched.
     */
    int (*compute_mea)(void* self, const float* stats, uint32_t levels,
                       uint32_t plane);
    /*
     * Fills the final-curve tail from the post-contrast statistics block.
     * finalContAllL() reads the statistics Evaluate2() recomputes after the
     * contrast and edge passes, so this callback is separate from the mea
     * build. Used only when `final` is set.
     */
    int (*compute_final)(void* self, const float* stats, uint32_t levels);
    /*
     * Fills the denoise tail's squared MAD entries for one plane from one
     * histogram per (level, direction), direction-major. `levels` is the
     * number of levels that plane ran, as for compute_mea().
     */
    int (*compute_mad)(void* self, const uint32_t* histogram, uint32_t levels,
                       uint32_t plane);
    /*
     * Fills the denoise-curve refinement tail from the pre-denoise and
     * post-denoise statistics blocks, both laid out as compute_mea()'s input.
     * `levels` is the pass's own L level count. The tail's first entry is set
     * to one when `ip_wavelet()` would run the refinement: the curve is
     * non-identity and the block's near-zero early-out does not fire. Used
     * only when `refine` is set.
     */
    int (*compute_refine)(void* self, const float* pre_stats,
                          const float* post_stats, uint32_t levels);
    /*
     * Fills the final soft radius's coefficient-domain second block from the
     * two decomposition statistics blocks: `stats_ll` is Evaluate2() over the
     * guided filter's output and `stats_dst` over the pre-filter L. Both are
     * laid out as compute_mea()'s input. Used only when `softend_thrend` is
     * set.
     */
    int (*compute_softend)(void* self, const float* stats_ll,
                           const float* stats_dst, uint32_t levels);
} RtLabWaveletWavelet;

/*
 * finalContAllL()'s level selection. CLmethod is decoded exactly as the block
 * decodes it: "one" keeps a single level, "inf" the levels above it, "sup" the
 * levels below it and "all" disables the selection. `choice_level` is
 * Lmethod - 1 (4 when Lmethod is zero), and `choice_dir`/`dir1`/`dir2` are the
 * Dirmethod arms: "all" keeps every direction, otherwise the two listed
 * directions are zeroed and the remaining one is kept.
 */
#define RT_LAB_WAVELET_CL_ONE 0u
#define RT_LAB_WAVELET_CL_INF 1u
#define RT_LAB_WAVELET_CL_SUP 2u
#define RT_LAB_WAVELET_CL_ALL 3u

#define RT_LAB_WAVELET_BACK_BLACK 0u
#define RT_LAB_WAVELET_BACK_GREY 1u
#define RT_LAB_WAVELET_BACK_RESID 2u

#define RT_LAB_WAVELET_DIR_ALL 0u
#define RT_LAB_WAVELET_DIR_ONE 1u
#define RT_LAB_WAVELET_DIR_TWO 2u
#define RT_LAB_WAVELET_DIR_THR 3u

typedef struct RtLabWaveletSelection {
    /* 1 when CLmethod selects levels (every value except "all"). */
    uint32_t active;
    uint32_t method;       /* RT_LAB_WAVELET_CL_* */
    uint32_t back_method;  /* RT_LAB_WAVELET_BACK_* */
    uint32_t choice_level;
    uint32_t choice_dir;   /* RT_LAB_WAVELET_DIR_* */
    uint32_t dir1;
    uint32_t dir2;
    /* The block's cp.maxilev: wavelet.thres, the level count the widget
     * advertises. finalContAllL() compares choice_level with it. */
    uint32_t max_ilev;
} RtLabWaveletSelection;

/*
 * The clarity arm's merge. `active` is 1 for the block's
 * `ushamethod` sharp/clari + `expclari` + `CLmethod != "all"` sequence;
 * `merge` is 1 when the second pass feeds the merge (the one/inf + black and
 * sup + resid arms). Every other Backmethod arm leaves the merge factors zero,
 * so the block returns the clamped first pass. The four factors are already
 * scaled as the block scales them: mergeL/mergeC over 100, and the -1.5
 * luminance factor of the black arms.
 */
typedef struct RtLabWaveletClarity {
    uint32_t active;
    uint32_t merge;
    /*
     * The clarity arm's first pass runs with CLmethod forced to "all", so its
     * L decomposition can be shorter than the profile's untrimmed count: the
     * trailing-zero trim applies. The launcher runs the first pass with this
     * many levels and the second with the context's wavelet level count.
     */
    uint32_t levels_first;
    float merge_l0;
    float merge_l;
    float merge_c0;
    float merge_c;
} RtLabWaveletClarity;

typedef struct RtLabWaveletContext {
    uint32_t size;
    uint32_t version;
    uint32_t magic;
    /*
     * 1 when the call site admits the configuration on the device. A zero
     * value makes the launcher refuse before it touches the Lab buffer.
     */
    uint32_t supported;
    void* self;
    /* Builds the per-level range tables and allocates the masks. */
    int (*build)(void* self, RtLabWaveletResult* out);
    /*
     * Runs the gamutlab BadpixelsLab() pre-pass on the supplied buffer when
     * it is needed, then fills the hue/chroma masks. The launcher calls it on
     * a scratch copy so a refusal leaves the caller's image untouched.
     */
    int (*prepare)(void* self, RtStageBuffer* lab);
    /* Runs the original host block so a residual produces the CPU result. */
    int (*run_host)(void* self, RtStageBuffer* lab);
    /* 1 when prepare() must run on a private copy (gamutlab protection). */
    uint32_t needs_scratch;
    uint32_t levels;
    uint32_t skinprotect;
    /* The wavelet contrast pass; zero-filled when it is off. */
    RtLabWaveletWavelet wavelet;
    /* CLmethod's level selection; `active` is zero for "all". */
    RtLabWaveletSelection selection;
    /* The clarity arm's merge; `active` is zero when it does not run. */
    RtLabWaveletClarity clarity;
} RtLabWaveletContext;

namespace rtengine::stage
{

/*
 * Parameter bounds. The GUI's mult sliders are 0..4, threshold 0..1 and skin
 * protection -100..100; the hue points are normalised by 100 and the widget
 * admits -0.4..2.1. A value outside these bounds is a residual, not a device
 * pass, so the host block's own arithmetic is the result.
 */
inline constexpr double LAB_WAVELET_MIN_MULT = 0.0;
inline constexpr double LAB_WAVELET_MAX_MULT = 4.0;
inline constexpr double LAB_WAVELET_MIN_THRESHOLD = 0.0;
inline constexpr double LAB_WAVELET_MAX_THRESHOLD = 1.0;
inline constexpr double LAB_WAVELET_MIN_SKIN = -100.0;
inline constexpr double LAB_WAVELET_MAX_SKIN = 100.0;
inline constexpr double LAB_WAVELET_MIN_HUE = -10.0;
inline constexpr double LAB_WAVELET_MAX_HUE = 10.0;

/* The wavelet tool's slider bounds. A value outside them is a residual. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGMA = 0.05;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGMA = 2.5;
inline constexpr double LAB_WAVELET_MIN_WAVELET_OFFSET = 0.33;
inline constexpr double LAB_WAVELET_MAX_WAVELET_OFFSET = 1.66;
inline constexpr double LAB_WAVELET_MIN_WAVELET_LOWTHR = 20.0;
inline constexpr double LAB_WAVELET_MAX_WAVELET_LOWTHR = 100.0;
inline constexpr int LAB_WAVELET_MIN_WAVELET_C = -100;
inline constexpr int LAB_WAVELET_MAX_WAVELET_C = 350;
inline constexpr int LAB_WAVELET_MIN_WAVELET_LEVELS = 1;
inline constexpr int LAB_WAVELET_MAX_WAVELET_LEVELS = 9;
inline constexpr int LAB_WAVELET_MIN_THRESHOLD_LEVEL = 1;
inline constexpr int LAB_WAVELET_MAX_THRESHOLD_LEVEL = 9;
inline constexpr int LAB_WAVELET_MIN_THRESHOLD2_LEVEL = 3;
inline constexpr int LAB_WAVELET_MAX_THRESHOLD2_LEVEL = 9;
inline constexpr int LAB_WAVELET_MIN_STRENGTH = 0;
inline constexpr int LAB_WAVELET_MAX_STRENGTH = 100;
inline constexpr double LAB_WAVELET_MIN_WAVELET_THREND = 0.0;
inline constexpr double LAB_WAVELET_MAX_WAVELET_THREND = 100.0;
/* The chroma panel's strength and its contrast-modulation sigma. */
inline constexpr int LAB_WAVELET_MIN_WAVELET_CHRO = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_CHRO = 100;
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGMACOL = 0.025;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGMACOL = 2.5;
/* The edge panel's slider ranges, from the widget definitions. */
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGVAL = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGVAL = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGRAD = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGRAD = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGTHRESH = -50;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGTHRESH = 100;
inline constexpr double LAB_WAVELET_MIN_WAVELET_EDGEFFECT = 0.05;
inline constexpr double LAB_WAVELET_MAX_WAVELET_EDGEFFECT = 2.5;
/* The Lipschitz control's sliders, from the widget definitions. */
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGEDETECT = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGEDETECT = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGEDETECTTHR = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGEDETECTTHR = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGEDETECTTHR2 = -10;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGEDETECTTHR2 = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGESENSI = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGESENSI = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_EDGEAMPLI = 0;
inline constexpr int LAB_WAVELET_MAX_WAVELET_EDGEAMPLI = 100;
/* The denoise panel's five level sliders. The widget's upper value (the
 * strength) is the one the block reads; the lower value belongs to the
 * denoise curve. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_NOISE = 0.0;
inline constexpr double LAB_WAVELET_MAX_WAVELET_NOISE = 100.0;
/* The denoise-curve refinement's sigma slider. ip_wavelet() divides by it,
 * so a value outside the widget range is a residual. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGM = 0.05;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGM = 3.5;
/*
 * The expert arm's per-level sigma pair (`Levelsigm`). The block reads the
 * top value for levels 0..3 and the bottom for 4..5 through the same
 * divisions as Sigm, so both ends share the widget's 0.05..3 range.
 */
inline constexpr double LAB_WAVELET_MIN_LEVELSIGM = 0.05;
inline constexpr double LAB_WAVELET_MAX_LEVELSIGM = 3.0;
/*
 * The expert arm's wavmed strength (`Limden`). The widget admits 0..1 and
 * the block derives the curve's breaking point from `1 - Limden`.
 */
inline constexpr double LAB_WAVELET_MIN_LIMDEN = 0.0;
inline constexpr double LAB_WAVELET_MAX_LIMDEN = 1.0;
/* The noise-balance slider (Ballum) maps to a thirteen-entry table through
 * index = 10 - ballum, so the widget's -2..10 range is also the table range. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_BALLUM = -2.0;
inline constexpr double LAB_WAVELET_MAX_WAVELET_BALLUM = 10.0;
/* The wavelet chroma-denoise sliders' widget range (0..100). */
inline constexpr double LAB_WAVELET_MIN_CHROMA_STRENGTH = 0.0;
inline constexpr double LAB_WAVELET_MAX_CHROMA_STRENGTH = 100.0;
/* The noise-balance (Balance chroma) slider's widget range (-100..100). At
 * six levels the forced arm runs WaveletDenoiseAllAB() with local == 1, where
 * the block never reads variC, so the slider is inert for this arm. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_BALCHROM = -100.0;
inline constexpr double LAB_WAVELET_MAX_WAVELET_BALCHROM = 100.0;
/* The Thrden slider's widget range (0..100). */
inline constexpr double LAB_WAVELET_MIN_THRDEN = 0.0;
inline constexpr double LAB_WAVELET_MAX_THRDEN = 100.0;
/* The 128-pixel gate ip_wavelet() applies before it enables denoise. */
inline constexpr int LAB_WAVELET_MIN_DENOISE_WINDOW = 128;
/* At least one level slider must exceed this before the block denoises. */
inline constexpr double LAB_WAVELET_DENOISE_ACTIVE = 0.1;
/* The final panel's sigmafin slider range. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGMAFIN = 0.025;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGMAFIN = 2.5;
/* The toning panel's sigmaton slider range. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGMATON = 0.025;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGMATON = 2.5;
/* The balance-chroma arm's slider ranges, from the widget definitions. */
inline constexpr int LAB_WAVELET_MIN_WAVELET_BALANCE = -30;
inline constexpr int LAB_WAVELET_MAX_WAVELET_BALANCE = 100;
inline constexpr int LAB_WAVELET_MIN_WAVELET_ITER = -3;
inline constexpr int LAB_WAVELET_MAX_WAVELET_ITER = 3;
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGMADIR = 0.025;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGMADIR = 2.5;
/*
 * The clarity panel's merge sliders. The widget admits -50..100 for both; the
 * merge reads them directly, so a value outside that range is a residual.
 */
inline constexpr double LAB_WAVELET_MIN_MERGE = -50.0;
inline constexpr double LAB_WAVELET_MAX_MERGE = 100.0;
/* The clarity panel's guided soft radius slider: 0..100 in steps of 0.5. */
inline constexpr double LAB_WAVELET_MIN_SOFT_RAD = 0.0;
inline constexpr double LAB_WAVELET_MAX_SOFT_RAD = 100.0;
/*
 * The final panel's guided soft radius (softradend) slider is 0..1000 in steps
 * of one, with the strength and detail-adjust sliders at 0..100 and -10..10.
 */
inline constexpr double LAB_WAVELET_MIN_SOFT_END = 0.0;
inline constexpr double LAB_WAVELET_MAX_SOFT_END = 1000.0;
inline constexpr double LAB_WAVELET_MIN_STREND = 0.0;
inline constexpr double LAB_WAVELET_MAX_STREND = 100.0;
inline constexpr int LAB_WAVELET_MIN_DETEND = -10;
inline constexpr int LAB_WAVELET_MAX_DETEND = 10;
/* Lmethod's combo box: the nine level entries and the "SUPE" entry. */
inline constexpr int LAB_WAVELET_MIN_LMETHOD = 0;
inline constexpr int LAB_WAVELET_MAX_LMETHOD = 10;
/* finalContAllL()'s fallback when Lmethod is zero. */
inline constexpr std::uint32_t LAB_WAVELET_DEFAULT_CHOICE_LEVEL = 4;

/* The block's `scale` argument at every export call site. The equaliser's
 * BadpixelsLab() radius is 4 / scale, and the launcher requires this value. */
inline constexpr int LAB_WAVELET_EXPORT_SCALE = RT_LAB_WAVELET_EXPORT_SCALE;

/*
 * The equaliser's last active level: the block trims every trailing
 * multiplier within 0.001 of 1. Zero means the block would leave the image
 * untouched.
 */
int labWaveletLastLevel(const double* mult);

/*
 * ImProcFunctions::Mad()'s median step over a 32768-bin histogram of
 * truncated absolute coefficients. Exposed for the groundwork test.
 */
float labWaveletMadFromHistogram(const std::uint32_t* histogram,
                                 std::uint32_t samples);

/*
 * The number of transform levels ip_wavelet() actually builds for the L
 * plane: the MaxLev slider, trimmed by the image-size caps, the ContExtra
 * level and the trailing zero contrast multipliers. Zero means the block
 * returns before the decomposition. The device reproduces this derivation
 * exactly. The a and b planes are not raised with L: ip_wavelet() clamps
 * levwava and levwavb with min(levwav, ...), so they keep the base level.
 * `force_all_method` evaluates the trailing-zero trim as if CLmethod were
 * "all", which is the clarity arm's first pass.
 */
int labWaveletTransformLevels(const procparams::ProcParams& params,
                              int width, int height,
                              bool force_all_method = false);

/*
 * The same derivation with ip_wavelet()'s trailing-zero trim skipped: the
 * number of levels the block builds before it drops dead multipliers. The
 * balance-chroma arm needs this because its a/b passes disable that trim
 * (`!cp.diag`/`!cp.finena`) while L may still trim, and this node carries one
 * level count for both. Admission refuses a profile whose two counts differ.
 */
int labWaveletUntrimmedLevels(const procparams::ProcParams& params,
                              int width, int height);

/*
 * Whether the wavelet pass is the node's supported set: the contrast panel,
 * the chroma panel's "link" arm, the edge panel's plain arm, the final
 * panel's curve arm and the toning panel's zero-protection arm, no residual,
 * noise or blur panel, full-tile processing, an admitted final soft radius,
 * no L contrast
 * skin protection, no avoid-colour-shift or blue-sky median, no extra level,
 * the default 8-tap Daubechies, a highlight/shadow method the block
 * implements, identity chroma-by-hue and hue curves, every scalar in range
 * and an image large enough for the undecimated Haar bands. The edge arm
 * additionally requires the plain sub-mode: no median level detection,
 * np-method smoothing or lipst influence, and the CU detection method whose
 * hard-coded curve path is the one ported. The final arm requires the two
 * curve controls only: no balance-chroma method, no residual tone mapping and
 * a sigmafin inside its slider range.
 *
 * CLmethod's "one"/"inf"/"sup" level selection is ported for every admitted
 * panel set: the selection is finalContAllL()'s own coefficient zeroing and
 * L0 background paint, so the device reproduces it whenever the selection is
 * active. The clarity arm additionally requires `ushamethod` sharp or clari,
 * `expclari` and a known Backmethod; it runs the block's two-pass sequence and
 * merge. Lmethod, Dirmethod, Backmethod and the merge sliders must be inside
 * the widget ranges whenever the selection runs.
 *
 * The guided soft radius (`softrad`) belongs to the clarity arm: the block
 * reads it only inside the two-pass sequence, so it is admitted only there and
 * must be inside its 0..100 slider range. The final panel's guided soft radius
 * (`softradend`) is admitted on its own: it needs `expfinal`, its strength and
 * detail-adjust sliders inside their ranges and the radius inside its 0..1000
 * range. The block's coefficient-domain second pass (`Thrend > 0`) and its hue
 * modulation (a non-identity `wavguidcurve`) are both ported and admitted
 * alongside it. It does not run together with the clarity arm or the clarity
 * soft radius.
 *
 * The balance-chroma arm (`BAmethod` "sli" or "cur") runs inside the final
 * panel's coefficient pass and, for the a/b planes, with `TMr`. The "cur"
 * variant needs a non-identity `opacityCurveW`, the sliders and `sigmadir`
 * must be inside their widget ranges, and the L and a/b level counts must
 * agree because this node carries one count for both planes.
 *
 * The denoise arm carries `quaMethod` "cons" (the standard passes) and "agre"
 * (the BiShrink pair before them on L and on each a/b plane). The pair reads
 * the same `vari`, `ballum` and squared MAD tail; only the coefficient passes
 * differ. An unknown method stays a residual because the block leaves
 * `cp.quamet` uninitialised.
 */
bool labWaveletWaveletSupported(const procparams::ProcParams& params,
                                int width, int height);

/*
 * Whether the block is the node's supported set: an enabled directional
 * pyramid equaliser, an enabled wavelet tool, or both, each inside its own
 * admitted set, with every parameter inside the ABI bounds. The image-size and
 * CPU device checks live in the launcher; the wavelet half-size check lives in
 * labWaveletWaveletSupported().
 */
bool labWaveletSupported(const procparams::ProcParams& params, int width,
                         int height);

/*
 * The block as it ran before the device node, moved here unchanged so the
 * call site, the host builtin and the tests share exactly one CPU
 * implementation.
 */
void labWaveletHostBlock(ImProcFunctions& ipf,
                         procparams::ProcParams& params, LabImage& lab);

/*
 * The host-owned setup for one lab.wavelet submission.
 *
 * `build` derives the last active level, the per-level range tables and the
 * mask allocation. `prepare` runs the gamutlab pre-pass on the buffer it is
 * given and then fills the masks from that prepared image. The context owns
 * no device resource.
 */
class LabWaveletContext
{
public:
    LabWaveletContext(procparams::ProcParams& params,
                      ImProcFunctions& ipf, int width, int height);
    ~LabWaveletContext();

    LabWaveletContext(const LabWaveletContext&) = delete;
    LabWaveletContext& operator=(const LabWaveletContext&) = delete;

    /* Fill the host-only descriptor handed over as RT_STAGE_ARG_PTR. */
    RtLabWaveletContext descriptor(bool supported);

    /* Build the range tables, the masks and the wavelet parameter table.
     * Returns 0 on success. */
    int build(RtLabWaveletResult& out);

    int prepare(RtStageBuffer& lab);

    int runHost(RtStageBuffer& lab);

    /* Fill one plane's per-level mea blocks from its statistics block. */
    int computeMea(const float* stats, uint32_t levels, uint32_t plane);

    /* Fill the final-curve tail from the post-contrast statistics block. */
    int computeFinalStats(const float* stats, uint32_t levels);

    /* Fill the denoise tail's squared MAD entries for one plane from band
     * histograms. */
    int computeMad(const std::uint32_t* histogram, uint32_t levels,
                   uint32_t plane);

    /* Fill the denoise-curve refinement tail from the pre- and post-denoise
     * statistics blocks. */
    int computeRefine(const float* pre_stats, const float* post_stats,
                      uint32_t levels);

    /* Fill the final soft radius's coefficient-domain second block from the
     * two decomposition statistics blocks. */
    int computeSoftend(const float* stats_ll, const float* stats_dst,
                       uint32_t levels);

private:
    procparams::ProcParams& params_;
    ImProcFunctions& ipf_;
    int width_;
    int height_;

    bool built_ = false;
    int levels_ = 0;
    float multi_[RT_LAB_WAVELET_MAX_LEVELS] = {};
    bool gamutlab_ = false;
    float skinprotect_ = 0.f;
    float b_l_ = 0.f;
    float t_l_ = 0.f;
    float t_r_ = 0.f;
    std::vector<float> luts_;
    std::vector<float> hue_;
    std::vector<float> chrom_;

    bool wavelet_active_ = false;
    bool wavelet_chroma_ = false;
    bool wavelet_edge_ = false;
    bool wavelet_final_ = false;
    bool wavelet_toning_ = false;
    bool wavelet_denoise_ = false;
    /* 1 when `quaMethod == "agre"` runs the BiShrink pair before the standard
     * arm on L and on each a/b plane. */
    bool wavelet_bishrink_ = false;
    bool wavelet_refine_ = false;
    bool wavelet_refine_active_ = false;
    /* The refinement's levref and its expert pairing. */
    std::uint32_t wavelet_refine_levels_ =
        RT_LAB_WAVELET_REFINE_LEVELS_STANDARD;
    bool wavelet_refine_expert_ = false;
    std::vector<float> wavelet_refine_curve_;
    /* The expert arm's wavmed and wavhigh curves, in the same encoding as
     * wavelet_refine_poly_*: count x, count y and count-1 slopes. */
    std::vector<float> wavelet_refine_med_x_;
    std::vector<float> wavelet_refine_med_y_;
    std::vector<float> wavelet_refine_med_slope_;
    std::vector<float> wavelet_refine_high_x_;
    std::vector<float> wavelet_refine_high_y_;
    std::vector<float> wavelet_refine_high_slope_;
    /* The L denoise pass's wavhuecurve polyline: knot count, abscissae,
     * ordinates and count-1 slopes. Empty for the identity curve. */
    std::vector<float> wavelet_hue_curve_;
    bool wavelet_ab_ = false;
    bool wavelet_final_refresh_ = false;
    /*
     * The balance-chroma arm. `wavelet_ba_l_` is the L arm (`expfinal`) and
     * `wavelet_ba_ab_` the a/b arm (`TMr`); `wavelet_ba_` is either.
     * `wavelet_ba_offset_` is the tail's index inside `wavelet_params_`.
     */
    bool wavelet_ba_ = false;
    bool wavelet_ba_l_ = false;
    bool wavelet_ba_ab_ = false;
    std::size_t wavelet_ba_offset_ = 0;
    /* 0 samples the wavdenoise LUT, 1 walks the Thrden wavlow polyline. */
    std::uint32_t wavelet_refine_mode_ = 0;
    std::vector<float> wavelet_refine_poly_x_;
    std::vector<float> wavelet_refine_poly_y_;
    std::vector<float> wavelet_refine_poly_slope_;
    int wavelet_levels_ = 0;
    float wavelet_skinprotect_ = 0.f;
    float wavelet_b_l_ = 0.f;
    float wavelet_t_l_ = 0.f;
    float wavelet_t_r_ = 0.f;
    float wavelet_b_r_ = 0.f;
    std::vector<float> wavelet_params_;

    /* The CLmethod selection, decoded once for the descriptor and the
     * launcher. `selection_active_` is false for "all". */
    bool selection_active_ = false;
    std::uint32_t cl_method_ = RT_LAB_WAVELET_CL_ALL;
    std::uint32_t back_method_ = RT_LAB_WAVELET_BACK_GREY;
    std::uint32_t choice_level_ = LAB_WAVELET_DEFAULT_CHOICE_LEVEL;
    std::uint32_t choice_dir_ = RT_LAB_WAVELET_DIR_ALL;
    std::uint32_t dir1_ = 2;
    std::uint32_t dir2_ = 3;
    std::uint32_t max_ilev_ = 0;
    /* The clarity arm and, when the merge is non-trivial, its factors. */
    bool clarity_active_ = false;
    bool clarity_merge_ = false;
    int clarity_levels_first_ = 0;
    float clarity_l0_ = 0.f;
    float clarity_l_ = 0.f;
    float clarity_c0_ = 0.f;
    float clarity_c_ = 0.f;

    /*
     * The clarity arm's guided soft radius. `wavelet_soft_` is set when the
     * arm runs and softrad is non-zero; the derived scalars are the block's
     * own guidedFilter() inputs, computed once at build time.
     */
    bool wavelet_soft_ = false;
    float soft_epsilon_ = 0.f;
    int soft_w_ = 0;
    int soft_h_ = 0;
    int soft_box_rad_ = 0;
    float soft_scale_dx_ = 1.f;
    float soft_scale_dy_ = 1.f;
    float soft_scale_ux_ = 1.f;
    float soft_scale_uy_ = 1.f;

    /*
     * The final panel's guided soft radius. It runs after the final pixel
     * curve on the reconstructed L and blends the filtered L with the
     * pre-filter value. The scalars are the block's own guidedFilter() inputs.
     */
    bool softend_active_ = false;
    float softend_blend_ = 0.f;
    float softend_epsilon_ = 0.f;
    int softend_w_ = 0;
    int softend_h_ = 0;
    int softend_box_rad_ = 0;
    float softend_scale_dx_ = 1.f;
    float softend_scale_dy_ = 1.f;
    float softend_scale_ux_ = 1.f;
    float softend_scale_uy_ = 1.f;
    /*
     * The block's coefficient-domain second pass (`Thrend > 0`). Its wavguid
     * FlatCurve is a four-point curve whose plateau and breaking point follow
     * thrend; the polyline is uploaded in the same encoding as the refinement
     * tail's. `softend_thrend_offset_` is the tail block's index inside
     * wavelet_params_, so computeSoftend() can fill the per-level scalars.
     */
    bool softend_thrend_ = false;
    float softend_thrend_value_ = 0.f;
    std::size_t softend_thrend_offset_ = 0;
    std::vector<float> softend_thrend_curve_;
    /*
     * The hue modulation (`wavguidcurve` non-identity): the block's own
     * wavguidCurve polyline, in the same encoding.
     */
    bool softend_hue_ = false;
    std::vector<float> softend_hue_curve_;
};

} // namespace rtengine::stage
