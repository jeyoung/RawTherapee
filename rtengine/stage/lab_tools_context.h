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

#include "LUT.h"

#include <cstdint>
#include <vector>

namespace rtengine
{
class Imagefloat;
class ImProcFunctions;
class LabImage;
class PipetteBuffer;
}

namespace rtengine::procparams
{
struct ProcParams;
}

/*
 * Host context of the lab.tools node.
 *
 * The device launcher receives this structure through RT_STAGE_ARG_PTR. It is
 * host-only: the pointer, the callbacks and everything they return live in
 * host memory, and nothing here is passed to a kernel. The launcher validates
 * the per-pixel arguments from the ABI on its own; the context supplies the
 * tone tables that need pow_F(), the sh_contrast NURBS curve, the working
 * colour matrices, the six Lab-curve tables, the three carried hue flat
 * curves and the Munsell table set, and it runs the original block for a
 * residual.
 *
 * The struct is C-compatible because the ABI is. `self` points at the C++
 * object that owns the ProcParams and ImProcFunctions references, so the
 * pointer must not outlive them.
 */
#define RT_LAB_TOOLS_CONTEXT_VERSION 7u
/* "LTOC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LAB_TOOLS_CONTEXT_MAGIC 0x4C544F43u
/* The Lab-mode tone table: one entry per 16-bit L value (0..32767). */
#define RT_LAB_TOOLS_LAB_LUT_ENTRIES 32768u
/* The RGB-mode tone table: one entry per 16-bit RGB code. */
#define RT_LAB_TOOLS_RGB_LUT_ENTRIES 65536u
/* Color::cachef[] / cachefy[] entries, needed by the RGB-mode round trip. */
#define RT_LAB_TOOLS_CACHE_ENTRIES 65536u
/* The Munsell Lch tables the vibrance avoidcolorshift path reads. The flat
 * set has RT_LAB_TOOLS_MUNSELL_TABLES entries of RT_LAB_TOOLS_MUNSELL_STRIDE
 * floats each; the host pads every table to the longest one (140). */
#define RT_LAB_TOOLS_MUNSELL_TABLES 197u
#define RT_LAB_TOOLS_MUNSELL_STRIDE 140u
/* Vibrance's host-built skin-tone hue curve. */
#define RT_LAB_TOOLS_SKIN_CURVE_ENTRIES 65536u
/*
 * The six Lab-curve tables. The L table is CurveFactory::complexLCurve()'s
 * output; the other five are the 65536-entry diagonal tables of
 * CurveFactory::complexsgnCurve() and CurveFactory::diagonalCurve2Lut().
 */
#define RT_LAB_TOOLS_LUMA_LUT_ENTRIES 32770u
#define RT_LAB_TOOLS_CURVE_LUT_ENTRIES 65536u
/*
 * The three hue flat curves: C=f(H), L=f(H) and H=f(H). `FlatCurve::getVal()`
 * is exactly piecewise linear, so a node at every polyline knot reproduces it
 * exactly; the host carries the polyline itself, as the wavelet refine arms
 * do, and falls back to refining a chord sampler where the polyline has more
 * knots than the slot holds. A curve that neither representation can follow
 * within the bound is refused.
 *
 * The nodes are doubles, not floats: a knot abscissa in [0,1] rounded to float
 * moves a dense steep curve by `slope * 6e-8`, which is 1.2e-4 for a 40 000
 * point zigzag and 240 times the bound. The residency's shape is float
 * counted, so a slot of RT_LAB_TOOLS_HUE_LUT_STRIDE doubles occupies
 * 2 * RT_LAB_TOOLS_HUE_LUT_STRIDE float words; the launcher uploads the
 * doubles' bytes exactly as it does the working-space matrices.
 *
 * Each table is a double node count, then that many x nodes and that many y
 * nodes, in one buffer of RT_LAB_TOOLS_HUE_CURVES slots of
 * RT_LAB_TOOLS_HUE_LUT_STRIDE doubles, in the order C=f(H), L=f(H), H=f(H).
 * The kernel interpolates the table in double, like the region mask tables.
 *
 * The node budget is the largest polyline `FlatCurve` can subdivide: its `ppn`
 * is capped at 65 500 and the periodic polylines add the two boundary knots,
 * so 65 536 nodes hold every curve the GUI's own sampling can produce. A
 * hand-built knot list above that falls back to the refiner.
 *
 * The fidelity bound is RT_LAB_TOOLS_HUE_LUT_ACCEPT on the curve value. The
 * largest lever is C=f(H), which turns a value error `e` into a chroma factor
 * error of `2e`; at chroma 200 and the block's 327.68 scale that is
 * 327.68 * 2 * 5e-7 * 200 = 0.066 LSB16, far below the one-LSB gate. H=f(H)
 * turns it into a hue error of 1.7e rad, i.e. 0.03 LSB16 at the largest a/b.
 * L=f(H) multiplies it by 2 * CC / amchroma before the lightness blend, which
 * stays well inside the gate for a chroma up to the Munsell limit. A table
 * that cannot meet the bound is refused, never approximated.
 */
