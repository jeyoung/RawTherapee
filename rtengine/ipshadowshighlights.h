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

#include <algorithm>

namespace rtengine
{

/*
 * Host-side arithmetic of the shadows/highlights block, shared by
 * ImProcFunctions::shadowsHighlights(), the lab.tools host block and the
 * device node's setup.
 *
 * The device node reproduces the mask and the guided filter in single
 * precision but cannot evaluate pow_F() or the sh_contrast NURBS curve, so the
 * per-round tone tables are built here, on the host, by the block's own code.
 * The helpers below are the exact derivations the block uses, so the call
 * site, the builtin and the node cannot drift apart.
 */

/* tonalwidth is a slider in 0..100; the mask threshold is its 16-bit value. */
inline float shadowsHighlightsThreshold(int tonalwidth)
{
    return tonalwidth * 327.68f;
}

/* mask[y][x] = pow4(l * scale) above/below the threshold, as the block has it.
 * `thresh` is shadowsHighlightsThreshold(). */
inline float shadowsHighlightsScale(float thresh, bool hl)
{
    return hl ? (thresh > 0.f ? 0.9f / thresh : 1.f) : thresh * 0.9f;
}

/*
 * The round's amount: the block calls apply() with hightli * 0.7 or
 * shado * 0.6, a double product truncated to int. A positive slider value
 * below 2 therefore still runs the round, with amount 0.
 */
inline int shadowsHighlightsAmount(int amount, bool hl)
{
    return static_cast<int>(static_cast<double>(amount) * (hl ? 0.7 : 0.6));
}

/* The guided-filter radius: float(rad) * 10 / scal, truncated by the int
 * parameter of guidedFilter(). */
inline int shadowsHighlightsRadius(int rad, int scal)
{
    return static_cast<int>(static_cast<float>(rad) * 10 / scal);
}

/*
 * guidedFilter()'s f_mean() radius for explicit subsampling: the float r /
 * subsampling truncated to int, then clamped to the array as f_mean() and
 * boxblur() clamp it. The result is never negative: LIM(x, 0, h) with a
 * negative upper bound is 0, which boxblur() treats as a copy.
 */
inline int guidedFilterBoxRadius(int radius, int subsampling, int width,
                                 int height)
{
    int rad = static_cast<int>(static_cast<float>(radius) /
                               static_cast<float>(subsampling));
    const int upper = (std::min(width, height) - 1) / 2 - 1;
    rad = std::max(0, std::min(rad, upper));
    return std::min(rad, std::min(width - 1, height - 1));
}

/*
 * Build one shadows/highlights tone table exactly as the block does.
 *
 * `f` must already have the block's size: 32768 in Lab mode, 65536 in RGB
 * mode. hl selects the highlight round (base gamma, no contrast curve);
 * otherwise the shadow round adds the sh_contrast NURBS curve.
 */
void shadowsHighlightsLut(const double (*ws)[3], const double (*iws)[3],
                          bool lab_mode, int amount, bool hl, bool multithread,
                          LUTf& f);

} // namespace rtengine
