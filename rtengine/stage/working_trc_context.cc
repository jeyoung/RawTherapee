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

#include "working_trc_context.h"

#include "color.h"
#include "curves.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "labimage.h"
#include "rtgui/threadutils.h"
#include "procparams.h"
#include "rtengine.h"
#include "sleef.h"
#include "utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>

namespace rtengine::stage
{

namespace
{

/* The twelve names ImProcFunctions::workingtrc() accepts before it builds the
 * output profile. */
constexpr const char* SUPPORTED_PROFILES[] = {
    "sRGB",       "Adobe RGB", "ProPhoto",  "WideGamut",
    "BruceRGB",   "Beta RGB",  "BestRGB",   "Rec2020",
    "ACESp0",     "ACESp1",    "JDCmax",    "JDCmax stdA"
};

constexpr int PRIMARIES_DEFAULT =
    toUnderlying(procparams::ColorManagementParams::Primaries::DEFAULT);
constexpr int ILLUMINANT_DEFAULT =
    toUnderlying(procparams::ColorManagementParams::Illuminant::DEFAULT);

/* WavOpacityCurveWL::Set() fills 501 entries (0..500). */
constexpr int OPACITY_CURVE_ENTRIES = 501;
/* complete_local_contrast() treats an all-0.5 curve as inactive. */
constexpr float OPACITY_CURVE_FLAT = 0.5f;

/* One pixel copy across pitches, in the engine's own chain order. */
void copy_first_pass(const RtStageBuffer& src, Imagefloat& dst)
{
    const auto* base = static_cast<const unsigned char*>(src.data);
    for (int y = 0; y < src.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            const auto* row = reinterpret_cast<const float*>(
                base + plane * src.plane_pitch + y * src.row_pitch);
            for (int x = 0; x < src.width; ++x) {
                if (plane == 0) {
                    dst.r(y, x) = row[x];
                } else if (plane == 1) {
                    dst.g(y, x) = row[x];
                } else {
                    dst.b(y, x) = row[x];
                }
            }
        }
    }
}

int build_thunk(void* self, RtWorkingTrcResult* out)
{
    return static_cast<WorkingTrcContext*>(self)->build(*out);
}

int run_host_thunk(void* self, RtStageBuffer* lab)
{
    return static_cast<WorkingTrcContext*>(self)->runHost(*lab);
}

} // namespace

bool workingTrcProfileSupported(const Glib::ustring& profile)
{
    for (const char* name : SUPPORTED_PROFILES) {
        if (profile == name) {
            return true;
        }
    }
    return false;
}

bool workingTrcSupported(const procparams::ProcParams& params)
{
    using Cm = procparams::ColorManagementParams;

    const Cm& icm = params.icm;

    if (icm.workingTRC == Cm::WorkingTrc::NONE || !icm.trcExp) {
        return false;
    }
    if (!workingTrcProfileSupported(icm.workingProfile)) {
        return false;
    }
    if (icm.aRendIntent != RI_RELATIVE) {
        return false;
    }
    if (toUnderlying(icm.wprim) != PRIMARIES_DEFAULT ||
        toUnderlying(icm.will) != ILLUMINANT_DEFAULT) {
        return false;
    }
    if (params.softlight.enabled && params.softlight.strength != 0) {
        return false;
    }
    if (icm.wmidtcie != 0.0) {
        return false;
    }
    if (icm.wsmoothciesli != 0.0) {
        return false;
    }
    if (icm.fbw) {
        return false;
    }

    /* Local contrast only runs when the expander is on and the working-TRC
     * opacity curve is not flat. Scan the whole 501-entry LUT: the host test
     * stops at 499 and misses the last entry, so a curve that changes only
     * there must stay on the host rather than be admitted as flat. */
    if (icm.wavExp) {
        Cm colparams = icm;
        WavOpacityCurveWL curve;
        colparams.getCurves(curve);
        /* An empty LUT means local contrast is inactive, exactly as
         * complete_local_contrast() treats it. */
        if (curve) {
            for (int i = 0; i < OPACITY_CURVE_ENTRIES; ++i) {
                if (curve[i] != OPACITY_CURVE_FLAT) {
                    return false;
                }
            }
        }
    }

    /* With default primaries the preservation pass is unreachable, so preser
     * needs no separate test. */
    return true;
}

