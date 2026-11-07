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

#include <cstddef>

#include "array2D.h"

namespace rtengine {
class ProgressListener;

namespace detail {

// Internal tile core; the image source handles progress setup and borders.
void amazeDemosaic(int winx,
                   int winy,
                   int winw,
                   int winh,
                   const array2D<float>& raw,
                   array2D<float>& red,
                   array2D<float>& green,
                   array2D<float>& blue,
                   const unsigned int cfa[2][2],
                   double initial_gain,
                   ProgressListener* listener,
                   std::size_t chunk_size);

}  // namespace detail
}  // namespace rtengine
