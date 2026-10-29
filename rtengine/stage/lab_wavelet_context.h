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
 * here is passed to a kernel. The context builds the per-level range tables
 * through the equaliser's own fillLut(), runs the hue/chroma precompute
 * through the equaliser's own SSE-aware code, and can run the original host
 * block for a residual.
 *
 * Gamutlab protection is a host pre-pass: the block calls BadpixelsLab()
 * before the equaliser when gamutlab is set and skin protection is non-zero.
 * That pass is a separable Gaussian analysis with host-specific SSE rounding,
 * so the device launcher runs it on a host copy and uploads the result rather
 * than approximating it. The prepared copy is what the hue/chroma masks are
 * built from, in the same order as the block.
 */
#define RT_LAB_WAVELET_CONTEXT_VERSION 1u
/* "LWCT": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LAB_WAVELET_CONTEXT_MAGIC 0x4C574354u
/* The number of levels of the directional pyramid. */
#define RT_LAB_WAVELET_MAX_LEVELS 6u
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
} RtLabWaveletResult;

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
 * Whether the block is the node's supported set: an enabled directional
 * pyramid equaliser in after-CBDL position, no wavelet pass, the CIE-CAM gate
 * open, and every parameter inside the ABI bounds. The image-size and CPU
 * device checks live in the launcher.
 */
bool labWaveletSupported(const procparams::ProcParams& params);

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

    /* Build the range tables and allocate the masks. Returns 0 on success. */
    int build(RtLabWaveletResult& out);

    int prepare(RtStageBuffer& lab);

    int runHost(RtStageBuffer& lab);

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
};

} // namespace rtengine::stage
