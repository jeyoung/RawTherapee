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
 * Deterministic sweep of the preview raw chain through the real
 * ImProcCoordinator.
 *
 * The session loads one image, applies an initial parameter set and then one
 * edit per state through the real StagedImageProcessor API, waiting for the
 * full-frame pass to settle after each edit. Every state therefore has a
 * usable history; the incremental path is the one under test.
 *
 *   preview-work-order-sweep <raw> <out-dir> [--timing=N [--clamp]]
 *   preview-work-order-sweep <raw> <out-dir> --fast-path [--timing=N]
 *   preview-work-order-sweep --print-switch
 *
 * Parity mode writes a private RT_PREVIEW_DUMP directory, waits for each pass
 * and copies preview.tif, work.tif and preview.json to
 * <out-dir>/<NN>-<state>/. Run the driver twice, once with the typed order
 * disabled (RT_WORK_ORDER=off) and once with it enabled (the default), then
 * compare the copies pixel-exactly.
 *
 * `--fast-path` runs the eligible sequence instead: every state is an edit the
 * viewport fast path can serve, and the harness leaves RT_VIEWPORT_FAST at its
 * default instead of forcing the full render. It prints `VIEWPORT enabled=...`
 * so the driver can prove the mode, and the driver compares each state against
 * the same state rendered with the fast path off.
 *
 * Timing mode does not dump. It loads the same image with the same profile
 * and preview scale, performs the same initial pass and then times `N` plain
 * exposure edits, printing one `TIMING <index> <milliseconds>` line each.
 * `--clamp` times toggles of the Clamp-OOG checkbox instead, using the
 * DARKFRAME action the editor sends.
 *
 * The harness prints `SWITCH work-order=<0|1> shadow=<0|1>` before the first
 * pass, so the driver can prove the kill switch. It never touches the
 * thumbnail cache or the persistent preview base cache, and it keeps the
 * settings and cache directories inside the output directory.
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <giomm.h>
#include <glib.h>
#include <glibmm.h>

#include "config.h"

#include "rtengine/rtapp.h"
#include "rtgui/options.h"

#include "rtengine/image8.h"
#include "rtengine/procparams.h"
#include "rtengine/editorrequest.h"
#include "rtengine/editorrequest_registry.h"
#include "rtengine/rtengine.h"
#include "rtengine/stage/stage_work.h"

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


/* Longest preview edge is chosen by the coordinator; this is its skip. */
constexpr int PREVIEW_SCALE = 4;
/* Generous because a 45 MP class image may be used with the same harness. */
constexpr int PASS_TIMEOUT_MS = 900000;
/* Settle window after imageReady, to catch a second queued pass. */
constexpr int SETTLE_QUIET_MS = 250;
/* Exposure step for the control edit and the timing samples. */
constexpr double EXPOSURE_STEP = 0.7;
/* Spot location used by the sweep; well inside any tested image. */
constexpr int SPOT_X = 200;
constexpr int SPOT_Y = 200;

/* Waits for completed full-frame preview passes. */
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

    bool waitFor(unsigned target, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this, target] { return ready_ >= target; });
    }

    /* After a pass has landed, require the generation to stop advancing. */
    bool waitSettled(unsigned target, int quietMs)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(quietMs);
        std::unique_lock<std::mutex> lock(mutex_);
        unsigned last = ready_;
        while (std::chrono::steady_clock::now() < deadline) {
            condition_.wait_for(lock, std::chrono::milliseconds(quietMs));
            if (ready_ == last) {
                return ready_ == target;
            }
            last = ready_;
        }
        return ready_ == target;
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

std::string joinPath(const std::string& a, const std::string& b)
{
    return (std::filesystem::path(a) / b).string();
}

/*
 * Minimal visible crop.
 *
 * The viewport fast path only renders crops that have a listener, so a
 * headless fast-path check must own one. The image itself is not consumed
 * here: Crop::update writes its viewport output through the preview dump
 * hook (`crop-main.tif`), which is the artefact the driver compares.
 */
class CropCapture final : public rtengine::DetailedCropListener
{
public:
    void setDetailedCrop(rtengine::IImage8*, rtengine::IImage8*,
                         const rtengine::procparams::ColorManagementParams&,
                         const rtengine::procparams::CropParams&,
                         const rtengine::procparams::CropGuideParams&,
                         int, int, int, int, int) override
    {
    }

