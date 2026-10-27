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
 * Specification test for the typed editor request and the event registry.
 *
 * Every intent must produce a recorded request decision: its kind, family
 * facts and dirty source. The test also scans the source tree so the retired
 * request mask cannot reappear.
 */

#include "rtengine/editorrequest.h"
#include "rtengine/editorrequest_registry.h"

#include "rtengine/procevents.h"
#include "rtengine/procparams.h"
#include "rtengine/stage/stage_work.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

using rtengine::EditorRequest;
using rtengine::EditorRequestRegistry;
using rtengine::ProcEvent;
using rtengine::RequestIntent;
using rtengine::coalesce;
using rtengine::crop_request_kind;
using rtengine::make_request;
using rtengine::producerEventKey;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

using Kind = EditorRequest::RequestKind;

/*
 * Recorded decision for one producer intent.
 *
 * These are the expected decisions of the intent table, not a computation
 * over a live mask.
 */
struct IntentExpectation {
    RequestIntent intent;
    const char* name;
    Kind kind;
    bool present;
    bool controlOnly;
    bool fullFrame;
    bool fullFrameAdmission;
    bool colorHlrWb;
    bool monitorOutput;
    bool imperativeRepaint;
    bool spot;
    bool raw;
    bool hdr;
    bool rgbProc;
    bool labChain;
    bool baseChain;
    bool crop;
    bool monitor;
};

const std::vector<IntentExpectation>& intentExpectations()
{
    static const std::vector<IntentExpectation> rows = {
        {RequestIntent::AllNoRaw, "AllNoRaw", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::Demosaic, "Demosaic", Kind::FullPreview,
         true, false, true, false, false, true, false, true, true, true,
         true, true, true, false, false},
        {RequestIntent::Hdr, "Hdr", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::SpotAdjust, "SpotAdjust", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::Transform, "Transform", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::WhiteBalance, "WhiteBalance", Kind::FullPreview,
         true, false, true, false, true, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::RepaintOnly, "RepaintOnly", Kind::RepaintOnly,
         true, false, false, false, false, false, true, false, false, false,
         false, false, false, false, false},
        {RequestIntent::ControlOnly, "ControlOnly", Kind::ParamsOnly,
         true, true, false, false, false, true, false, false, false, false,
         false, false, false, false, false},
        {RequestIntent::Compr, "Compr", Kind::FullPreview,
         true, false, true, true, false, true, false, true, false, true,
         true, true, true, false, true},
        {RequestIntent::CaptureSharpen, "CaptureSharpen", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::DarkFrame, "DarkFrame", Kind::FullPreview,
         true, false, true, true, false, true, false, true, true, true,
         true, true, true, false, false},
        {RequestIntent::First, "First", Kind::FullPreview,
         true, false, true, true, false, true, false, true, true, true,
         true, true, true, false, true},
        {RequestIntent::AutoExposure, "AutoExposure", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, false, false, false},
        {RequestIntent::LuminanceCurve, "LuminanceCurve", Kind::ParamsOnly,
         true, false, false, false, false, true, false, false, false, false,
         false, true, false, false, false},
        {RequestIntent::DirPyrEqualizer, "DirPyrEqualizer", Kind::ParamsOnly,
         true, false, false, false, false, true, false, false, false, false,
         false, true, false, false, false},
        {RequestIntent::RgbCurve, "RgbCurve", Kind::ParamsOnly,
         true, false, false, false, false, true, false, false, false, false,
         true, true, false, false, false},
        {RequestIntent::Sharpening, "Sharpening", Kind::ParamsOnly,
         true, false, false, false, false, true, false, false, false, false,
         false, true, false, false, false},
        {RequestIntent::Retinex, "Retinex", Kind::FullPreview,
         true, false, true, false, false, true, false, true, false, true,
         true, true, true, false, false},
        {RequestIntent::Crop, "Crop", Kind::ParamsOnly,
         true, false, false, false, false, false, false, false, false, false,
         false, false, false, true, false},
        {RequestIntent::Monitor, "Monitor", Kind::ParamsOnly,
         true, false, false, true, false, true, false, false, false, false,
         false, false, true, false, true},
        {RequestIntent::NoRequest, "NoRequest", Kind::ParamsOnly,
         false, true, false, false, false, true, false, false, false, false,
         false, false, false, false, false},
        {RequestIntent::HighQuality, "HighQuality", Kind::HighQuality,
         true, false, false, false, false, true, false, false, false, false,
         false, false, false, false, false}
    };
    return rows;
}

