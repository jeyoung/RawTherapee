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
 * rectangular spot, the local log-encoding tool, the local dehaze tool, the
 * local CBDL tool, the Colour & Light chroma/lightness/contrast sliders and
 * L/C/H curves, the vibrance tool, the shadows/highlights tool, the soft-light
 * tool, the Gaussian local-contrast tool, the deltaE scope, the
 * avoid-colour-shift Munsell method and the unconditional per-spot Lab-to-RGB
 * clamp. The block also applies the final Munsell pass and the clamp for an
 * inactive spot, so a block whose spots do no tool work is still a clamp-only
 * block.
 *
 * The tools run in Lab_Local()'s order: log encoding, dehaze, CBDL, vibrance,
 * shadows/highlights, soft light, local contrast, then Colour & Light. The
 * context is C-compatible because the ABI is. `self` points at the C++ object
 * that owns the ProcParams, ImProcFunctions and LabImage references, so the
 * pointer must not outlive them.
 *
 * The per-spot record is a flat float table rather than a structure mirrored
 * between C++ and the device: Lab_Local() takes 270 parameters and only a
 * small, known subset reaches the supported paths. Slots are named by the
 * RT_LOCAL_ADJUSTMENTS_SLOT_* constants and are written once per spot.
 */
#define RT_LOCAL_ADJUSTMENTS_CONTEXT_VERSION 7u
/* "LAJC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LOCAL_ADJUSTMENTS_CONTEXT_MAGIC 0x4C414A43u
/* The block has no bounded spot count, but a profile that carries more spots
 * than this is outside the device set and runs the host block. */
#define RT_LOCAL_ADJUSTMENTS_MAX_SPOTS 64u
/* Floats per spot record; the slot constants below must stay inside it. */
#define RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE 133u
/* A curve table is one LUTf of the block's own size. */
#define RT_LOCAL_ADJUSTMENTS_LUT_ENTRIES 65536u
/* Color::gammatab_srgb and Color::igammatab_srgb, the sRGB gamma pair the
 * soft-light curve reads. */
#define RT_LOCAL_ADJUSTMENTS_GAMMA_LUT_ENTRIES 65536u
#define RT_LOCAL_ADJUSTMENTS_LIGHT_LUT_ENTRIES 32770u
/* shadowsHighlightsLut()'s Lab-mode tone table, one entry per 16-bit L. */
#define RT_LOCAL_ADJUSTMENTS_SH_LUT_ENTRIES 32768u
/* shadowsHighlights() passes guidedFilter() an explicit subsampling of four
 * and an epsilon of 0.075. The fast filter needs at least three samples per
 * axis, so a shadows/highlights ROI below 4 * 3 px is a host execution. */
#define RT_LOCAL_ADJUSTMENTS_SH_SUBSAMPLING 4
#define RT_LOCAL_ADJUSTMENTS_SH_EPSILON 0.075f
#define RT_LOCAL_ADJUSTMENTS_SH_MIN_ROI 12
/* Local CBDL's own guard is `bfw > 65 && bfh > 65`; a smaller ROI does no
 * CBDL work and is not a refusal, because the host block skips the tool. */
#define RT_LOCAL_ADJUSTMENTS_CBDL_MIN_ROI 66
/* cbdl_local_temp()'s directional pyramid: six levels at scales 1..32, and
 * one 0x20000-entry range table per level, as dirpyrFillLut() builds them. */