    void getWindow(int& cx, int& cy, int& cw, int& ch, int& skip) override
    {
        cx = x_;
        cy = y_;
        cw = width_;
        ch = height_;
        skip = skip_;
    }

    void setWindow(int x, int y, int width, int height, int skip)
    {
        x_ = x;
        y_ = y;
        width_ = width;
        height_ = height;
        skip_ = skip;
    }

private:
    int x_ = 64;
    int y_ = 64;
    int width_ = 384;
    int height_ = 256;
    int skip_ = 4;
};

/*
 * The fit-zoom viewport: the whole image at the preview skip. The crop window
 * is in full-image coordinates with its own skip (Crop::setCropSizes), so the
 * immediate crop cost is what a user waits for at fit zoom.
 */
constexpr int CROP_X = 0;
constexpr int CROP_Y = 0;

bool copyDump(const std::string& dumpDir, const std::string& outDir,
              const std::string& name, unsigned pass)
{
    const std::string target = joinPath(outDir, name);
    std::error_code error;
    std::filesystem::create_directories(target, error);
    if (error) {
        std::fprintf(stderr, "sweep: cannot create %s: %s\n", target.c_str(),
                     error.message().c_str());
        return false;
    }

    /* The dump records its own pass counter; a stale file must fail loudly. */
    std::ifstream json(joinPath(dumpDir, "preview.json"));
    unsigned recorded = 0;
    std::string line;
    while (std::getline(json, line)) {
        const size_t key = line.find("\"pass\":");
        if (key == std::string::npos) {
            continue;
        }
        const char* value = line.c_str() + key + std::strlen("\"pass\":");
        recorded = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
    }
    if (recorded != pass) {
        std::fprintf(stderr,
                     "sweep: stale dump for %s: pass %u, expected %u\n",
                     name.c_str(), recorded, pass);
        return false;
    }

    for (const char* file : {"preview.tif", "work.tif", "preview.json"}) {
        std::filesystem::copy_file(
            joinPath(dumpDir, file), joinPath(target, file),
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            std::fprintf(stderr, "sweep: cannot copy %s: %s\n", file,
                         error.message().c_str());
            return false;
        }
    }

    /*
     * The visible crop is dumped only when the pass rendered one, which the
     * fast-path check requires. The descriptor has no pass counter, so the
     * file is whatever the last crop render of the settled state produced.
     */
    const std::string cropSource = joinPath(dumpDir, "crop-main.tif");
    if (std::filesystem::exists(cropSource)) {
        std::filesystem::copy_file(
            cropSource, joinPath(target, "crop-main.tif"),
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            std::fprintf(stderr, "sweep: cannot copy crop-main.tif: %s\n",
                         error.message().c_str());
            return false;
        }
    }

    return true;
}

class Session
{
public:
    Session() = default;

