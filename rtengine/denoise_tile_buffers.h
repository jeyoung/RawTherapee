/*
 *  This file is part of RawTherapee.
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

#include <algorithm>
#include <cstddef>

namespace rtengine
{

// Two tile rows keep enough contributions in flight for the deterministic merge.
constexpr int TILE_MERGE_BAND_ROWS = 2;

inline std::size_t denoiseTileBufferCount(int tileRows, int tileCols)
{
    if (tileRows <= 0 || tileCols <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(std::min(TILE_MERGE_BAND_ROWS, tileRows))
        * static_cast<std::size_t>(tileCols);
}

} // namespace rtengine
