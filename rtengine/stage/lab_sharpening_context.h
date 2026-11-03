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
#include <string>
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
 * Host side of the lab.sharpening node.
 *
 * The node runs the Lab block in its production order:
 *
 *   impulsedenoise() -> defringe() -> MLsharpen() -> MLmicrocontrast()
 *
 * The first two and the last run only in the Lab arm; MLsharpen runs in both
 * arms because the call sites invoke it before the CIE-CAM split. The main
 * sharpen pass is not part of this node and keeps the shared `output.sharpen`
 * path.
 *
 * The block is atomic: if any enabled tool is outside the device set, the
 * whole block runs on the host, so a Lab image is never half device and half
 * host. The context owns the host-built hue table of the defringe FlatCurve
 * and the callback that runs the original host block for a residual, so a
 * refused or unsupported configuration still produces the CPU result and is
 * recorded as a host execution.
 */

#define RT_LAB_SHARPENING_CONTEXT_VERSION 1u
/* "LSHP": rejects a stale or unrelated pointer before any callback runs. */
#define RT_LAB_SHARPENING_CONTEXT_MAGIC 0x4C534850u

/* The hue table is a uniform sample of the defringe FlatCurve. FlatCurve
 * stores a dense polyline, so linear interpolation between neighbouring
 * samples is exact to well below the float rounding of the curve value; the
 * build checks that bound and refuses a curve it cannot follow. */
#define RT_LAB_SHARPENING_HUE_ENTRIES 65537u
/* A refused table means a residual, never an approximation. */
#define RT_LAB_SHARPENING_HUE_MAX_ERROR 1e-6
/* The interpolation check samples the curve at this many points, independent
 * of the table resolution. */
#define RT_LAB_SHARPENING_HUE_CHECK_POINTS 8192u

/* The GUI ranges: impulse threshold 0..100, defringe threshold 0..100 and
 * defringe radius 0.5..5.0. The radius bounds below also admit the export's
 * prescaled value and the no-op below 0.25. */
inline constexpr int LAB_SHARPENING_MIN_THRESHOLD = 0;
inline constexpr int LAB_SHARPENING_MAX_THRESHOLD = 100;
inline constexpr double LAB_SHARPENING_MIN_RADIUS = 0.0;
inline constexpr double LAB_SHARPENING_MAX_RADIUS = 25.0;
/* gaussianBlur()'s identity limit: below it the blur copies the plane, so the
 * chromaticity difference is zero and the whole defringe pass is a no-op. */
inline constexpr double LAB_SHARPENING_RADIUS_NOOP = 0.25;
/* gaussianBlur()'s separable-IIR limit: below it the host uses the 3x3, 5x5 or
 * 7x7 kernels, whose border rules this node does not implement. */
inline constexpr double LAB_SHARPENING_RADIUS_IIR = 1.15;

/* The GUI ranges: sharpen edges passes 1..4 and amount 0..100, microcontrast
 * amount 0..100, contrast 0..200 and uniformity 0..10. The lower ends below
 * also admit the values that make the host pass an exact no-op (passes 0,
 * amount 0, microcontrast amount below 1 and contrast exactly 100). */
inline constexpr int LAB_SHARPENING_EDGES_MIN_PASSES = 0;
inline constexpr int LAB_SHARPENING_EDGES_MAX_PASSES = 4;
inline constexpr double LAB_SHARPENING_EDGES_MIN_AMOUNT = 0.0;
inline constexpr double LAB_SHARPENING_EDGES_MAX_AMOUNT = 100.0;
inline constexpr double LAB_SHARPENING_MICRO_MIN_AMOUNT = 0.0;
inline constexpr double LAB_SHARPENING_MICRO_MAX_AMOUNT = 100.0;
/* MLmicrocontrast()'s own early-out: amount below 1.0 leaves the image. */
inline constexpr double LAB_SHARPENING_MICRO_NOOP_AMOUNT = 1.0;
inline constexpr double LAB_SHARPENING_MICRO_MIN_CONTRAST = 0.0;
inline constexpr double LAB_SHARPENING_MICRO_MAX_CONTRAST = 200.0;
/* MLmicrocontrast()'s own early-out: contrast exactly 100 leaves the image. */
inline constexpr double LAB_SHARPENING_MICRO_NOOP_CONTRAST = 100.0;
inline constexpr int LAB_SHARPENING_MICRO_MIN_UNIFORMITY = 0;
inline constexpr int LAB_SHARPENING_MICRO_MAX_UNIFORMITY = 10;

