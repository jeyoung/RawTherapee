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
 * Focused parity harness for the thumbnail stage graph.
 *
 * For a raw input it generates the same thumbnail twice through the graph,
 * keeps the 8-bit RGB output of both in memory, compares it byte for byte and
 * writes a PPM per pass so the result can be inspected and hashed. Both the
 * embedded-JPEG quick path and the raw fallback are covered.
 *
 * For a non-raw input (a raster extension) it covers the `loadFromImage()`
 * path instead: the Inspector load is checked first, because it must return an
 * 8-bit image without entering `processImage()`, then `processImage()` is run
 * twice and compared, and its graph line must name every node of the full
 * thumbnail chain.
 *
 *   thumbnail-stage-test <raw-or-raster> [pp3] [height] [out-dir]
 *
 * The harness never touches the thumbnail cache.
 */

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <unistd.h>

#include <glib.h>
#include <giomm.h>
#include <glibmm.h>

#include "config.h"

#include "rtengine/rtapp.h"
#include "rtgui/options.h"

#include "rtengine/colortemp.h"
#include "rtengine/formats.h"
#include "rtengine/image8.h"
#include "rtengine/procparams.h"
#include "rtengine/rtengine.h"
#include "rtengine/rtthumbnail.h"
#include "rtengine/stdimagesource.h"
#include "rtengine/utils.h"

namespace
{

struct Pixels {
    int width = 0;
    int height = 0;
    std::string data;
};

/*
 * Capture what a block writes to stderr, so the harness can inspect the
 * thumbnail graph lines without relying on the caller's redirection.
 */
class StderrCapture
{
public:
    StderrCapture()
    {
        std::fflush(stderr);
        savedFd_ = dup(fileno(stderr));
        capture_ = std::tmpfile();
        if (savedFd_ < 0 || !capture_) {
            std::fprintf(stderr, "capture: cannot redirect stderr\n");
            if (savedFd_ >= 0) {
                close(savedFd_);
                savedFd_ = -1;
            }
            if (capture_) {
                std::fclose(capture_);
                capture_ = nullptr;
            }
            return;
        }
        if (dup2(fileno(capture_), fileno(stderr)) < 0) {
            std::fprintf(stderr, "capture: cannot redirect stderr\n");
            close(savedFd_);
            savedFd_ = -1;
            std::fclose(capture_);
            capture_ = nullptr;
            return;
        }
        active_ = true;
    }

    ~StderrCapture()
    {
        finish();
    }

    std::string finish()
    {
        std::string text;
        if (!capture_) {
            return text;
        }

        std::fflush(stderr);
        if (active_) {
            dup2(savedFd_, fileno(stderr));
            active_ = false;
        }
        if (savedFd_ >= 0) {
            close(savedFd_);
            savedFd_ = -1;
        }

        std::rewind(capture_);
        char buffer[4096];
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof buffer, capture_)) > 0) {
            text.append(buffer, count);
        }
        std::fclose(capture_);
        capture_ = nullptr;
        return text;
    }

private:
    int savedFd_ = -1;
    FILE* capture_ = nullptr;
    bool active_ = false;
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

bool runEmbedded(const std::string& file, const rtengine::procparams::ProcParams& params,
                 int height, Pixels& pixels)
{
    rtengine::eSensorType sensorType = rtengine::ST_NONE;
    int w = 0;
    int h = height;
    std::unique_ptr<rtengine::Thumbnail> thumb(
        rtengine::Thumbnail::loadQuickFromRaw(file, sensorType, w, h, 1, TRUE));
    if (!thumb) {
        std::fprintf(stderr, "embedded: loadQuickFromRaw failed\n");
        return false;
    }

    std::unique_ptr<rtengine::IImage8> image(
        thumb->quickProcessImage(params, height, rtengine::TI_Nearest));
    return capture(image.get(), pixels);
}

