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

#include <glibmm/ustring.h>

#include <cstdint>
#include <memory>
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
 * Host context of the working.trc node.
 *
 * The device launcher receives this structure through RT_STAGE_ARG_PTR. It is
 * host-only: the pointer, the callbacks and everything they return live in
 * host memory, and nothing here is passed to a kernel. The launcher validates
 * the per-pixel arguments from the ABI on its own; the context only supplies
 * the colour setup that cannot be computed without lcms.
 *
 * The struct is C-compatible because the ABI is. `self` points at the C++
 * object that owns the ProcParams and ImProcFunctions references, so the
 * pointer must not outlive them.
 */
#define RT_WORKING_TRC_CONTEXT_VERSION 3u
/* "WTRC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_WORKING_TRC_CONTEXT_MAGIC 0x57545243u
/* The gamma table buildGpuProfile() returns. */
#define RT_WORKING_TRC_GAMMA_ENTRIES 65536u
/* One entry per 16-bit luminance code, as the host tone-equaliser table. */
#define RT_WORKING_TRC_TONE_ENTRIES 65536u
/* The equaliser's fifteen luminance bands. */
#define RT_WORKING_TRC_TONE_BANDS 15u

typedef struct RtWorkingTrcResult {
    double meanx;
    double meany;
    /* Nine entries each, row-major, valid until the context is destroyed.
     * `matrix` is XYZ -> linear from buildGpuProfile(). */
    /* Working RGB -> XYZ, for apsatur()'s norm3() only: the second pass
     * input is prepared on the host (see prepare()). */
    const float* toxyz;
    const float* matrix;
    const float* gamma;
    /* {type, p0..p6}: the profile's parametric TRC, used outside the table's
     * 0..1 range where lcms evaluates the curve directly. */
    const float* curve;
} RtWorkingTrcResult;

typedef struct RtWorkingTrcContext {
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
     * Runs the setup phase on the host first pass held by the context and
     * fills the matrices, gamma table and probe means. Returns 0 on success.
     */
    int (*build)(void* self, RtWorkingTrcResult* out);
    /*
     * Runs the original host block on the Lab buffer. The builtin calls this
     * so a residual and a refused device request produce the CPU result.
     */
    int (*run_host)(void* self, RtStageBuffer* lab);
    /*
     * Runs the host prefix's first half on `lab`: lab2rgb() and the linear
     * reference copy. The fixed sRGB inverse is a separate call because the
     * midtone equaliser runs between them. The launcher calls it, not the
     * call site, because the soft-light pre-pass runs on the device first: it
     * passes a private copy of the kernel's readback, so a later refusal
     * leaves the caller's image untouched. Returns 0 on success.
     */
    int (*prepare)(void* self, const RtStageBuffer* lab);
    /*
     * View over the prepared host first pass (lab2rgb output), for the
     * midtone equaliser upload and readback. Returns 0 on success.
     */
    int (*host_image)(void* self, RtStageBuffer* out);
    /*
     * Runs the fixed sRGB inverse on the host first pass. Called after any
     * device midtone equaliser and before build(). Returns 0 on success.
     */
    int (*first_pass)(void* self);
    /*
     * Views over the built second-pass input and the linear pre-TRC
     * reference, valid after build() succeeds.
     */
    int (*images)(void* self, RtStageBuffer* xyz, RtStageBuffer* linear);
    /*
     * Soft-light pre-pass. `softlight` is 1 when the device kernel runs
     * before prepare(); `softlight_strength` is the block's blend percentage
     * and the two row-major matrices are the host's float working-space pair
     * (RGB -> XYZ and XYZ -> RGB). Host-only, like the rest of the context.
     */
    uint32_t softlight;
    int32_t softlight_strength;
    float softlight_wp[9];
    float softlight_wip[9];
    /*
     * Tone-equaliser arms. Each flag is 1 when the block runs that equaliser
     * and the node applies it on the device. The host builds the 65536-entry
     * correction table, the fifteen band factors, their Gaussian sum and the
     * per-pixel `xlogf(2)` divisor; `tone_gain` and `tone_inv_gain` are the
     * block's `1/65535 * 2^-pivot` and its reciprocal. `tone_wy` is the
     * working-space luminance row `Color::rgbLuminance()` multiplies in
     * double. The LUT and factor pointers stay valid until the context is
     * destroyed.
     */
    uint32_t tone_mid;
    uint32_t tone_smooth;
    float tone_gain;
    float tone_inv_gain;
    const float* tone_mid_lut;
    const float* tone_smooth_lut;
    float tone_mid_factors[RT_WORKING_TRC_TONE_BANDS];
    float tone_smooth_factors[RT_WORKING_TRC_TONE_BANDS];
    float tone_mid_wsum;
    float tone_smooth_wsum;
    float tone_mid_log2;
    float tone_smooth_log2;
    double tone_wy[3];
} RtWorkingTrcContext;

