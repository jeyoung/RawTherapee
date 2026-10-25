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

/*
 * Focused parity harness for the opt-in thumbnail stage graph.
 *
 * It generates the same thumbnail twice through the graph, keeps the 8-bit
 * RGB output of
 * both in memory, compares it byte for byte and writes a PPM per pass so the
 * result can be inspected and hashed.
 *
 *   thumbnail-stage-test <raw> [pp3] [height] [out-dir]
 *
 * Both the embedded-JPEG quick path and the raw fallback are covered. The
 * harness never touches the thumbnail cache.
 */

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <glib.h>
#include <giomm.h>
#include <glibmm.h>

#include "config.h"

#include "rtengine/rtapp.h"
#include "rtgui/options.h"

#include "rtengine/colortemp.h"
#include "rtengine/image8.h"
#include "rtengine/procparams.h"
#include "rtengine/rtengine.h"
#include "rtengine/rtthumbnail.h"

namespace
{

struct Pixels {
    int width = 0;
    int height = 0;
    std::string data;
};

bool capture(rtengine::IImage8* image, Pixels& out)
{
    if (!image) {
        return false;
    }

    out.width = image->getWidth();
    out.height = image->getHeight();
    out.data.assign(reinterpret_cast<const char*>(image->getData()),
                    static_cast<size_t>(out.width) * size_t(out.height) * 3u);
    return out.width > 0 && out.height > 0;
}

bool same(const Pixels& a, const Pixels& b)
{
    return a.width == b.width && a.height == b.height && a.data == b.data;
}

bool writePpm(const std::string& path, const Pixels& pixels)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", pixels.width, pixels.height);
    const bool ok = std::fwrite(pixels.data.data(), 1, pixels.data.size(), file)
                    == pixels.data.size();
    std::fclose(file);
    return ok;
}

void setGraph(bool enabled)
{
}

bool runEmbedded(const std::string& file, const rtengine::procparams::ProcParams& params,
                 int height, bool graph, Pixels& pixels)
{
    setGraph(graph);

    rtengine::eSensorType sensorType = rtengine::ST_NONE;
    int w = 0;
    int h = height;
    std::unique_ptr<rtengine::Thumbnail> thumb(
        rtengine::Thumbnail::loadQuickFromRaw(file, sensorType, w, h, 1, TRUE));
    if (!thumb) {
        std::fprintf(stderr, "embedded: loadQuickFromRaw failed (graph=%d)\n", graph);
        return false;
    }

    std::unique_ptr<rtengine::IImage8> image(
        thumb->quickProcessImage(params, height, rtengine::TI_Nearest));
    return capture(image.get(), pixels);
}

bool runRaw(const std::string& file, const rtengine::procparams::ProcParams& params,
            const rtengine::FramesMetaData* metadata, int height, bool graph,
            Pixels& pixels)
{
    setGraph(graph);

    rtengine::eSensorType sensorType = rtengine::ST_NONE;
    int w = 0;
    int h = height;
    std::unique_ptr<rtengine::Thumbnail> thumb(
        rtengine::Thumbnail::loadFromRaw(file, sensorType, w, h, 1, params.wb.equal,
                                         params.wb.observer, TRUE, &params.raw));
    if (!thumb) {
        std::fprintf(stderr, "raw: loadFromRaw failed (graph=%d)\n", graph);
        return false;
    }

    double scale = 0.0;
    std::unique_ptr<rtengine::IImage8> image(
        thumb->processImage(params, sensorType, height, rtengine::TI_Bilinear,
                            metadata, scale, true));
    return capture(image.get(), pixels);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <raw> [pp3] [height] [out-dir]\n", argv[0]);
        return 2;
    }

    const std::string file = argv[1];
    const std::string pp3 = argc > 2 ? argv[2] : "";
    const int height = argc > 3 ? std::atoi(argv[3]) : 160;
    const std::string outDir = argc > 4 ? argv[4] : ".";
    if (height < 8 || height > 2048) {
        std::fprintf(stderr, "height out of range: %d\n", height);
        return 2;
    }

    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    Glib::init();
    Gio::init();

    try {
        App::get().setArgv0(DATA_SEARCH_PATH);
        Options::load(true);
    } catch (const Options::Error& error) {
        std::fprintf(stderr, "options: %s\n", error.what());
        return 2;
    }

    rtengine::procparams::ProcParams params;
    if (!pp3.empty() && params.load(pp3) != 0) {
        std::fprintf(stderr, "failed to load %s\n", pp3.c_str());
        return 2;
    }

    std::unique_ptr<rtengine::FramesMetaData> metadata(
        rtengine::FramesMetaData::fromFile(file));
    if (!metadata) {
        std::fprintf(stderr, "failed to read metadata of %s\n", file.c_str());
        return 2;
    }

    bool ok = true;

    Pixels embeddedLegacy;
    Pixels embeddedGraph;
    if (!runEmbedded(file, params, height, false, embeddedLegacy)
            || !runEmbedded(file, params, height, true, embeddedGraph)) {
        return 2;
    }
    if (!same(embeddedLegacy, embeddedGraph)) {
        std::fprintf(stderr, "FAIL embedded: legacy and graph differ\n");
        ok = false;
    } else {
        std::printf("PASS embedded: %dx%d, %zu bytes\n", embeddedLegacy.width,
                    embeddedLegacy.height, embeddedLegacy.data.size());
    }
    ok = writePpm(outDir + "/embedded-legacy.ppm", embeddedLegacy) && ok;
    ok = writePpm(outDir + "/embedded-graph.ppm", embeddedGraph) && ok;

    Pixels rawLegacy;
    Pixels rawGraph;
    if (!runRaw(file, params, metadata.get(), height, false, rawLegacy)
            || !runRaw(file, params, metadata.get(), height, true, rawGraph)) {
        return 2;
    }
    if (!same(rawLegacy, rawGraph)) {
        std::fprintf(stderr, "FAIL raw: legacy and graph differ\n");
        ok = false;
    } else {
        std::printf("PASS raw: %dx%d, %zu bytes\n", rawLegacy.width,
                    rawLegacy.height, rawLegacy.data.size());
    }
    ok = writePpm(outDir + "/raw-legacy.ppm", rawLegacy) && ok;
    ok = writePpm(outDir + "/raw-graph.ppm", rawGraph) && ok;


    std::printf("thumbnail-stage-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