bool runRaw(const std::string& file, const rtengine::procparams::ProcParams& params,
            const rtengine::FramesMetaData* metadata, int height, Pixels& pixels)
{
    rtengine::eSensorType sensorType = rtengine::ST_NONE;
    int w = 0;
    int h = height;
    std::unique_ptr<rtengine::Thumbnail> thumb(
        rtengine::Thumbnail::loadFromRaw(file, sensorType, w, h, 1, params.wb.equal,
                                         params.wb.observer, TRUE, &params.raw));
    if (!thumb) {
        std::fprintf(stderr, "raw: loadFromRaw failed\n");
        return false;
    }

    double scale = 0.0;
    std::unique_ptr<rtengine::IImage8> image(
        thumb->processImage(params, sensorType, height, rtengine::TI_Bilinear,
                            metadata, scale, true));
    return capture(image.get(), pixels);
}

/*
 * The non-raw path: the GUI loads a reduced thumbnail with loadFromImage() and
 * then calls processImage() with the cache metadata. The metadata is read from
 * the file itself here, which is the same FramesMetaData interface.
 */
bool runImage(const std::string& file, const rtengine::procparams::ProcParams& params,
              const rtengine::FramesMetaData* metadata, int height, Pixels& pixels)
{
    int w = 0;
    int h = height;
    std::unique_ptr<rtengine::Thumbnail> thumb(
        rtengine::Thumbnail::loadFromImage(file, w, h, 1, params.wb.equal,
                                           params.wb.observer));
    if (!thumb) {
        std::fprintf(stderr, "image: loadFromImage failed\n");
        return false;
    }

    double scale = 0.0;
    std::unique_ptr<rtengine::IImage8> image(
        thumb->processImage(params, rtengine::ST_NONE, height,
                            rtengine::TI_Bilinear, metadata, scale, true));
    return capture(image.get(), pixels);
}

/*
 * Inspector mode is a decode plus an Image8 resize: loadFromImage() with
 * inspectorMode=true must return a full-size Image8 and must not enter the
 * profile pipeline, so no processImage() graph line may appear.
 */
bool runInspector(const std::string& file, std::string& log)
{
    int fullW = 0;
    int fullH = 0;
    {
        rtengine::StdImageSource source;
        if (source.load(file) != 0) {
            std::fprintf(stderr, "inspector: StdImageSource failed\n");
            return false;
        }
        source.getFullSize(fullW, fullH);
    }

    int w = 0;
    int h = 0;
    unsigned char* data = nullptr;
    double scale = 0.0;

    {
        StderrCapture capture;
        std::unique_ptr<rtengine::Thumbnail> thumb(
            rtengine::Thumbnail::loadFromImage(file, w, h, 1, 1.0,
                                               rtengine::ColorTemp::DEFAULT_OBSERVER,
                                               true));
        if (thumb) {
            int dimW = 0;
            int dimH = 0;
            thumb->getDimensions(dimW, dimH, scale);
            data = thumb->getImage8Data();
            if (dimW != w || dimH != h) {
                std::fprintf(stderr,
                             "inspector: dimensions %dx%d do not match %dx%d\n",
                             dimW, dimH, w, h);
                w = 0;
                h = 0;
            }
        }
        log = capture.finish();
    }
    // Keep the diagnostic lines visible to the caller's log.
    std::fputs(log.c_str(), stderr);

    if (!data) {
        std::fprintf(stderr, "inspector: loadFromImage returned no Image8\n");
        return false;
    }
    if (w != fullW || h != fullH) {
        std::fprintf(stderr,
                     "inspector: %dx%d is not the full image %dx%d\n",
                     w, h, fullW, fullH);
        return false;
    }
    if (scale != 1.0) {
        std::fprintf(stderr, "inspector: scale is %f, expected 1.0\n", scale);
        return false;
    }
    if (log.find("thumbnail-graph: processImage") != std::string::npos) {
        std::fprintf(stderr,
                     "inspector: processImage ran, but Inspector mode has no "
                     "profile pipeline\n");
        return false;
    }

    std::printf("PASS inspector: %dx%d Image8, no processImage graph line\n",
                w, h);
    return true;
}

