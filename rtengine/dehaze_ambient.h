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

/*
 * The ambient-light reduction of ImProcFunctions::dehazeloc().
 *
 * dehazeloc() extracts three guided-filtered channels and then derives the
 * ambient colour and a maximum distance from a down-sized dark channel and
 * two exact 95th percentiles. The percentile selection (std::nth_element) and
 * the double-precision average are a block-global reduction: they cannot be
 * reproduced by an order-independent device pass. The stage device therefore
 * runs the guided filters on the device, reads the rescaled channels back and
 * calls this wrapper, which runs the block's own estimate_ambient_light() and
 * get_dark_channel_downsized() unchanged.
 *
 * `R`, `G` and `B` are width * height row pointers, in the coordinate frame
 * the block feeds to estimate_ambient_light(): the ROI itself when it fits the
 * 200 px cap, otherwise its nearest-neighbour rescale. `patchsize` is the
 * block's initial patch size (before the final `max(max(W, H) / 600, 2)`
 * adjustment). On success `ambient` and `maxDistance` hold the block's values.
 *
 * Returns false when no dark-channel sample is inside the block's acceptance
 * range. In that case the host reads an uninitialised ambient[] (a residual
 * defect of the block), so the caller must run the host block atomically
 * instead of reproducing it.
 */
bool dehazeAmbientPrefix(const float* const* R, const float* const* G,
                         const float* const* B, int width, int height,
                         int patchsize, float ambient[3],
                         float& maxDistance);

} // namespace rtengine