    ~Session()
    {
        if (crop_ != nullptr) {
            crop_->destroy();
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

    bool open(const std::string& file,
              const rtengine::procparams::ProcParams& params,
              bool withCrop)
    {
        int errorCode = 0;
        image_ = rtengine::InitialImage::load(Glib::ustring(file), true,
                                             &errorCode);
        if (image_ == nullptr || errorCode != 0) {
            std::fprintf(stderr,
                         "sweep: failed to load the image (error %d)\n",
                         errorCode);
            if (image_ != nullptr) {
                image_->decreaseRef();
                image_ = nullptr;
            }
            return false;
        }

        processor_ = rtengine::StagedImageProcessor::create(image_);
        if (processor_ == nullptr) {
            std::fprintf(stderr, "sweep: cannot create the processor\n");
            return false;
        }

        processor_->setPreviewImageListener(&preview_);
        processor_->setMonitorProfile(
            App::get().options().rtSettings.monitorProfile,
            rtengine::RI_RELATIVE);
        processor_->setPreviewScale(PREVIEW_SCALE);

        rtengine::procparams::ProcParams* editing =
            processor_->beginUpdateParams();
        *editing = params;
        processor_->endUpdateParams(eventFor(rtengine::RequestIntent::First));

        if (!preview_.waitFor(1, PASS_TIMEOUT_MS)) {
            std::fprintf(stderr, "sweep: the initial preview timed out\n");
            return false;
        }
        if (!preview_.waitSettled(1, SETTLE_QUIET_MS)) {
            std::fprintf(stderr, "sweep: the initial preview did not settle\n");
            return false;
        }

        if (withCrop) {
            /*
             * A visible crop is what the viewport fast path exists for. It
             * must exist before the eligible edits, so every hit has a
             * listener to repaint and the dump has a crop to compare.
             */
            crop_ = processor_->createCrop(nullptr, false);
            if (crop_ == nullptr) {
                std::fprintf(stderr, "sweep: cannot create the crop window\n");
                return false;
            }
            const int fullWidth = processor_->getFullWidth();
            const int fullHeight = processor_->getFullHeight();
            assert(fullWidth > 0 && fullHeight > 0);
            cropCapture_.setWindow(CROP_X, CROP_Y, fullWidth, fullHeight,
                                   PREVIEW_SCALE);
            crop_->setListener(&cropCapture_);
            crop_->setWindow(CROP_X, CROP_Y, fullWidth, fullHeight,
                             PREVIEW_SCALE);
            crop_->fullUpdate();
        }
        return true;
    }

    /*
     * Apply one parameter set with the mask the editor sends, wait for the
     * incremental pass to land, read the normalised parameters back and
     * return the elapsed wall time.
     */
    bool apply(const rtengine::procparams::ProcParams& params,
               rtengine::RequestIntent intent,
               rtengine::procparams::ProcParams& normalised, double& elapsedMs,
               bool readBack = true)
    {
        const unsigned target = preview_.ready() + 1;
        const auto start = std::chrono::steady_clock::now();

        rtengine::procparams::ProcParams* editing =
            processor_->beginUpdateParams();
        *editing = params;
        processor_->endUpdateParams(eventFor(intent));

        if (!preview_.waitFor(target, PASS_TIMEOUT_MS)) {
            std::fprintf(stderr, "sweep: the incremental preview timed out\n");
            return false;
        }
        if (!preview_.waitSettled(target, SETTLE_QUIET_MS)) {
            std::fprintf(stderr,
                         "sweep: the incremental preview did not settle\n");
            return false;
        }

        elapsedMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
        if (readBack) {
            processor_->getParams(&normalised, false);
        } else {
            /*
             * Keep the caller's parameter set, as the editor does. The engine
             * normalises deterministic values (notably wb.green) in its own
             * copy, and a request built from that copy would differ from the
             * pre-pass base snapshot and fall back with `white-balance`.
             */
            normalised = params;
        }
        return true;
    }

    /* Drive the same 100% crossing the main preview reports to the engine. */
    bool zoom(bool full, double& elapsedMs)
    {
        const unsigned target = preview_.ready() + 1;
        const auto start = std::chrono::steady_clock::now();

        processor_->setMainPreviewFullResolution(full);

        if (!preview_.waitFor(target, PASS_TIMEOUT_MS)) {
            std::fprintf(stderr,
                         "sweep: the high-detail crossing did not render\n");
            return false;
        }
        if (!preview_.waitSettled(target, SETTLE_QUIET_MS)) {
            std::fprintf(stderr,
                         "sweep: the high-detail crossing did not settle\n");
            return false;
        }

        elapsedMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
        return true;
    }

    unsigned ready() const
    {
        return preview_.ready();
    }

    /* The engine's live parameters, after the pass normalised them. */
    void params(rtengine::procparams::ProcParams& out) const
    {
        processor_->getParams(&out, false);
    }

private:
    rtengine::InitialImage* image_ = nullptr;
    rtengine::StagedImageProcessor* processor_ = nullptr;
    rtengine::DetailedCrop* crop_ = nullptr;
    CropCapture cropCapture_;
    PreviewWait preview_;
};

struct State
{
    std::string name;
    rtengine::RequestIntent intent;
    void (*mutate)(rtengine::procparams::ProcParams&);
};

State spotAdd()
{
    return {"spot-add", rtengine::RequestIntent::SpotAdjust,
            [](rtengine::procparams::ProcParams& params) {
                params.spot.enabled = true;
                params.spot.entries.clear();
                rtengine::procparams::SpotEntry entry;
                entry.sourcePos = rtengine::Coord(SPOT_X, SPOT_Y);
                entry.targetPos = rtengine::Coord(SPOT_X, SPOT_Y);
                params.spot.entries.push_back(entry);
            }};
}

State spotRemove()
{
    return {"spot-remove", rtengine::RequestIntent::SpotAdjust,
            [](rtengine::procparams::ProcParams& params) {
                params.spot.enabled = false;
                params.spot.entries.clear();
            }};
}

State retinexOn()
{
    return {"retinex-on", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.retinex.enabled = true;
            }};
}

/*
 * A retinex buffer input edited while retinex is already enabled. The
 * preparation inside the demosaic body chooses the buffer layout from the
 * colour space and writes its forward gamma into the planes, so the body must
 * re-run; toggling only `enabled` does not exercise that path.
 */
State retinexColorspace()
{
    return {"retinex-colorspace", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.retinex.retinexcolorspace = "HSLLOG";
            }};
}