void checkIntentTable()
{
    for (const IntentExpectation& row : intentExpectations()) {
        const std::string label = std::string("intent ") + row.name;
        const EditorRequest request = make_request(row.intent);

        check(request.kind == row.kind, label + ": kind");
        check(request.present == row.present, label + ": present");
        check(request.controlOnly == row.controlOnly, label + ": controlOnly");
        check(request.fullFrameIntent == row.fullFrame,
              label + ": fullFrameIntent");
        check(request.fullFrameAdmission == row.fullFrameAdmission,
              label + ": fullFrameAdmission");
        check(request.colorHlrWbRequest == row.colorHlrWb,
              label + ": colorHlrWbRequest");
        check(request.monitorOutputRequest == row.monitorOutput,
              label + ": monitorOutputRequest");
        check(request.imperativeRepaint == row.imperativeRepaint,
              label + ": imperativeRepaint");
        check(request.spotPending == row.spot, label + ": spotPending");
        check(request.rawIntent == row.raw, label + ": rawIntent");
        check(request.hdrIntent == row.hdr, label + ": hdrIntent");
        check(request.rgbProcIntent == row.rgbProc, label + ": rgbProcIntent");
        check(request.labChainIntent == row.labChain, label + ": labChain");
        check(request.baseChainIntent == row.baseChain, label + ": baseChain");
        check(request.cropIntent == row.crop, label + ": cropIntent");
        check(request.monitorIntent == row.monitor, label + ": monitorIntent");
        check(request.derivesFromParameters(),
              label + ": derives from parameters");
        check(request.dirty.blocks.empty(), label + ": no producer dirty set");

        if (row.present) {
            check(request.identity() != 0, label + ": non-zero identity");
        }
    }

    check(make_request(RequestIntent::RepaintOnly).imperativeRepaint,
          "the repaint intent is imperative");
    check(make_request(RequestIntent::ControlOnly).controlBitRequest,
          "the control intent carries the control fact");
    check(make_request(RequestIntent::WhiteBalance).colorHlrWbRequest,
          "the white-balance intent carries the Color/Coloropp fact");
    check(make_request(RequestIntent::DirPyrEqualizer).identity()
              == make_request(RequestIntent::Sharpening).identity(),
          "same-family intents share their request identity");
}

/* A producer that names its dirty blocks gets an execution set. */
void checkProducerDirtyBlocks()
{
    const EditorRequest seeded = make_request(
        RequestIntent::Transform, {"transform"},
        rtengine::stage::StagePath::Preview);

    check(seeded.dirtySource == EditorRequest::DirtySource::ProducerProvided,
          "producer blocks set ProducerProvided");
    check(seeded.dirty.valid, "producer dirty set is valid");
    check(seeded.dirty.ids.count("transform") != 0,
          "producer dirty set contains the seed");
    check(seeded.dirty.ids.count("local.adjustments") != 0,
          "producer dirty set closes over the first consumer");
    check(seeded.dirty.ids.count("monitor.output") != 0,
          "producer dirty set contains the path resident");
    check(seeded.dirty.forced == false, "producer dirty set is not forced");
    check(!seeded.derivesFromParameters(),
          "producer dirty set does not derive from parameters");
    check(seeded.identity() != make_request(RequestIntent::Transform).identity(),
          "producer dirty blocks change the identity");

    const EditorRequest second = make_request(
        RequestIntent::Transform, {"rgb.proc"},
        rtengine::stage::StagePath::Preview);
    const EditorRequest merged = coalesce(seeded, second);
    check(merged.dirtySource
              == EditorRequest::DirtySource::ProducerProvided,
          "coalescing two producer sets keeps ProducerProvided");
    check(merged.dirty.ids.count("transform") != 0,
          "coalescing unions the producer sets");
    check(merged.dirty.ids.count("rgb.proc") != 0,
          "coalescing keeps both seeds");

    const EditorRequest mixed = coalesce(make_request(RequestIntent::Transform),
                                         second);
    check(mixed.dirtySource
              == EditorRequest::DirtySource::ProducerProvided,
          "a derived request does not erase a producer set");
}

