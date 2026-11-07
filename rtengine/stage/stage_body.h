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
class FramesMetaData;
class Imagefloat;
class ImProcFunctions;
class LabImage;
}

namespace rtengine::procparams
{
struct ProcParams;
}

namespace rtengine::stage
{

/*
 * Call-site geometry of the shared transform body.
 *
 * The preview and crop paths pass their working-size rectangle and full-frame
 * extent; export passes the full frame and sets `fullImage`/`useOriginalBuffer`
 * as its old call did. `transformFamily` runs the directional-pyramid
 * equaliser that the preview and crop apply inside the transform node; export
 * leaves it to lab.tools.
 */
struct TransformBodyOptions {
    int cx = 0;
    int cy = 0;
    int sx = 0;
    int sy = 0;
    int oW = 0;
    int oH = 0;
    int fW = 0;
    int fH = 0;
    const FramesMetaData* metadata = nullptr;
    int rotateDegree = 0;
    bool needTransform = false;
    bool transformFamily = false;
    bool fullImage = false;
    bool useOriginalBuffer = false;
    int scale = 1;
};

/*
 * Run the geometry transform into a caller-allocated destination and, when
 * requested, the after-transform directional-pyramid equaliser. The caller
 * owns both images and decides whether the destination replaces the source.
 */
void transformHostBlock(ImProcFunctions& ipf,
                        const procparams::ProcParams& params,
                        Imagefloat& source, Imagefloat& destination,
                        const TransformBodyOptions& options);

/* Optional measurements of the gamut body before it converts back to Lab. */
struct GamutBodyResult {
    float maxRgb = 0.f;
    float maxSat = 0.f;
};

/*
 * The end-of-process working-space gamut compression and gain. The caller
 * gates the body and, on export, may run the device node first; the Lab image
 * is updated in place. `measure` fills `result` with the preview's maximum
 * RGB and saturation values, which its listener publishes.
 */
void gamutHostBlock(ImProcFunctions& ipf,
                    const procparams::ProcParams& params, LabImage& lab,
                    bool measure = false, GamutBodyResult* result = nullptr);

} // namespace rtengine::stage
