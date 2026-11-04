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
 * rectangular spot, the local log-encoding tool, the local tone-mapping tool,
 * the local dehaze tool, the local CBDL tool, the Colour & Light
 * chroma/lightness/contrast sliders and
 * L/C/H curves, the vibrance tool, the shadows/highlights tool, the soft-light
 * tool, the Gaussian local-contrast tool, the local sharpening tool, the
 * local Blur & Noise tool (Gaussian and median arms), the Retinex tool, the
 * CIE CAM (CAM16) tool, the common mask for the log and CIE arms, the deltaE
 * scope, the avoid-colour-shift Munsell method and the unconditional
 * per-spot Lab-to-RGB clamp. The block also
 * applies the final Munsell pass and the clamp for an inactive spot, so a
 * block whose spots do no tool work is still a clamp-only block.
 *
 * The tools run in Lab_Local()'s order: log encoding, blur, tone mapping,
 * dehaze, Retinex, CBDL, vibrance, shadows/highlights, soft light, local
 * contrast, local sharpening, Colour & Light, then the CIE CAM. The common
 * mask is built before its owning tool and applied by that tool's blend. The
 * context is C-compatible because the ABI is. `self`
 * points at the C++ object that owns the ProcParams, ImProcFunctions and
 * LabImage references, so the pointer must not outlive them.
 *
 * The per-spot record is a flat float table rather than a structure mirrored
 * between C++ and the device: Lab_Local() takes 270 parameters and only a
 * small, known subset reaches the supported paths. Slots are named by the
 * RT_LOCAL_ADJUSTMENTS_SLOT_* constants and are written once per spot.
 */
#define RT_LOCAL_ADJUSTMENTS_CONTEXT_VERSION 16u
/* "LAJC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LOCAL_ADJUSTMENTS_CONTEXT_MAGIC 0x4C414A43u
/* The block has no bounded spot count, but a profile that carries more spots
 * than this is outside the device set and runs the host block. */