/* Coalescing keeps the union of the typed facts and recomputes the shape. */
void checkCoalesce()
{
    const EditorRequest control = make_request(RequestIntent::ControlOnly);
    const EditorRequest repaint = make_request(RequestIntent::RepaintOnly);
    const EditorRequest crop = make_request(RequestIntent::Crop);

    check(coalesce(repaint, repaint).kind == Kind::RepaintOnly,
          "two bare repaints stay RepaintOnly");
    check(coalesce(repaint, repaint).imperativeRepaint,
          "two bare repaints stay imperative");
    check(coalesce(repaint, control).kind == Kind::ParamsOnly,
          "a control request turns a repaint into ParamsOnly");
    check(!coalesce(repaint, control).imperativeRepaint,
          "a control request clears the imperative repaint");

    const EditorRequest cropAndRepaint = coalesce(crop, repaint);
    check(cropAndRepaint.cropIntent && cropAndRepaint.repaintRequest,
          "crop and repaint facts union");
    check(cropAndRepaint.monitorOutputRequest,
          "two suppressing requests produce a rendering union");

    const EditorRequest reload =
        coalesce(make_request(RequestIntent::Hdr),
                 make_request(RequestIntent::LuminanceCurve));
    check(reload.kind == Kind::FullPreview,
          "the full-frame family wins over a parameter request");

    const EditorRequest accurate =
        coalesce(make_request(RequestIntent::AllNoRaw),
                 make_request(RequestIntent::HighQuality));
    check(accurate.kind == Kind::HighQuality,
          "high quality wins over the reload family");
    check(accurate.highDetailIntent,
          "high quality keeps its fact in the union");

    const EditorRequest empty;
    const EditorRequest present = make_request(RequestIntent::Crop);
    check(coalesce(empty, present).identity() == present.identity(),
          "coalescing with an empty request keeps the request");
    check(!coalesce(empty, empty).present,
          "coalescing two empty requests stays empty");
}

/* The fast-path augmentations set the named facts directly. */
void checkAugmentations()
{
    EditorRequest crop = make_request(RequestIntent::Crop);
    check(!crop.transformFamilyIntent,
          "a bare crop does not claim transform-family work");
    crop.markCropTransform();
    check(crop.transformFamilyIntent,
          "the PC-vignette augmentation sets the transform family");
    check(crop.kind == Kind::FullPreview,
          "the PC-vignette augmentation widens the kind");
    check(crop.spotPending && crop.hdrIntent && crop.rgbProcIntent,
          "the PC-vignette augmentation carries the transform families");

    EditorRequest lumacurve = make_request(RequestIntent::LuminanceCurve);
    lumacurve.markLabPixels(true, true);
    check(lumacurve.labChainIntent,
          "the fast-path Lab augmentation keeps labChainIntent");
    check(lumacurve.luminanceIntent && lumacurve.colorIntent,
          "the fast-path Lab augmentation names both pixel tools");
    check(lumacurve.kind == Kind::ParamsOnly,
          "the fast-path Lab augmentation keeps the parameter kind");

    EditorRequest single = make_request(RequestIntent::RepaintOnly);
    single.markLabPixels(true, false);
    check(single.luminanceIntent && !single.colorIntent,
          "the Lab augmentation keeps the exact family it was given");

    EditorRequest moved = make_request(RequestIntent::RepaintOnly);
    moved.markViewportMoved();
    check(moved.viewportMoved, "the panning requeue records the move");
    check(moved.monitorIntent, "the panning requeue names the monitor family");
    check(moved.monitorOutputRequest && moved.fullFrameAdmission,
          "the panning requeue admits the full-frame render");
    check(!moved.imperativeRepaint,
          "the panning requeue clears the imperative repaint");

    EditorRequest base = make_request(RequestIntent::Crop);
    base.markBaseRefresh(true);
    check(base.fullFrameIntent && base.spotPending,
          "a base refresh claims the reload and spot work");
    check(base.monitorIntent, "a base refresh names the monitor family");
    check(base.kind == Kind::FullPreview,
          "a base refresh widens the kind");

    EditorRequest hlr = make_request(RequestIntent::WhiteBalance);
    hlr.markColorHlrDemosaic();
    check(hlr.rawIntent && hlr.spotPending,
          "the Color/Coloropp augmentation names demosaic and spot work");

    EditorRequest rebuilt = make_request(RequestIntent::LuminanceCurve);
    rebuilt.markDemosaicRebuilt();
    check(rebuilt.fullFrameIntent && rebuilt.captureSharpenIntent,
          "the demosaic rebuild claims the reload and capture sharpening");

    EditorRequest deferred = make_request(RequestIntent::RepaintOnly);
    rtengine::stage::DirtyBlocks blocks;
    blocks.ids.insert("monitor.output");
    blocks.ids.insert("rgb.proc");
    blocks.ids.insert("lab.tools");
    blocks.ids.insert("local.adjustments");
    deferred.absorbDeferredExecution(blocks);
    check(deferred.monitorIntent && deferred.rgbProcIntent
              && deferred.lumaCurveIntent && deferred.autoExposureIntent,
          "the deferred record sets its typed stage facts");
    check(!deferred.imperativeRepaint,
          "the deferred record clears the imperative repaint");
}

