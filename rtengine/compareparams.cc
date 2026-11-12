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
#include "compareparams.h"

namespace rtengine
{
namespace procparams
{

ProcParams makeComparisonParams(const ProcParams& live, const ProcParams& snapshot)
{
    ProcParams result = snapshot;

    result.crop = live.crop;
    result.cropGuide = live.cropGuide;
    result.coarse = live.coarse;
    result.commonTrans = live.commonTrans;
    result.rotate = live.rotate;
    result.distortion = live.distortion;
    result.perspective = live.perspective;
    result.lensProf = live.lensProf;

    return result;
}

bool geometryDiffers(const ProcParams& live, const ProcParams& snapshot)
{
    return live.crop != snapshot.crop
        || live.cropGuide != snapshot.cropGuide
        || live.coarse != snapshot.coarse
        || live.commonTrans != snapshot.commonTrans
        || live.rotate != snapshot.rotate
        || live.distortion != snapshot.distortion
        || live.perspective != snapshot.perspective
        || live.lensProf != snapshot.lensProf;
}

} // namespace procparams
} // namespace rtengine
