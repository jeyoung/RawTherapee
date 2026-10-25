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
 * Focused legacy-vs-graph parity harness for the crop/viewport stage graph.
 *
 * For every viewport state (fit, 100%, zoomed and panned) it loads the image
 * twice, runs the full-frame preview once per copy and then the detailed crop,
 * with RT_STAGE_GRAPH unset on the first copy and RT_STAGE_GRAPH=1 on the
 * second. The two detailed crop images (display and output space) are compared
 * byte for byte and written as PPM so the result can be inspected.
 *
 *   crop-stage-test <raw> [pp3] [out-dir] [--stages] [--cbdl=aft]
 *
 * --stages enables a fixed, documented subset of otherwise idle tools so that
 * lab.tools, lab.sharpening, lab.wavelet, working.trc, ciecam, gamut and
 * local.adjustments all run. --cbdl=aft selects the after-RGB directional
 * pyramid position instead of the before-RGB one, and --no-ciecam leaves CIE
 * CAM off so that the before-RGB position and sharpening are reached.
 *
 * The harness never touches the thumbnail cache or the preview base cache, and
 * it keeps the settings and cache directories inside the output directory.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <glib.h>
#include <glibmm.h>
#include <giomm.h>

#include "config.h"

#include "rtengine/rtapp.h"
#include "rtgui/options.h"

#include "rtengine/colortemp.h"
#include "rtengine/dcrop.h"
#include "rtengine/image8.h"
#include "rtengine/procparams.h"
#include "rtengine/refreshmap.h"
#include "rtengine/rtengine.h"

namespace
{

/* 8-bit RGB samples, the data a DetailedCropListener receives. */
struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> data;

    bool operator==(const Pixels& other) const
    {
        return width == other.width && height == other.height
               && data == other.data;
    }
};

struct Window {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int skip = 1;
};

enum class CropState {
    Fit,
    Full,
    Zoomed
};

const char* stateName(CropState state)
{
    switch (state) {
    case CropState::Fit:
        return "fit";
    case CropState::Full:
        return "full";
    case CropState::Zoomed:
        return "zoomed";
    }
    return "unknown";
}

/* Longest preview edge used to derive the fit scale. */
constexpr int FIT_PREVIEW_EDGE = 1000;
/* Largest crop skip the GUI can ask for (zoom 100%..1000%). */
constexpr int MAX_CROP_SKIP = 10;
/* Viewport size at 100%, in image pixels. */
constexpr int FULL_VIEW_WIDTH = 960;
constexpr int FULL_VIEW_HEIGHT = 640;
/* Deliberately off-centre window for the panned state. */
constexpr int ZOOM_SKIP = 2;
constexpr int ZOOM_OFFSET_X_DIV = 4;
constexpr int ZOOM_OFFSET_Y_DIV = 5;

Window windowFor(CropState state, int fullWidth, int fullHeight)
{
    Window window;

    switch (state) {
    case CropState::Fit:
        window.x = 0;
        window.y = 0;
        window.width = fullWidth;
        window.height = fullHeight;
        window.skip = std::clamp((fullWidth + FIT_PREVIEW_EDGE - 1) / FIT_PREVIEW_EDGE,
                                 1, MAX_CROP_SKIP);
        break;
    case CropState::Full:
        window.width = std::min(FULL_VIEW_WIDTH, fullWidth);
        window.height = std::min(FULL_VIEW_HEIGHT, fullHeight);
        window.x = (fullWidth - window.width) / 2;
        window.y = (fullHeight - window.height) / 2;
        window.skip = 1;
        break;
    case CropState::Zoomed:
        window.x = fullWidth / ZOOM_OFFSET_X_DIV;
        window.y = fullHeight / ZOOM_OFFSET_Y_DIV;
        window.width = std::min(fullWidth / 2, fullWidth - window.x);
        window.height = std::min(fullHeight / 2, fullHeight - window.y);
        window.skip = ZOOM_SKIP;
        break;
    }

    return window;
}