/* The event registry keeps identity and resolves typed requests. */
void checkRegistry()
{
    EditorRequestRegistry* registry = EditorRequestRegistry::getInstance();

    const ProcEvent event = registry->newEvent();
    const EditorRequest typed = make_request(RequestIntent::WhiteBalance);
    registry->registerRequest(event, typed);

    check(registry->hasRegisteredRequest(event),
          "a dynamic event has a registered request");
    check(registry->requestFor(event).identity() == typed.identity(),
          "the typed request survives the registry");

    const ProcEvent second = registry->newEvent();
    check(second != event, "each dynamic event gets a fresh identity");

    /* Static identities resolve to their recorded typed classification. */
    using namespace rtengine;
    check(registry->requestFor(ProcEvent(EvCTRotate)).kind
              == Kind::FullPreview,
          "a rotate event resolves to the full-preview decision");
    check(registry->requestFor(ProcEvent(EvCrop)).cropIntent,
          "the crop event resolves to a crop request");
    check(registry->requestFor(ProcEvent(EvOProfile)).monitorIntent,
          "the output profile event resolves to the monitor family");
    check(registry->requestFor(ProcEvent(EvShrEnabled)).luminanceIntent,
          "the sharpening event resolves to the Lab pixel family");
    check(!registry->requestFor(ProcEvent(1)).present,
          "an obsolete event resolves to no request");
}

/* Producer event keys separate requests that differ in any typed fact. */
void checkProducerEventKey()
{
    const EditorRequest luminance = make_request(
        RequestIntent::DirPyrEqualizer);
    EditorRequest colour = luminance;
    colour.luminanceIntent = false;
    colour.labPixelIntent = true;

    check(luminance.identity() != colour.identity(),
          "the exact Lab family is part of the identity");
    check(producerEventKey(luminance, "HISTORY_MSG_LAB")
              != producerEventKey(colour, "HISTORY_MSG_LAB"),
          "same message with different facts gets distinct keys");
    check(producerEventKey(luminance, "HISTORY_MSG_LAB")
              == producerEventKey(luminance, "HISTORY_MSG_LAB"),
          "the same producer action keeps its key");

    const EditorRequest allNoRaw = make_request(RequestIntent::AllNoRaw);
    const EditorRequest rgbCurve = make_request(RequestIntent::RgbCurve);
    check(allNoRaw.baseChainIntent && !rgbCurve.baseChainIntent,
          "AllNoRaw is a base-chain request and RgbCurve is not");
    check(producerEventKey(allNoRaw, "msg")
              != producerEventKey(rgbCurve, "msg"),
          "different base-chain requests get distinct keys");
}