#define RT_LAB_TOOLS_HUE_CURVES 3u
#define RT_LAB_TOOLS_HUE_LUT_NODES 65536u
#define RT_LAB_TOOLS_HUE_LUT_STRIDE (1u + 2u * RT_LAB_TOOLS_HUE_LUT_NODES)
/* The slot's width in the residency's float-counted shape: two float words per
 * double node. */
#define RT_LAB_TOOLS_HUE_LUT_WORDS (2u * RT_LAB_TOOLS_HUE_LUT_STRIDE)
#define RT_LAB_TOOLS_HUE_LUT_ACCEPT 5.0e-7
/* params.labCurve.gamutmunselmethod, with the block's numbering: NONE 0,
 * LAB 1, XYZ 2, XYZREL 3 and MUN 4. All five are device methods; LAB runs
 * Color::gamutLchonly()'s RGB overload before the Munsell arm, exactly as the
 * block does. */
#define RT_LAB_TOOLS_MUNSELL_NONE 0u
#define RT_LAB_TOOLS_MUNSELL_LAB 1u
#define RT_LAB_TOOLS_MUNSELL_XYZ 2u
#define RT_LAB_TOOLS_MUNSELL_XYZREL 3u
#define RT_LAB_TOOLS_MUNSELL_MUN 4u
/*
 * One direction's Young-van Vliet IIR coefficients, in the three forms the
 * host's gaussianBlur() paths use. Each array is B, b1, b2, b3 and the nine
 * boundary-matrix entries M[0][0] .. M[2][2], in that order. See
 * gaussianIirCoefficients() in gauss.h for the provenance.
 */
#define RT_LAB_TOOLS_IIR_TERMS 13u
#define RT_LAB_TOOLS_IIR_VERSION 1u
/*
 * Lab colour-correction regions (ImProcFunctions::labColorCorrectionRegions).
 * The device set admits one to four regions. Each region's hue, chromaticity
 * and lightness masks are host-sampled FlatCurves: the host evaluates
 * FlatCurve::getVal() on a uniform grid and the kernel interpolates it, so the
 * host never exports a curve the kernel would have to re-derive. The
 * chroma argument is xlin2log(c * 327.68/48000, 10); the host samples to four,
 * past the point where FlatCurve's final interval begins (its synthetic 3.0
 * endpoint), and the kernel extrapolates beyond that with the same slope the
 * host uses. The lightness argument is raw L/32768, so it is sampled to the
 * same point and a negative value wraps once, as FlatCurve::getVal() does.
 */
#define RT_LAB_TOOLS_MAX_REGIONS 4u
#define RT_LAB_TOOLS_REGION_CURVES 3u
/*
 * A sampled mask curve is a piecewise-linear table: a float node count, then
 * that many x nodes and that many y nodes. The host refines the table where
 * the chord deviates from FlatCurve::getVal(), so a smooth curve needs a few
 * hundred nodes and a control-point kink costs a short run of nodes on each
 * side. A curve that needs more than the maximum is refused.
 */
#define RT_LAB_TOOLS_REGION_LUT_NODES 32768u
#define RT_LAB_TOOLS_REGION_LUT_STRIDE (1u + 2u * RT_LAB_TOOLS_REGION_LUT_NODES)
/* Per-region floats, indexed by the RT_LAB_TOOLS_REGION_* constants below. */
#define RT_LAB_TOOLS_REGION_PARAM_STRIDE 14u
#define RT_LAB_TOOLS_REGION_CHROMA_DOMAIN 4.0f
#define RT_LAB_TOOLS_REGION_LIGHT_DOMAIN 4.0f
/* The region sliders' ABI ranges. A value outside them is a residual. The
 * a/b correction is the LabGrid widget's normalised value, which is bounded
 * by the GUI at +-1; the range admits the larger values a hand-edited profile
 * can carry without letting abcoord() overflow the device's float. */