bool capture(rtengine::IImage8* image, Pixels& out)
{
    if (image == nullptr) {
        return false;
    }

    out.width = image->getWidth();
    out.height = image->getHeight();
    if (out.width < 1 || out.height < 1) {
        return false;
    }
    out.data.assign(image->getData(),
                    image->getData()
                        + static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 3u);
    return true;
}

bool writePpm(const std::string& path, const Pixels& pixels)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", pixels.width, pixels.height);
    const bool ok = std::fwrite(pixels.data.data(), 1, pixels.data.size(), file)
                    == pixels.data.size();
    std::fclose(file);
    return ok;
}

/* FNV-1a, only for a compact log line; the pass/fail decision is a byte compare. */
std::uint64_t fingerprint(const Pixels& pixels)
{
    std::uint64_t hash = 1469598103934665603ULL;
    const std::uint64_t prime = 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(pixels.width);
    hash *= prime;
    hash ^= static_cast<std::uint64_t>(pixels.height);
    hash *= prime;
    for (unsigned char value : pixels.data) {
        hash ^= value;
        hash *= prime;
    }
    return hash;
}

struct Difference {
    size_t differingBytes = 0;
    int maxDelta = 0;
};

Difference compare(const Pixels& a, const Pixels& b)
{
    Difference difference;
    if (a.width != b.width || a.height != b.height
            || a.data.size() != b.data.size()) {
        difference.differingBytes = std::max(a.data.size(), b.data.size());
        difference.maxDelta = 255;
        return difference;
    }
    for (size_t i = 0; i < a.data.size(); ++i) {
        const int delta = std::abs(static_cast<int>(a.data[i])
                                   - static_cast<int>(b.data[i]));
        if (delta != 0) {
            ++difference.differingBytes;
        }
        difference.maxDelta = std::max(difference.maxDelta, delta);
    }
    return difference;
}

/* Waits for the first full-frame preview pass. */
class PreviewWait final : public rtengine::PreviewImageListener
{
public:
    void setImage(rtengine::IImage8*, double,
                  const rtengine::procparams::CropParams&,
                  const rtengine::procparams::CropGuideParams&) override {}

    void delImage(rtengine::IImage8*) override {}

    void imageReady(const rtengine::procparams::CropParams&,
                    const rtengine::procparams::CropGuideParams&) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = true;
        }
        condition_.notify_all();
    }

    bool wait(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this] { return ready_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool ready_ = false;
};

/* Supplies the viewport window and keeps the delivered detailed crop. */
class CropCapture final : public rtengine::DetailedCropListener
{
public:
    CropCapture(rtengine::StagedImageProcessor* processor, CropState state)
        : processor_(processor), state_(state)
    {
    }

    void setDetailedCrop(
        rtengine::IImage8* img,
        rtengine::IImage8* imgtrue,
        const rtengine::procparams::ColorManagementParams&,
        const rtengine::procparams::CropParams&,
        const rtengine::procparams::CropGuideParams&,
        int cx, int cy, int cw, int ch, int skip) override
    {
        displayCaptured_ = capture(img, display_);
        outputCaptured_ = capture(imgtrue, output_);
        window_.x = cx;
        window_.y = cy;
        window_.width = cw;
        window_.height = ch;
        window_.skip = skip;
    }

    void getWindow(int& cx, int& cy, int& cw, int& ch, int& skip) override
    {
        const Window window = windowFor(state_,
                                        processor_->getFullWidth(),
                                        processor_->getFullHeight());
        cx = window.x;
        cy = window.y;
        cw = window.width;
        ch = window.height;
        skip = window.skip;
    }

    bool captured() const
    {
        return displayCaptured_ && outputCaptured_;
    }

    const Pixels& display() const
    {
        return display_;
    }

    const Pixels& output() const
    {
        return output_;
    }

    const Window& window() const
    {
        return window_;
    }

private:
    rtengine::StagedImageProcessor* processor_;
    CropState state_;
    Pixels display_;
    Pixels output_;
    Window window_;
    bool displayCaptured_ = false;
    bool outputCaptured_ = false;
};

constexpr int PREVIEW_SCALE = 4;
constexpr int PREVIEW_TIMEOUT_MS = 600000;

struct Result {
    bool ok = false;
    Window window;
    Pixels display;
    Pixels output;
};