void workingTrcHostBlock(ImProcFunctions& ipf,
                         const procparams::ProcParams& params,
                         LabImage& labView)
{
    ipf.softLight(&labView, params.softlight);

    if (params.icm.workingTRC != procparams::ColorManagementParams::WorkingTrc::NONE &&
        params.icm.trcExp) {
        const int GW = labView.W;
        const int GH = labView.H;
        std::unique_ptr<LabImage> provis;
        const float pres = 0.01f * params.icm.preser;
        if (params.icm.trcExp) { //local contrast
            int level_hr = 7;
            int maxlevpo = 9;
            bool wavcurvecont = false;
            procparams::WaveletParams WaveParams = params.wavelet;
            procparams::ColorManagementParams Colparams = params.icm;
            WavOpacityCurveWL icmOpacityCurveWL;
            Colparams.getCurves(icmOpacityCurveWL);
            ipf.complete_local_contrast(&labView, &labView, WaveParams, Colparams, icmOpacityCurveWL, 1, level_hr, maxlevpo, wavcurvecont);
        }

        if (pres > 0.f && params.icm.wprim != procparams::ColorManagementParams::Primaries::DEFAULT) {
            provis.reset(new LabImage(GW, GH));
            provis->CopyFrom(&labView);
        }

        const std::unique_ptr<Imagefloat> tmpImage1(new Imagefloat(GW, GH));

        ipf.lab2rgb(labView, *tmpImage1, params.icm.workingProfile);

        const float satu = params.icm.wapsat;
        // apsatur() needs the pre-TRC image as a reference; keep the copy
        // only when that pass will actually run.
        std::unique_ptr<Imagefloat> tmpImage2;

        if (satu > 0.f) {
            tmpImage2.reset(new Imagefloat(GW, GH));
            tmpImage1.get()->copyData(tmpImage2.get());
        }

        const float gamtone = params.icm.wGamma;
        const float slotone = params.icm.wSlope;

        int illum = toUnderlying(params.icm.will);
        const int prim = toUnderlying(params.icm.wprim);

        Glib::ustring prof = params.icm.workingProfile;

        cmsHTRANSFORM dummy = nullptr;
        int ill = 0;
        bool gamutcontrol = params.icm.gamut;
        int catc = toUnderlying(params.icm.wcat);
        int locprim = 0;
        const int midton = params.icm.wmidtcie;
        if (midton != 0) {
            ToneEqualizerParams teParams;
            teParams.enabled = true;
            teParams.regularization = 0.f;
            teParams.pivot = 0.f;
            teParams.bands[0] = 0;
            teParams.bands[2] = midton;
            teParams.bands[4] = 0;
            teParams.bands[5] = 0;
            int mid = abs(midton);
            int threshmid = 50;
            if (mid > threshmid) {
                teParams.bands[1] = sign(midton) * (mid - threshmid);
                teParams.bands[3] = sign(midton) * (mid - threshmid);
            }
            ipf.toneEqualizer(tmpImage1.get(), teParams, prof, 1, false);
        }

        float rdx, rdy, grx, gry, blx, bly = 0.f;
        float meanx, meany, meanxe, meanye, maxdat = 0.f;
        double p[6] = {0., 0., 0., 0., 0., 0.};

        ipf.workingtrc(0, tmpImage1.get(), tmpImage1.get(), GW, GH, -5, prof, 2.4, 12.92310, 0, ill, 0, 0, rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, p, dummy, true, false, false, false);
        ipf.workingtrc(0, tmpImage1.get(), tmpImage1.get(), GW, GH, 5, prof, gamtone, slotone, catc, illum, prim, locprim, rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, p, dummy, false, true, true, gamutcontrol);

        if (satu > 0.f) {
            assert(tmpImage2);
            ipf.apsatur(0, tmpImage1.get(), tmpImage2.get(), GW, GH, satu);
        }
        const float smoothisli = params.icm.wsmoothciesli;

        if (smoothisli > 0.f) {
            ToneEqualizerParams teParams;
            teParams.enabled = true;
            teParams.regularization = 0.f;
            teParams.pivot = 0.f;
            teParams.bands[0] = 0;
            teParams.bands[1] = 0;
            teParams.bands[2] = 0;
            teParams.bands[3] = 0;
            teParams.bands[4] = -40; //arbitrary value to adapt with WhiteEvjz - here White Ev # 10
            teParams.bands[5] = -80; //8 Ev and above
            bool Evsix = true;
            if (Evsix) { //EV = 6 majority of images
                teParams.bands[4] = -30 * smoothisli;
                float smmothsli5 = std::min(smoothisli, 1.f);
                teParams.bands[5] = -80 * smmothsli5;
            }

            ipf.toneEqualizer(tmpImage1.get(), teParams, prof, 1, false);
        }

        ipf.rgb2lab(*tmpImage1, labView, params.icm.workingProfile);

        // labView and provis
        if (provis) {
            ipf.preserv(&labView, provis.get(), GW, GH);
        }

        if (params.icm.fbw) {
#ifdef _OPENMP
            #pragma omp parallel for
#endif

            for (int x = 0; x < GH; x++)
                for (int y = 0; y < GW; y++) {
                    labView.a[x][y] = 0.f;
                    labView.b[x][y] = 0.f;
                }
        }
    }
}