#define RT_LOCAL_ADJUSTMENTS_MAX_SPOTS 64u
/* Floats per spot record; the slot constants below must stay inside it. */
#define RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE 384u
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
/* Local sharpening. EXP_SHARP is 1 when Lab_Local()'s RL arm runs: the tool
 * on, the spot active and no explicit mask, `methodcap == "rl"`, an amount of
 * at least one, and a radius above the block's 0.42 no-op threshold but below
 * the 1.15 sigma where the sharable RLD kernels stop. The block runs
 * deconvsharpeningloc() on the ROI L plane and blends with Sharp_Local()'s
 * deltaE scope, whose senstype 0 reads `lp.senssha`. A radius at or below
 * 0.42 is the block's own no-op and is recorded as a skipped pass, not a
 * refusal. SHAR_DAMPING is the block's effective `shardamping`, after the
 * host fixup: zero selects the undamped recurrence, a positive value the
 * damped one. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPSHARP 133u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SENSSHA 134u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_AMOUNT 135u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_RADIUS 136u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_ITER 137u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_CONTRAST 138u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_BLUR 139u
#define RT_LOCAL_ADJUSTMENTS_SLOT_SHAR_DAMPING 140u
/* Blur & Noise. EXPBLUR is 1 when the host's second blur gate runs with the
 * tool on, the spot active and no mask, in either admitted arm: `blMethod ==
 * "blur"` (Gaussian) or `"med"` (median) in their normal direction, with the
 * noise, grain, guided-filter, bilateral, non-local-means and denoise
 * additions idle and the FFT switch off. BLUR_ARM is 0 for the Gaussian arm
 * and 1 for the median arm. BLUR_RADIUS is the block's
 * `lp.rad / (sk * 1.4)`; the context also exports its Gaussian coefficients
 * so the device reads the host's own sigma. BLUR_CHRO is 0 for luminance, 1
 * for chroma and 2 for all three planes. BLUR_SENS is `lp.sensbn`, the scope
 * sensitivity. BLUR_MED and BLUR_ITERS are the median arm's `lp.medmet`
 * (0 3x3 strong, 1 5x5 strong, 2 7x7, 3 9x9) and `lp.it` pass count. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPBLUR 141u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLUR_CHRO 142u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLUR_SENS 143u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLUR_RADIUS 144u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLUR_ARM 145u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLUR_MED 146u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BLUR_ITERS 147u
/* Tone Mapping. EXP_TONEMAP is 1 when Lab_Local()'s "tm" block runs: the tool
 * on an active spot with every explicit mask idle, the global EPD pass
 * disabled, a non-zero strength and an ROI of at least 128 px on both axes
 * (the block's own mDEN gate; a smaller ROI makes the tool a no-op). The
 * device runs EPDToneMaplocal()'s edge-preserving decomposition over the ROI
 * and blends it with transit_shapedetect2()'s senstype-8 arm. TM_SENS is the
 * block's `lp.senstm`, TM_REPART the recovery weight `1 - 0.01 * repartm`,
 * TM_GAMMA the tone gamma. The remaining scalars are host-built exactly as
 * EPDToneMaplocal() and EdgePreservingDecomposition build them: the
 * compression exponent `expf(-stren)`, the detail boost (zero for a negative
 * strength), the decompression `temp`, the colour-correction factor `sat`,
 * the effective edge stopping (capped at three when the global local-contrast
 * tool is on, else four), the EPD scale `scaltm / sk`, the auto iteration
 * count and the effective reweighting count (zero for an ROI smaller than
 * 300 px on either axis). */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_TONEMAP 148u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_SENS 149u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_REPART 150u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_GAMMA 151u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_COMPRESSION 152u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_DETAILBOOST 153u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_TEMP 154u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_SAT 155u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_EDGEST 156u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_SCALE 157u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_ITERATES 158u
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_REWEIGHT 159u
/* `equiltm`, the block's luminance-normalisation switch. The GUI default is
 * on, so both arms are admitted: the device reproduces
 * normalize_mean_dt()'s two double-precision reductions and its `a * data + b`
 * rescale. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_TM_EQUIL 160u
/* Retinex. EXP_RETINEX is 1 when Lab_Local()'s call==2 arm runs: the tool on
 * an active spot, a strength of at least 0.2 (the block's own gate), no
 * explicit mask and an ROI of at least 5 px wide and more than 5 px tall.
 * The device reproduces MSRLocal()'s multi-scale pass, its three mean/stdv
 * reductions, both 501-entry transmission/gain tables and
 * transit_shapedetect_retinex()'s senstype-4 and senstype-5 blends.
 *
 * Every scalar the host derives is exported: the block's strength and the
 * `pow(strength, 0.4)` light/dark factor, the variance, the darkness and
 * lightness multipliers, `limd ^ 1.7` and its reciprocal, the clip, the
 * offset, the log/linear switch, the `pond` scale, the soft-radius-derived
 * deltaE sensitivity, the `strcli` boost, the senstype-4 scope argument and
 * the two curve-table offsets. RETI_BLUR_STAGES is the scale count and
 * RT_LOCAL_ADJUSTMENTS_SLOT_RETI_BLUR_BASE the first of ten stages, each
 * holding the box-blur iteration count and six host-computed radii in
 * RT_LOCAL_ADJUSTMENTS_RETI_BLUR_STRIDE floats.
 */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_RETINEX 161u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_STRENGTH 162u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_SCALE 163u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_VART 164u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_DARK 165u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_LIGHT 166u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_VALUE 167u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_LIMD 168u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_ILIMD 169u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_CLIPTM 170u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_OFFS 171u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_LOGLIN 172u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_POND 173u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_SOFTRADIUS 174u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_STRCLI 175u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_SENSIBEFORE 176u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_GAIN_OFFSET 177u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_GAIN_ACTIVE 178u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_TRANS_OFFSET 179u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_TRANS_ACTIVE 180u
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_BLUR_STAGES 181u
/* The block's `strength = str / 100`, which intp() uses on the final blend. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_STR 182u
/* Ten box-blur stages, each n + six radii, starting here. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_RETI_BLUR_BASE 200u

/* CIE CAM (CAM16). EXP_CIE is 1 when Lab_Local()'s "com" block runs on an
 * active spot: the tool on, the spot active, no explicit mask and an ROI at
 * least mSP = 5 px on both axes. The device runs ciecamloc_02float()'s
 * call==0 arm for the CAM16 appearance transform, the block's final gain and
 * gamut round trip and transit_shapedetect2()'s senstype-31 blend.
 *
 * Every scalar the host derives is exported rather than recomputed, so the
 * kernel reads the block's own values: the two set of CAM16 viewing
 * conditions (initcam1float()/initcam2float() outputs), the two whites, the
 * derived powers and coefficients, and the tone-slider amounts. The kernel
 * reads the host-built CAMBrightCurveJ and CAMBrightCurveQ tables, which
 * depend only on the admitted light sliders (the contrast sliders, whose
 * tables read the ROI histogram, stay on the host).
 */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_CIE 270u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_C 271u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_NC 272u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_AW 273u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_FL 274u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_WH 275u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_NBB 276u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_NCB 277u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_CZ 278u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_D 279u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_POW1 280u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_POW1N 281u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_COEFQ 282u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_QPRO 283u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_PFL 284u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_PLUM 285u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_XW1 286u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_YW1 287u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_ZW1 288u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_XW2 289u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_YW2 290u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_ZW2 291u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_C2 292u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_NC2 293u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_AWJ 294u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_FLJ 295u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_NBBJ 296u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_NCBJ 297u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_CZJ 298u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_DJ 299u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_RECCMCZ 300u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_COE 301u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_MCHR 302u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_SCHR 303u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_CCHR 304u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_HUE 305u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_RSTPROTECT 306u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_SENS 307u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_REPART 308u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_CAMJ_OFFSET 309u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_CAMQ_OFFSET 310u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_HUERED 311u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_SCHRRED 312u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_HUEGREEN 313u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_SCHRGREEN 314u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_HUEBLUE 315u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CIE_SCHRBLUE 316u