void enableStageCoverage(rtengine::procparams::ProcParams& params, bool afterRgb,
                         bool ciecam)
{
    // Every flag below is a plain ProcParams switch with a valid default
    // configuration; nothing needs a sidecar file or an external profile.
    params.sh.enabled = true;
    params.sh.highlights = 20;
    params.sh.shadows = 20;
    params.localContrast.enabled = true;
    params.vibrance.enabled = true;
    params.vibrance.pastels = 15;
    params.vibrance.saturated = 10;
    params.dirpyrequalizer.enabled = true;
    // A non-identity level gain, so the block does visible work in either
    // position instead of aliasing the default identity multipliers.
    params.dirpyrequalizer.mult[1] = 1.15;
    params.dirpyrequalizer.mult[3] = 1.10;
    params.dirpyrequalizer.cbdlMethod = afterRgb ? "aft" : "bef";
    params.wavelet.enabled = true;
    params.epd.enabled = true;
    params.epd.strength = 0.3;
    params.softlight.enabled = true;
    params.softlight.strength = 25;
    params.cg.enabled = true;
    // Enable denoising so the auto-noise analysis runs at 100% and the crop
    // path exercises the analysis node instead of its inert branch.
    params.dirpyrDenoise.enabled = true;
    params.colorappearance.enabled = ciecam;
    params.colorappearance.modelmethod = "02";
    params.icm.workingTRC = rtengine::procparams::ColorManagementParams::WorkingTrc::GAMMA_2_2;
    params.icm.trcExp = true;
    params.locallab.enabled = true;
    params.locallab.spots.emplace_back();
}

bool runCrop(const std::string& file,
             const rtengine::procparams::ProcParams& params,
             CropState state,
             bool graph,
             Result& result)
{
    g_setenv("RT_STAGE_GRAPH", graph ? "1" : "0", TRUE);

    int errorCode = 0;
    rtengine::InitialImage* image =
        rtengine::InitialImage::load(Glib::ustring(file), true, &errorCode);
    if (image == nullptr || errorCode != 0) {
        std::fprintf(stderr, "crop-stage-test: failed to load the image (error %d)\n",
                     errorCode);
        if (image != nullptr) {
            image->decreaseRef();
        }
        return false;
    }

    rtengine::StagedImageProcessor* processor =
        rtengine::StagedImageProcessor::create(image);

    PreviewWait preview;
    processor->setPreviewImageListener(&preview);
    // A fixed monitor transform: the parity comparison must not depend on the
    // host display profile, and both passes must use the same one.
    processor->setMonitorProfile(App::get().options().rtSettings.monitorProfile,
                                 rtengine::RI_RELATIVE);
    processor->setPreviewScale(PREVIEW_SCALE);

    rtengine::procparams::ProcParams* editing = processor->beginUpdateParams();
    *editing = params;
    processor->endUpdateParams(FIRST);

    if (!preview.wait(PREVIEW_TIMEOUT_MS)) {
        std::fprintf(stderr, "crop-stage-test: the preview pass timed out\n");
        rtengine::StagedImageProcessor::destroy(processor);
        return false;
    }

    rtengine::DetailedCrop* crop = processor->createCrop(nullptr, false);
    CropCapture capture(processor, state);
    crop->setListener(&capture);
    crop->fullUpdate();

    result.ok = capture.captured();
    result.window = capture.window();
    result.display = capture.display();
    result.output = capture.output();

    delete crop;
    rtengine::StagedImageProcessor::destroy(processor);

    if (!result.ok) {
        std::fprintf(stderr, "crop-stage-test: the crop was not delivered (graph=%d)\n",
                     graph ? 1 : 0);
    }

    return result.ok;
}

void reportWindow(const Window& window)
{
    std::printf("window %dx%d at %d,%d skip %d", window.width, window.height,
                window.x, window.y, window.skip);
}