State retinexGamma()
{
    return {"retinex-gamma", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.retinex.gammaretinex = "low";
            }};
}

State workProfileRetinexOn()
{
    return {"workprofile-retinex-on", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.icm.workingProfile = "ProPhoto";
            }};
}

State retinexOff()
{
    return {"retinex-off", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.retinex.enabled = false;
            }};
}

State workProfileRetinexOff()
{
    return {"workprofile-retinex-off", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.icm.workingProfile = "Rec2020";
            }};
}

State filmNegativeOn()
{
    return {"filmneg-on", rtengine::RequestIntent::AllNoRaw,
            [](rtengine::procparams::ProcParams& params) {
                params.filmNegative.enabled = true;
            }};
}

State filmNegativeOff()
{
    return {"filmneg-off", rtengine::RequestIntent::AllNoRaw,
            [](rtengine::procparams::ProcParams& params) {
                params.filmNegative.enabled = false;
            }};
}

State captureSharpeningOn()
{
    return {"capsharp-on", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.pdsharpening.enabled = true;
                params.pdsharpening.contrast += 0.1;
            }};
}

State captureSharpeningOff()
{
    return {"capsharp-off", rtengine::RequestIntent::Demosaic,
            [](rtengine::procparams::ProcParams& params) {
                params.pdsharpening.enabled = false;
                params.pdsharpening.contrast -= 0.1;
            }};
}

/* The fit state is the same before and after the 100% excursion. */
struct ZoomState
{
    const char* name;
    bool fullResolution;
};

const State exposureState()
{
    return {"exposure", rtengine::RequestIntent::AutoExposure,
            [](rtengine::procparams::ProcParams& params) {
                params.toneCurve.expcomp += EXPOSURE_STEP;
            }};
}

const State* sweepStates(size_t& count)
{
    static const State states[] = {
        spotAdd(), spotRemove(),
        retinexOn(), retinexColorspace(), retinexGamma(),
        workProfileRetinexOn(), retinexOff(),
        workProfileRetinexOff(),
        filmNegativeOn(), filmNegativeOff(),
        captureSharpeningOn(), captureSharpeningOff(),
    };
    count = sizeof(states) / sizeof(states[0]);
    return states;
}

/*
 * Edits the viewport fast path can serve.
 *
 * Each mask is the one the GUI sends for the tool and each mutation changes a
 * parent LUT or an auto-exposure input, so the full-frame preview output
 * depends on the edit: a fast-path bug that rebuilt the wrong LUT or skipped a
 * stage changes the dump, and the driver catches it as a non-zero difference
 * against the same state rendered with RT_VIEWPORT_FAST=0.
 */
const State* fastPathStates(size_t& count)
{
    static const State states[] = {
        exposureState(),
        {"tone-curve", rtengine::RequestIntent::AutoExposure,
         [](rtengine::procparams::ProcParams& params) {
             params.toneCurve.contrast += 5;
         }},
        {"lab-curve", rtengine::RequestIntent::LuminanceCurve,
         [](rtengine::procparams::ProcParams& params) {
             params.labCurve.brightness += 5;
         }},
        {"wavelet-curve", rtengine::RequestIntent::DirPyrEqualizer,
         [](rtengine::procparams::ProcParams& params) {
             params.wavelet.enabled = true;
             params.wavelet.wavclCurve = {0.0, 0.0, 0.5, 0.6, 1.0, 1.0};
         }},
        exposureState(),
    };
    count = sizeof(states) / sizeof(states[0]);
    return states;
}

const ZoomState zoomStates[] = {
    {"zoom-100", true},
    {"zoom-fit", false},
};