#define RT_LAB_TOOLS_REGION_MAX_AB 20.0
#define RT_LAB_TOOLS_REGION_MAX_SATURATION 100.0
#define RT_LAB_TOOLS_REGION_MIN_SLOPE 0.1
#define RT_LAB_TOOLS_REGION_MAX_SLOPE 4.0
#define RT_LAB_TOOLS_REGION_MAX_OFFSET 0.1
#define RT_LAB_TOOLS_REGION_MIN_POWER 0.1
#define RT_LAB_TOOLS_REGION_MAX_POWER 4.0
#define RT_LAB_TOOLS_REGION_MIN_BLUR -10.0
#define RT_LAB_TOOLS_REGION_MAX_BLUR 100.0
/* Region parameter slots. */
#define RT_LAB_TOOLS_REGION_ABCA 0u
#define RT_LAB_TOOLS_REGION_ABCB 1u
#define RT_LAB_TOOLS_REGION_RS 2u
#define RT_LAB_TOOLS_REGION_SLOPE 3u
#define RT_LAB_TOOLS_REGION_OFFSET 4u
#define RT_LAB_TOOLS_REGION_POWER 5u
#define RT_LAB_TOOLS_REGION_CHANNEL 6u
#define RT_LAB_TOOLS_REGION_MASK_BLUR 7u
#define RT_LAB_TOOLS_REGION_RADIUS_AB 8u
#define RT_LAB_TOOLS_REGION_RADIUS_L 9u
#define RT_LAB_TOOLS_REGION_HUE_ACTIVE 10u
#define RT_LAB_TOOLS_REGION_CHROMA_ACTIVE 11u
#define RT_LAB_TOOLS_REGION_LIGHT_ACTIVE 12u
#define RT_LAB_TOOLS_REGION_RESERVED 13u

typedef struct RtLabToolsIirDirection {
    /* The SSE float lane: B, b1, b2, b3 and sse_M narrowed to float, which is
     * what gaussHorizontalSse()/gaussVerticalSse() feed _mm_set1_ps(). */
    float lane[RT_LAB_TOOLS_IIR_TERMS];
    /* The same sse_M in double, for those functions' non-vectorised tail
     * lines, which evaluate the recurrence in double and narrow each store. */
    double border[RT_LAB_TOOLS_IIR_TERMS];
    /* The double-precision path at and above sigma 25, with M. */
    double wide[RT_LAB_TOOLS_IIR_TERMS];
} RtLabToolsIirDirection;

typedef struct RtLabToolsIir {
    uint32_t size;
    uint32_t version;
    /* 1 when the host's gaussianBlur() takes the double path for this sigma. */
    uint32_t double_path;
    uint32_t reserved;
    RtLabToolsIirDirection horizontal;
    RtLabToolsIirDirection vertical;
} RtLabToolsIir;