bool hasProcessImageNodes(const std::string& log)
{
    static const char* const nodes[] = {
        "thumbnail.scale", "thumbnail.transform", "thumbnail.whitebalance",
        "thumbnail.rgb", "thumbnail.lab", "thumbnail.output"};
    const std::string prefix = "thumbnail-graph: processImage: active,";
    const size_t start = log.find(prefix);
    if (start == std::string::npos) {
        std::fprintf(stderr, "image: no processImage graph line\n");
        return false;
    }
    const size_t end = log.find('\n', start);
    const std::string line = log.substr(start, end - start);

    for (const char* node : nodes) {
        if (line.find(node) == std::string::npos) {
            std::fprintf(stderr, "image: graph line misses %s: %s\n", node,
                         line.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <raw-or-raster> [pp3] [height] [out-dir]\n",
                     argv[0]);
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

    if (rtengine::isRasterExtension(rtengine::getFileExtension(file))) {
        std::string inspectorLog;
        if (!runInspector(file, inspectorLog)) {
            return 2;
        }

        Pixels imageFirst;
        Pixels imageSecond;
        std::string imageLog;
        {
            StderrCapture capture;
            if (!runImage(file, params, metadata.get(), height, imageFirst)) {
                return 2;
            }
            imageLog = capture.finish();
        }
        // Keep the graph line visible to the caller's log.
        std::fputs(imageLog.c_str(), stderr);
        if (!runImage(file, params, metadata.get(), height, imageSecond)) {
            return 2;
        }

        if (!same(imageFirst, imageSecond)) {
            std::fprintf(stderr,
                         "FAIL image: the two graph passes differ\n");
            ok = false;
        } else if (!hasProcessImageNodes(imageLog)) {
            ok = false;
        } else {
            std::printf("PASS image: %dx%d, %zu bytes, six nodes\n",
                        imageFirst.width, imageFirst.height,
                        imageFirst.data.size());
        }
        ok = writePpm(outDir + "/image-first.ppm", imageFirst) && ok;
        ok = writePpm(outDir + "/image-second.ppm", imageSecond) && ok;
    } else {
        Pixels embeddedLegacy;
        Pixels embeddedGraph;
        if (!runEmbedded(file, params, height, embeddedLegacy)
                || !runEmbedded(file, params, height, embeddedGraph)) {
            return 2;
        }
        if (!same(embeddedLegacy, embeddedGraph)) {
            std::fprintf(stderr, "FAIL embedded: the two graph passes differ\n");
            ok = false;
        } else {
            std::printf("PASS embedded: %dx%d, %zu bytes\n",
                        embeddedLegacy.width, embeddedLegacy.height,
                        embeddedLegacy.data.size());
        }
        ok = writePpm(outDir + "/embedded-legacy.ppm", embeddedLegacy) && ok;
        ok = writePpm(outDir + "/embedded-graph.ppm", embeddedGraph) && ok;

        Pixels rawLegacy;
        Pixels rawGraph;
        if (!runRaw(file, params, metadata.get(), height, rawLegacy)
                || !runRaw(file, params, metadata.get(), height, rawGraph)) {
            return 2;
        }
        if (!same(rawLegacy, rawGraph)) {
            std::fprintf(stderr, "FAIL raw: the two graph passes differ\n");
            ok = false;
        } else {
            std::printf("PASS raw: %dx%d, %zu bytes\n", rawLegacy.width,
                        rawLegacy.height, rawLegacy.data.size());
        }
        ok = writePpm(outDir + "/raw-legacy.ppm", rawLegacy) && ok;
        ok = writePpm(outDir + "/raw-graph.ppm", rawGraph) && ok;
    }

    std::printf("thumbnail-stage-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
