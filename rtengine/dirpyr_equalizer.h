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

#include "LUT.h"

namespace rtengine
{

/*
 * The directional pyramid equaliser's two host-side building blocks, shared
 * with the lab.wavelet device node so the host block and the device setup
 * cannot drift.
 */

/*
 * dirpyr_equalizer.cc's fillLut(): the per-level range table. `level` selects
 * the level's noise band, `mult` is the already-modulated multiplier and
 * `skinprot` shifts the table by -1 when protection is active.
 */
void dirpyrFillLut(LUTf& irangefn, int level, double dirpyrThreshold,
                   float mult, float skinprot);

/*
 * The equaliser's hue/chroma precompute: xatan2f(b, a) and
 * sqrt(a*a + b*b) / 327.68 for every pixel. The host path uses its SSE2
 * variants when the build has them, so keeping one implementation guarantees
 * the device node sees the same masks the block computes.
 */
void dirpyrHueChroma(const float* const* l_a, const float* const* l_b,
                     float** hue, float** chrom, int width, int height);

} // namespace rtengine