typedef struct RtLabToolsResult {
    /*
     * {a_scale, a_base, b_scale, b_base} of the LabGrid pass, each already
     * rounded to float by the block's own derivation.
     */
    const float* labgrid;
    /* The two tone tables of the active mode, RT_LAB_TOOLS_*_LUT_ENTRIES. */
    const float* lut_highlight;
    const float* lut_shadow;
    /* Tone-table length: 32768 in Lab mode, 65536 in RGB mode. */
    uint32_t lut_entries;
    /* The working RGB -> XYZ and XYZ -> RGB matrices, nine floats each. Only
     * the RGB-mode blend reads them. */
    const float* ws;
    const float* iws;
    /* Color::cachef[] and cachefy[], the 65536-sample Lab caches. */
    const float* cachef;
    const float* cachefy;
    /*
     * The flat Munsell table set and one entry per table with that table's
     * own length. Both are read only when vibrance runs on the device.
     */
    const float* munsell;
    const float* munsell_sizes;
    /* The host-built vibrance skin curve, read only when skin_curve_set. */
    const float* skin_curve;
    uint32_t munsell_tables;
    /* 1 when Color::init()'s curve was non-identity and the table is built. */
    uint32_t skin_curve_set;
    /*
     * The six Lab-curve tables. `lab_curve_l` has lab_curve_l_entries entries
     * (RT_LAB_TOOLS_LUMA_LUT_ENTRIES); the other five share
     * lab_curve_entries (RT_LAB_TOOLS_CURVE_LUT_ENTRIES). They are null and
     * the lengths are zero unless lab_curve_active is 1.
     */
    const float* lab_curve_l;
    const float* lab_curve_a;
    const float* lab_curve_b;
    const float* lab_curve_cc;
    const float* lab_curve_lc;
    const float* lab_curve_cl;
    uint32_t lab_curve_l_entries;
    uint32_t lab_curve_entries;
    /* 1 when the block would apply the Lab curves to the image. */
    uint32_t lab_curve_active;
    /* The six activity flags curve[] and the a/b/cc/lc/cl tables sit behind. */
    uint32_t lab_curve_l_active;
    uint32_t lab_curve_a_active;
    uint32_t lab_curve_b_active;
    uint32_t lab_curve_cc_active;
    uint32_t lab_curve_lc_active;
    uint32_t lab_curve_cl_active;
    /*
     * The hue flat-curve family: one buffer of lab_curve_hue_stride doubles
     * holding RT_LAB_TOOLS_HUE_CURVES sampled tables, and the three activity
     * flags. The family is inactive when the chromaticity slider is at its
     * minimum (-100): the block then never constructs the curves.
     */
    const double* lab_curve_hue;
    uint32_t lab_curve_hue_stride;
    uint32_t lab_curve_ch_active;
    uint32_t lab_curve_lh_active;
    uint32_t lab_curve_hh_active;
    /* The chromaticity slider, -100..100; the kernel derives chromapro. */
    int32_t lab_curve_chromaticity;
    /* RT_LAB_TOOLS_MUNSELL_NONE, _LAB, _XYZ, _XYZREL or _MUN. */
    uint32_t lab_curve_munsell;
    /*
     * The red and skin protection inputs: settings->protectred and
     * settings->protectredh as the block reads them, 100 - rstprotection,
     * settings->amchroma and the C=f(C)/C=f(L) chroma scale (adjustr). The
     * kernel derives the block's clamps from the raw values.
     */
    float lab_curve_rst_protection;
    float lab_curve_protect_red;
    float lab_curve_protect_red_h;
    float lab_curve_amount_chroma;
    float lab_curve_adjust;
    uint32_t lab_curve_lcredsk;
    /*
     * Local contrast. The context exports the block's own float narrowing of
     * the three sliders, the integer radius, the call site's scale and the
     * host-built IIR coefficients. The coefficients are host memory read by
     * the launcher; they are never uploaded.
     */
    uint32_t local_contrast_enabled;
    uint32_t local_contrast_radius;
    uint32_t local_contrast_scale;
    float local_contrast_amount;
    float local_contrast_darkness;
    float local_contrast_lightness;
    const RtLabToolsIir* local_contrast_iir;
    /*
     * The Lab-region set. `region_count` is 0 when no region pass runs.
     * `region_luts` holds region_count * 3 * region_lut_entries floats,
     * region-major and hue/chroma/lightness in that order; an inactive curve's
     * slice is zero and is never read. `region_params` is region_count
     * RT_LAB_TOOLS_REGION_PARAM_STRIDE floats, indexed by the
     * RT_LAB_TOOLS_REGION_* slots.
     */
    const float* region_luts;
    const float* region_params;
    /*
     * The working matrices at the host's full double precision. The region
     * CDL and channel-copy round trips are host double matrix products; the
     * float matrices above are one ulp away and can move a gamut-edge sample
     * by two LSB16, so the region program reads these instead. The Lab-curve
     * kernel reads the inverse for L=f(H)'s gamut loop, which is
     * Color::xyz2rgb() on the block's double matrix, and the forward matrix
     * as Color::gamutmap()'s `p`: chromiLuminanceCurve() passes its `wp`,
     * which is ICCStore::workingSpaceMatrix() with the RGB primaries in the
     * columns, and gamutmap() indexes exactly that layout.
     */
    const double* ws_double;
    const double* iws_double;
    uint32_t region_count;
    uint32_t region_lut_stride;
    /*
     * 1 when chromiLuminanceCurve() runs a black-and-white arm, which is the
     * block's `bwonly`: `blackwhite.enabled && !colorToning.enabled`. The
     * launcher runs the cheap zeroing (the block's early return) only when
     * `lab_curve_active` is 0. With the Lab-curve pass active,
     * `rt_lab_tools_lab_curve` runs the full `bwToning` arm in the same slot
     * and writes a and b itself: the grey, or the a/b tables' deviation from
     * the original chroma. The combination with colour toning is 0 because
     * the block's `bwToning` is then false and rgbProc() has already done the
     * black-and-white output.
     */
    uint32_t black_white;
} RtLabToolsResult;