/* Common mask. `maskcalccol()` builds the mask the log-encoding and CIE CAM
 * tools pass to transit_shapedetect2() as their deltaE reference. The admitted
 * arm is the luminance mask curve alone: the chroma and hue curves are at
 * their constant-one defaults, which zero the mask's a/b channels, and the
 * recovery blend (`maskrecov()`) is the decay arm. MASK_LOG and MASK_CIE
 * record which tool enabled the mask; MASK_CURVE_ACTIVE is the curve's own
 * `utili` flag and MASK_CURVE_OFFSET the 501-entry LUT. MASK_RECOTH is the
 * effective recovery threshold after the block's `2 - x` inversion for a
 * value below one; MASK_LOW, MASK_HIGH and MASK_DECAY are the other
 * `maskrecov()` scalars. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG 317u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE 318u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG_ACTIVE 319u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG_OFFSET 320u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG_RECOTH 321u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG_LOW 322u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG_HIGH 323u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_LOG_DECAY 324u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE_ACTIVE 325u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE_OFFSET 326u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE_RECOTH 327u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE_LOW 328u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE_HIGH 329u
#define RT_LOCAL_ADJUSTMENTS_SLOT_MASK_CIE_DECAY 330u
/* LocLLmaskCurve's LUT size: `(500 / 32768) * L` indexes it with a
 * 501-point interpolation. */
#define RT_LOCAL_ADJUSTMENTS_MASK_CURVE_ENTRIES 501u
/* maskcalccol() blurs its own mask with gaussianBlur() at 1 / sk; the export
 * call site passes sk = 1. maskrecov()'s box blur uses 10 / sk, three times. */
#define RT_LOCAL_ADJUSTMENTS_MASK_SIGMA 1.0
#define RT_LOCAL_ADJUSTMENTS_MASK_BOX_RADIUS 10
#define RT_LOCAL_ADJUSTMENTS_MASK_BOX_PASSES 3
/* The admitted `maskrecov()` sliders are the GUI's. */
/* The recovery threshold's upper half (1..2) also enables the mask itself as
 * transit_shapedetect2()'s deltaE reference; that combined arm needs one more
 * reference build and stays a host residual, so only the lower half is
 * admitted. The block inverts it to the same 1..2 effective threshold the
 * record carries and the launcher validates. */
#define RT_LOCAL_ADJUSTMENTS_MASK_MIN_RECOTH 1.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MAX_RECOTH 2.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MIN_RECOTH_INPUT 0.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MAX_RECOTH_INPUT 1.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MIN_LOW 0.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MAX_LOW 100.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MIN_HIGH 0.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MAX_HIGH 100.0
#define RT_LOCAL_ADJUSTMENTS_MASK_MIN_DECAY 0.5
#define RT_LOCAL_ADJUSTMENTS_MASK_MAX_DECAY 4.0
/* The CIE mask's own blur and contrast sliders. Above the 0.25 structure
 * gate the block builds a structure mask (and may take optfft), so the
 * admitted blur stays below it; contrast zero keeps buildBlendMask() idle. */
#define RT_LOCAL_ADJUSTMENTS_MASK_MAX_BLUR 0.25

