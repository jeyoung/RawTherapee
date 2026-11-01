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
#define RT_LAB_WAVELET_CONTEXT_VERSION 4u
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
#define RT_LAB_WAVELET_WAVELET_PARAMS 80u
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
/* Slots 48..79 hold Daubechies analysis lo/hi and synthesis lo/hi. */
#define RT_LW_PARAM_FILTER_ANAL_LO 48
#define RT_LW_PARAM_FILTER_ANAL_HI 56
#define RT_LW_PARAM_FILTER_SYNTH_LO 64
#define RT_LW_PARAM_FILTER_SYNTH_HI 72
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
 * The exact length of the wavelet parameter table. Both the host builder and
 * the launcher derive it from this one function so a table built without a
 * tail can never be read past its end.
 */
inline std::size_t labWaveletParamEntries(std::uint32_t planes,
                                          std::uint32_t levels, bool edge,
                                          bool final_curves)
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
    uint32_t levels;
    uint32_t taps;
    uint32_t entries;
    /* 1 for L only, 3 when the a/b planes also run (the chroma arm, the edge
     * panel or the final curves, whose level-trim change keeps the block's
     * a/b round trip). */
    uint32_t planes;
    /* 1 when the final-curve tail is present in `params`. */
    uint32_t final;
    float* params;
    int (*compute_mea)(void* self, const float* stats, uint32_t levels,
                       uint32_t plane);
    /*
     * Fills the final-curve tail from the post-contrast statistics block.
     * finalContAllL() reads the statistics Evaluate2() recomputes after the
     * contrast and edge passes, so this callback is separate from the mea
     * build. Used only when `final` is set.
     */
    int (*compute_final)(void* self, const float* stats, uint32_t levels);
} RtLabWaveletWavelet;

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
/* The final panel's sigmafin slider range. */
inline constexpr double LAB_WAVELET_MIN_WAVELET_SIGMAFIN = 0.025;
inline constexpr double LAB_WAVELET_MAX_WAVELET_SIGMAFIN = 2.5;

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
 * The number of transform levels ip_wavelet() actually builds for this image:
 * the MaxLev slider, trimmed by the image-size caps, the ContExtra level and
 * the trailing zero contrast multipliers. Zero means the block returns before
 * the decomposition. The device reproduces this derivation exactly.
 */
int labWaveletTransformLevels(const procparams::ProcParams& params,
                              int width, int height);

/*
 * Whether the wavelet pass is the node's supported set: the contrast panel,
 * the chroma panel's "link" arm, the edge panel's plain arm and the final
 * panel's curve arm, no residual, toning, noise, clarity or blur panel, the
 * "all" level method, full-tile processing, no soft radius, no L contrast
 * skin protection, no avoid-colour-shift or blue-sky median, no extra level,
 * the default 8-tap Daubechies, a highlight/shadow method the block
 * implements, identity chroma-by-hue and hue curves, every scalar in range
 * and an image large enough for the undecimated Haar bands. The edge arm
 * additionally requires the plain sub-mode: no median level detection,
 * np-method smoothing or lipst influence, and the CU detection method whose
 * hard-coded curve path is the one ported. The final arm requires the two
 * curve controls only: no balance-chroma method, no residual tone mapping
 * (soft radius end is refused) and a sigmafin inside its slider range.
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
    bool wavelet_ab_ = false;
    int wavelet_levels_ = 0;
    float wavelet_skinprotect_ = 0.f;
    float wavelet_b_l_ = 0.f;
    float wavelet_t_l_ = 0.f;
    float wavelet_t_r_ = 0.f;
    float wavelet_b_r_ = 0.f;
    std::vector<float> wavelet_params_;
};

} // namespace rtengine::stage
