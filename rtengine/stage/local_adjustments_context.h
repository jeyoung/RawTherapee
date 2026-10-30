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
#include "stage/lab_tools_context.h"

#include <cstdint>
#include <vector>

namespace rtengine
{
class Imagefloat;
class ImProcFunctions;
class LabImage;
struct LocalAdjustmentsSpotParams;
}

namespace rtengine::procparams
{
struct ProcParams;
}

/*
 * Host context of the local.adjustments node.
 *
 * The node covers a spot set of ImProcFunctions::Lab_Local(): an elliptical or
 * rectangular spot, the Colour & Light chroma/lightness/contrast sliders and
 * L/C/H curves, the vibrance tool, the Gaussian local-contrast tool, the deltaE
 * scope, the avoid-colour-shift Munsell method and the unconditional per-spot
 * Lab-to-RGB clamp. The block also applies the final Munsell pass and the clamp
 * for an inactive spot, so a block whose spots do no tool work is still a
 * clamp-only block.
 *
 * The tools run in Lab_Local()'s order: vibrance, local contrast, then Colour &
 * Light. The context is C-compatible because the ABI is. `self` points at the
 * C++ object that owns the ProcParams, ImProcFunctions and LabImage references,
 * so the pointer must not outlive them.
 *
 * The per-spot record is a flat float table rather than a structure mirrored
 * between C++ and the device: Lab_Local() takes 270 parameters and only a
 * small, known subset reaches the supported paths. Slots are named by the
 * RT_LOCAL_ADJUSTMENTS_SLOT_* constants and are written once per spot.
 */
#define RT_LOCAL_ADJUSTMENTS_CONTEXT_VERSION 2u
/* "LAJC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LOCAL_ADJUSTMENTS_CONTEXT_MAGIC 0x4C414A43u
/* The block has no bounded spot count, but a profile that carries more spots
 * than this is outside the device set and runs the host block. */
#define RT_LOCAL_ADJUSTMENTS_MAX_SPOTS 64u
/* Floats per spot record; the slot constants below must stay inside it. */
#define RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE 82u
/* A curve table is one LUTf of the block's own size. */
#define RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES 65536u
#define RT_LOCAL_ADJUSTMENTS_LIGHT_LUT_ENTRIES 32770u
/* No table at this offset. Offsets below 2^24 are exact in a float. */
#define RT_LOCAL_ADJUSTMENTS_NO_LUT (-1.0f)
/* The largest total LUT allocation, in floats. Every offset stays exact in a
 * float and the whole set fits the smallest device budget with the image. */
#define RT_LOCAL_ADJUSTMENTS_MAX_LUT_ENTRIES (1u << 22)
/* spot.shape, as calcLocalParams() maps it. */
#define RT_LOCAL_ADJUSTMENTS_SHAPE_ELI 0u
#define RT_LOCAL_ADJUSTMENTS_SHAPE_RECT 1u
/* spot.avoidgamutMethod, as avoidcolshi() maps it. NONE and MUNS are the
 * device methods; LAB, XYZ and XYZREL need Color::gamutLchonly() or
 * Color::gamutmap() and are residuals. */
#define RT_LOCAL_ADJUSTMENTS_AVOID_NONE 0u
#define RT_LOCAL_ADJUSTMENTS_AVOID_MUNS 4u
/* The tail clamp of Lab_Local(). 0 skips it, 1 keeps every RGB channel at or
 * above epsi, 2 additionally applies clipR(). The block always takes one of
 * the two clamp arms for a supported profile because the tone curve mode
 * selects notzero; the mode is exported rather than assumed. */
#define RT_LOCAL_ADJUSTMENTS_CLAMP_NONE 0u
#define RT_LOCAL_ADJUSTMENTS_CLAMP_NOTZERO 1u
#define RT_LOCAL_ADJUSTMENTS_CLAMP_LAPLACIAN 2u
/* Lab_Local()'s two negative-value guards and the deltaE scope constants. */
#define RT_LOCAL_ADJUSTMENTS_EPSI0 0.000001f
#define RT_LOCAL_ADJUSTMENTS_EPSI 0.00001f
#define RT_LOCAL_ADJUSTMENTS_LIMSCOPE 80
#define RT_LOCAL_ADJUSTMENTS_MAXSCOPE 1.25f
#define RT_LOCAL_ADJUSTMENTS_MINSCOPE 0.025f