typedef struct RtLabToolsContext {
    uint32_t size;
    uint32_t version;
    uint32_t magic;
    /*
     * 1 when the call site admits the configuration on the device. A zero
     * value makes the launcher refuse before it touches the Lab buffer, which
     * is how a residual configuration is recorded as a host execution.
     */
    uint32_t supported;
    void* self;
    /*
     * Builds the four LabGrid parameters, the two tone tables and the working
     * colour setup. Returns 0 on success.
     */
    int (*build)(void* self, RtLabToolsResult* out);
    /*
     * Runs the original host block on the Lab buffer. The builtin calls this
     * so a residual and a refused device request produce the CPU result.
     */
    int (*run_host)(void* self, RtStageBuffer* lab);
} RtLabToolsContext;

namespace rtengine::stage
{

/*
 * The block's hist16 refill: one histogram of the input L, used when the Lab
 * curve has a non-zero contrast. It is a host pass because the histogram is
 * a later node's input; the device only consumes the resulting lumacurve.
 */
void labToolsBuildHistogram(const LabImage& lab, LUTu& hist16);

/*
 * The block's `scal` argument at every export call site. shadowsHighlights()
 * divides the guided-filter radius by it, so a different value would be a
 * different configuration; the device ABI carries it and the launcher
 * requires this value.
 */
inline constexpr int LAB_TOOLS_EXPORT_SCALE = 1;

/*
 * Local contrast's ABI ranges. The radius admits values below the UI
 * minimum of 20 because the parity matrix exercises sigma 1 and 10; the
 * amount admits
 * 2 rather than the UI maximum of 1 for the same reason. Darkness and
 * lightness use the UI's 0..3. A value outside these bounds is a residual, not
 * a device pass, so the host block's own arithmetic is the result.
 */
inline constexpr int LAB_TOOLS_LC_MIN_RADIUS = 1;
inline constexpr int LAB_TOOLS_LC_MAX_RADIUS = 200;
inline constexpr double LAB_TOOLS_LC_MAX_AMOUNT = 2.0;
inline constexpr double LAB_TOOLS_LC_MAX_DARKNESS = 3.0;
inline constexpr double LAB_TOOLS_LC_MAX_LIGHTNESS = 3.0;
/* gaussianBlurImpl()'s GAUSS_DOUBLE in gauss.cc, which selects the
 * double-precision IIR path. */
inline constexpr double LAB_TOOLS_LC_DOUBLE_SIGMA = 25.0;

/*
 * The LUT instances the host block reads and writes. They belong to the
 * caller: the export ImageProcessor owns one set for the whole pipeline and
 * the test builds its own. The block rebuilds every curve it uses, so the
 * instances only need the export defaults (`lumacurve` sized 32770).
 */
struct LabToolsLuts {
    LUTu* hist16;
    LUTf* lumacurve;
    LUTu* dummy;
    LUTf* clcurve;
    LUTf* curve1;
    LUTf* curve2;
    LUTf* satcurve;
    LUTf* lhskcurve;
};

/*
 * Call-site options of the shared lab.tools body.
 *
 * The interactive paths keep their own curve preparation, viewport snapshots
 * and shared LUT objects, so they pass `buildLuts=false` and supply the six
 * curve flags they already computed. Export builds its own tables. `scale`
 * reaches shadows/highlights, local contrast and EPD; `lutScale` is the
 * sampling divisor of the curve tables the body builds. `chromiWidth` is the
 * width argument the chromiLuminanceCurve call has always received: the
 * preview width, or 1 on the crop and export paths. `epdBeforeVibrance`
 * preserves export's order; the interactive paths run vibrance and the region
 * pass first. `applyAftDirpy` runs the after-CBDL equaliser that the preview
 * applies at the end of this block.
 */
struct LabToolsBodyOptions {
    int scale = 1;
    int lutScale = 1;
    int chromiWidth = 1;
    PipetteBuffer* pipetteBuffer = nullptr;
    bool buildLuts = true;
    bool applyAftDirpy = false;
    bool epdBeforeVibrance = true;
    bool utili = false;
    bool ccutili = false;
    bool cclutili = false;
    bool clcutili = false;
    LUTu* histC = nullptr;
    LUTu* histL = nullptr;
};

/*
 * Whether the block is the lab.tools node's supported set: the LabGrid colour
 * toning pass, shadows/highlights, local contrast, vibrance and/or the Lab
 * curves, with every other pixel tool off.
 *
 * Fail closed. A Lab curve with non-zero `contrast` is admitted; its hist16
 * refill is built by the call site before the node. A chromaticity slider
 * outside its -100..100 GUI range, a C=f(H), L=f(H) or H=f(H) flat curve
 * neither the carried polyline nor the refined table can follow within
 * RT_LAB_TOOLS_HUE_LUT_ACCEPT, EPD and any colour-toning method other than
 * LabGrid. A local contrast whose sliders are outside the ABI range is a
 * residual as well. Black-and-white output is
 * admitted through both of chromiLuminanceCurve()'s arms: the cheap one when
 * the Lab curves are off (it only zeroes a and b) and the full `bwToning` one
 * when they are on. With colour toning the block's `bwonly` is false, so
 * neither arm runs: the combination is admitted and the block does the same
 * work as with black-and-white off, because rgbProc() has already produced
 * the black-and-white image and the toning. A profile where no tool would run
 * is a residual too: the node would be a no-op and the host block records it.
 */
bool labToolsSupported(const procparams::ProcParams& params);
/* True when a shadows/highlights round would run for the given slider
 * values, as ImProcFunctions::shadowsHighlights() decides it. */
bool labToolsShadowsHighlightsActive(const procparams::ProcParams& params);
/* True when ImProcFunctions::vibrance() would touch a pixel: the skin curve is
 * non-identity or a slider is set. The predicate and the device launcher both
 * use it, so a vibrance block that is a no-op records impl=host. */
bool labToolsVibranceActive(const procparams::ProcParams& params);
/* True when ImProcFunctions::localContrast() would touch a pixel: the tool is
 * enabled. It always writes the L plane, even with amount 0, because the
 * block's LIM() can clamp an out-of-range sample. */
bool labToolsLocalContrastActive(const procparams::ProcParams& params);
/* True when chromiLuminanceCurve()'s Lab-curve pass runs on the device: the
 * curves are enabled. The six per-table activity flags are separate: a pass
 * whose tables are all the identity still rewrites L, a and b through the
 * block's divides, so it is not a no-op. */
bool labToolsLabCurveActive(const procparams::ProcParams& params);
/*
 * True when the colour-toning method is LabRegions and the bounded set holds:
 * no mask display, one to four regions, every mask curve empty, FCT_Linear or
 * a FlatCurve the host can sample faithfully, and every slider inside the ABI
 * range. The predicate and the setup apply the same checks, so a refused
 * profile never reaches the launcher as a device configuration.
 */
bool labToolsRegionsActive(const procparams::ProcParams& params);
/* guidedfilter.cc's file-local calculate_subsampling(): the guided-filter
 * subsampling the region pass gets when it passes no explicit value. */
int labToolsRegionSubsampling(int width, int height, int radius);
/* The block's r1/r2 derivation from a region's maskBlur and the call site's
 * scale. */
void labToolsRegionRadii(double mask_blur, int scale, int* radius_ab,
                         int* radius_l);


/*
 * The block as it ran before the device node, moved here unchanged so the call
 * site, the host builtin and the tests share exactly one CPU implementation.
 */
void labToolsHostBlock(ImProcFunctions& ipf,
                       const procparams::ProcParams& params,
                       LabImage& lab,
                       LabToolsLuts& luts,
                       bool& autili,
                       bool& butili,
                       const LabToolsBodyOptions& options = {});

/*
 * Build and submit the lab.tools node for `lab`.
 *
 * `supported` is the call site's predicate. The host setup may still refuse
 * the configuration; the node is then submitted with the support flag clear,
 * exactly as the export block does. `skip` is CurveFactory's sampling
 * divisor: 1 for the export block, 16 for the reduced thumbnail path, whose
 * curve tables are built at a coarser sampling. `node` names the kernel
 * request; the thumbnail block submits the same program under
 * `thumbnail.lab`, so its record names the unit that owns the pass.
 *
 * Returns true when stage::run() reports success. The caller owns the host
 * fallback, which must run only when this returns false.
 */
bool labToolsSubmit(const procparams::ProcParams& params,
                    ImProcFunctions& ipf,
                    LabImage& lab,
                    LabToolsLuts luts,
                    bool& autili,
                    bool& butili,
                    const char* node,
                    int skip,
                    bool supported);

/*
 * The host-owned setup for one lab.tools submission.
 *
 * `build` derives the four LabGrid parameters with the block's float
 * arithmetic, builds the shadows/highlights tone tables through the block's
 * own shadowsHighlightsLut(), and exports the working matrices and the Lab
 * caches. The context owns no device resource. `runHost` runs the extracted
 * host block with device recursion disabled by the caller.
 */
class LabToolsContext
{
public:
    /*
     * `autili` and `butili` are the block's complexsgnCurve() flags. The
     * export ImageProcessor owns them because stage_output_convert() reads
     * them, so the context writes through to the caller's members.
     */
    LabToolsContext(const procparams::ProcParams& params,
                    ImProcFunctions& ipf,
                    LabToolsLuts luts,
                    bool& autili,
                    bool& butili,
                    int skip = LAB_TOOLS_EXPORT_SCALE);
    ~LabToolsContext();