namespace rtengine::stage
{

/*
 * The fixed first pass of the block: Color::igamma2's curve, which the host
 * spells 2.4 / 12.92310 at the call site. These are invariants of the sRGB
 * inverse, not user settings, so they are named here rather than carried in
 * the ABI.
 */
inline constexpr double WORKING_TRC_FIRST_GAMMA = 2.4;
inline constexpr double WORKING_TRC_FIRST_SLOPE = 12.92310;

/*
 * Largest first-pass sample the device setup admits. In-gamut data lands at
 * or below 1; a white pixel can round a few ulps above it. A larger overshoot
 * is real out-of-gamut data, where the host's lcms transform no longer behaves
 * as the matrix-shaper the device reproduces, so the setup refuses it.
 */
inline constexpr float WORKING_TRC_FIRST_PASS_MAX = 1.001f;

/*
 * The working profiles whose primaries and illuminant the block derives, in
 * ImProcFunctions::workingtrc(). A name outside this list makes the host return
 * before it builds the profile, so it cannot be a device case.
 */
bool workingTrcProfileSupported(const Glib::ustring& profile);

/*
 * guidedfilter.cc's calculate_subsampling() for the tone equaliser's fixed
 * radius of five: one below a 600-pixel extent, five otherwise.
 */
int workingTrcToneSubsampling(int width, int height);

/*
 * The subset of the block this stage runs on the device. Soft light is
 * admitted at its GUI strength range 1..100 (zero is the block's own no-op);
 * the two tone equalisers are admitted at their GUI ranges (`wmidtcie`
 * -100..100, `wsmoothciesli` 0..1.5) with the block's fixed pivot, scale and
 * zero regularization. Everything else is a residual and must execute the
 * host builtin: local contrast, preservation, black-and-white output,
 * non-default primaries or illuminant, a non-relative intent, an out-of-range
 * tone-equaliser or soft-light value, and a non-matrix-shaper profile.
 */
bool workingTrcSupported(const procparams::ProcParams& params);

/*
 * The block as it ran before the device node, moved here unchanged so the call
 * site, the host builtin and the tests share exactly one CPU implementation.
 */
void workingTrcHostBlock(ImProcFunctions& ipf,
                         const procparams::ProcParams& params,
                         LabImage& lab);

/*
 * The host-owned setup for one working.trc submission.
 *
 * The node's device work starts at the second pass. `prepare` runs the prefix
 * on the host - lab2rgb(), the linear reference copy and, in `firstPass()`,
 * the fixed sRGB inverse - so the probe, the profile and the pass-2 input are
 * byte-identical to the CPU block whatever the device compiler does. `build`
 * runs the setup only path on that host first pass, calls buildGpuProfile()
 * and keeps the matrices, gamma table and means. The context owns no device
 * resource.
 */
class WorkingTrcContext
{
public:
    WorkingTrcContext(const procparams::ProcParams& params,
                      ImProcFunctions& ipf);
    ~WorkingTrcContext();

    WorkingTrcContext(const WorkingTrcContext&) = delete;
    WorkingTrcContext& operator=(const WorkingTrcContext&) = delete;

    /* Fill the host-only descriptor handed over as RT_STAGE_ARG_PTR. */
    RtWorkingTrcContext descriptor(bool supported);

    /*
     * Run the host prefix's first half: lab2rgb() and the linear reference
     * copy. False when the input or an allocation fails.
     */
    bool prepare(const LabImage& lab);

    /*
     * View over the prepared host first pass (lab2rgb output) for the
     * midtone-equaliser device pass. Returns 0 on success.
     */
    int hostImage(RtStageBuffer& out) const;

    /*
     * Run the fixed sRGB inverse on the host first pass, after any device
     * midtone equaliser. False when prepare() has not run.
     */
    bool firstPass();

    /*
     * The launcher's form of prepare(): copy the supplied three-plane buffer
     * to a private LabImage first, so the device soft-light readback never
     * becomes the caller's image. Returns 0 on success.
     */
    int prepareBuffer(const RtStageBuffer& lab);

    /*
     * Views over the built second-pass input and the linear reference, for
     * the launcher to upload. Returns 0 on success.
     */
    int images(RtStageBuffer& xyz, RtStageBuffer& linear) const;

    /*
     * Build the probe/profile on the host prefix and the second-pass input:
     * toxyz() followed by the host gamutmap(). The device never evaluates the
     * gamut map, whose double matrix the GPU compilers do not reproduce.
     */
    int build(RtWorkingTrcResult& out);

    /* Host second-pass input (XYZ after the optional gamut map). */
    Imagefloat& xyz() const;
    /* Pre-TRC linear reference for apsatur(). */
    Imagefloat& linearReference() const;
    int runHost(RtStageBuffer& lab);

    /* Probe means of the last build(), for parity evidence. */
    double meanx() const
    {
        return meanx_;
    }
    double meany() const
    {
        return meany_;
    }

    bool built() const
    {
        return built_;
    }

private:
    const procparams::ProcParams& params_;
    ImProcFunctions& ipf_;

    std::unique_ptr<Imagefloat> firstPass_;
    std::unique_ptr<Imagefloat> linearRef_;
    std::unique_ptr<Imagefloat> xyz_;
    bool prepared_ = false;
    bool built_ = false;
    std::vector<float> gamma_;
    std::vector<float> toneMidLut_;
    std::vector<float> toneSmoothLut_;
    float toneMidFactors_[RT_WORKING_TRC_TONE_BANDS] = {};
    float toneSmoothFactors_[RT_WORKING_TRC_TONE_BANDS] = {};
    float toneMidWsum_ = 0.f;
    float toneSmoothWsum_ = 0.f;
    float toneMidLog2_ = 0.f;
    float toneSmoothLog2_ = 0.f;
    double toneWy_[3] = {};
    float curve_[8] = {};
    float toxyz_[9] = {};
    float matrix_[9] = {};
    double meanx_ = 0.0;
    double meany_ = 0.0;
};

} // namespace rtengine::stage