/* Per-spot record slots. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_XC 0u
#define RT_LOCAL_ADJUSTMENTS_SLOT_YC 1u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LX 2u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LY 3u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LXL 4u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LYT 5u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TRANS 6u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TRANSWEAK 7u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TRANSGRAD 8u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RAD 9u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIR 10u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CHRO 11u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CONT 12u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LIGH 13u
#define RT_LOCAL_ADJUSTMENTS_SLOT_STRUCO 14u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENS 15u
#define RT_LOCAL_ADJUSTMENTS_SLOT_THR 16u
#define RT_LOCAL_ADJUSTMENTS_SLOT_ITERAT 17u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BALANCE 18u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BALANCEH 19u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLURCOL 20u
/* calc_ref() outputs: the reference hue/chroma/lightness, the Sobel reference
 * and the area average complexCurvelocal() takes. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_HUEREF 21u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CHROMAREF 22u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LUMAREF 23u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SOBELREF 24u
#define RT_LOCAL_ADJUSTMENTS_SLOT_AVGE 25u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAPE 26u
#define RT_LOCAL_ADJUSTMENTS_SLOT_ACTIV 27u
#define RT_LOCAL_ADJUSTMENTS_SLOT_ISLOCAL 28u
#define RT_LOCAL_ADJUSTMENTS_SLOT_COLORENA 29u
#define RT_LOCAL_ADJUSTMENTS_SLOT_DELTAE 30u
#define RT_LOCAL_ADJUSTMENTS_SLOT_QCURVE 31u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MERGE 32u
#define RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDGAMUT 33u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CLAMPMODE 34u
#define RT_LOCAL_ADJUSTMENTS_SLOT_HRENABLED 35u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLWH 36u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPCOLOR 37u
#define RT_LOCAL_ADJUSTMENTS_SLOT_GAMC 38u
#define RT_LOCAL_ADJUSTMENTS_SLOT_REPARCOL 39u
#define RT_LOCAL_ADJUSTMENTS_SLOT_STRCOL 40u
#define RT_LOCAL_ADJUSTMENTS_SLOT_STRCOLAB 41u
#define RT_LOCAL_ADJUSTMENTS_SLOT_STRCOLH 42u
/* LUT offsets into the result's flat table, or RT_LOCAL_ADJUSTMENTS_NO_LUT. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_LL_OFFSET 43u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CL_OFFSET 44u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LC_OFFSET 45u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CC_OFFSET 46u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LH_OFFSET 47u
#define RT_LOCAL_ADJUSTMENTS_SLOT_HH_OFFSET 48u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CH_OFFSET 49u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_OFFSET 50u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_OFFSET 51u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_OFFSET 52u
/* 1 when the table at the matching offset is present and must be read. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_LL_ACTIVE 53u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CL_ACTIVE 54u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LC_ACTIVE 55u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CC_ACTIVE 56u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LH_ACTIVE 57u
#define RT_LOCAL_ADJUSTMENTS_SLOT_HH_ACTIVE 58u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CH_ACTIVE 59u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SATUR_ACTIVE 60u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SATURMOINS_ACTIVE 61u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LIGHT_ACTIVE 62u
/* The working-profile chroma scale the C=f(C) and L=f(C) curves are indexed
 * with, and 0.01 * settings->amchroma clamped to the block's 0.05..2 range,
 * which the L=f(H) curve uses. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_ADJUSTR 63u
#define RT_LOCAL_ADJUSTMENTS_SLOT_AMOUNTCHROM 64u
#define RT_LOCAL_ADJUSTMENTS_SLOT_AVOIDNEG 65u
/* Vibrance. EXP_VIBRANCE is 1 when the tool would change a pixel, which needs
 * the tool on, the spot active, a non-zero slider or a non-identity skin
 * curve; the host's gamma, gradients, warm/ciecam path and masks are
 * residuals. The skin curve is a 65536-entry table in the result's flat set. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPVIBRANCE 66u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSV 67u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PASTELS 68u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SATURATED 69u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_TOP 70u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_THR_BOTTOM 71u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_PROTECTSKINS 72u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_AVOIDCOLORSHIFT 73u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_OFFSET 74u
#define RT_LOCAL_ADJUSTMENTS_SLOT_VIB_SKIN_ACTIVE 75u
/* Local contrast. EXP_CONTRAST is 1 when the tool would change a pixel, which
 * needs the tool on, the spot active and a non-zero amount. The Gaussian
 * radius and its coefficients are the separate result member. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPCONTRAST 76u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSLC 77u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LC_AMOUNT 78u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LC_DARKNESS 79u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LC_LIGHTNESS 80u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LC_RADIUS 81u

typedef struct RtLocalAdjustmentsResult {
    /* spot_count RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE floats, spot-major. */
    const float* spots;
    uint32_t spot_count;
    uint32_t spot_stride;
    /* The flat curve tables, indexed by the records' offsets. */
    const float* luts;
    uint32_t lut_count;
    /* The working RGB -> XYZ and XYZ -> RGB matrices, nine floats each. The
     * host evaluates their products in double for the clamp; the device reads
     * these float values. */
    const float* ws;
    const float* iws;
    /* Color::cachef[] and cachefy[], the 65536-sample Lab caches the RGB to
     * Lab half of the clamp reads. */
    const float* cachef;
    const float* cachefy;
    /* The flat Munsell table set and one length per table, for the MUNS
     * method of the final avoidcolshi() pass. */
    const float* munsell;
    const float* munsell_sizes;
    uint32_t munsell_tables;
    /* RT_LOCAL_ADJUSTMENTS_CLAMP_* for every spot. */
    uint32_t clamp_mode;
    /* The host-built Gaussian coefficients for origblur, sigma
     * (2 + 0.2 * blurcolde) / sk with sk = 1. Only the SSE path is admitted;
     * the launcher refuses double_path. */
    const RtLabToolsIir* iir;
    /* The same coefficients at sigma 3/sk = 3, the deltaE reference radius
     * transit_shapedetect2() uses for the vibrance (senstype 2) and local
     * contrast (senstype 10) tools. Only the SSE path is admitted. */
    const RtLabToolsIir* iir_vibrance;
    /* Local contrast's own Gaussian, sigma = lcradius / sk. Both the SSE and
     * the double path are admitted; the launcher reads double_path. */
    const RtLabToolsIir* iir_local_contrast;
} RtLocalAdjustmentsResult;

