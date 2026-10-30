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

#include "denoise_tile_buffers.h"

#include <cstdio>
#include <limits>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main()
{
    using rtengine::denoiseTileBufferCount;
    check(denoiseTileBufferCount(1, 45) == 45, "one-row panorama reserves one row");
    check(denoiseTileBufferCount(1, 1) == 1, "one tile needs at most one buffer");
    check(denoiseTileBufferCount(2, 5) == 10, "two rows retain the full merge band");
    check(denoiseTileBufferCount(3, 5) == 10, "odd final band reuses the full band");
    check(denoiseTileBufferCount(5, 1) == 2, "one column retains two active rows");
    check(denoiseTileBufferCount(0, 5) == 0
              && denoiseTileBufferCount(3, 0) == 0
              && denoiseTileBufferCount(-1, 5) == 0
              && denoiseTileBufferCount(3, -1) == 0,
          "empty or invalid grids reserve no buffers");
    const int largest = std::numeric_limits<int>::max();
    check(denoiseTileBufferCount(2, largest)
              == static_cast<std::size_t>(largest) * 2,
          "multiplication does not overflow an int intermediate");

    for (int rows = 1; rows <= 5; ++rows) {
        for (int cols = 1; cols <= 5; ++cols) {
            const std::size_t allocated = denoiseTileBufferCount(rows, cols);
            for (int start = 0; start < rows; start += rtengine::TILE_MERGE_BAND_ROWS) {
                const int activeRows = std::min(rtengine::TILE_MERGE_BAND_ROWS, rows - start);
                const std::size_t last = static_cast<std::size_t>(activeRows) * cols - 1;
                check(last < allocated, "every full or partial band's last index fits");
            }
        }
    }
    if (failures) {
        return 1;
    }
    std::puts("denoise-tile-buffers: all checks passed");
    return 0;
}