/*
 * Capture sharpening. Lab_Local()'s `"cap"` arm is separate from the RL arm:
 * it copies the clipped ROI, converts it to working RGB, builds a contrast
 * mask from RGB2L and runs CaptureDeconvSharpening_SE() on the Rec2020
 * luminance, then multiplies the RGB channels by the new/old ratio and
 * returns through rgb2lab(). EXP_CAPTURE is 1 when the arm runs: the tool on,
 * an active spot with no explicit mask, a clipped ROI of at least mSPsharpCS
 * (150) px on both axes, and the configuration below.
 *
 * The admitted configuration keeps the deconvolution deterministic and
 * scalar: no gamma wrapper (`deconvCogam == 1`), no corner radius boost
 * (`deconvCoBoost == 0`), the manual contrast (`deconvAutoshar == false`) so
 * buildBlendMask2()'s data-dependent tile search is not needed, and the
 * GUI's iteration radius. CAP_SIGMA is `capradius`, CAP_ITER the truncated
 * `deconvCoLat`, CAP_CONTRAST the manual `sharcontrast` and CAP_REPART the
 * `1 - 0.01 * reparsha` blend weight. The device reproduces the block's tiled
 * division/multiplication recurrence, including the end-of-row/column tiles,
 * the `itercheck` early exit and the sigma-selected 3x3..13x13 kernels.
 */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_CAPTURE 331u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_SIGMA 332u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_ITER 333u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_CONTRAST 334u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_REPART 335u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_GAMMA 336u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_KERNEL_OFFSET 337u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_KERNEL_N 338u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_BORDER 339u
#define RT_LOCAL_ADJUSTMENTS_SLOT_CAP_CHECK_STOP 340u
/* Local exposure. EXP_EXPOSURE is 1 when Lab_Local()'s exposure block runs
 * with the PDE arm: the tool on, the spot active, no explicit mask,
 * `linear > 0`, `laplacexp > 0.1`, the gamma wrapper at 1, no recovery
 * weight, gradient, chroma, black-point, shadow-recovery, structure or fat
 * addition, and an ROI whose width and height are each an `fftw_size[]`
 * entry, so optfft() is the identity. EXP_SENS is `lp.sensex` (senstype 1).
 * The tone tables are the block's own hltonecurveloc, shtonecurveloc and
 * tonecurveloc; the six DCT tables are host-built for the exact ROI so the
 * device reproduces FFTW's REDFT10/REDFT01 pair. */
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_EXPOSURE 341u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXPCOMP 342u
#define RT_LOCAL_ADJUSTMENTS_SLOT_HLCOMP 343u
#define RT_LOCAL_ADJUSTMENTS_SLOT_HLCOMPTHR 344u
#define RT_LOCAL_ADJUSTMENTS_SLOT_LAPLACEXP 345u
#define RT_LOCAL_ADJUSTMENTS_SLOT_BALANEXP 346u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_LINEAR 347u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_GAM 348u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_BLUREXP 349u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_SENS 350u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_HL_OFFSET 351u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_SH_OFFSET 352u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_TONE_OFFSET 353u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_FWDW_OFFSET 354u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_FWDH_OFFSET 355u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_INVW_OFFSET 356u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_INVH_OFFSET 357u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_POISW_OFFSET 358u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_POISH_OFFSET 359u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_BFW 360u
#define RT_LOCAL_ADJUSTMENTS_SLOT_EXP_BFH 361u
/* iplocallab.cc's mSPsharpCS: buildBlendMask2() needs 150 px on both axes.
 * A smaller ROI makes the capture block a no-op, not a refusal. */
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MIN_ROI 150
/* The GUI ranges of the capture sliders. */
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MIN_SIGMA 0.4
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MAX_SIGMA 2.5
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MIN_ITER 0
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MAX_ITER 100
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MIN_CONTRAST 3
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MAX_CONTRAST 200
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MIN_REPART 0.0
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MAX_REPART 100.0
/* CaptureDeconvSharpening_SE()'s kernel selection thresholds. The offset-zero
 * arm picks the kernel from sigma alone; the border also depends on whether
 * the iteration count exceeds 30. */
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_SIGMA_3 0.6
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_SIGMA_5 0.84
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_SIGMA_7 1.15
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_SIGMA_9 1.5
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_BORDER_LOW 5
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_BORDER_HIGH 7
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_BORDER_WIDE 8
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_BORDER_ITER_LIMIT 30
/* The block's tile geometry and skipped-tile floor. */
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_TILE 32
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MAX_BORDER 8
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MAX_FULL 48
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_MIN_BLEND 0.01f
/* One workgroup per tile; the size is the kernel's local work size. */
#define RT_LOCAL_ADJUSTMENTS_CAPTURE_WORKGROUP 64u

