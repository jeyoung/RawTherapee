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
#include <cstdio>

#include "rtengine/compareparams.h"

using namespace rtengine::procparams;

namespace
{

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "geometry test failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

void setGeometry(ProcParams& p, int seed)
{
    p.crop.enabled = true;
    p.crop.x = seed;
    p.crop.y = seed + 1;
    p.crop.w = seed + 2;
    p.crop.h = seed + 3;
    p.crop.fixratio = seed % 2 != 0;
    p.crop.ratio = Glib::ustring::compose("%1:1", seed + 1);
    p.crop.orientation = seed % 2 ? "landscape" : "portrait";

    p.cropGuide.enabled = seed % 2 != 0;
    p.cropGuide.bleed = seed;

    p.coarse.rotate = (seed * 90) % 360;
    p.coarse.hflip = seed % 2 != 0;
    p.coarse.vflip = seed % 3 == 0;

    p.commonTrans.method = seed % 2 ? "log" : "linear";
    p.commonTrans.autofill = seed % 2 != 0;
    p.commonTrans.scale = 1.0 + seed * 0.01;

    p.rotate.degree = seed * 0.5;
    p.distortion.amount = seed * 0.01;
    p.distortion.defish = seed % 2 != 0;
    p.distortion.focal_length = 12.0 + seed;

    p.perspective.method = seed % 2 ? "simple" : "camera_based";
    p.perspective.render = seed % 2 != 0;
    p.perspective.horizontal = seed * 0.3;
    p.perspective.vertical = seed * 0.4;
    p.perspective.camera_pitch = seed * 0.5;

    p.lensProf.lcMode = seed % 2 ? LensProfParams::LcMode::LCP : LensProfParams::LcMode::NONE;
    p.lensProf.lcpFile = Glib::ustring::compose("profile-%1.lcp", seed);
    p.lensProf.useDist = seed % 2 != 0;
    p.lensProf.useVign = seed % 2 == 0;
    p.lensProf.useCA = seed % 3 == 0;
}

} // namespace

int main()
{
    ProcParams live;
    ProcParams snapshot;

    setGeometry(live, 2);
    setGeometry(snapshot, 7);

    live.toneCurve.expcomp = 0.75;
    snapshot.toneCurve.expcomp = -1.25;

    const ProcParams merged = makeComparisonParams(live, snapshot);

    CHECK(merged.crop == live.crop);
    CHECK(merged.cropGuide == live.cropGuide);
    CHECK(merged.coarse == live.coarse);
    CHECK(merged.commonTrans == live.commonTrans);
    CHECK(merged.rotate == live.rotate);
    CHECK(merged.distortion == live.distortion);
    CHECK(merged.perspective == live.perspective);
    CHECK(merged.lensProf == live.lensProf);
    CHECK(merged.toneCurve.expcomp == snapshot.toneCurve.expcomp);

    CHECK(geometryDiffers(live, snapshot));

    ProcParams exposureOnly = live;
    exposureOnly.toneCurve.expcomp = 2.0;
    CHECK(!geometryDiffers(live, exposureOnly));

    std::printf("snapshot compare geometry test: PASS\n");
    return 0;
}
