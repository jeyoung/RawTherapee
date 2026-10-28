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
#define RT_WORKING_TRC_CONTEXT_VERSION 1u
/* "WTRC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_WORKING_TRC_CONTEXT_MAGIC 0x57545243u
/* The gamma table buildGpuProfile() returns. */
#define RT_WORKING_TRC_GAMMA_ENTRIES 65536u

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
 * The subset of the block this stage runs on the device. Everything else is a
 * residual and must execute the host builtin: soft light, local contrast, both
 * tone equalisers, preservation, black-and-white output, non-default primaries
 * or illuminant, a non-relative intent, and a non-matrix-shaper profile.
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
 * on the host - lab2rgb(), the linear reference copy and the fixed sRGB
 * inverse - so the probe, the profile and the pass-2 input are byte-identical
 * to the CPU block whatever the device compiler does. `build` runs the setup
 * only path on that host first pass, calls buildGpuProfile() and keeps the
 * matrices, gamma table and means. The context owns no device resource.
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
     * Run the host prefix: lab2rgb(), the linear reference copy and the fixed
     * sRGB inverse. False when the input or an allocation fails.
     */
    bool prepare(const LabImage& lab);

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
    bool built_ = false;
    std::vector<float> gamma_;
    float curve_[8] = {};
    float toxyz_[9] = {};
    float matrix_[9] = {};
    double meanx_ = 0.0;
    double meany_ = 0.0;
};

} // namespace rtengine::stage