typedef struct RtLocalAdjustmentsContext {
    uint32_t size;
    uint32_t version;
    uint32_t magic;
    /*
     * 1 when the call site admits the configuration on the device. A zero
     * value makes the launcher refuse before it touches the Lab buffer, which
     * is how a residual configuration is recorded as a host execution.
     */
    uint32_t supported;
    /* Number of spots in the block; build() takes an index in 0..count-1. */
    uint32_t spot_count;
    void* self;
    /*
     * Builds one spot's flat record and curve table. `spot` selects the spot
     * in the profile; `lab` supplies the pixels calc_ref() reads for the
     * references and must hold the image as it stands *before* that spot,
     * because each spot's reference is the result of the previous one. The
     * launcher calls this once per spot and reads the buffer back between
     * calls, so a single build cannot prepare a later spot. `lab` may be null
     * when only the tables are wanted, in which case the reference slots are
     * left at zero. Returns 0 on success.
     */
    int (*build)(void* self, int32_t spot, const RtStageBuffer* lab,
                 RtLocalAdjustmentsResult* out);
    /*
     * Runs the original host block on the buffer. The builtin calls this so a
     * residual and a refused device request produce the CPU result. The block
     * owns the pre-CBDL pass and the final lab2rgb(), so the buffer's input
     * pixels are ignored; the finished Lab image is copied back.
     */
    int (*run_host)(void* self, RtStageBuffer* lab);
} RtLocalAdjustmentsContext;