    LabToolsContext(const LabToolsContext&) = delete;
    LabToolsContext& operator=(const LabToolsContext&) = delete;

    /* Fill the host-only descriptor handed over as RT_STAGE_ARG_PTR. */
    RtLabToolsContext descriptor(bool supported);

    /* Build the tables and matrices. Returns 0 on success. */
    int build(RtLabToolsResult& out);

    int runHost(RtStageBuffer& lab);

private:
    const procparams::ProcParams& params_;
    ImProcFunctions& ipf_;
    LabToolsLuts luts_;
    /*
     * CurveFactory's `skip` argument. The export block samples its curves at
     * 1; the reduced thumbnail path samples them at 16, so the device context
     * must build the same tables the thumbnail call site would.
     */
    int skip_;

    bool built_ = false;
    /* complexsgnCurve()'s two flags, owned by the block's caller. */
    bool* autili_ = nullptr;
    bool* butili_ = nullptr;
    float labgrid_[4] = {};
    std::vector<float> lut_highlight_;
    std::vector<float> lut_shadow_;
    float ws_[9] = {};
    float iws_[9] = {};
    double ws_double_[9] = {};
    double iws_double_[9] = {};
    std::vector<float> cachef_;
    std::vector<float> cachefy_;
    std::vector<float> munsell_;
    std::vector<float> munsell_sizes_;
    std::vector<float> skin_curve_;
    bool vibrance_active_ = false;
    bool skin_curve_set_ = false;
    std::vector<float> lab_curve_l_;
    std::vector<float> lab_curve_a_;
    std::vector<float> lab_curve_b_;
    std::vector<float> lab_curve_cc_;
    std::vector<float> lab_curve_lc_;
    std::vector<float> lab_curve_cl_;
    bool lab_curve_active_ = false;
    bool lab_curve_l_active_ = false;
    bool lab_curve_a_active_ = false;
    bool lab_curve_b_active_ = false;
    bool lab_curve_cc_active_ = false;
    bool lab_curve_lc_active_ = false;
    bool lab_curve_cl_active_ = false;
    std::vector<double> lab_curve_hue_;
    bool lab_curve_ch_active_ = false;
    bool lab_curve_lh_active_ = false;
    bool lab_curve_hh_active_ = false;
    int32_t lab_curve_chromaticity_ = 0;
    uint32_t lab_curve_munsell_ = RT_LAB_TOOLS_MUNSELL_NONE;
    float lab_curve_rst_protection_ = 0.f;
    float lab_curve_protect_red_ = 0.f;
    float lab_curve_protect_red_h_ = 0.f;
    float lab_curve_amount_chroma_ = 0.f;
    float lab_curve_adjust_ = 1.f;
    bool lab_curve_lcredsk_ = true;
    bool local_contrast_active_ = false;
    uint32_t local_contrast_radius_ = 0;
    uint32_t local_contrast_scale_ = 0;
    float local_contrast_amount_ = 0.f;
    float local_contrast_darkness_ = 1.f;
    float local_contrast_lightness_ = 1.f;
    RtLabToolsIir local_contrast_iir_ = {};
    bool regions_active_ = false;
    std::vector<float> region_luts_;
    std::vector<float> region_params_;
    bool black_white_ = false;
};

} // namespace rtengine::stage