#define RT_LOCAL_ADJUSTMENTS_CBDL_LEVELS 6u
#define RT_LOCAL_ADJUSTMENTS_CBDL_LUT_ENTRIES 0x20000u
/* dehazeloc()'s geometry. `patchsize` starts as max(int(5 / scale), 2); the
 * stage runs only at scale 1, which the context checks, so the initial patch
 * is five. The ambient estimate rescales the guided-filtered channels to at
 * most 200 px on the longer axis and samples the dark channel with the
 * initial patch; the final dark-channel patch is max(max(W, H) / 600, 2). */
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_SCALE 1.0
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_PATCH0 5
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_MIN_PATCH 2
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_SIZECAP 200
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_FINAL_DIVISOR 600
/* guidedfilter.cc's calculate_subsampling(): an image whose longer axis fits
 * 600 px is filtered without subsampling. */
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_GUIDE_SMALL 600
/* dehazeloc()'s final guided-filter radius is four times its dark-channel
 * patch. */
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_RADIUS_FACTOR 4
/* dehazeloc()'s two fixed tolerances, as written in the block. */
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_EPSILON 1e-1f
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_GUIDE_EPSILON 1e-5f
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_TEPS 1e-6f
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_LUMINANCE_FLOOR 1e-5f
/* The darkest ambient channel below which dehazeloc() declares "no haze" and
 * restores the image unchanged. */
#define RT_LOCAL_ADJUSTMENTS_DEHAZE_AMBIENT_FLOOR 0.01f
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
/* Shadows/highlights. EXP_SHADHIGHLIGHTS is 1 when the tool would change a
 * pixel: the tool on, the spot active and a positive highlights or shadows
 * slider. The method is Lab-mode "std"; the tonal widths, radius, sensitivity
 * and deltaE blur radius are the block's own values. The two tone tables are
 * appended to the result's flat set, one per non-zero round. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHADHIGHLIGHTS 82u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSSH 83u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_HIGHLIGHTS 84u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_SHADOWS 85u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_RADIUS 86u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_TONAL 87u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_TONAL 88u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_BLUR 89u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_OFFSET 90u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_HL_ACTIVE 91u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_OFFSET 92u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SH_SH_ACTIVE 93u
/* Soft light. EXP_SOFT is 1 when the host's soft-light block would run: the
 * tool on, the spot active, the per-pixel "soft" method and a strength above
 * one. The sRGB gamma pair is the separate block-global result member. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPSOFT 94u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SOFT_STRENGTH 95u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSSOFT 96u
/* Local CBDL. EXP_CBDL is 1 when a CBDL sub-pass will run: the host's
 * cbdlena guard, an ROI wider and taller than 65 pixels (the block's own
 * guard), and at least one pyramid level the luminance or chroma pass would
 * change. The level count is the block's trailing-identity trim; the chroma
 * multipliers are derived from `chromacb` and the luminance multipliers. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPCBDL 97u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSCB 98u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_THRESHOLD 99u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CLARITY 100u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA 101u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT0 102u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT1 103u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT2 104u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT3 105u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT4 106u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_MULT5 107u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_LEVELS 108u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_LEVELS 109u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_OFFSET 110u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_LUMA_ACTIVE 111u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_OFFSET 112u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CBDL_CHROMA_ACTIVE 113u
/* Local dehaze. EXPDENAZE is 1 when the host's dehaze block runs: the retinex
 * enable (expreti) is on, the spot is active, there is no explicit mask, and
 * the dehaze strength is non-zero. The retinex tool itself must be a no-op
 * (strength zero); an active retinex configuration is a residual. The
 * strength, depth and saturation slots hold the block's effective values:
 * `lp.dehaze`, `lp.depth` and the int truncation of `dehazeSaturation`. The
 * deltaE blend is senstype 30 and uses the retinex sensitivity (SENSH). */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPDENAZE 114u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSH 115u