void printSwitch()
{
    std::printf("SWITCH work-order=%d shadow=%d\n",
                rtengine::stage::workOrderEnabled() ? 1 : 0,
                rtengine::stage::workShadowEnabled() ? 1 : 0);

    /*
     * Mirror the coordinator's kill-switch reading so the driver can prove
     * which mode the process actually ran. The coordinator reads the variable
     * once per process; this probe runs before the first pass.
     */
    const char* value = g_getenv("RT_VIEWPORT_FAST");
    const bool viewportEnabled = value == nullptr || value[0] == '\0'
        || g_strcmp0(value, "0") != 0;
    std::printf("VIEWPORT enabled=%d\n", viewportEnabled ? 1 : 0);
}

bool printState(unsigned index, const std::string& name, unsigned pass,
                double elapsedMs)
{
    std::printf("STATE %u %s pass=%u ms=%.1f\n", index, name.c_str(), pass,
                elapsedMs);
    return true;
}

int runParity(const std::string& file, const std::string& outDir,
              bool fastPath)
{
    const std::string dumpDir = joinPath(outDir, "dump");
    std::error_code error;
    std::filesystem::create_directories(dumpDir, error);
    if (error) {
        std::fprintf(stderr, "sweep: cannot create %s: %s\n", dumpDir.c_str(),
                     error.message().c_str());
        return 2;
    }
    g_setenv("RT_PREVIEW_DUMP", dumpDir.c_str(), TRUE);

    rtengine::procparams::ProcParams params;
    Session session;
    if (!session.open(file, params, fastPath)) {
        return 2;
    }

    unsigned pass = 1;
    /* The initial pass is copied as the reference state for the driver. */
    if (!copyDump(dumpDir, outDir, "00-initial", pass)) {
        return 2;
    }
    printState(0, "initial", pass, 0.0);

    size_t stateCount = 0;
    const State* states = fastPath ? fastPathStates(stateCount)
                                   : sweepStates(stateCount);
    unsigned index = 1;

    for (size_t i = 0; i < stateCount; ++i) {
        rtengine::procparams::ProcParams next = params;
        states[i].mutate(next);

        rtengine::procparams::ProcParams normalised;
        double elapsed = 0.0;
        if (!session.apply(next, states[i].intent, normalised, elapsed,
                           !fastPath)) {
            return 2;
        }
        params = normalised;
        ++pass;

        char name[64];
        std::snprintf(name, sizeof(name), "%02u-%s", index,
                      states[i].name.c_str());
        if (!copyDump(dumpDir, outDir, name, pass)) {
            return 2;
        }
        printState(index, states[i].name, pass, elapsed);
        ++index;
    }

    /*
     * The zoom crossings and the trailing exposure belong to the full-state
     * sweep. The fast-path sequence is exactly the eligible edits: a zoom
     * crossing is a high-detail transition and would fall back by design.
     */
    if (!fastPath) {
        for (const ZoomState& zoom : zoomStates) {
            double elapsed = 0.0;
            if (!session.zoom(zoom.fullResolution, elapsed)) {
                return 2;
            }
            ++pass;

            char name[64];
            std::snprintf(name, sizeof(name), "%02u-%s", index, zoom.name);
            if (!copyDump(dumpDir, outDir, name, pass)) {
                return 2;
            }
            printState(index, zoom.name, pass, elapsed);
            ++index;
        }
    }

    if (!fastPath) {
        rtengine::procparams::ProcParams next = params;
        exposureState().mutate(next);

        rtengine::procparams::ProcParams normalised;
        double elapsed = 0.0;
        if (!session.apply(next, exposureState().intent, normalised, elapsed)) {
            return 2;
        }
        params = normalised;
        ++pass;

        char name[64];
        std::snprintf(name, sizeof(name), "%02u-exposure", index);
        if (!copyDump(dumpDir, outDir, name, pass)) {
            return 2;
        }
        printState(index, "exposure", pass, elapsed);
    }

    return 0;
}