/* A crop reallocation must refill the base even for a parameter update. */
void checkCropReallocationWorkOrder()
{
    rtengine::procparams::ProcParams params;

    const rtengine::stage::BlockFingerprints fingerprints =
        rtengine::stage::stage_fingerprints(
            rtengine::stage::StagePath::Crop, params);

    rtengine::stage::RenderStateKey state;
    state.cropSkip = 1;
    state.requestKind = crop_request_kind(Kind::ParamsOnly, false);

    rtengine::stage::WorkInputs inputs;
    inputs.path = rtengine::stage::StagePath::Crop;
    inputs.previousState = &state;
    inputs.previousFingerprints = &fingerprints;
    inputs.previousParams = &params;
    inputs.currentState = &state;
    inputs.currentFingerprints = &fingerprints;
    inputs.requestKind = state.requestKind;
    inputs.params = &params;

    const rtengine::stage::WorkOrder steady =
        rtengine::stage::derive_crop_work(inputs);
    check(!rtengine::stage::workItemRuns(
              steady, rtengine::stage::WorkItem::rawBaseRebuild),
          "a steady parameter update does not rebuild the crop base");

    state.requestKind = crop_request_kind(Kind::ParamsOnly, true);
    inputs.requestKind = state.requestKind;

    const rtengine::stage::WorkOrder reallocated =
        rtengine::stage::derive_crop_work(inputs);
    check(rtengine::stage::workItemRuns(
              reallocated, rtengine::stage::WorkItem::rawBaseRebuild),
          "a reallocation forces the crop base rebuild");
    check(rtengine::stage::workItemRuns(
              reallocated, rtengine::stage::WorkItem::analysisRun),
          "a reallocation forces the crop analysis");

    check(crop_request_kind(Kind::HighQuality, true)
              == rtengine::stage::RequestKind::FullCrop,
          "a reallocation wins over a high-quality crop request");
    check(crop_request_kind(Kind::FullPreview, false)
              == rtengine::stage::RequestKind::FullCrop,
          "a full preview maps to a full crop");
}

/* Tool -> intent wiring. */
void checkProducerSource(
    const char* file, const char* historyMsg, const char* intent,
    const char* label)
{
    const std::string path = std::string(RT_SOURCE_DIR) + "/" + file;
    std::ifstream stream(path);

    if (!stream) {
        check(false, std::string(label) + ": cannot read " + path);
        return;
    }

    std::string line;
    bool foundMessage = false;
    bool foundIntent = false;

    while (std::getline(stream, line)) {
        if (line.find(historyMsg) == std::string::npos) {
            continue;
        }
        foundMessage = true;
        if (line.find(std::string("RequestIntent::") + intent)
                != std::string::npos) {
            foundIntent = true;
        }
    }

    check(foundMessage, std::string(label) + ": message not found");
    check(foundIntent, std::string(label) + ": wrong intent");
}

void checkConvertedProducerIntents()
{
    checkProducerSource("rtgui/tools/filmnegative.cc",
                        "HISTORY_MSG_FILMNEGATIVE_VALUES", "AllNoRaw",
                        "film negative");
    checkProducerSource("rtgui/tools/spot.cc", "HISTORY_MSG_SPOT_ENTRY",
                        "SpotAdjust", "spot entry");
    checkProducerSource("rtgui/tools/whitebalance.cc",
                        "HISTORY_MSG_WBALANCE_OBSERVER10", "WhiteBalance",
                        "white balance");
    checkProducerSource("rtgui/tools/dehaze.cc", "HISTORY_MSG_DEHAZE_ENABLED",
                        "Hdr", "dehaze");
    checkProducerSource("rtgui/tools/perspective.cc",
                        "HISTORY_MSG_PERSP_METHOD", "Transform",
                        "perspective");
    checkProducerSource("rtgui/tools/compressgamut.cc",
                        "HISTORY_MSG_CG_COLORSPACE", "Compr",
                        "gamut compression");
    checkProducerSource("rtgui/tools/pdsharpening.cc",
                        "HISTORY_MSG_PDSHARPEN_CONTRAST", "CaptureSharpen",
                        "capture sharpening");
    checkProducerSource("rtgui/tools/bayerpreprocess.cc",
                        "HISTORY_MSG_PREPROCESS_PDAFLINESFILTER", "DarkFrame",
                        "dark frame");
    checkProducerSource("rtgui/tools/cropguide.cc",
                        "HISTORY_MSG_CROP_GUIDE_ENABLED", "RepaintOnly",
                        "crop guide");
    checkProducerSource("rtgui/tools/preprocesswb.cc",
                        "HISTORY_MSG_PREPROCWB_MODE", "First",
                        "preprocess white balance");
    checkProducerSource("rtgui/tools/resize.cc",
                        "HISTORY_MSG_RESIZE_LONGEDGE", "ControlOnly",
                        "resize");
}

