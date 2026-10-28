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

#include <cstddef>
#include <vector>

namespace rtengine
{

/*
 * Geometry of one rotation/scale transform, in the form both the classic
 * OpenCL backend and the stage device consume.
 *
 * The map is built in double precision on the host and stored as floats,
 * because the kernel needs the integer cell and the fractional offsets that
 * the source loop would otherwise recompute per pixel. Keeping one builder
 * stops the two backend copies drifting apart.
 */
struct TransformGeometry {
    int cx = 0;
    int cy = 0;
    int sx = 0;
    int sy = 0;
    int oW = 0;
    int oH = 0;
    double hscale = 1.0;
    double vscale = 1.0;
    double cost = 1.0;
    double sint = 0.0;
};

/*
 * Fill `map` with four floats per destination pixel: source cell x, source
 * cell y, and the fractional offsets inside the cell. Both dimensions must be
 * positive.
 */
inline void buildTransformMap(const TransformGeometry& geometry, int dstWidth,
                              int dstHeight, std::vector<float>& map)
{
    const double w2 = static_cast<double>(geometry.oW) / 2.0 - 0.5;
    const double h2 = static_cast<double>(geometry.oH) / 2.0 - 0.5;
    const double centerX = geometry.cx - w2;
    const double centerY = geometry.cy - h2;

    map.resize(static_cast<std::size_t>(dstWidth) * dstHeight * 4);

    for (int y = 0; y < dstHeight; ++y) {
        for (int x = 0; x < dstWidth; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * dstWidth + x) * 4;
            const double xd =
                geometry.hscale * (static_cast<double>(x) + centerX);
            const double yd =
                geometry.vscale * (static_cast<double>(y) + centerY);
            const double dxr = xd * geometry.cost - yd * geometry.sint;
            const double dyr = xd * geometry.sint + yd * geometry.cost;
            double dx = dxr + w2;
            double dy = dyr + h2;
            int xc = static_cast<int>(dx);
            dx -= xc;
            xc -= geometry.sx;
            int yc = static_cast<int>(dy);
            dy -= yc;
            yc -= geometry.sy;
            map[index] = static_cast<float>(xc);
            map[index + 1] = static_cast<float>(yc);
            map[index + 2] = static_cast<float>(dx);
            map[index + 3] = static_cast<float>(dy);
        }
    }
}

} // namespace rtengine
