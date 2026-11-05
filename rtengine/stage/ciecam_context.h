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
class ColorAppearance;
class ImProcFunctions;
class LabImage;
}

namespace rtengine::procparams
{
struct ProcParams;
}

/*
 * Host context of the `ciecam` node.
 *
 * The device launcher receives this structure through RT_STAGE_ARG_PTR. It is
 * host-only: the pointer and the tables it returns live in host memory and
 * nothing here is passed to a kernel except the numeric tables. The context
 * rebuilds the viewing-condition scalars, the two brightness tables and the
 * optional working-space inverse from the same helpers the host block calls.
 */
#define RT_CIECAM_CONTEXT_VERSION 1u
/* "CIEC": rejects a stale or unrelated pointer before any callback runs. */
#define RT_CIECAM_CONTEXT_MAGIC 0x43494543u

typedef struct RtCiecamResult {
    /* RT_CIECAM_REC_COUNT floats in the slot order ciecam_cl.h documents. */
    const float* rec;
    /* The two 32768-entry brightness tables, already divided as the host does. */
    const float* lutj;
    const float* lutq;
    /* The 9-entry working-space inverse, row-major. */
    const float* wip;
} RtCiecamResult;

typedef struct RtCiecamContext {
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
     * Fills the tables and returns 0 on success. The host block runs the same
     * setup, so a failure is a refusal, never a partially built request.
     */
    int (*build)(void* self, RtCiecamResult* out);
    /*
     * Runs the original host block on the Lab buffer. The builtin calls this
     * so a residual and a refused device request produce the CPU result.
     */
    int (*run_host)(void* self, RtStageBuffer* lab);
} RtCiecamContext;

namespace rtengine::stage
{

/*
 * The subset of the `ciecam` node this stage runs on the device.
 *
 * The single-pass CIE-CAM path is admitted for both colour appearance models,
 * CIECAM02 (`modelmethod == "02"`) and CAM16 (`modelmethod == "16"`): the
 * classical models, no tone mapping, no EPD, no automatic CIELAB sharpening
 * and no per-hue user colour curve. Every other configuration is a host
 * residual. `customColCurve*` are the six curve objects the call site builds;
 * only their identity is admitted.
 */
bool ciecamSupported(const procparams::ProcParams& params,
                     const ColorAppearance& customColCurve1,
                     const ColorAppearance& customColCurvered,
                     const ColorAppearance& customColCurvegreen,
                     const ColorAppearance& customColCurveblue,
                     const ColorAppearance& customColCurve2,
                     const ColorAppearance& customColCurve3);

/*
 * The block as it ran before the device node, moved here unchanged so the call
 * site, the host builtin and the tests share exactly one CPU implementation.
 */
void ciecamHostBlock(ImProcFunctions& ipf,
                     const procparams::ProcParams& params, LabImage& lab,
                     float adap, const ColorAppearance& customColCurve1,
                     const ColorAppearance& customColCurvered,
                     const ColorAppearance& customColCurvegreen,
                     const ColorAppearance& customColCurveblue,
                     const ColorAppearance& customColCurve2,
                     const ColorAppearance& customColCurve3);

/*
 * The host-owned setup for one ciecam submission.
 *
 * `build` runs the block's histogram pass, its viewing-condition setup, both
 * `initcam` calls, `curveJfloat()` for the two brightness tables and, when the
 * old Lab gamut control is on, the working-space inverse. The context owns no
 * device resource.
 */
class CiecamContext
{
public:
    CiecamContext(const procparams::ProcParams& params, ImProcFunctions& ipf,
                  const LabImage& lab, float adap,
                  const ColorAppearance& customColCurve1,
                  const ColorAppearance& customColCurvered,
                  const ColorAppearance& customColCurvegreen,
                  const ColorAppearance& customColCurveblue,
                  const ColorAppearance& customColCurve2,
                  const ColorAppearance& customColCurve3);
    ~CiecamContext();

    CiecamContext(const CiecamContext&) = delete;
    CiecamContext& operator=(const CiecamContext&) = delete;

    /* Fill the host-only descriptor handed over as RT_STAGE_ARG_PTR. */
    RtCiecamContext descriptor(bool supported);

    int build(RtCiecamResult& out);
    int runHost(RtStageBuffer& lab);

    bool built() const
    {
        return built_;
    }

private:
    const procparams::ProcParams& params_;
    ImProcFunctions& ipf_;
    const LabImage& lab_;
    float adap_;
    const ColorAppearance& customColCurve1_;
    const ColorAppearance& customColCurvered_;
    const ColorAppearance& customColCurvegreen_;
    const ColorAppearance& customColCurveblue_;
    const ColorAppearance& customColCurve2_;
    const ColorAppearance& customColCurve3_;

    std::vector<float> rec_;
    std::vector<float> lutj_;
    std::vector<float> lutq_;
    float wip_[9] = {};
    bool built_ = false;
};

} // namespace rtengine::stage