/*
 * Remove comments from a source file so a comment cannot make the guard pass
 * or fail for the wrong reason. String and character literals are respected.
 */
std::string stripComments(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool lineComment = false;
    bool blockComment = false;
    bool inString = false;
    bool inChar = false;

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        const char next =
            index + 1 < text.size() ? text[index + 1] : '\0';

        if (lineComment) {
            if (current == '\n') {
                lineComment = false;
                result += current;
            }
            continue;
        }
        if (blockComment) {
            if (current == '*' && next == '/') {
                blockComment = false;
                ++index;
            } else if (current == '\n') {
                result += current;
            }
            continue;
        }
        if (inString || inChar) {
            result += current;
            if (current == '\\' && next != '\0') {
                result += next;
                ++index;
            } else if ((inString && current == '"')
                       || (inChar && current == '\'')) {
                inString = false;
                inChar = false;
            }
            continue;
        }
        if (current == '/' && next == '/') {
            lineComment = true;
            ++index;
            continue;
        }
        if (current == '/' && next == '*') {
            blockComment = true;
            ++index;
            continue;
        }
        if (current == '"') {
            inString = true;
        } else if (current == '\'') {
            inChar = true;
        }
        result += current;
    }

    return result;
}

/* Every `newEvent` call in `rtgui/` must name a typed request. */
void checkNoLegacyProducerCalls()
{
    namespace fs = std::filesystem;

    const fs::path root = fs::path(RT_SOURCE_DIR) / "rtgui";
    const std::regex callPattern(
        R"(newEvent\s*\(\s*([A-Za-z_][A-Za-z0-9_:]*))");

    std::size_t files = 0;
    std::size_t calls = 0;
    std::vector<std::string> violations;

    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end;
         it != end && !error; it.increment(error)) {
        if (!it->is_regular_file()) {
            continue;
        }

        const std::string suffix = it->path().extension().string();
        if (suffix != ".cc" && suffix != ".h" && suffix != ".hpp") {
            continue;
        }

        const std::string relative =
            fs::relative(it->path(), root).generic_string();
        if (relative == "eventmapper.cc" || relative == "eventmapper.h") {
            continue;
        }

        std::ifstream stream(it->path());
        if (!stream) {
            check(false, "producer scan cannot read " + it->path().string());
            continue;
        }

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string content = stripComments(buffer.str());

        ++files;
        for (std::sregex_iterator match(
                 content.begin(), content.end(), callPattern),
             last;
             match != last; ++match) {
            ++calls;
            const std::string argument = (*match)[1].str();
            if (argument.rfind("rtengine::", 0) != 0) {
                const std::size_t offset =
                    static_cast<std::size_t>(match->position());
                const std::size_t line = static_cast<std::size_t>(
                    std::count(content.begin(), content.begin() + offset,
                               '\n')) + 1;
                violations.push_back(relative + ":" + std::to_string(line)
                                     + ": " + argument);
            }
        }
    }

    check(!error, "producer scan completed: " + error.message());
    check(files > 0, "producer scan found GUI sources");
    check(calls > 0, "producer scan found typed producer calls");
    for (const std::string& violation : violations) {
        check(false, "legacy newEvent producer call: " + violation);
    }

    std::printf("editorrequest-test: producer scan: %zu files, %zu calls\n",
                files, calls);
}

