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

#include "stage_body.h"

#include "color.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"
#include "sleef.h"

#include <algorithm>
#include <memory>

namespace rtengine::stage
{

void transformHostBlock(ImProcFunctions& ipf,
                        const procparams::ProcParams& params,
                        Imagefloat& source, Imagefloat& destination,
                        const TransformBodyOptions& options)
{
    if (options.needTransform) {
        ipf.transform(&source, &destination, options.cx, options.cy,
                      options.sx, options.sy, options.oW, options.oH,
                      options.fW, options.fH, options.metadata,
                      options.rotateDegree, options.fullImage,
                      options.useOriginalBuffer);
    } else {
        source.copyData(&destination);
    }

    if (options.transformFamily) {
        const int width = destination.getWidth();
        const int height = destination.getHeight();
        LabImage lab(width, height);
        ipf.rgb2lab(destination, lab, params.icm.workingProfile);
        ipf.dirpyrequalizer(&lab, options.scale);
        ipf.lab2rgb(lab, destination, params.icm.workingProfile);
    }
}

void gamutHostBlock(ImProcFunctions& ipf,
                    const procparams::ProcParams& params, LabImage& lab,
                    bool measure, GamutBodyResult* result)
{
    const bool exec = params.icm.wgamut !=
                          procparams::ColorManagementParams::Wwgamut::NONE
                      || params.icm.wgamgain != 0.f;

    if (params.icm.workingTRC ==
            procparams::ColorManagementParams::WorkingTrc::NONE
        || !params.icm.trcExp || !exec) {
        return;
    }

    const int width = lab.W;
    const int height = lab.H;
    const TMatrix workingMatrix =
        ICCStore::getInstance()->workingSpaceMatrix(params.icm.workingProfile);
    const double wp[3][3] = {
        {workingMatrix[0][0], workingMatrix[0][1], workingMatrix[0][2]},
        {workingMatrix[1][0], workingMatrix[1][1], workingMatrix[1][2]},
        {workingMatrix[2][0], workingMatrix[2][1], workingMatrix[2][2]}};
    const TMatrix workingInverse =
        ICCStore::getInstance()->workingSpaceInverseMatrix(
            params.icm.workingProfile);
    const double wip[3][3] = {
        {workingInverse[0][0], workingInverse[0][1], workingInverse[0][2]},
        {workingInverse[1][0], workingInverse[1][1], workingInverse[1][2]},
        {workingInverse[2][0], workingInverse[2][1], workingInverse[2][2]}};

    std::unique_ptr<Imagefloat> comp(new Imagefloat(width, height));

#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            float X, Y, Z = 0.f;
            Color::Lab2XYZ(lab.L[i][j], lab.a[i][j], lab.b[i][j], X, Y, Z);
            Color::xyz2rgb(X, Y, Z, comp->r(i, j), comp->g(i, j),
                           comp->b(i, j), wip);
        }
    }

    const float gain = pow_F(2.f, static_cast<float>(params.icm.wgamgain));
    if (params.icm.wgamgain != 0.f) {
#ifdef _OPENMP
        #pragma omp parallel for
#endif
        for (int i = 0; i < height; ++i) {
            for (int j = 0; j < width; ++j) {
                comp->r(i, j) *= gain;
                comp->g(i, j) *= gain;
                comp->b(i, j) *= gain;
            }
        }
    }

    float mac = 0.f;
    float mac0 = 0.f;
    float mac1 = 0.f;
    float mac2 = 0.f;
    int beginend = 1;
    int nbsegam = 0;
    float powe = 1.f;
    if (params.icm.wgamut !=
        procparams::ColorManagementParams::Wwgamut::NONE) {
        ipf.gamutcompr(comp.get(), comp.get(), beginend, powe, nbsegam, mac,
                       mac0, mac1, mac2);
    }

    if (measure && result != nullptr) {
        float rgbmax = 0.f;
        float satmax = 0.f;
#ifdef _OPENMP
        #pragma omp parallel for reduction(max:rgbmax) reduction(max:satmax)
#endif
        for (int i = 0; i < height; ++i) {
            for (int j = 0; j < width; ++j) {
                const float r = comp->r(i, j);
                const float g = comp->g(i, j);
                const float b = comp->b(i, j);
                rgbmax = std::max(rgbmax, rtengine::max(r, g, b));
                float h, s, l = 0.f;
                Color::rgb2hsl(r, g, b, h, s, l);
                satmax = std::max(satmax, s);
            }
        }
        result->maxRgb = rgbmax / 65535.f;
        result->maxSat = satmax;
    }

#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            float x, y, z = 0.f;
            Color::rgbxyz(comp->r(i, j), comp->g(i, j), comp->b(i, j), x, y,
                          z, wp);
            Color::XYZ2Lab(x, y, z, lab.L[i][j], lab.a[i][j], lab.b[i][j]);
        }
    }
}

} // namespace rtengine::stage