bool compareState(CropState state, const Result& legacy, const Result& graph,
                  const std::string& outDir)
{
    bool ok = true;

    if (legacy.window.x != graph.window.x || legacy.window.y != graph.window.y
            || legacy.window.width != graph.window.width
            || legacy.window.height != graph.window.height
            || legacy.window.skip != graph.window.skip) {
        std::fprintf(stderr,
                     "FAIL %s: the requested window differs: legacy ",
                     stateName(state));
        reportWindow(legacy.window);
        std::fprintf(stderr, ", graph ");
        reportWindow(graph.window);
        std::fprintf(stderr, "\n");
        ok = false;
    }

    const Difference display = compare(legacy.display, graph.display);
    const Difference output = compare(legacy.output, graph.output);

    if (display.differingBytes != 0 || output.differingBytes != 0) {
        std::fprintf(stderr,
                     "FAIL %s: legacy and graph differ: display %zu bytes "
                     "(max delta %d), output %zu bytes (max delta %d)\n",
                     stateName(state), display.differingBytes, display.maxDelta,
                     output.differingBytes, output.maxDelta);
        ok = false;
    } else {
        std::printf("PASS %s: ", stateName(state));
        reportWindow(legacy.window);
        std::printf(", display %dx%d fingerprint %016llx, output %dx%d "
                    "fingerprint %016llx, AE=0\n",
                    legacy.display.width, legacy.display.height,
                    static_cast<unsigned long long>(fingerprint(legacy.display)),
                    legacy.output.width, legacy.output.height,
                    static_cast<unsigned long long>(fingerprint(legacy.output)));
    }

    ok = writePpm(outDir + "/" + stateName(state) + "-legacy.ppm",
                  legacy.display) && ok;
    ok = writePpm(outDir + "/" + stateName(state) + "-graph.ppm",
                  graph.display) && ok;
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <raw> [pp3] [out-dir] [--stages] [--cbdl=aft] "
                     "[--no-ciecam]\n",
                     argv[0]);
        return 2;
    }

    const std::string file = argv[1];
    std::string pp3;
    std::string outDir = ".";
    bool stageCoverage = false;
    bool afterRgb = false;
    bool ciecam = true;

    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--stages") {
            stageCoverage = true;
        } else if (argument == "--cbdl=aft") {
            afterRgb = true;
        } else if (argument == "--no-ciecam") {
            ciecam = false;
        } else if (argument.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            return 2;
        } else if (pp3.empty()) {
            pp3 = argument;
        } else {
            outDir = argument;
        }
    }

    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    Glib::init();
    Gio::init();

    // Keep the run self-contained: no settings, cache or preview-base files
    // outside the output directory, and no persistent preview base reuse
    // between the legacy and graph passes.
    const std::string settingsDir = outDir + "/settings";
    const std::string cacheDir = outDir + "/cache";
    g_setenv("RT_SETTINGS", settingsDir.c_str(), TRUE);
    g_setenv("RT_CACHE", cacheDir.c_str(), TRUE);
    g_setenv("RT_PREVIEW_BASE_CACHE", "0", TRUE);

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
    if (stageCoverage) {
        enableStageCoverage(params, afterRgb, ciecam);
    }

    const CropState states[] = {CropState::Fit, CropState::Full, CropState::Zoomed};
    bool ok = true;

    for (CropState state : states) {
        Result legacy;
        Result graph;

        if (!runCrop(file, params, state, false, legacy)
                || !runCrop(file, params, state, true, graph)) {
            return 2;
        }

        ok = compareState(state, legacy, graph, outDir) && ok;
    }

    g_unsetenv("RT_STAGE_GRAPH");

    // Parity is only evidence when the graph actually ran: a rejected plan
    // falls back to the legacy sequence and would compare legacy with itself.
    const unsigned graphRuns = rtengine::crop_graph::runs();
    const unsigned graphFallbacks = rtengine::crop_graph::fallbacks();
    const unsigned expectedRuns = sizeof(states) / sizeof(states[0]);
    std::printf("crop graph: %u plans executed, %u fallbacks (expected %u/0)\n",
                graphRuns, graphFallbacks, expectedRuns);
    if (graphRuns != expectedRuns || graphFallbacks != 0) {
        std::fprintf(stderr,
                     "FAIL: the crop graph did not execute every state\n");
        ok = false;
    }

    std::printf("crop-stage-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