#define RT_LOCAL_ADJUSTMENTS_SLOT_DEHAZE_STRENGTH 116u
#define RT_LOCAL_ADJUSTMENTS_SLOT_DEHAZE_DEPTH 117u
#define RT_LOCAL_ADJUSTMENTS_SLOT_DEHAZE_SATURATION 118u
/* Local log encoding. EXP_LOG is 1 when Lab_Local()'s log block runs: the tool
 * on, the spot active, no explicit log mask, no CIE CAM and detail zero. The
 * block copies the ROI to RGB, calls log_encode(), applies the tool's own
 * recovery weight (`repar`) and converts back. The apply lambda's scalars are
 * host-built: `gray`, `shadows_range`, `dynamic_range`, `comprthlog` from
 * `comprlog`, and `linbase` from find_gray(). `LOG_SENS` is the senstype-11
 * sensitivity (lp.sensilog); `LOG_REPART` is 1 - 0.01 * repar; `LOG_KAB_PRE`
 * is balancedeltaE(balance) / 327.68^2 evaluated before the senstype-11
 * threshold/balance adjustment. The adjusted `thr`, `iterat` and `balance`
 * are written into the shared slots so later tools see the block's mutation.
 * detail, the log gradient and autocompute are residuals. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPLOG 119u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_SENS 120u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_GRAY 121u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_SHADOWS_RANGE 122u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_DYNAMIC_RANGE 123u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_COMPRLOG 124u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_COMPRFACTOR 125u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_COMPRTHLOG 126u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_LINBASE 127u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_NOISE 128u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_LOG2 129u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_SATCONTROL 130u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_REPART 131u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LOG_KAB_PRE 132u

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
    /* The same working RGB -> XYZ matrix in double, nine values. The dehaze
     * recovery and the log encode call Color::rgbLuminance(r, g, b, ws) with
     * the block's TMatrix, which accumulates in double: the device needs the
     * double values for the ambient luminance and log's norm2/saturation. */
    const double* ws_double;
    /* Color::cachef[] and cachefy[], the 65536-sample Lab caches the RGB to
     * Lab half of the clamp reads. */
    const float* cachef;
    const float* cachefy;
    /* Color::gammatab_srgb[] and igammatab_srgb[], the 65536-sample sRGB
     * gamma pair the soft-light curve reads. */
    const float* gamma_srgb;
    const float* igamma_srgb;
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
    /* shadows/highlights' deltaE reference blur, sigma
     * (2 + 0.2 * blurSHde) / sk. Only the SSE path is admitted. */
    const RtLabToolsIir* iir_shadows_highlights;
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
 * any combination of local dehaze, local CBDL, Colour & Light, vibrance,
 * shadows/highlights, soft light and local contrast; the tool variants and
 * ranges are checked separately. The block has no pre-CBDL pass and no global
 * dehaze; the local dehaze is part of the spot path.
 *
 * Local dehaze is admitted only through its retinex gate: expreti on, the
 * retinex strength zero and a non-zero dehaz. dehazeblack, show-depth, an
 * explicit mask, a soft radius, a non-zero retinex strength and an
 * out-of-range strength, depth, saturation or sensitivity are residuals.
 *
 * Local CBDL is admitted with the block's own multipliers, threshold, clarity,
 * chroma and sensitivity. Its soft radius and contresid are residuals, as is a
 * tool mask; an ROI at or below 65 px on either axis is the block's own no-op.
 *
 * The predicate is block-level on purpose: the block is atomic, so one spot
 * outside the set makes every spot a host execution. An inactive spot is
 * supported: it does no tool work, but the block's per-spot clamp still runs,
 * and that clamp is part of the node.
 *
 * shadows/highlights is admitted in its Lab-mode "std" method only, with
 * tone-eq, GHS, the GHS local-contrast arm, the gradient, recovery blend and
 * every mask left at their defaults. Its per-round tone tables are host-built
 * by the block's own shadowsHighlightsLut() and the guided filter is the
 * lab.tools kernel.
 *
 * Soft light is admitted in its per-pixel "soft" method only; "reti" is the
 * retinex path and stays a residual. The sRGB gamma tables are host-built by
 * Color::init() and the working matrices are the block's own.
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
    double wsDouble_[9] = {};
    float iws_[9] = {};
    std::vector<float> cachef_;
    std::vector<float> cachefy_;
    std::vector<float> gammaSrgb_;
    std::vector<float> igammaSrgb_;
    std::vector<float> munsell_;
    std::vector<float> munsell_sizes_;
    RtLabToolsIir iir_ = {};
    RtLabToolsIir iirVib_ = {};
    RtLabToolsIir iirLc_ = {};
    RtLabToolsIir iirSh_ = {};
    uint32_t clamp_mode_ = RT_LOCAL_ADJUSTMENTS_CLAMP_NONE;
};

} // namespace rtengine::stage
