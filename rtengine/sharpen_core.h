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

namespace rtengine
{

namespace procparams
{
struct SharpeningParams;
}

/*
 * The sharpening algorithm without the ImProcFunctions frame.
 *
 * L points at one contiguous float plane with a row pitch of W samples and a
 * plane pitch of W * H samples, which is what LabImage::L[0] provides. The
 * host builtin and the CPU entry point share this function so the two cannot
 * drift apart.
 */
void sharpen_luminance(float* L, int W, int H,
                       const procparams::SharpeningParams& sharpenParam,
                       double scale, bool multiThread, bool showMask);

/*
 * Richardson-Lucy deconvolution over the same plane. The member
 * ImProcFunctions::deconvsharpening delegates here, and the sharpening core
 * calls it for method "rld".
 */
void deconv_sharpening_luminance(float** luminance, float** tmp,
                                 const float* const* blend, int W, int H,
                                 const procparams::SharpeningParams& sharpenParam,
                                 double scale);

} // namespace rtengine