/* Argument order; see the header comment of lab_sharpening_cl.h. */
enum LabSharpeningArg : std::size_t {
    LAB_SHARPENING_ARG_LUMINANCE = 0,
    LAB_SHARPENING_ARG_A,
    LAB_SHARPENING_ARG_B,
    LAB_SHARPENING_ARG_IMPULSE_ENABLED,
    LAB_SHARPENING_ARG_IMPULSE_THRESHOLD,
    LAB_SHARPENING_ARG_DEFRINGE_ENABLED,
    LAB_SHARPENING_ARG_DEFRINGE_RADIUS,
    LAB_SHARPENING_ARG_DEFRINGE_THRESHOLD,
    LAB_SHARPENING_ARG_HUE_TABLE,
    LAB_SHARPENING_ARG_HUE_ENTRIES,
    LAB_SHARPENING_ARG_HUE_CURVE,
    LAB_SHARPENING_ARG_EDGES_ENABLED,
    LAB_SHARPENING_ARG_EDGES_PASSES,
    LAB_SHARPENING_ARG_EDGES_AMOUNT,
    LAB_SHARPENING_ARG_EDGES_THREECHANNELS,
    LAB_SHARPENING_ARG_MICRO_ENABLED,
    LAB_SHARPENING_ARG_MICRO_MATRIX,
    LAB_SHARPENING_ARG_MICRO_AMOUNT,
    LAB_SHARPENING_ARG_MICRO_CONTRAST,
    LAB_SHARPENING_ARG_MICRO_UNIFORMITY,
    LAB_SHARPENING_ARG_CONTEXT,
    LAB_SHARPENING_NODE_ARGS
};

/* Everything the core reads, independent of the production parameter struct. */
struct LabSharpeningNodeParams {
    int impulse_enabled = 0;
    int impulse_threshold = 0;
    int defringe_enabled = 0;
    double defringe_radius = 0.0;
    int defringe_threshold = 0;
    /* 1 when the profile has an active defringe hue curve. */
    int hue_curve = 0;
    int edges_enabled = 0;
    int edges_passes = 0;
    double edges_amount = 0.0;
    /* 1 when the edge pass is restricted to L, 0 for L, a and b. */
    int edges_threechannels = 0;
    int micro_enabled = 0;
    /* 1 for the 3x3 matrix, 0 for the 5x5 matrix. */
    int micro_matrix = 0;
    double micro_amount = 0.0;
    double micro_contrast = 0.0;
    int micro_uniformity = 0;
};

typedef struct RtLabSharpeningContext {
    uint32_t size;
    uint32_t version;
    uint32_t magic;
    /* 1 when the call site admits the configuration on the device. */
    uint32_t supported;
    void* self;
    /* Runs the original host block on the three tight single-plane buffers. */
    int (*run_host)(void* self, RtStageBuffer* luminance, RtStageBuffer* a,
                    RtStageBuffer* b);
} RtLabSharpeningContext;