/* deconvsharpeningloc() has two arms. Damping zero is the undamped RLD path:
 * the division/multiplication recurrence the output.sharpen core already
 * covers. A positive damping value is the damped path: a plain Gaussian blur,
 * dcdamping()'s factor and a GAUSS_MULT blur. `damp > 0` selects it and the
 * factor is `damp / 5.0`; the GUI range is 0..100, so both arms are admitted.
 * The block's own guard is `lp.shrad > 0.42`; below it no sharpen work runs.
 * Above 1.15 the core uses its separable IIR arm, which has no device RLD
 * kernel, so those radii stay on the host. The damped arm's forward blur is
 * the standard Gaussian: below sigma 0.6 the host selects its 3x3 FIR, whose
 * damped recurrence does not meet this node's 1-LSB16 parity, so a damped
 * spot below 0.6 is a host residual. At and above 0.6 the forward blur is the
 * standard IIR pair. */
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_NOOP_RADIUS 0.42
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_SIGMA_LIMIT 1.15
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_DAMPED_MIN_RADIUS 0.6
/* The block's blur pre-pass is skipped below 0.25; a larger sharblur is a
 * residual for the same reason as output sharpening. */
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_BLUR_LIMIT 0.25
/* iplocallab.cc's mSPsharp: deconvsharpeningloc() needs 39 px on both axes
 * for buildBlendMask(). A smaller extent makes the block return before its
 * tail clamp, so the whole block is a host execution. */
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_MIN_ROI 39
/* iplocallab.cc's `del`: the RL arm sharpens `lx + lxL + del` by
 * `ly + lyT + del`, a zero-padded extent rather than the clipped image ROI. */
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_ROI_PAD 3
/* Sharp_Local()'s deltaE reference blur: gaussianBlur() at 3 / sk, with
 * sk = 1 for the export call site. */
#define RT_LOCAL_ADJUSTMENTS_SHARPEN_SCOPE_SIGMA 3.0

/* Blur & Noise. The block runs only at scale one and derives its Gaussian
 * sigma as `lp.rad / (sk * 1.4)`. The Gaussian arm's own gate needs a spot
 * radius above 1.6, and above 30 (or with fftwbl) the block switches to the
 * FFT path, which has no device kernel. Within that range gaussianBlur() is
 * the SSE IIR pair: at or below sigma 0.6 it is the 3x3 FIR and at or above
 * 25 it is the double path, and neither has a matching device recurrence.
 * The median arm does not read the radius at all, but the same 30 ceiling
 * keeps optfft() out of the ROI. BlurNoise_Local() blends with a scope base
 * of four, not transit_shapedetect2()'s two. Its deltaE reference is the
 * block's default 3 / sk blur, the same sigma as the sharpening scope. */
#define RT_LOCAL_ADJUSTMENTS_BLUR_SK 1.0
#define RT_LOCAL_ADJUSTMENTS_BLUR_RADIUS_DIVISOR 1.4
#define RT_LOCAL_ADJUSTMENTS_BLUR_MIN_RADIUS 1.6f
#define RT_LOCAL_ADJUSTMENTS_BLUR_MAX_RADIUS 30.0f
#define RT_LOCAL_ADJUSTMENTS_BLUR_SCOPE_BASE 4.0f
/* The GUI's minimum spot radius; the median arm has no radius gate of its own
 * but the block's optfft() branch starts above 30. */
#define RT_LOCAL_ADJUSTMENTS_BLUR_MEDIAN_MIN_RADIUS 0.0f
/* Median_Denoise()'s pass count and the GUI's medMethod range. */
#define RT_LOCAL_ADJUSTMENTS_BLUR_MIN_ITERS 1
#define RT_LOCAL_ADJUSTMENTS_BLUR_MAX_ITERS 4
/* lp.medmet: 0 is TYPE_3X3_STRONG, 1 TYPE_5X5_STRONG, 2 TYPE_7X7, 3
 * TYPE_9X9. The kernel's half extent is medmet + 1. */
#define RT_LOCAL_ADJUSTMENTS_BLUR_MIN_MED 0
#define RT_LOCAL_ADJUSTMENTS_BLUR_MAX_MED 3
/* Blur & Noise's two arms. */
#define RT_LOCAL_ADJUSTMENTS_BLUR_ARM_GAUSSIAN 0
#define RT_LOCAL_ADJUSTMENTS_BLUR_ARM_MEDIAN 1
/* iplocallab.cc's mSP: the blur block only runs when the ROI is at least this
 * size on both axes. */
#define RT_LOCAL_ADJUSTMENTS_BLUR_MIN_ROI 5