WorkingTrcContext::WorkingTrcContext(const procparams::ProcParams& params,
                                     ImProcFunctions& ipf)
    : params_(params), ipf_(ipf)
{
}

WorkingTrcContext::~WorkingTrcContext() = default;

RtWorkingTrcContext WorkingTrcContext::descriptor(bool supported)
{
    RtWorkingTrcContext context{};
    context.size = sizeof(RtWorkingTrcContext);
    context.version = RT_WORKING_TRC_CONTEXT_VERSION;
    context.magic = RT_WORKING_TRC_CONTEXT_MAGIC;
    context.supported = supported ? 1u : 0u;
    context.self = this;
    context.build = &build_thunk;
    context.run_host = &run_host_thunk;
    return context;
}

bool WorkingTrcContext::prepare(const LabImage& lab)
{
    if (lab.W <= 0 || lab.H <= 0) {
        return false;
    }

    firstPass_.reset(new Imagefloat(lab.W, lab.H));
    linearRef_.reset(new Imagefloat(lab.W, lab.H));
    if (firstPass_ == nullptr || linearRef_ == nullptr) {
        return false;
    }

    /*
     * The host prefix: the block's own lab2rgb() and fixed first pass. The
     * reference copy is taken before the inverse so apsatur() has the same
     * linear image the block copies. Doing this on the host is what makes the
     * probe, the profile and the pass-2 input independent of the device
     * compiler's float semantics.
     */
    ipf_.lab2rgb(lab, *firstPass_, params_.icm.workingProfile);
    firstPass_->copyData(linearRef_.get());

    float rdx = 0.f;
    float rdy = 0.f;
    float grx = 0.f;
    float gry = 0.f;
    float blx = 0.f;
    float bly = 0.f;
    float meanx = 0.f;
    float meany = 0.f;
    float meanxe = 0.f;
    float meanye = 0.f;
    float maxdat = 0.f;
    double p[6] = {0., 0., 0., 0., 0., 0.};
    cmsHTRANSFORM dummy = nullptr;
    int illum = 0;
    Glib::ustring prof = params_.icm.workingProfile;

    ipf_.workingtrc(0, firstPass_.get(), firstPass_.get(), lab.W, lab.H, -5,
                    prof, WORKING_TRC_FIRST_GAMMA, WORKING_TRC_FIRST_SLOPE,
                    0, illum, 0, 0, rdx, rdy, grx, gry, blx, bly, meanx,
                    meany, meanxe, meanye, maxdat, p, dummy, true, false,
                    false, false);
    return true;
}

