/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
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

enum eGaussType {GAUSS_STANDARD, GAUSS_MULT, GAUSS_DIV};
static constexpr auto GAUSS_SKIP = 0.25;
/* Above this sigma gaussianBlur() uses the double-precision IIR recurrence. */
static constexpr auto GAUSS_DOUBLE_LIMIT = 25.0;

/*
 * The Young-van Vliet IIR coefficients gaussianBlur() derives for one sigma,
 * in the three forms the host's paths use:
 *
 *   M      gaussHorizontal()/gaussVertical(), the double-precision path used
 *          at and above GAUSS_DOUBLE_LIMIT;
 *   sse_M  gaussHorizontalSse()/gaussVerticalSse(), the SSE float path used
 *          below it. Its non-vectorised tail lines evaluate the
 *          recurrence in double with these values and narrow each store.
 *
 * The stage device port reads them so it runs the host's own arithmetic
 * instead of re-deriving it. The normalisations are repeated in the same
 * operation order as those functions, so the values are bit-identical.
 */
struct GaussianIirCoefficients {
    double b1 = 0.0;
    double b2 = 0.0;
    double b3 = 0.0;
    double B = 0.0;
    double M[3][3] = {};
    double sse_M[3][3] = {};
};

void gaussianBlur(float** src, float** dst, const int W, const int H, const double sigma, bool useBoxBlur = false, eGaussType gausstype = GAUSS_STANDARD, float** buffer2 = nullptr);

GaussianIirCoefficients gaussianIirCoefficients(double sigma);