/* Lab_Local()'s tone-map block runs only when the ROI is at least mDEN = 128
 * px on both axes; a smaller ROI makes the tool a no-op rather than a
 * refusal. EPDToneMaplocal() caps its edge stopping at three when the global
 * local-contrast tool is enabled and at four otherwise, and turns it into an
 * automatic iteration count of `edgest * 15`. */
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_ROI 128
#define RT_LOCAL_ADJUSTMENTS_TM_EDGEST_LOCAL_CONTRAST 3.0
#define RT_LOCAL_ADJUSTMENTS_TM_EDGEST_DEFAULT 4.0
#define RT_LOCAL_ADJUSTMENTS_TM_ITERATE_FACTOR 15.0f
/* The largest automatic iteration count: the edge stopping caps at four. */
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_ITERATES 60
/* CompressDynamicRange() drops the reweighting passes when both axes are
 * below 300 px. */
#define RT_LOCAL_ADJUSTMENTS_TM_REWEIGHT_LIMIT 300
/* The GUI ranges the tone-map sliders are admitted over. */
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_STRENGTH (-0.5)
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_STRENGTH 2.0
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_GAMMA 0.4
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_GAMMA 4.0
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_EDGE_STOPPING 0.1
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_EDGE_STOPPING 4.0
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_SCALE 0.1
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_SCALE 10.0
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_REPART 1.0
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_REPART 100.0
/* The recovery weight the record carries, `1 - 0.01 * repartm`. */
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_WEIGHT 0.0f
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_WEIGHT 0.99f
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_SATUR (-100.0)
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_SATUR 100.0
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_REWEIGHT 0
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_REWEIGHT 3
#define RT_LOCAL_ADJUSTMENTS_TM_MIN_SENS 0
#define RT_LOCAL_ADJUSTMENTS_TM_MAX_SENS 100
/* Scratch planes the tone-map kernel needs: the log-domain source, the blur
 * iterate, the conjugate-gradient vectors (r, s, direction, product), the edge
 * weights, the five system diagonals and the six incomplete-Cholesky
 * diagonals. */
#define RT_LOCAL_ADJUSTMENTS_TM_SCRATCH_PLANES 18

/*
 * Retinex. Lab_Local()'s call==2 arm requires `bfw >= mSP && bfh > mSP` with
 * mSP = 5, and the scale count is the GUI's 1..10. `scalereti == 1` selects
 * MSRLocal()'s separate contrast arm, which builds a DiagonalCurve from the
 * ROI mean; that arm is a residual here, so the admitted range starts at two.
 * A scale's Gaussian is the host's iterated box blur: at most six iterations,
 * each with its own radius. The two curve tables are LocretigainCurve and
 * LocretitransCurve, 501 entries each.
 */
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_ROI 5
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_SCALES 10
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_BOX_ITERATIONS 6
#define RT_LOCAL_ADJUSTMENTS_RETI_BLUR_STRIDE 7
#define RT_LOCAL_ADJUSTMENTS_RETI_GAIN_ENTRIES 501
#define RT_LOCAL_ADJUSTMENTS_RETI_TRANS_ENTRIES 501
/* The block's strength gate; a smaller non-zero strength is a no-op. */
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_STRENGTH 0.2
/* The GUI ranges of the Retinex sliders. */
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_STRENGTH 100.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_SCALE 2
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_SCALE 10
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_NEIGH 0.1
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_NEIGH 1500.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_VART 0.1
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_VART 500.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_LIMD 1.2
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_LIMD 100.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_CLIPTM 0.02
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_CLIPTM 2.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_DARKNESS 0.01
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_DARKNESS 6.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_LIGHTNESS 0.01
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_LIGHTNESS 4.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_OFFS (-16386.0)
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_OFFS 32768.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MIN_SOFT_RADIUS 0.0
#define RT_LOCAL_ADJUSTMENTS_RETI_MAX_SOFT_RADIUS 100.0

/*
 * CIE CAM. Lab_Local()'s "com" block needs an ROI at least mSP = 5 px on
 * both axes. The CAMBrightCurveJ and CAMBrightCurveQ tables are 32768-entry
 * LUTf, the same size ciecamloc_02float() constructs. `pqremapcam16` above
 * 100 selects the CAM16-PQ (c16 == 21) transform, which is a residual; at or
 * below it the block runs the plain CAM16 (c16 == 16) arm.
 *
 * The admitted sliders are the GUI ranges. The contrast sliders
 * (`contlcie`, `contqcie`) build their curve from the ROI histogram, which
 * depends on the tools that ran before the CIE pass in the same spot, so they
 * are residuals; the light sliders need no histogram and are admitted. The
 * final gain and gamut compression is admitted only at its neutral settings.
 */
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_ROI 5
#define RT_LOCAL_ADJUSTMENTS_CIE_CAM_ENTRIES 32768u
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_PLUM 100.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_GRAY 1.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_GRAY 100.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_TARGET_GRAY 5.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_TARGET_GRAY 80.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_ABS 0.01
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_ABS 16384.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_CATAD (-100.0)
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_CATAD 100.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_SLIDER (-100.0)
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_SLIDER 100.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_PROTECT 0.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_PROTECT 100.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_SENS 0
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_SENS 100
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_BLUR 2.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_BLUR 100.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MIN_REPART 1.0
#define RT_LOCAL_ADJUSTMENTS_CIE_MAX_REPART 100.0
/* The block's reheat point for the target white, and the source white
 * (5000 K) the forward transform is anchored to. */