Imagefloat& WorkingTrcContext::linearReference() const
{
    return *linearRef_;
}

Imagefloat& WorkingTrcContext::xyz() const
{
    return *xyz_;
}

int WorkingTrcContext::build(RtWorkingTrcResult& out)
{
    if (built_) {
        out.meanx = meanx_;
        out.meany = meany_;
        out.toxyz = toxyz_;
        out.matrix = matrix_;
        out.gamma = gamma_.data();
        out.curve = curve_;
        return 0;
    }
    if (firstPass_ == nullptr) {
        return 1;
    }

    const int width = firstPass_->getWidth();
    const int height = firstPass_->getHeight();
    /*
     * The float lcms transform the host uses is only a matrix shaper inside
     * the working gamut. Outside it the block's abstract-profile input
     * carries values above 1, where lcms does not behave as the plain
     * matrix-plus-TRC the device reproduces; keep that case on the host
     * instead of approximating it.
     */
    float maxFirst = 0.0f;
    bool finite = true;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float r = firstPass_->r(y, x);
            const float g = firstPass_->g(y, x);
            const float b = firstPass_->b(y, x);
            finite = finite && std::isfinite(r) && std::isfinite(g) &&
                     std::isfinite(b);
            maxFirst = std::max(maxFirst, std::max(r, std::max(g, b)));
        }
    }
    if (!finite || maxFirst > WORKING_TRC_FIRST_PASS_MAX) {
        return 4;
    }

    const procparams::ColorManagementParams& icm = params_.icm;
    const float gamtone = icm.wGamma;
    const float slotone = icm.wSlope;
    int illum = toUnderlying(icm.will);
    const int prim = toUnderlying(icm.wprim);
    const int catc = toUnderlying(icm.wcat);
    Glib::ustring prof = icm.workingProfile;

    cmsHTRANSFORM dummy = nullptr;
    float rdx = 0.f;
    float rdy = 0.f;
    float grx = 0.f;
    float gry = 0.f;
    float blx = 0.f;
    float bly = 0.f;
    float meanxe = 0.f;
    float meanye = 0.f;
    float maxdat = 0.f;
    double p[6] = {0., 0., 0., 0., 0., 0.};
    float meanx = 0.f;
    float meany = 0.f;
    double primMatrix[9] = {};
    cmsHPROFILE profile = nullptr;

    ipf_.workingtrc(0, firstPass_.get(), firstPass_.get(), width, height, 5,
                    prof, gamtone, slotone, catc, illum, prim, 0, rdx, rdy,
                    grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat,
                    p, dummy, false, true, true, icm.gamut, &meanx, &meany,
                    &profile, true, primMatrix);

    if (profile == nullptr) {
        return 2;
    }

    bool ready = false;
    int curveType = 0;
    {
        MyMutex::MyLock lcmsLock(*lcmsMutex);
        ready = buildGpuProfile(profile, matrix_, gamma_);

        auto* trc = static_cast<cmsToneCurve*>(
            cmsReadTag(profile, cmsSigRedTRCTag));
        if (trc != nullptr) {
            curveType = cmsGetToneCurveParametricType(trc);
            const cmsFloat64Number* params = cmsGetToneCurveParams(trc);
            if (params != nullptr) {
                for (int i = 0; i < 7; ++i) {
                    curve_[i + 1] =
                        std::isfinite(params[i])
                            ? static_cast<float>(params[i])
                            : 0.0f;
                }
            }
        }
        cmsCloseProfile(profile);
    }

    if (!ready || gamma_.size() != RT_WORKING_TRC_GAMMA_ENTRIES ||
        curveType != 5) {
        /* The device path is type 5 only. A CUSTOM WorkingTRC with gamma < 1
         * builds the inverse type -5 curve, whose steep inverse amplifies the
         * remaining float-versus-lcms post residual through the Lab a
         * cancellation; it is a named host residual, never an approximation. */
        return 3;
    }
    curve_[0] = static_cast<float>(curveType);

    /*
     * The second pass calls workingtrc() with normalizeIn == false, so the
     * pre-transform matrix is the working matrix itself; the reference it
     * consumes is the 0..1 first-pass output. The setup call above also
     * applied the epsilon clamp, exactly as the host block does before it
     * reads toxyz.
     */
    const TMatrix wprof =
        ICCStore::getInstance()->workingSpaceMatrix(icm.workingProfile);
    float toxyz[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            toxyz_[r * 3 + c] = static_cast<float>(wprof[r][c]);
            toxyz[r][c] = toxyz_[r * 3 + c];
        }
    }

    /* Prepare the device second-pass input on the host: toxyz and, when the
     * block asks for it, Color::gamutmap() with its double primaries matrix. */
    double primMat[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            primMat[r][c] = primMatrix[r * 3 + c];
        }
    }
    xyz_.reset(new Imagefloat(width, height));
    if (xyz_ == nullptr) {
        return 5;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float r = firstPass_->r(y, x);
            const float g = firstPass_->g(y, x);
            const float b = firstPass_->b(y, x);
            float X = toxyz[0][0] * r + toxyz[0][1] * g + toxyz[0][2] * b;
            float Y = toxyz[1][0] * r + toxyz[1][1] * g + toxyz[1][2] * b;
            float Z = toxyz[2][0] * r + toxyz[2][1] * g + toxyz[2][2] * b;
            if (icm.gamut) {
                Color::gamutmap(X, Y, Z, primMat);
            }
            xyz_->r(y, x) = X;
            xyz_->g(y, x) = Y;
            xyz_->b(y, x) = Z;
        }
    }

    meanx_ = meanx;
    meany_ = meany;
    built_ = true;

    out.meanx = meanx_;
    out.meany = meany_;
    out.toxyz = toxyz_;
    out.matrix = matrix_;
    out.gamma = gamma_.data();
    out.curve = curve_;
    return 0;
}

