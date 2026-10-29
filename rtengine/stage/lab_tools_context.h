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
 * tone tables that need pow_F(), the sh_contrast NURBS curve and the working
 * colour matrices, and it runs the original block for a residual.
 *
 * The struct is C-compatible because the ABI is. `self` points at the C++
 * object that owns the ProcParams and ImProcFunctions references, so the
 * pointer must not outlive them.
 */
#define RT_LAB_TOOLS_CONTEXT_VERSION 1u
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
 * toning pass, shadows/highlights and/or vibrance, with every other pixel tool
 * off.
 *
 * Fail closed. Lab curves (which own the histogram refill and the Lab LUTs),
 * local contrast, EPD, black-and-white output and any colour-toning method
 * other than LabGrid are residuals. So is a profile where no tool would run:
 * the node would be a no-op and the host block records it.
 */
bool labToolsSupported(const procparams::ProcParams& params);
/* True when a shadows/highlights round would run for the given slider
 * values, as ImProcFunctions::shadowsHighlights() decides it. */
bool labToolsShadowsHighlightsActive(const procparams::ProcParams& params);
/* True when ImProcFunctions::vibrance() would touch a pixel: the skin curve is
 * non-identity or a slider is set. The predicate and the device launcher both
 * use it, so a vibrance block that is a no-op records impl=host. */
bool labToolsVibranceActive(const procparams::ProcParams& params);


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
};

} // namespace rtengine::stage
