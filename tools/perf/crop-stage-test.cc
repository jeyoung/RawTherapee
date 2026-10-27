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
 * Focused crop/viewport stage-graph parity harness.
 *
 * For every viewport state (fit, 100%, zoomed and panned) it loads the image
 * twice, runs the full-frame preview once per copy and then the detailed crop,
 * twice through the graph, once per copy. The two detailed crop images (display and output space) are compared
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
#include <map>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
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
#include "rtengine/editorrequest.h"
#include "rtengine/editorrequest_registry.h"
#include "rtengine/rtengine.h"

namespace
{

/*
 * Typed event for one harness request. The registry owns the request; the
 * harness only needs the identity token the engine interface accepts.
 */
rtengine::ProcEvent eventFor(rtengine::RequestIntent intent)
{
    static std::map<int, rtengine::ProcEvent> events;

    auto it = events.find(static_cast<int>(intent));
    if (it != events.end()) {
        return it->second;
    }

    rtengine::ProcEvent event =
        rtengine::EditorRequestRegistry::getInstance()->newEvent();
    rtengine::EditorRequestRegistry::getInstance()->registerRequest(
        event, rtengine::make_request(intent));
    events.emplace(static_cast<int>(intent), event);
    return event;
}


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

/* Waits for full-frame preview passes. */
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
            ++ready_;
        }
        condition_.notify_all();
    }

    /* Waits until at least `target` passes have completed. */
    bool waitFor(unsigned target, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this, target] { return ready_ >= target; });
    }

    unsigned ready() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    unsigned ready_ = 0;
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
        const bool displayCaptured = capture(img, display_);
        const bool outputCaptured = capture(imgtrue, output_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window_.x = cx;
            window_.y = cy;
            window_.width = cw;
            window_.height = ch;
            window_.skip = skip;
            captured_ = displayCaptured && outputCaptured;
            ++generation_;
        }
        condition_.notify_all();
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

    /* Waits until at least `target` crops have been delivered. */
    bool waitFor(unsigned target, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this, target] {
                                       return captured_ && generation_ >= target;
                                   });
    }

    bool captured() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return captured_;
    }

    Pixels display() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return display_;
    }

    Pixels output() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return output_;
    }

    Window window() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return window_;
    }

    unsigned generation() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    void setState(CropState state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }

private:
    rtengine::StagedImageProcessor* processor_;
    CropState state_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Pixels display_;
    Pixels output_;
    Window window_;
    bool captured_ = false;
    unsigned generation_ = 0;
};

constexpr int PREVIEW_SCALE = 4;
constexpr int PREVIEW_TIMEOUT_MS = 600000;
/* A sequence step waits for one preview pass and one crop delivery. */
constexpr int UPDATE_TIMEOUT_MS = 600000;

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

/*
 * One loaded image, one processor and one detailed crop.
 *
 * The crop is created with a listener for `state`, the initial parameters are
 * applied with FIRST and the first crop is produced by fullUpdate(). A
 * sequence step then applies the next parameter set with the mask the GUI
 * would send and waits for the crop the coordinator renders in response, so
 * the incremental path under test is the real one, not a synthetic ALL pass.
 */
class Session
{
public:
    Session(const std::string& file,
            const rtengine::procparams::ProcParams& params,
            CropState state)
        : state_(state)
    {
        int errorCode = 0;
        image_ = rtengine::InitialImage::load(Glib::ustring(file), true,
                                             &errorCode);
        if (image_ == nullptr || errorCode != 0) {
            std::fprintf(stderr,
                         "crop-stage-test: failed to load the image (error %d)\n",
                         errorCode);
            if (image_ != nullptr) {
                image_->decreaseRef();
                image_ = nullptr;
            }
            return;
        }

        processor_ = rtengine::StagedImageProcessor::create(image_);
        processor_->setPreviewImageListener(&preview_);
        // A fixed monitor transform: the parity comparison must not depend on
        // the host display profile.
        processor_->setMonitorProfile(
            App::get().options().rtSettings.monitorProfile,
            rtengine::RI_RELATIVE);
        processor_->setPreviewScale(PREVIEW_SCALE);

        rtengine::procparams::ProcParams* editing =
            processor_->beginUpdateParams();
        *editing = params;
        processor_->endUpdateParams(eventFor(rtengine::RequestIntent::First));

        if (!preview_.waitFor(1, PREVIEW_TIMEOUT_MS)) {
            std::fprintf(stderr,
                         "crop-stage-test: the preview pass timed out\n");
            return;
        }

        crop_ = processor_->createCrop(nullptr, false);
        capture_.reset(new CropCapture(processor_, state));
        crop_->setListener(capture_.get());
        crop_->fullUpdate();
        ready_ = capture_->waitFor(1, UPDATE_TIMEOUT_MS);
    }