/*
 * The retired request mask must not reappear. Scan every engine, GUI and tool
 * source file, ignoring comments, for the deleted units, constants, helpers
 * and integer event actions.
 */
void checkMaskRetirementGuard()
{
    namespace fs = std::filesystem;

    const fs::path sourceRoot(RT_SOURCE_DIR);
    const std::vector<fs::path> roots = {
        sourceRoot / "rtengine", sourceRoot / "rtgui", sourceRoot / "tools"
    };

    const std::regex maskConstant(R"(\bM_[A-Z][A-Z0-9_]*\b)");
    const std::regex integerAction(R"(newEvent\s*\(\s*[0-9])");
    const std::vector<std::string> allowedMathMacros = {
        "M_PI", "M_E", "M_LN2", "M_LN10", "M_SQRT2", "M_SQRT1_2"
    };
    const std::vector<std::string> forbidden = {
        "refreshmap", "stage_mask_map", "RefreshMapper",
        "from_legacy_mask", "legacyGateMask", "planningAgreesWithLegacyMask",
        "executionPayload", "viewportCropTodo", "viewportCropPayload",
        "compatibility_mask", "deferredAnalysisMask"
    };

    std::size_t files = 0;
    std::vector<std::string> violations;

    for (const fs::path& root : roots) {
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, error), end;
             it != end && !error; it.increment(error)) {
            if (!it->is_regular_file()) {
                continue;
            }

            const std::string suffix = it->path().extension().string();
            if (suffix != ".cc" && suffix != ".h" && suffix != ".hpp"
                    && suffix != ".c") {
                continue;
            }

            const std::string relative =
                fs::relative(it->path(), sourceRoot).generic_string();
            if (relative == "rtgui/eventmapper.cc"
                    || relative == "rtgui/eventmapper.h"
                    || relative
                        == "rtengine/stage/tests/editorrequest_test.cc") {
                continue;
            }

            std::ifstream stream(it->path());
            if (!stream) {
                check(false, "guard cannot read " + it->path().string());
                continue;
            }

            std::ostringstream buffer;
            buffer << stream.rdbuf();
            const std::string content = stripComments(buffer.str());
            ++files;

            for (const std::string& token : forbidden) {
                if (content.find(token) != std::string::npos) {
                    violations.push_back(relative + ": " + token);
                }
            }

            for (std::sregex_iterator match(
                     content.begin(), content.end(), maskConstant),
                 last;
                 match != last; ++match) {
                if (std::find(allowedMathMacros.begin(),
                              allowedMathMacros.end(),
                              match->str())
                        != allowedMathMacros.end()) {
                    continue;
                }
                violations.push_back(relative + ": " + match->str());
            }

            if (std::regex_search(content, integerAction)) {
                violations.push_back(
                    relative + ": newEvent with an integer action");
            }
        }
    }

    check(files > 0, "mask guard scanned the source tree");
    std::printf("editorrequest-test: mask guard: %zu files\n", files);
    for (const std::string& violation : violations) {
        check(false, "retired mask surface: " + violation);
    }
}

void expectAbort(ProcEvent event, const char* label)
{
    const pid_t pid = fork();

    if (pid == 0) {
        EditorRequestRegistry::getInstance()->requestFor(event);
        _exit(0);
    }

    if (pid < 0) {
        check(false, std::string(label) + ": fork failed");
        return;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        check(false, std::string(label) + ": waitpid failed");
        return;
    }

    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          std::string(label) + ": expected an abort");
}

void checkContractViolations()
{
    expectAbort(ProcEvent(rtengine::NUMOFEVENTS + 100000), "unknown event");
    expectAbort(ProcEvent(-1), "negative event");
}

} // namespace

int main()
{
    checkIntentTable();
    checkProducerDirtyBlocks();
    checkCoalesce();
    checkAugmentations();
    checkRegistry();
    checkProducerEventKey();
    checkCropReallocationWorkOrder();
    checkConvertedProducerIntents();
    checkNoLegacyProducerCalls();
    checkMaskRetirementGuard();
    checkContractViolations();

    if (failures == 0) {
        std::puts("editorrequest-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