#define RT_LOCAL_ADJUSTMENTS_CIE_TEMP_BASE 5000
#define RT_LOCAL_ADJUSTMENTS_CIE_TEMP_UP 30.0
#define RT_LOCAL_ADJUSTMENTS_CIE_TEMP_DOWN 70.0
/* ciecamloc_02float()'s fixed epsil, and the four hue sectors of the per-hue
 * brightness sliders (all zero in the admitted set). */
#define RT_LOCAL_ADJUSTMENTS_CIE_EPSIL 0.0001f
#define RT_LOCAL_ADJUSTMENTS_CIE_HUE_RED_END 100.0f
#define RT_LOCAL_ADJUSTMENTS_CIE_HUE_GREEN_END 190.0f
#define RT_LOCAL_ADJUSTMENTS_CIE_HUE_BLUE_END 340.0f
#define RT_LOCAL_ADJUSTMENTS_CIE_HUE_ATTENUATION 0.555f

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
    /* Local exposure's deltaE reference blur, sigma (2 + 0.2 * blurexpde) /
     * sk. Only the SSE path is admitted. */
    const RtLabToolsIir* iir_exposure;
    /* Local contrast's own Gaussian, sigma = lcradius / sk. Both the SSE and
     * the double path are admitted; the launcher reads double_path. */
    const RtLabToolsIir* iir_local_contrast;
    /* shadows/highlights' deltaE reference blur, sigma
     * (2 + 0.2 * blurSHde) / sk. Only the SSE path is admitted. */
    const RtLabToolsIir* iir_shadows_highlights;
    /* Blur & Noise's Gaussian arm, sigma `lp.rad / (sk * 1.4)`. Only the SSE
     * path is admitted. The median arm does not read it. */
    const RtLabToolsIir* iir_blur;
    /* transit_shapedetect2()'s senstype-31 deltaE reference blur, sigma
     * (2 + 0.2 * blurciede) / sk. Only the SSE path is admitted. */
    const RtLabToolsIir* iir_cie;
    /* maskcalccol()'s own Gaussian, sigma 1 / sk. Both the log and the CIE
     * mask pass their mask through it before blendmask()/maskrecov(). */
    const RtLabToolsIir* iir_mask;
    /* The same working XYZ -> RGB matrix in double, nine values. The block's
     * final gain and gamut round trip calls Color::xyz2rgb(), which multiplies
     * with the double matrix. */
    const double* iws_double;
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
 * no recursion, inversion, explicit per-tool mask, soft radius, merge,
 * gradient, Lab grid, RGB curve, exposure curve or tool outside the set. The
 * common mask is admitted for the log and CIE arms only; its luminance curve
 * and recovery blend are device cases and the rest is fail-closed. An active
 * spot runs
 * any combination of local dehaze, local CBDL, Colour & Light, vibrance,
 * shadows/highlights, soft light, local contrast and local sharpening; the
 * tool variants and ranges are checked separately. The block has no pre-CBDL
 * pass and no global dehaze; the local dehaze is part of the spot path.
 *
 * Local sharpening has two arms. The `"rl"` (RLD) arm is admitted undamped or
 * damped, with no gamma wrapper, no blur pre-pass and a radius whose RLD
 * sigma is below the core's separable limit. The damping parameter is the
 * block's effective value after the `"bef"` fixup and spans the GUI's 0..100
 * range; a damped spot needs a radius of at least 0.6, where the forward blur
 * is the standard IIR pair. `shargam != 1`, `sharblur >= 0.25`, `sharshow`, a
 * radius outside 0.4..1.15, a damped radius below 0.6, masks, a soft radius,
 * out-of-range sliders and a damping value outside 0..100 are residuals. A
 * radius at or below 0.42 is the block's own no-op and is admitted without a
 * device pass. A partial spot with a non-zero transition is also a residual:
 * Sharp_Local() reads the padded deconvolution result across the transition
 * ring, which the block's `lx + lxL + 3` extent does not cover. A zero-width
 * transition keeps the ring inside the nominal extent.
 *
 * The `"cap"` (capture) arm is admitted without a gamma wrapper, without the
 * corner radius boost and with the manual contrast: `deconvCogam == 1`,
 * `deconvCoBoost == 0` and `deconvAutoshar == false`. The sigma, iteration
 * count, contrast and recovery sliders stay inside the GUI ranges. The
 * automatic tile contrast, the gamma wrapper, the corner boost and the mask
 * display are residuals. An ROI below the block's 150 px mSPsharpCS gate is a
 * no-op, not a device pass.
 *
 * Blur & Noise admits two arms. The Gaussian arm is `blMethod == "blur"` in
 * its normal direction with `chroMethod` luminance, chroma or all; the block's
 * sigma is `lp.rad / (sk * 1.4)`, so the spot radius must be above the block's
 * 1.6 gate and at most 30, and the device reads the host-built coefficients
 * for that sigma. The median arm is `blMethod == "med"` with `medMethod`
 * `33`/`55`/`77`/`99` and 1..4 passes: Median_Denoise() replaces the Gaussian,
 * so the radius only has to stay at or below 30 to keep optfft() out. Both arms
 * need the noise (`strength`), grain (`strengr`), guided-filter, bilateral,
 * non-local-means and FFT additions idle, no explicit mask, and the wavelet
 * denoise curve must leave DeNoise() idle. DeNoise() only runs when the tool is
 * on and either the Quality method is not `"none"` with a non-zero curve amount
 * or the `"enhden"` quality method arms artifact shaping, so the GUI's default
 * denoise curve is inert under the default `"none"` Quality method. A radius
 * outside its arm's range, the guided and inverse arms, `medMethod "none"`,
 * out-of-range passes, a non-zero denoise amount with an active Quality method,
 * `qualityMethod "enhden"`, an FFT or mask request and the `"nlmean"` quality
 * method are residuals.
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
 *
 * Local exposure is admitted in its PDE arm only: `expmet == 1` (the block's
 * hard-wired value), `linear > 0`, `laplacexp > 0.1`, `gamex == 1`, no black
 * point, shadow recovery, chroma, gradient, structure, fat, mask or soft
 * radius and `reparexp == 100`. The ROI must be a 64, 65, 80, 96, 100 or 128
 * px `fftw_size[]` entry on each axis, so optfft() leaves the extent
 * unchanged. The tone plane is exlabLocal()'s host-built highlight, shadow
 * and tone LUT sequence, and the Laplacian input is bit-identical to the
 * host; the device runs the threshold, a direct double DCT pair, the
 * Poisson scaling, normalize_mean_dt() and the senstype-1 blend. Other
 * exposure settings and an ROI outside the set are residuals.
 *
 * CIE CAM is admitted in its CAM16 "com" mode only: `expcie` on, `modecie`
 * `"com"`, `modecam` `"cam16"`, `pqremapcam16` at most 100 (the plain CAM16
 * arm) and `smoothciemet` `"none"`. The device runs ciecamloc_02float()'s
 * call==0 transform with every viewing-condition, white-point, acceptance and
 * lightness/brightness slider at its GUI range, plus the chroma, saturation,
 * colourfulness, hue and red-protection sliders. The per-hue brightness
 * sliders and their curves, the CIE tone curves, the contrast sliders (whose
 * tables read the ROI histogram the host builds after the earlier tools), the
 * Jz pre-processing, the pre-CAM RGB tone mapping, the mask display, the
 * recovery mask, `detailcie`'s local contrast, the gradient and the final gain
 * and gamut compression are residuals.
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

/* Whether a local-exposure ROI is one of the admitted sizes. The block's
 * optfft() shrinks an ROI to the largest fftw_size[] entry not above each
 * axis, so an exact entry leaves the PDE extent unchanged. The set is capped
 * at 128 px because the device DCT is a direct transform. */
bool localAdjustmentsExposureRoiAdmitted(int width, int height);

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
    double iwsDouble_[9] = {};
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
    RtLabToolsIir iirBlur_ = {};
    RtLabToolsIir iirCie_ = {};
    RtLabToolsIir iirMask_ = {};
    RtLabToolsIir iirExposure_ = {};
    uint32_t clamp_mode_ = RT_LOCAL_ADJUSTMENTS_CLAMP_NONE;
};

} // namespace rtengine::stage
