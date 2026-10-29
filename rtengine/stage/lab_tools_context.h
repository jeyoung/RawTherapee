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
 * colour matrices, the six Lab-curve tables and the Munsell table set, and it
 * runs the original block for a residual.
 *
 * The struct is C-compatible because the ABI is. `self` points at the C++
 * object that owns the ProcParams and ImProcFunctions references, so the
 * pointer must not outlive them.
 */
#define RT_LAB_TOOLS_CONTEXT_VERSION 3u
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
/* params.labCurve.gamutmunselmethod, as chromiLuminanceCurve() maps it. NONE
 * and MUN are the device methods; LAB, XYZ and XYZREL are residuals because
 * they need gamutLchonly() and gamutmap(). */
#define RT_LAB_TOOLS_MUNSELL_NONE 0u
#define RT_LAB_TOOLS_MUNSELL_MUN 4u
/*
 * One direction's Young-van Vliet IIR coefficients, in the three forms the
 * host's gaussianBlur() paths use. Each array is B, b1, b2, b3 and the nine
 * boundary-matrix entries M[0][0] .. M[2][2], in that order. See
 * gaussianIirCoefficients() in gauss.h for the provenance.
 */
#define RT_LAB_TOOLS_IIR_TERMS 13u
#define RT_LAB_TOOLS_IIR_VERSION 1u

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
    /* RT_LAB_TOOLS_MUNSELL_NONE or RT_LAB_TOOLS_MUNSELL_MUN. */
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
 * Whether the block is the lab.tools node's supported set: the LabGrid colour
 * toning pass, shadows/highlights, local contrast, vibrance and/or the Lab
 * curves, with every other pixel tool off.
 *
 * Fail closed. A Lab curve whose `contrast` is non-zero owns the hist16 refill
 * a later node reads, so it is a residual. So are the chromaticity family
 * (the chromaticity slider and the C=f(H), L=f(H) and H=f(H) flat curves), the
 * LAB, XYZ and XYZREL Munsell methods, black-and-white output (which
 * chromiLuminanceCurve() also touches), EPD and any colour-toning method other
 * than LabGrid. A local contrast whose sliders are outside the ABI range is a
 * residual as well. A profile where no tool would run is a residual too: the
 * node would be a no-op and the host block records it.
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
 * curves are enabled and `contrast` is zero. The six per-table activity flags
 * are separate: a pass whose tables are all the identity still rewrites L, a
 * and b through the block's divides, so it is not a no-op. */
bool labToolsLabCurveActive(const procparams::ProcParams& params);


/*
 * The block as it ran before the device node, moved here unchanged so the call
 * site, the host builtin and the tests share exactly one CPU implementation.
 */
void labToolsHostBlock(ImProcFunctions& ipf,
                       const procparams::ProcParams& params,
                       LabImage& lab,
                       LabToolsLuts& luts,
                       bool& autili,
                       bool& butili);

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
                    bool& butili);
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

    bool built_ = false;
    /* complexsgnCurve()'s two flags, owned by the block's caller. */
    bool* autili_ = nullptr;
    bool* butili_ = nullptr;
    float labgrid_[4] = {};
    std::vector<float> lut_highlight_;
    std::vector<float> lut_shadow_;
    float ws_[9] = {};
    float iws_[9] = {};
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
};

} // namespace rtengine::stage
