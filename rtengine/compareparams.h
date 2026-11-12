/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 RawTherapee contributors
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

#include "procparams.h"

namespace rtengine
{
namespace procparams
{

/**
 * Builds the parameters used for a snapshot comparison.
 *
 * The comparison keeps the live view's geometry so the visible area cannot
 * shift, and takes the snapshot's non-geometry adjustments (exposure, tone,
 * colour, local edits, sharpening, noise reduction, ...).
 *
 * Geometry fields taken from live:
 * crop, cropGuide, coarse, commonTrans, rotate, distortion, perspective and
 * lensProf. The lens profile is treated as geometry; its vignetting component
 * therefore also follows the live view.
 */
ProcParams makeComparisonParams(const ProcParams& live, const ProcParams& snapshot);

/**
 * True when any geometry field above differs between the two parameter sets.
 */
bool geometryDiffers(const ProcParams& live, const ProcParams& snapshot);

} // namespace procparams
} // namespace rtengine
