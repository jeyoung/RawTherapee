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

#include "ciecam_context.h"

#include "stage/algorithms/ciecam_cl.h"

#include "ciecam02.h"
#include "cieimage.h"
#include "color.h"
#include "colortemp.h"
#include "curves.h"
#include "iccstore.h"
#include "improcfun.h"
#include "labimage.h"
#include "opthelper.h"
#include "procparams.h"
#include "rt_math.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace rtengine::stage
{

namespace
{

/* The host's surround categories and their f, c, nc triples. */
bool surroundCoefficients(const Glib::ustring& name, float& f, float& c,
                          float& nc)
{
    if (name == "Average") {
        f = 1.0f;
        c = 0.69f;
        nc = 1.0f;
    } else if (name == "Dim") {
        f = 0.9f;
        c = 0.59f;
        nc = 0.9f;
    } else if (name == "Dark") {
        f = 0.8f;
        c = 0.525f;
        nc = 0.8f;
    } else if (name == "ExtremelyDark") {
        f = 0.8f;
        c = 0.41f;
        nc = 0.8f;
    } else {
        return false;
    }
    return true;
}

int algorithmIndex(const Glib::ustring& algo)
{
    if (algo == "JC") {
        return 0;
    }
    if (algo == "JS") {
        return 1;
    }
    if (algo == "QM") {
        return 2;
    }
    /* "ALL" and the historical "No" both take the full arm. */
    return 3;
}

bool buildFailed(const std::vector<float>& rec)
{
    for (float value : rec) {
        if (!std::isfinite(value)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool ciecamSupported(const procparams::ProcParams& params,
                     const ColorAppearance& customColCurve1,
                     const ColorAppearance& customColCurvered,
                     const ColorAppearance& customColCurvegreen,
                     const ColorAppearance& customColCurveblue,
                     const ColorAppearance& customColCurve2,
                     const ColorAppearance& customColCurve3)
{
    const procparams::ColorAppearanceParams& ca = params.colorappearance;

    if (!ca.enabled) {
        return false;
    }
    if (ca.modelmethod != "02") {
        return false;
    }
    if (ca.tonecie || params.epd.enabled || ca.badpixsl != 0) {
        return false;
    }
    if (ca.datacie) {
        return false;
    }
    if (settings == nullptr || settings->autocielab) {
        return false;
    }
    if (ca.algo != "JC" && ca.algo != "JS" && ca.algo != "QM" &&
        ca.algo != "ALL" && ca.algo != "No") {
        return false;
    }
    float f = 1.0f;
    float c = 1.0f;
    float nc = 1.0f;
    if (!surroundCoefficients(ca.surrsrc, f, c, nc) ||
        !surroundCoefficients(ca.surround, f, c, nc)) {
        return false;
    }
    if (ca.wbmodel != "RawT" && ca.wbmodel != "RawTCAT02" &&
        ca.wbmodel != "free") {
        return false;
    }
    if (!(ca.greenout > 0.0) || !(ca.greensc > 0.0)) {
        return false;
    }
    if (!(ca.adaplum > 0.0) || ca.ybscen <= 0 || ca.ybout <= 0) {
        return false;
    }
    /*
     * The chroma, saturation and colourfulness sliders drive
     * Color::skinredfloat(), whose skin-range boundaries turn a last-bit
     * difference in the hue into a visible step. They stay host residuals
     * until the branch decisions are bit-exact; the tone, hue and gamut
     * controls are admitted.
     */
    if (ca.chroma != 0.0 || ca.schroma != 0.0 || ca.mchroma != 0.0 ||
        ca.colorhred != 0.0 || ca.schromared != 0.0 ||
        ca.colorhgreen != 0.0 || ca.schromagreen != 0.0 ||
        ca.colorhblue != 0.0 || ca.schromablue != 0.0 ||
        ca.rstprotection != 0.0) {
        return false;
    }
    /* Only the identity of the six user colour curves is admitted. */
    if (bool(customColCurve1) || bool(customColCurvered) ||
        bool(customColCurvegreen) || bool(customColCurveblue) ||
        bool(customColCurve2) || bool(customColCurve3)) {
        return false;
    }
    return true;
}

void ciecamHostBlock(ImProcFunctions& ipf,
                     const procparams::ProcParams& params, LabImage& lab,
                     float adap, const ColorAppearance& customColCurve1,
                     const ColorAppearance& customColCurvered,
                     const ColorAppearance& customColCurvegreen,
                     const ColorAppearance& customColCurveblue,
                     const ColorAppearance& customColCurve2,
                     const ColorAppearance& customColCurve3)
{
    if (!params.colorappearance.enabled) {
        return;
    }

    CieImage cieView(lab.W, lab.H);
    LUTu dummyJ;
    LUTu dummyC;
    LUTf camBrightJ;
    LUTf camBrightQ;
    float mean = NAN;
    float d = 0.f;
    float dj = 0.f;
    float yb = 0.f;
    ipf.ciecam_02float(&cieView, adap, 1, 2, &lab, &params, customColCurve1,
                       customColCurvered, customColCurvegreen,
                       customColCurveblue, customColCurve2, customColCurve3,
                       dummyJ, dummyC, camBrightJ, camBrightQ, mean, 0, 1,
                       true, d, dj, yb, 1);
}

CiecamContext::CiecamContext(
    const procparams::ProcParams& params, ImProcFunctions& ipf,
    const LabImage& lab, float adap, const ColorAppearance& customColCurve1,
    const ColorAppearance& customColCurvered,
    const ColorAppearance& customColCurvegreen,
    const ColorAppearance& customColCurveblue,
    const ColorAppearance& customColCurve2,
    const ColorAppearance& customColCurve3) :
    params_(params),
    ipf_(ipf),
    lab_(lab),
    adap_(adap),
    customColCurve1_(customColCurve1),
    customColCurvered_(customColCurvered),
    customColCurvegreen_(customColCurvegreen),
    customColCurveblue_(customColCurveblue),
    customColCurve2_(customColCurve2),
    customColCurve3_(customColCurve3),
    rec_(RT_CIECAM_REC_COUNT, 0.0f),
    lutj_(RT_CIECAM_TABLE_ENTRIES, 0.0f),
    lutq_(RT_CIECAM_TABLE_ENTRIES, 0.0f)
{
}

CiecamContext::~CiecamContext() = default;

RtCiecamContext CiecamContext::descriptor(bool supported)
{
    RtCiecamContext descriptor{};
    descriptor.size = sizeof(RtCiecamContext);
    descriptor.version = RT_CIECAM_CONTEXT_VERSION;
    descriptor.magic = RT_CIECAM_CONTEXT_MAGIC;
    descriptor.supported = supported ? 1u : 0u;
    descriptor.self = this;
    descriptor.build = [](void* self, RtCiecamResult* out) {
        return static_cast<CiecamContext*>(self)->build(*out);
    };
    descriptor.run_host = [](void* self, RtStageBuffer* lab) {
        return static_cast<CiecamContext*>(self)->runHost(*lab);
    };
    return descriptor;
}

int CiecamContext::build(RtCiecamResult& out)
{
    const procparams::ColorAppearanceParams& ca = params_.colorappearance;
    const int width = lab_.W;
    const int height = lab_.H;
    if (width <= 0 || height <= 0) {
        return 1;
    }

    const int alg = algorithmIndex(ca.algo);
    const bool needJ = alg == 0 || alg == 1 || alg == 3;
    const bool needQ = alg == 2 || alg == 3;

    /*
     * The block's histogram pass. It runs over the input Lab before the
     * transform, exactly as ciecam_02float() computes it for the two
     * brightness curves and the automatic scene grey.
     */
    LUTu histJ;
    LUTu histQ;
    double sum = 0.0;
    if (needJ) {
        histJ(RT_CIECAM_TABLE_ENTRIES);
        histJ.clear();
    }
    if (needQ) {
        histQ(RT_CIECAM_TABLE_ENTRIES);
        histQ.clear();
    }
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            const float currL = lab_.L[i][j] / 327.68f;
            float koef;
            if (currL > 50.f) {
                if (currL > 70.f) {
                    if (currL > 80.f) {
                        if (currL > 85.f) {
                            koef = 0.97f;
                        } else {
                            koef = 0.93f;
                        }
                    } else {
                        koef = 0.87f;
                    }
                } else {
                    if (currL > 60.f) {
                        koef = 0.85f;
                    } else {
                        koef = 0.8f;
                    }
                }
            } else {
                if (currL > 10.f) {
                    if (currL > 20.f) {
                        if (currL > 40.f) {
                            koef = 0.75f;
                        } else {
                            koef = 0.7f;
                        }
                    } else {
                        koef = 0.9f;
                    }
                } else {
                    koef = 1.0f;
                }
            }
            if (needJ) {
                const int index =
                    std::min(static_cast<int>(koef * lab_.L[i][j]),
                             static_cast<int>(RT_CIECAM_TABLE_ENTRIES - 1));
                histJ[index]++;
            }
            if (needQ) {
                const int index = static_cast<int>(
                    CLIP(static_cast<int>(32768.f *
                                          sqrtf((koef * lab_.L[i][j]) /
                                                32768.f))));
                histQ[std::min(index,
                               static_cast<int>(RT_CIECAM_TABLE_ENTRIES - 1))]++;
            }
            sum += static_cast<double>(koef) * static_cast<double>(lab_.L[i][j]);
        }
    }
    const double mean = (sum / (static_cast<double>(height) * width)) / 327.68;

    float yb;
    if (ca.autoybscen) {
        if (mean < 15.0) {
            yb = 3.0f;
        } else if (mean < 30.0) {
            yb = 5.0f;
        } else if (mean < 40.0) {
            yb = 10.0f;
        } else if (mean < 45.0) {
            yb = 15.0f;
        } else if (mean < 50.0) {
            yb = 18.0f;
        } else if (mean < 55.0) {
            yb = 23.0f;
        } else if (mean < 60.0) {
            yb = 30.0f;
        } else if (mean < 70.0) {
            yb = 40.0f;
        } else if (mean < 80.0) {
            yb = 60.0f;
        } else if (mean < 90.0) {
            yb = 80.0f;
        } else {
            yb = 90.0f;
        }
    } else {
        yb = static_cast<float>(ca.ybscen);
    }

    float f = 1.0f;
    float c = 0.69f;
    float nc = 1.0f;
    if (!surroundCoefficients(ca.surrsrc, f, c, nc)) {
        return 1;
    }
    float f2 = 1.0f;
    float c2 = 0.69f;
    float nc2 = 1.0f;
    if (!surroundCoefficients(ca.surround, f2, c2, nc2)) {
        return 1;
    }

    double Xw = 1.0;
    double Zw = 1.0;
    double Xwout = 1.0;
    double Zwout = 1.0;
    double Xwsc = 1.0;
    double Zwsc = 1.0;
    ColorTemp::temp2mulxyz(params_.wb.temperature, params_.wb.method,
                           params_.wb.observer, Xw, Zw);
    ColorTemp::temp2mulxyz(ca.tempout, "Custom", params_.wb.observer, Xwout,
                           Zwout);
    ColorTemp::temp2mulxyz(ca.tempsc, "Custom", params_.wb.observer, Xwsc,
                           Zwsc);

    const float xw = 100.0f * static_cast<float>(Xw);
    const float yw = 100.0f;
    const float zw = 100.0f * static_cast<float>(Zw);
    const float xwd = 100.0f * static_cast<float>(Xwout);
    const float zwd = 100.0f * static_cast<float>(Zwout);
    const float ywd = 100.0f / static_cast<float>(ca.greenout);
    const float xws = 100.0f * static_cast<float>(Xwsc);
    const float zws = 100.0f * static_cast<float>(Zwsc);
    const float yws = 100.0f / static_cast<float>(ca.greensc);

    float xw1 = 0.f;
    float yw1 = 0.f;
    float zw1 = 0.f;
    if (ca.wbmodel == "RawT") {
        xw1 = 96.46f;
        yw1 = 100.0f;
        zw1 = 82.445f;
    } else if (ca.wbmodel == "RawTCAT02") {
        xw1 = xw;
        yw1 = yw;
        zw1 = zw;
    } else {
        xw1 = xws;
        yw1 = yws;
        zw1 = zws;
    }
    const float xw2 = xwd;
    const float yw2 = ywd;
    const float zw2 = zwd;

    const float la = ca.autoadapscen ? adap_
                                     : static_cast<float>(ca.adapscen);
    const float la2 = static_cast<float>(ca.adaplum);
    if (!(la > 0.0f) || !(la2 > 0.0f)) {
        return 1;
    }
    const float deg = static_cast<float>(ca.degree) / 100.0f;
    const float pilot = ca.autodegree ? 2.0f : deg;
    const float degout = static_cast<float>(ca.degreeout) / 100.0f;
    const float pilotout = ca.autodegreeout ? 2.0f : degout;

    float chr = 0.f;
    if (alg == 0 || alg == 3) {
        chr = static_cast<float>(ca.chroma);
        if (chr == -100.0f) {
            chr = -99.8f;
        }
    }
    float schr = 0.f;
    if (alg == 1 || alg == 3) {
        schr = static_cast<float>(ca.schroma);
        if (schr > 0.f) {
            schr /= 2.f;
        }
        if (alg == 3) {
            if (schr == -100.0f) {
                schr = -99.0f;
            }
            if (schr == 100.0f) {
                schr = 98.0f;
            }
        } else if (schr == -100.0f) {
            schr = -99.8f;
        }
    }
    float mchr = 0.f;
    if (alg == 2 || alg == 3) {
        mchr = static_cast<float>(ca.mchroma);
        if (mchr == -100.0f) {
            mchr = -99.8f;
        }
        if (mchr == 100.0f) {
            mchr = 99.9f;
        }
    }

    const float hue = static_cast<float>(ca.colorh);
    const float rstprotection = 100.f - static_cast<float>(ca.rstprotection);
    const bool gamu = ca.gamut;
    const int highlight = params_.toneCurve.hrenabled ? 1 : 0;

    /* The two brightness tables, built with the block's own helper. */
    if (needJ) {
        LUTf camJ(RT_CIECAM_TABLE_ENTRIES, LUT_CLIP_ABOVE);
        Ciecam02::curveJfloat(static_cast<float>(ca.jlight),
                              static_cast<float>(ca.contrast), 0.6f, histJ,
                              camJ);
        camJ /= 327.68f;
        for (int i = 0; i < RT_CIECAM_TABLE_ENTRIES; ++i) {
            lutj_[i] = camJ[i];
        }
    }
    if (needQ) {
        LUTf camQ(RT_CIECAM_TABLE_ENTRIES, LUT_CLIP_ABOVE);
        Ciecam02::curveJfloat(static_cast<float>(ca.qbright),
                              static_cast<float>(ca.qcontrast), 0.6f, histQ,
                              camQ);
        for (int i = 0; i < RT_CIECAM_TABLE_ENTRIES; ++i) {
            lutq_[i] = camQ[i];
        }
    }

    const float yb2 = static_cast<float>(ca.ybout);
    const int c16 = 1;
    const float plum = 100.f;
    float n = 0.f;
    float d = 0.f;
    float nbb = 0.f;
    float ncb = 0.f;
    float cz = 0.f;
    float aw = 0.f;
    float wh = 0.f;
    float pfl = 0.f;
    float fl = 0.f;
    Ciecam02::initcam1float(yb, pilot, f, la, xw, yw, zw, n, d, nbb, ncb, cz,
                            aw, wh, pfl, fl, c, c16, plum);
    float nj = 0.f;
    float dj = 0.f;
    float nbbj = 0.f;
    float ncbj = 0.f;
    float czj = 0.f;
    float awj = 0.f;
    float flj = 0.f;
    Ciecam02::initcam2float(yb2, pilotout, f2, la2, xw2, yw2, zw2, nj, dj,
                            nbbj, ncbj, czj, awj, flj, c16, plum);

    const float pow1 = pow_F(1.64f - pow_F(0.29f, n), 0.73f);
    const float pow1n = pow_F(1.64f - pow_F(0.29f, nj), 0.73f);
    const float reccmcz = 1.f / (c2 * czj);
    const float coefQ = 32767.f / wh;
    const float qproFactor = (0.4f / c) * (aw + 4.0f);
    const float coe = pow_F(fl, 0.25f);

    rec_[RT_CIECAM_REC_C] = c;
    rec_[RT_CIECAM_REC_NC] = nc;
    rec_[RT_CIECAM_REC_AW] = aw;
    rec_[RT_CIECAM_REC_FL] = fl;
    rec_[RT_CIECAM_REC_WH] = wh;
    rec_[RT_CIECAM_REC_NBB] = nbb;
    rec_[RT_CIECAM_REC_NCB] = ncb;
    rec_[RT_CIECAM_REC_CZ] = cz;
    rec_[RT_CIECAM_REC_D] = d;
    rec_[RT_CIECAM_REC_POW1] = pow1;
    rec_[RT_CIECAM_REC_POW1N] = pow1n;
    rec_[RT_CIECAM_REC_COEFQ] = coefQ;
    rec_[RT_CIECAM_REC_QPRO] = qproFactor;
    rec_[RT_CIECAM_REC_PFL] = pfl;
    rec_[RT_CIECAM_REC_PLUM] = plum;
    rec_[RT_CIECAM_REC_XW1] = xw1;
    rec_[RT_CIECAM_REC_YW1] = yw1;
    rec_[RT_CIECAM_REC_ZW1] = zw1;
    rec_[RT_CIECAM_REC_XW2] = xw2;
    rec_[RT_CIECAM_REC_YW2] = yw2;
    rec_[RT_CIECAM_REC_ZW2] = zw2;
    rec_[RT_CIECAM_REC_C2] = c2;
    rec_[RT_CIECAM_REC_NC2] = nc2;
    rec_[RT_CIECAM_REC_AWJ] = awj;
    rec_[RT_CIECAM_REC_FLJ] = flj;
    rec_[RT_CIECAM_REC_NBBJ] = nbbj;
    rec_[RT_CIECAM_REC_NCBJ] = ncbj;
    rec_[RT_CIECAM_REC_CZJ] = czj;
    rec_[RT_CIECAM_REC_DJ] = dj;
    rec_[RT_CIECAM_REC_RECCMCZ] = reccmcz;
    rec_[RT_CIECAM_REC_COE] = coe;
    rec_[RT_CIECAM_REC_MCHR] = mchr;
    rec_[RT_CIECAM_REC_SCHR] = schr;
    rec_[RT_CIECAM_REC_CHR] = chr;
    rec_[RT_CIECAM_REC_HUE] = hue;
    rec_[RT_CIECAM_REC_RSTPROTECT] = rstprotection;
    rec_[RT_CIECAM_REC_GAMU] = gamu ? 1.0f : 0.0f;
    rec_[RT_CIECAM_REC_EPSIL] = 0.0001f;
    rec_[RT_CIECAM_REC_ALG] = static_cast<float>(alg);
    rec_[RT_CIECAM_REC_HIGHLIGHT] = static_cast<float>(highlight);
    rec_[RT_CIECAM_REC_HUERED] = static_cast<float>(ca.colorhred);
    rec_[RT_CIECAM_REC_SCHRRED] = static_cast<float>(ca.schromared);
    rec_[RT_CIECAM_REC_HUEGREEN] = static_cast<float>(ca.colorhgreen);
    rec_[RT_CIECAM_REC_SCHRGREEN] = static_cast<float>(ca.schromagreen);
    rec_[RT_CIECAM_REC_HUEBLUE] = static_cast<float>(ca.colorhblue);
    rec_[RT_CIECAM_REC_SCHRBLUE] = static_cast<float>(ca.schromablue);

    if (buildFailed(rec_) || !(aw > 0.0f) || !(wh > 0.0f) ||
        !(cz > 0.0f) || !(czj > 0.0f)) {
        return 1;
    }

    if (gamu) {
        const TMatrix wip =
            ICCStore::getInstance()->workingSpaceInverseMatrix(
                params_.icm.workingProfile);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                wip_[row * 3 + column] = static_cast<float>(wip[row][column]);
            }
        }
    }

    out.rec = rec_.data();
    out.lutj = lutj_.data();
    out.lutq = lutq_.data();
    out.wip = wip_;
    built_ = true;
    return 0;
}

int CiecamContext::runHost(RtStageBuffer& lab)
{
    if (lab.data == nullptr || lab.format != RT_STAGE_F32 || lab.planes != 3 ||
        lab.width <= 0 || lab.height <= 0) {
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

    ciecamHostBlock(ipf_, params_, image, adap_, customColCurve1_,
                    customColCurvered_, customColCurvegreen_,
                    customColCurveblue_, customColCurve2_, customColCurve3_);

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