int WorkingTrcContext::runHost(RtStageBuffer& lab)
{
    if (lab.data == nullptr || lab.format != RT_STAGE_F32 ||
        lab.planes != 3 || lab.width <= 0 || lab.height <= 0) {
        return 1;
    }

    LabImage image(lab.width, lab.height);
    const auto* base = static_cast<const unsigned char*>(lab.data);
    for (int y = 0; y < lab.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            const auto* row = reinterpret_cast<const float*>(
                base + plane * lab.plane_pitch + y * lab.row_pitch);
            float* dst = plane == 0 ? image.L[y]
                        : plane == 1 ? image.a[y]
                                     : image.b[y];
            std::memcpy(dst, row, static_cast<std::size_t>(lab.width) *
                                      sizeof(float));
        }
    }

    workingTrcHostBlock(ipf_, params_, image);

    auto* out = static_cast<unsigned char*>(lab.data);
    for (int y = 0; y < lab.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            auto* row = reinterpret_cast<float*>(
                out + plane * lab.plane_pitch + y * lab.row_pitch);
            const float* src = plane == 0 ? image.L[y]
                               : plane == 1 ? image.a[y]
                                            : image.b[y];
            std::memcpy(row, src, static_cast<std::size_t>(lab.width) *
                                     sizeof(float));
        }
    }
    return 0;
}

} // namespace rtengine::stage