int runTiming(const std::string& file, const std::string& outDir, int samples,
              bool clamp)
{
    rtengine::procparams::ProcParams params;
    Session session;
    /*
     * Timing always owns a visible crop: the fast path's saving is the crop
     * repaint it serves, and without one the harness would measure only the
     * deferred full pass.
     */
    if (!session.open(file, params, true)) {
        return 2;
    }

    printState(0, "initial", session.ready(), 0.0);

    bool high = true;
    for (int sample = 0; sample < samples; ++sample) {
        rtengine::procparams::ProcParams next = params;
        const rtengine::RequestIntent intent = clamp
            ? rtengine::RequestIntent::DarkFrame
            : rtengine::RequestIntent::AutoExposure;
        if (clamp) {
            next.toneCurve.clampOOG = high;
        } else {
            next.toneCurve.expcomp = high ? EXPOSURE_STEP : -EXPOSURE_STEP;
        }
        high = !high;

        rtengine::procparams::ProcParams normalised;
        double elapsed = 0.0;
        if (!session.apply(next, intent, normalised, elapsed)) {
            return 2;
        }
        params = normalised;
        std::printf("TIMING %d %.3f\n", sample, elapsed);
    }

    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    /*
     * The kill-switch probe needs no image and no engine: the environment is
     * read once per process, so the driver invokes the binary once per value.
     */
    if (argc >= 2 && std::string(argv[1]) == "--print-switch") {
        printSwitch();
        return 0;
    }

    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <raw> <out-dir> [--timing=N] [--clamp]"
                     " [--fast-path]\n"
                     "       %s --print-switch\n",
                     argv[0], argv[0]);
        return 2;
    }

    const std::string file = argv[1];
    std::string outDir = argv[2];
    {
        std::error_code absoluteError;
        const std::filesystem::path absolute =
            std::filesystem::absolute(outDir, absoluteError);
        if (!absoluteError) {
            outDir = absolute.string();
        }
    }
    int timingSamples = 0;
    bool timingClamp = false;
    bool fastPath = false;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--timing=", 0) == 0) {
            timingSamples = std::atoi(arg.c_str() + std::strlen("--timing="));
        } else if (arg == "--clamp") {
            timingClamp = true;
        } else if (arg == "--fast-path") {
            fastPath = true;
        } else {
            std::fprintf(stderr, "sweep: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    Glib::init();
    Gio::init();

    const std::string settingsDir = joinPath(outDir, "settings");
    const std::string cacheDir = joinPath(outDir, "cache");
    std::error_code error;
    std::filesystem::create_directories(settingsDir, error);
    std::filesystem::create_directories(cacheDir, error);
    if (error) {
        std::fprintf(stderr, "sweep: cannot create the run directories\n");
        return 2;
    }
    g_setenv("RT_SETTINGS", settingsDir.c_str(), TRUE);
    g_setenv("RT_CACHE", cacheDir.c_str(), TRUE);
    /*
     * The sweep disables the base cache by default so a state dump cannot be
     * served from an earlier state's snapshot. RT_SWEEP_BASE_CACHE=1 keeps it
     * on for the dedicated evidence run that proves a deferred flush neither
     * invalidates nor rebuilds the base.
     */
    const char* keepBaseCache = g_getenv("RT_SWEEP_BASE_CACHE");
    if (keepBaseCache == nullptr || g_strcmp0(keepBaseCache, "1") != 0) {
        g_setenv("RT_PREVIEW_BASE_CACHE", "0", TRUE);
    }
    /*
     * The full-state sweep must make every state a settled full-frame pass, so
     * the viewport fast path is switched off: it deliberately repaints a crop
     * and defers the full pass, which would make a state's dump and its shadow
     * record belong to different passes. The fast-path sequence keeps the
     * ambient value, so the driver can run the same states with it on and off.
     */
    if (fastPath) {
        /*
         * Leave RT_VIEWPORT_FAST exactly as the driver set it, so the same
         * binary can run the forced-full side and the fast side.
         */
        g_setenv("RT_PREVIEW_PROFILE", "1", TRUE);
    } else {
        g_setenv("RT_VIEWPORT_FAST", "0", TRUE);
    }

    try {
        App::get().setArgv0(DATA_SEARCH_PATH);
        Options::load(true);
    } catch (const Options::Error& error) {
        std::fprintf(stderr, "options: %s\n", error.what());
        return 2;
    }

    /*
     * The high-detail decision below 100% follows the Preview Demosaic
     * Method preference. FAST keeps the fit preview fast so the 100% crossing
     * is the transition that actually schedules the full-demosaic work; the
     * per-tab FAST toggle is left off so 100% needs the full demosaic.
     */
    App::get().mut_options().prevdemo = PD_Fast;

    printSwitch();

    return timingSamples > 0
               ? runTiming(file, outDir, timingSamples, timingClamp)
               : runParity(file, outDir, fastPath);
}
