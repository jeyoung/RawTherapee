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

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "snapshot compare test failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

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

    const ProcParams base = live;

    ProcParams changed = base;
    changed.toneCurve.expcomp += 1.0;
    CHECK(!comparisonRequiresRawStage(base, changed));
    CHECK(!comparisonRequiresRawStage(changed, base));

    changed = base;
    changed.wb.method = "Custom";
    changed.wb.temperature = base.wb.temperature + 500;
    CHECK(comparisonRequiresRawStage(base, changed));
    CHECK(comparisonRequiresRawStage(changed, base));

    ProcParams customA = base;
    customA.wb.method = "Custom";
    ProcParams customB = customA;
    customB.wb.temperature += 500;
    CHECK(comparisonRequiresRawStage(customA, customB));

    customB = customA;
    customB.wb.green += 0.1;
    CHECK(comparisonRequiresRawStage(customA, customB));

    for (const char* autoMethod : {"Camera", "autold", "autitcgreen"}) {
        ProcParams autoA = base;
        autoA.wb.method = autoMethod;
        ProcParams autoB = autoA;
        autoB.wb.temperature += 500;
        autoB.wb.green += 0.1;
        CHECK(!comparisonRequiresRawStage(autoA, autoB));
    }

    changed = base;
    changed.raw.expos += 1.0;
    CHECK(comparisonRequiresRawStage(base, changed));

    changed = base;
    changed.lensProf.lcpFile += "-other";
    CHECK(comparisonRequiresRawStage(base, changed));

    changed = base;
    changed.retinex.enabled = !base.retinex.enabled;
    CHECK(comparisonRequiresRawStage(base, changed));

    changed = base;
    changed.pdsharpening.enabled = !base.pdsharpening.enabled;
    CHECK(comparisonRequiresRawStage(base, changed));

    ProcParams manualPd = base;
    manualPd.pdsharpening.enabled = true;
    manualPd.pdsharpening.autoRadius = false;
    manualPd.pdsharpening.autoContrast = false;
    manualPd.pdsharpening.deconvradius = 0.5;
    manualPd.pdsharpening.contrast = 20.0;
    changed = manualPd;
    changed.pdsharpening.deconvradius = 0.7;
    CHECK(comparisonRequiresRawStage(manualPd, changed));
    changed = manualPd;
    changed.pdsharpening.contrast = 30.0;
    CHECK(comparisonRequiresRawStage(manualPd, changed));

    ProcParams autoPd = base;
    autoPd.pdsharpening.autoRadius = true;
    autoPd.pdsharpening.autoContrast = true;
    changed = autoPd;
    changed.pdsharpening.deconvradius += 0.5;
    changed.pdsharpening.contrast += 0.5;
    CHECK(!comparisonRequiresRawStage(autoPd, changed));

    changed = autoPd;
    changed.pdsharpening.deconvradiusOffset += 0.1;
    CHECK(comparisonRequiresRawStage(autoPd, changed));

    changed = base;
    changed.toneCurve.hrenabled = !base.toneCurve.hrenabled;
    CHECK(comparisonRequiresRawStage(base, changed));

    changed = base;
    changed.filmNegative.enabled = !base.filmNegative.enabled;
    CHECK(!comparisonRequiresRawStage(base, changed));

    changed = base;
    changed.icm.outputProfile += "-other";
    CHECK(!comparisonRequiresRawStage(base, changed));

    changed = base;
    changed.icm.inputProfile += "-other";
    CHECK(!comparisonRequiresRawStage(base, changed));

    ProcParams noPaths = base;
    noPaths.raw.df_autoselect = false;
    noPaths.raw.dark_frame.clear();
    noPaths.raw.ff_AutoSelect = false;
    noPaths.raw.ff_file.clear();
    changed = noPaths;
    changed.raw.dark_frame = "/nonexistent/rt-dark.dng";
    changed.raw.ff_file = "/nonexistent/rt-flat.dng";
    CHECK(!comparisonRequiresRawStage(noPaths, changed));

    ProcParams relativeA = noPaths;
    relativeA.raw.dark_frame = "relative-dark.dng";
    relativeA.raw.ff_file = "relative-flat.dng";
    ProcParams relativeB = relativeA;
    relativeB.raw.dark_frame = "other-dark.dng";
    CHECK(comparisonRequiresRawStage(relativeA, relativeB));
    CHECK(comparisonRequiresRawStage(noPaths, relativeA));

    CHECK(!comparisonRequiresRawStage(base, base));

    std::printf("snapshot compare geometry test: PASS\n");
    return 0;
}