namespace rtengine::stage
{

class LabSharpeningContext;

/* Validate the argument kinds and the tight single-plane buffers. */
bool lab_sharpening_decode(const RtStageArg* args, uint32_t nargs,
                           LabSharpeningNodeParams& params,
                           RtStageBuffer& luminance, RtStageBuffer& a,
                           RtStageBuffer& b, RtStageBuffer& hue,
                           std::string& error);

/*
 * The device set: at least one enabled tool, every enabled parameter set
 * inside its GUI range, the image large enough for the 5x5 windows, and
 * either no defringe or a defringe radius in the separable-IIR range (or the
 * no-op below 0.25). A defringe that runs needs device fp64 for its double
 * chromaticity mean. Anything else is a residual, never an approximation.
 */
bool lab_sharpening_admitted(const LabSharpeningNodeParams& params, int width,
                             int height, bool has_fp64, std::string& error);

/* 1 when at least one enabled tool would change a plane. */
bool lab_sharpening_active(const LabSharpeningNodeParams& params);

/*
 * Everything the node reads, derived from the production parameters.
 * `ciecam_arm` mirrors the call site: impulse denoise, defringe and
 * microcontrast are skipped there because the CIE-CAM pipeline owns them on
 * the CieImage, while ML sharpen edges still runs on the Lab image.
 */
LabSharpeningNodeParams
labSharpeningNodeParams(const procparams::ProcParams& params,
                        bool ciecam_arm = false);

/*
 * Build the argument list for one submission. The context's hue table and the
 * descriptor must outlive the call. Shared by the call site and the tests so
 * both submit the same ABI.
 */
void labSharpeningArgs(LabImage& lab, const LabSharpeningNodeParams& node,
                       LabSharpeningContext& context,
                       RtLabSharpeningContext& descriptor,
                       std::vector<RtStageArg>& args);

/*
 * Whether the Lab block is the node's supported set. `ciecam_arm` is true when
 * the caller's gate leaves impulse denoise, defringe and microcontrast to the
 * CIE-CAM pipeline, which runs on the CieImage and is not this node.
 */
bool labSharpeningSupported(const procparams::ProcParams& params, int width,
                            int height, bool ciecam_arm, std::string* error);

/*
 * The block as it ran before the device node, moved here unchanged so the call
 * site, the host builtin and the tests share exactly one CPU implementation.
 */
void labSharpeningHostBlock(ImProcFunctions& ipf,
                            procparams::ProcParams& params, LabImage& lab,
                            bool ciecam_arm);

/*
 * Submit the block to the device and fall back to the host block. This is the
 * only entry point the Lab call sites use.
 */
void labSharpeningRun(ImProcFunctions& ipf, procparams::ProcParams& params,
                      LabImage& lab, bool ciecam_arm);

/*
 * Host-owned setup for one lab.sharpening submission. `build` derives the
 * defringe hue table through the profile's own FlatCurve; the context owns no
 * device resource.
 */
class LabSharpeningContext
{
public:
    LabSharpeningContext(procparams::ProcParams& params, ImProcFunctions& ipf,
                         int width, int height, bool ciecam_arm = false);
    ~LabSharpeningContext();

    LabSharpeningContext(const LabSharpeningContext&) = delete;
    LabSharpeningContext& operator=(const LabSharpeningContext&) = delete;

    /* Fill the host-only descriptor handed over as RT_STAGE_ARG_PTR. */
    RtLabSharpeningContext descriptor(bool supported);

    /* Build the hue table. Returns 0 on success; an error means the call site
     * must run the host block. */
    int build(std::string* error = nullptr);

    int runHost(RtStageBuffer* luminance, RtStageBuffer* a, RtStageBuffer* b);

    const std::vector<float>& hueTable() const
    {
        return hue_table_;
    }
    int hueEntries() const
    {
        return hue_entries_;
    }
    bool hueCurve() const
    {
        return hue_curve_;
    }

private:
    static int runHostThunk(void* self, RtStageBuffer* luminance,
                            RtStageBuffer* a, RtStageBuffer* b);

    procparams::ProcParams& params_;
    ImProcFunctions& ipf_;
    int width_;
    int height_;
    bool ciecam_arm_;

    std::vector<float> hue_table_;
    int hue_entries_ = 0;
    bool hue_curve_ = false;
};

} // namespace rtengine::stage