    ~Session()
    {
        if (crop_ != nullptr) {
            delete crop_;
            crop_ = nullptr;
        }
        if (processor_ != nullptr) {
            /*
             * The coordinator owns the image-source reference and releases it
             * in its destructor, so the harness must not release it again.
             */
            rtengine::StagedImageProcessor::destroy(processor_);
            processor_ = nullptr;
            image_ = nullptr;
        }
        if (image_ != nullptr) {
            image_->decreaseRef();
            image_ = nullptr;
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool ready() const
    {
        return ready_;
    }

    /*
     * Apply a new parameter set and wait for the coordinator to render it.
     *
     * `todo` is the mask the GUI would send for this edit. At 100% zoom the
     * coordinator renders the crop for every non-void mask, which is the path
     * this harness measures.
     */
    bool apply(const rtengine::procparams::ProcParams& params,
               rtengine::RequestIntent intent)
    {
        const unsigned previewTarget = preview_.ready() + 1;
        const unsigned cropTarget = capture_->generation() + 1;

        rtengine::procparams::ProcParams* editing =
            processor_->beginUpdateParams();
        *editing = params;
        processor_->endUpdateParams(eventFor(intent));

        if (!preview_.waitFor(previewTarget, UPDATE_TIMEOUT_MS)) {
            std::fprintf(stderr,
                         "crop-stage-test: the incremental preview timed out\n");
            return false;
        }
        if (!capture_->waitFor(cropTarget, UPDATE_TIMEOUT_MS)) {
            std::fprintf(stderr,
                         "crop-stage-test: the incremental crop timed out\n");
            return false;
        }
        return true;
    }

    Result result() const
    {
        Result out;
        out.ok = ready_;
        out.window = capture_->window();
        out.display = capture_->display();
        out.output = capture_->output();
        return out;
    }

    /*
     * Move the viewport to another state and repaint it, the way a zoom or a
     * pan does. The crop reallocates its buffers, so this exercises the forced
     * full path and the reallocation rather than the incremental one.
     */
    bool refresh(CropState state)
    {
        const unsigned cropTarget = capture_->generation() + 1;
        capture_->setState(state);
        crop_->fullUpdate();
        if (!capture_->waitFor(cropTarget, UPDATE_TIMEOUT_MS)) {
            std::fprintf(stderr,
                         "crop-stage-test: the geometry crop timed out\n");
            return false;
        }
        return true;
    }

    Window window() const
    {
        return capture_->window();
    }

private:
    rtengine::InitialImage* image_ = nullptr;
    rtengine::StagedImageProcessor* processor_ = nullptr;
    rtengine::DetailedCrop* crop_ = nullptr;
    std::unique_ptr<CropCapture> capture_;
    PreviewWait preview_;
    CropState state_;
    bool ready_ = false;
};

bool runCrop(const std::string& file,
             const rtengine::procparams::ProcParams& params,
             CropState state,
             Result& result)
{
    Session session(file, params, state);
    if (!session.ready()) {
        return false;
    }
    result = session.result();
    return result.ok;
}

void reportWindow(const Window& window)
{
    std::printf("window %dx%d at %d,%d skip %d", window.width, window.height,
                window.x, window.y, window.skip);
}

bool compareState(CropState state, const Result& baseline,
                  const Result& candidate, const std::string& outDir)
{
    bool ok = true;

    if (baseline.window.x != candidate.window.x
            || baseline.window.y != candidate.window.y
            || baseline.window.width != candidate.window.width
            || baseline.window.height != candidate.window.height
            || baseline.window.skip != candidate.window.skip) {
        std::fprintf(stderr,
                     "FAIL %s: the requested window differs: baseline ",
                     stateName(state));
        reportWindow(baseline.window);
        std::fprintf(stderr, ", candidate ");
        reportWindow(candidate.window);
        std::fprintf(stderr, "\n");
        ok = false;
    }

    const Difference display = compare(baseline.display, candidate.display);
    const Difference output = compare(baseline.output, candidate.output);

    if (display.differingBytes != 0 || output.differingBytes != 0) {
        std::fprintf(stderr,
                     "FAIL %s: baseline and candidate differ: display %zu "
                     "bytes (max delta %d), output %zu bytes (max delta %d)\n",
                     stateName(state), display.differingBytes, display.maxDelta,
                     output.differingBytes, output.maxDelta);
        ok = false;
    } else {
        std::printf("PASS %s: ", stateName(state));
        reportWindow(baseline.window);
        std::printf(", display %dx%d fingerprint %016llx, output %dx%d "
                    "fingerprint %016llx, AE=0\n",
                    baseline.display.width, baseline.display.height,
                    static_cast<unsigned long long>(
                        fingerprint(baseline.display)),
                    baseline.output.width, baseline.output.height,
                    static_cast<unsigned long long>(
                        fingerprint(baseline.output)));
    }

    ok = writePpm(outDir + "/" + stateName(state) + "-baseline.ppm",
                  baseline.display) && ok;
    ok = writePpm(outDir + "/" + stateName(state) + "-candidate.ppm",
                  candidate.display) && ok;
    return ok;
}

/*
 * One incremental edit: the mask the GUI sends for that edit and the parameter
 * change behind it.
 *
 * The masks are the real composites from the retired request table, not single bits: a
 * white-balance or demosaic edit carries the init family because the source has to be
 * reloaded, and a sharpening edit carries the whole Lab chain. They exercise
 * the per-block work inside the nodes; the graph plan no longer reads them.
 */
struct EditScenario {
    const char* name;
    rtengine::RequestIntent intent;
    /*
     * Extra scenarios are not part of the default sequence: they exist to
     * exercise the spot, denoise and HDR bodies on demand. `--only=<name>`
     * selects one explicitly, `--extra-states` adds all of them.
     */
    bool extra;
    void (*mutate)(rtengine::procparams::ProcParams&);
};

const std::vector<EditScenario>& editScenarios()
{
    static const std::vector<EditScenario> scenarios = {
        {"wb", rtengine::RequestIntent::WhiteBalance, false,
         [](rtengine::procparams::ProcParams& p) {
             /*
              * A manual white balance: with the automatic method the engine
              * recomputes the temperature from the image on every pass and the
              * edit would not reach the pipeline.
              */
             p.wb.enabled = true;
             p.wb.method = "Custom";
             p.wb.temperature += 800;
         }},
        {"demosaic", rtengine::RequestIntent::Demosaic, false,
         [](rtengine::procparams::ProcParams& p) {
             p.pdsharpening.enabled = true;
         }},
        {"transform", rtengine::RequestIntent::Transform, false,
         [](rtengine::procparams::ProcParams& p) { p.rotate.degree = 1.5; }},
        {"local", rtengine::RequestIntent::AutoExposure, false,
         [](rtengine::procparams::ProcParams& p) {
             p.locallab.enabled = true;
             if (p.locallab.spots.empty()) {
                 p.locallab.spots.emplace_back();
             }
             p.locallab.spots.back().struc += 10.0;
         }},
        {"sharpening", rtengine::RequestIntent::DirPyrEqualizer, false,
         [](rtengine::procparams::ProcParams& p) {
             p.sharpening.enabled = true;
             p.sharpening.amount += 100;
         }},
        {"ciecam", rtengine::RequestIntent::AutoExposure, false,
         [](rtengine::procparams::ProcParams& p) {
             p.colorappearance.enabled = true;
             p.colorappearance.modelmethod = "02";
         }},
        {"ciecam-off", rtengine::RequestIntent::AutoExposure, false,
         [](rtengine::procparams::ProcParams& p) {
             p.colorappearance.enabled = false;
         }},
        {"crop", rtengine::RequestIntent::Crop, false,
         [](rtengine::procparams::ProcParams& p) {
             p.crop.enabled = true;
             p.crop.fixratio = true;
             p.crop.ratio = "3:2";
         }},
        {"spot", rtengine::RequestIntent::SpotAdjust, true,
         [](rtengine::procparams::ProcParams& p) {
             /*
              * An in-image coordinate for the representative sample. The spot
              * body allocates the buffer and copies it back even when the
              * removal lies outside the 100% viewport, so the state exercises
              * spotApply and spotRestore on both sides of the base path.
              */
             p.spot.enabled = true;
             p.spot.entries.clear();
             rtengine::procparams::SpotEntry entry;
             entry.sourcePos = rtengine::Coord(200, 200);
             entry.targetPos = rtengine::Coord(200, 200);
             p.spot.entries.push_back(entry);
         }},
        {"denoise", rtengine::RequestIntent::AllNoRaw, true,
         [](rtengine::procparams::ProcParams& p) {
             p.dirpyrDenoise.enabled = true;
             p.dirpyrDenoise.luma += 5;
         }},
        {"fattal", rtengine::RequestIntent::Hdr, true,
         [](rtengine::procparams::ProcParams& p) {
             p.fattal.enabled = true;
             p.fattal.amount += 10;
         }},
        /*
         * Regression: a bare high-quality request (opening a detail window,
         * imagearea.cc:809) does not carry the HDR family, but the crop widens it to
         * the legacy "everything" request. It must therefore still rebuild
         * the dehazed base. The reference is a fresh full render of the same
         * parameters, so a skipped HDR body shows up as a byte difference.
         *
         * The tone curve carries a second regression: the engine derives a
         * matched curve from the raw histogram when the parameters ask for
         * matching without carrying one, and every render consumes the result.
         * The state above those parameters explicitly instead of relying on
         * the sidecar: with an unmatched curve, a pass that skips the
         * derivation renders a whole-image tone difference against the fresh
         * render, not a small residual.
         */
        {"highqual-dehaze", rtengine::RequestIntent::HighQuality, false,
         [](rtengine::procparams::ProcParams& p) {
             p.dehaze.enabled = true;
             p.dehaze.strength = 60;
             p.toneCurve.histmatching = true;
             p.toneCurve.fromHistMatching = false;
             p.toneCurve.curveMode = rtengine::procparams::ToneCurveMode::STD;
             p.toneCurve.curve.assign(1, 0.0);
             p.toneCurve.curve2.assign(1, 0.0);
         }},
    };
    return scenarios;
}

/* Compares the display and output images, not the requested window. */
Difference difference(const Result& a, const Result& b)
{
    const Difference display = compare(a.display, b.display);
    const Difference output = compare(a.output, b.output);
    Difference total;
    total.differingBytes = display.differingBytes + output.differingBytes;
    total.maxDelta = std::max(display.maxDelta, output.maxDelta);
    return total;
}

/*
 * Reports one incremental comparison.
 *
 * `candidate` is the incremental graph result and `reference` a fresh render
 * of the final state. The two must match exactly; there is no legacy execution
 * path to compare against.
 */
bool reportSequence(const std::string& label,
                    rtengine::RequestIntent intent,
                    const Result& candidate, const Result& reference,
                    const std::string& outDir)
{
    const Difference fresh = difference(candidate, reference);

    const bool geometryDiffers =
        candidate.display.width != reference.display.width
        || candidate.display.height != reference.display.height
        || candidate.output.width != reference.output.width
        || candidate.output.height != reference.output.height;

    const bool ok = fresh.differingBytes == 0 && fresh.maxDelta == 0
                    && !geometryDiffers;

    std::printf("%s seq %s: intent=%d fresh AE=%d (%zu bytes)\n",
                ok ? "PASS" : "FAIL", label.c_str(),
                static_cast<int>(intent),
                fresh.maxDelta, fresh.differingBytes);

    writePpm(outDir + "/seq-" + label + "-candidate.ppm", candidate.display);
    writePpm(outDir + "/seq-" + label + "-reference.ppm", reference.display);
    return ok;
}

bool runSequence(const std::string& file,
                 const rtengine::procparams::ProcParams& params,
                 const EditScenario& scenario,
                 const std::string& outDir)
{
    rtengine::procparams::ProcParams changed = params;
    scenario.mutate(changed);

    Session candidate(file, params, CropState::Full);
    if (!candidate.ready() || !candidate.apply(changed, scenario.intent)) {
        std::fprintf(stderr, "FAIL seq %s: the candidate session failed\n",
                     scenario.name);
        return false;
    }
    const Result incrementalDerived = candidate.result();

    Result reference;
    if (!runCrop(file, changed, CropState::Full, reference)) {
        std::fprintf(stderr, "FAIL seq %s: the reference session failed\n",
                     scenario.name);
        return false;
    }

    return reportSequence(scenario.name, scenario.intent, incrementalDerived,
                          reference, outDir);
}

/*
 * Zoom and pan inside one editor session, then compare with a fresh render at
 * the new viewport. The crop reallocates its buffers here, so the derivation
 * is forced: this is the regression check for the resize path, not for the
 * incremental one.
 */
bool runGeometrySequence(const std::string& file,
                         const rtengine::procparams::ProcParams& params,
                         const std::string& outDir)
{
    Session candidate(file, params, CropState::Full);
    if (!candidate.ready() || !candidate.refresh(CropState::Zoomed)) {
        std::fprintf(stderr,
                     "FAIL seq geometry: the candidate session failed\n");
        return false;
    }
    const Result incrementalDerived = candidate.result();

    Result reference;
    if (!runCrop(file, params, CropState::Zoomed, reference)) {
        std::fprintf(stderr,
                     "FAIL seq geometry: the reference session failed\n");
        return false;
    }

    return reportSequence("geometry", rtengine::RequestIntent::Crop, incrementalDerived, reference,
                          outDir);
}

/*
 * A forced high-quality pass over a base that local adjustments and a visible
 * spot have already written into. The change is an exposure edit, so the
 * request reaches the preview while the spot family stays unchanged; the
 * pass must refill origCrop and reapply the spots, exactly as the legacy
 * `todo = ALL` did. A forced pass that reruns the bodies on the old buffers
 * double-applies the local adjustments, and one that refills without the
 * spot work loses the removal; both show up against the fresh render.
 */
bool runForcedRefillSequence(const std::string& file,
                             const rtengine::procparams::ProcParams& params,
                             const std::string& outDir)
{
    rtengine::procparams::ProcParams configured = params;
    configured.locallab.enabled = true;
    configured.locallab.spots.emplace_back();
    configured.spot.enabled = true;
    rtengine::procparams::SpotEntry entry;
    entry.sourcePos = rtengine::Coord(1500, 1000);
    entry.targetPos = rtengine::Coord(1600, 1100);
    configured.spot.entries.push_back(entry);

    rtengine::procparams::ProcParams changed = configured;
    changed.toneCurve.expcomp = 0.5;

    Session candidate(file, configured, CropState::Full);
    if (!candidate.ready() || !candidate.apply(changed, rtengine::RequestIntent::HighQuality)) {
        std::fprintf(stderr,
                     "FAIL seq highqual-refill: the candidate session failed\n");
        return false;
    }

    Result reference;
    if (!runCrop(file, changed, CropState::Full, reference)) {
        std::fprintf(stderr,
                     "FAIL seq highqual-refill: the reference session failed\n");
        return false;
    }

    return reportSequence("highqual-refill", rtengine::RequestIntent::HighQuality, candidate.result(),
                          reference, outDir);
}

/*
 * Times a fixed number of identical-shaped incremental updates.
 *
 * The measured interval covers the whole coordinator pass (preview and crop),
 * which is identical in every mode; the difference between two runs isolates
 * the cost of the dirty-block derivation plus any extra inert stage call the
 * derived set adds. Alternating the exposure keeps a real parameter change in
 * every iteration so the derivation has work to do.
 */
bool runBenchmark(const std::string& file,
                  const rtengine::procparams::ProcParams& params,
                  int count, const std::string& edit)
{
    Session session(file, params, CropState::Full);
    if (!session.ready()) {
        std::fprintf(stderr, "FAIL bench: the session failed\n");
        return false;
    }

    constexpr int WARMUP = 2;
    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count + WARMUP; ++i) {
        rtengine::procparams::ProcParams changed = params;
        rtengine::RequestIntent intent = rtengine::RequestIntent::RgbCurve;
        if (edit == "crop") {
            changed.crop.enabled = (i % 2 == 0);
            intent = rtengine::RequestIntent::Crop;
        } else {
            changed.toneCurve.expcomp = (i % 2 == 0) ? 0.05 : -0.05;
        }

        const auto start = std::chrono::steady_clock::now();
        if (!session.apply(changed, intent)) {
            std::fprintf(stderr, "FAIL bench: update %d failed\n", i);
            return false;
        }
        const auto end = std::chrono::steady_clock::now();
        if (i >= WARMUP) {
            const double elapsed =
                std::chrono::duration<double, std::milli>(end - start).count();
            timings.push_back(elapsed);
            std::printf("TIMING %d %.3f\n", i - WARMUP, elapsed);
        }
    }

    std::sort(timings.begin(), timings.end());
    const double total =
        std::accumulate(timings.begin(), timings.end(), 0.0);
    const double mean = total / static_cast<double>(timings.size());
    const double median = timings[timings.size() / 2];
    const double p95 = timings[(timings.size() * 95) / 100];

    std::printf("bench edit=%s: %d updates, mean %.1f ms, median %.1f ms, "
                "p95 %.1f ms, min %.1f ms, max %.1f ms\n",
                edit.c_str(), count, mean, median, p95,
                timings.front(), timings.back());
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <raw> [pp3] [out-dir] [--stages] [--sequences] "
                     "[--extra-states] "
                     "[--only=<scenario>] [--bench=<n>] [--bench-edit=crop] "
                     "[--out=<dir>] [--cbdl=aft] [--no-ciecam]\n",
                     argv[0]);
        return 2;
    }

    const std::string file = argv[1];
    std::string pp3;
    std::string outDir = ".";
    bool stageCoverage = false;
    bool sequences = false;
    bool extraStates = false;
    bool afterRgb = false;
    bool ciecam = true;
    std::string onlyScenario;
    int benchCount = 0;
    std::string benchEdit = "exposure";

    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--stages") {
            stageCoverage = true;
        } else if (argument == "--sequences") {
            sequences = true;
        } else if (argument == "--extra-states") {
            extraStates = true;
        } else if (argument == "--cbdl=aft") {
            afterRgb = true;
        } else if (argument == "--no-ciecam") {
            ciecam = false;
        } else if (argument.rfind("--only=", 0) == 0) {
            onlyScenario = argument.substr(7);
        } else if (argument.rfind("--bench-edit=", 0) == 0) {
            benchEdit = argument.substr(13);
            if (benchEdit != "exposure" && benchEdit != "crop") {
                std::fprintf(stderr, "unknown benchmark edit: %s\n",
                             benchEdit.c_str());
                return 2;
            }
        } else if (argument.rfind("--bench=", 0) == 0) {
            benchCount = std::atoi(argument.substr(8).c_str());
            if (benchCount < 1) {
                std::fprintf(stderr, "invalid benchmark count\n");
                return 2;
            }
            sequences = true;
        } else if (argument.rfind("--out=", 0) == 0) {
            outDir = argument.substr(6);
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
    // between passes. The engine requires RT_SETTINGS to be absolute.
    {
        std::error_code error;
        const std::filesystem::path absolute =
            std::filesystem::absolute(outDir, error);
        if (!error) {
            outDir = absolute.string();
        }
    }
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

    if (benchCount > 0) {
        const bool benchOk =
            runBenchmark(file, params, benchCount, benchEdit);
        std::printf("crop-stage-test: %s\n", benchOk ? "PASS" : "FAIL");
        return benchOk ? 0 : 1;
    }

    const unsigned dirtyDerivationsBefore =
        rtengine::crop_graph::dirtyDerivations();

    for (CropState state : states) {
        Result baseline;
        Result candidate;

        if (!onlyScenario.empty()) {
            continue;
        }

        if (!runCrop(file, params, state, baseline)
                || !runCrop(file, params, state, candidate)) {
            return 2;
        }

        ok = compareState(state, baseline, candidate, outDir) && ok;
    }

    /*
     * A static state always starts from no history, so its derivation is
     * forced. The incremental scenarios are the ones that exercise the
     * parameter and render-state comparison.
     */
    if (sequences) {
        if (onlyScenario.empty() || onlyScenario == "geometry") {
            ok = runGeometrySequence(file, params, outDir) && ok;
        }
        if (onlyScenario.empty() || onlyScenario == "highqual-refill") {
            ok = runForcedRefillSequence(file, params, outDir) && ok;
        }
        for (const EditScenario& scenario : editScenarios()) {
            if (!onlyScenario.empty() && onlyScenario != scenario.name) {
                continue;
            }
            if (onlyScenario.empty() && scenario.extra && !extraStates) {
                continue;
            }
            ok = runSequence(file, params, scenario, outDir) && ok;
        }
    }

    // Parity is only evidence when the graph actually ran: a rejected plan
    // aborts the process.
    const unsigned graphRuns = rtengine::crop_graph::runs();
    const unsigned graphFallbacks = rtengine::crop_graph::fallbacks();
    const unsigned dirtyDerivations =
        rtengine::crop_graph::dirtyDerivations() - dirtyDerivationsBefore;
    std::printf("crop graph: %u plans executed, %u plan fallbacks, "
                "%u dirty derivations\n",
                graphRuns, graphFallbacks, dirtyDerivations);
    if (graphRuns == 0 || graphFallbacks != 0 || dirtyDerivations == 0) {
        std::fprintf(stderr,
                     "FAIL: the crop graph or dirty derivation did not run\n");
        ok = false;
    }

    std::printf("crop-stage-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