namespace rtengine::stage
{

/*
 * Whether the whole block is the local.adjustments node's supported set: every
 * spot is a normal (not exclude/full/main) elliptical or rectangular spot with
 * no recursion, inversion, explicit mask, soft radius, merge, gradient, Lab
 * grid, RGB curve, exposure curve or tool outside the set. An active spot runs
 * any combination of Colour & Light, vibrance and local contrast; the vibrance
 * and local-contrast variants and ranges are checked separately. The block has
 * no pre-CBDL or dehaze pass.
 *
 * The predicate is block-level on purpose: the block is atomic, so one spot
 * outside the set makes every spot a host execution. An inactive spot is
 * supported: it does no tool work, but the block's per-spot clamp still runs,
 * and that clamp is part of the node.
 */
bool localAdjustmentsSupported(const procparams::ProcParams& params);

/*
 * The call-site dispatch for one local.adjustments submission.
 *
 * `submit` is false for an inactive block: no spots or locallab disabled.
 * Such a block must not enter the node and must not round-trip the base image
 * through Lab. `prefill` is true only when the block is active and the
 * whole-block predicate is true; only then does the call site convert
 * baseImg into the Lab buffer before submitting. A residual active block
 * still submits with the support flag clear so the builtin records impl=host.
 */
struct LocalAdjustmentsDispatch {
    bool submit = false;
    bool prefill = false;
    bool supported = false;
};

LocalAdjustmentsDispatch localAdjustmentsDispatch(
    const procparams::ProcParams& params);

/* The clamp arm Lab_Local() takes after the spots. The tone curve mode makes
 * notzero true in every exported profile, so a supported block always has an
 * arm; notlaplacian depends on the exposure, soft-light, CBDL and GHS
 * leftover state. */
uint32_t localAdjustmentsClampMode(const procparams::ProcParams& params,
                                   int spot);

/*
 * The block as it ran before the device node, moved here unchanged so the call
 * site, the host builtin and the tests share one CPU implementation.
 *
 * It applies the shardamping fixups, the optional pre-CBDL directional
 * pyramid equaliser, rgb2lab() into `lab`, the spot loop and the final
 * lab2rgb(). `lab` must not be null; the block fills it in place.
 */
void localAdjustmentsHostBlock(ImProcFunctions& ipf,
                               procparams::ProcParams& params,
                               Imagefloat* baseImg,
                               LabImage* lab,
                               int fw, int fh);

/*
 * The host-owned setup for one local.adjustments submission.
 *
 * `build` runs calcLocalParams(), calc_ref(), complexCurvelocal() and the
 * curve factories on the host, so the device sees the block's own values. The
 * context owns no device resource. `runHost` runs the extracted host block.
 */
class LocalAdjustmentsContext
{
public:
    /*
     * `baseImg` and `labView` are the export call site's images. The block
     * may replace the Lab pixels and reads the base image for the pre-CBDL
     * pass, so both references are kept for runHost().
     */
    LocalAdjustmentsContext(procparams::ProcParams& params,
                            ImProcFunctions& ipf,
                            Imagefloat* baseImg,
                            LabImage*& labView,
                            int fw, int fh);
    ~LocalAdjustmentsContext();

    LocalAdjustmentsContext(const LocalAdjustmentsContext&) = delete;
    LocalAdjustmentsContext& operator=(const LocalAdjustmentsContext&) = delete;

    /* Fill the host-only descriptor handed over as RT_STAGE_ARG_PTR. */
    RtLocalAdjustmentsContext descriptor(bool supported);

    /* Build one spot's record and tables. Returns 0 on success. */
    int build(int spot, const RtStageBuffer* lab,
              RtLocalAdjustmentsResult& out);

    int runHost(RtStageBuffer& lab);

private:
    procparams::ProcParams& params_;
    ImProcFunctions& ipf_;
    Imagefloat* base_;
    LabImage*& labView_;
    int fw_;
    int fh_;

    bool built_ = false;
    bool globals_built_ = false;
    std::vector<float> spots_;
    std::vector<float> luts_;
    float ws_[9] = {};
    float iws_[9] = {};
    std::vector<float> cachef_;
    std::vector<float> cachefy_;
    std::vector<float> munsell_;
    std::vector<float> munsell_sizes_;
    RtLabToolsIir iir_ = {};
    RtLabToolsIir iirVib_ = {};
    RtLabToolsIir iirLc_ = {};
    uint32_t clamp_mode_ = RT_LOCAL_ADJUSTMENTS_CLAMP_NONE;
};

} // namespace rtengine::stage
