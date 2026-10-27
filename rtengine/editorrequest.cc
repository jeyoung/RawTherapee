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
 * Typed editor request construction and coalescing.
 *
 * A producer names a RequestIntent. This unit maps the intent to the named
 * pipeline families it covers and derives the request facts once, so no
 * integer mask reaches the coordinator, the plan or a stage body.
 */

#include "editorrequest.h"

#include "procparams.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>

namespace rtengine
{

namespace
{

/*
 * Preview-path blocks the viewport fast path can serve, and the base blocks it
 * cannot. A producer that reports its own dirty set is held to the same
 * envelope the request families describe; the ids are the canonical preview
 * descriptors (stage_pipeline.cc).
 */
bool viewportDeltaBlock(const std::string& id)
{
    static const std::set<std::string> delta = {
        "local.adjustments", "rgb.proc", "lab.tools", "lab.wavelet",
        "working.trc", "ciecam", "gamut", "monitor.output"
    };
    return delta.count(id) != 0;
}

bool viewportBaseBlock(const std::string& id)
{
    static const std::set<std::string> base = {
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
        "analysis", "transform"
    };
    return base.count(id) != 0;
}

uint64_t hashCombine(uint64_t hash, uint64_t value)
{
    /*
     * FNV-1a. The identity stays inside one process, so portability across
     * builds is not required, but a fixed function keeps the tests and the
     * coordinator agreeing on equality. The offset basis is the 64-bit
     * FNV-1a constant 14695981039346656037.
     */
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

/*
 * The pipeline families one intent names.
 *
 * These are the named families of the retired request mask, kept as plain
 * booleans because they are the vocabulary of the intent table. They are not
 * a bitfield: nothing packs, masks or compares them, and the request the
 * caller receives carries the derived facts, never this struct.
 */
struct RequestFamilies {
    bool control = false;
    bool repaintOnly = false;
    bool highQuality = false;
    bool whiteBalance = false;
    bool spot = false;
    bool captureSharpen = false;
    bool monitor = false;
    bool retinex = false;
    bool crop = false;
    bool preprocess = false;
    bool raw = false;
    bool init = false;
    bool lineDenoise = false;
    bool hdr = false;
    bool transform = false;
    bool blurmap = false;
    bool autoExposure = false;
    bool rgbCurve = false;
    bool lumaCurve = false;
    bool luminance = false;
    bool color = false;
};

/*
 * The family set of one intent. The rows are the intent definitions, not a
 * mask table: each row names the operations the tool or static event asks
 * for.
 */
RequestFamilies familiesFor(RequestIntent intent)
{
    RequestFamilies f;

    switch (intent) {
    case RequestIntent::AllNoRaw:
        f.spot = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::Demosaic:
        f.spot = true;
        f.raw = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::Hdr:
        f.spot = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::SpotAdjust:
        f.spot = true;
        f.hdr = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::Transform:
        f.spot = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::WhiteBalance:
        f.whiteBalance = true;
        f.spot = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::RepaintOnly:
        f.repaintOnly = true;
        break;
    case RequestIntent::ControlOnly:
        f.control = true;
        break;
    case RequestIntent::Compr:
        f.spot = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        f.monitor = true;
        break;
    case RequestIntent::CaptureSharpen:
        f.spot = true;
        f.captureSharpen = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::DarkFrame:
    case RequestIntent::First:
        f.spot = true;
        f.preprocess = true;
        f.raw = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        f.monitor = intent == RequestIntent::First;
        break;
    case RequestIntent::AutoExposure:
        f.spot = true;
        f.hdr = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::LuminanceCurve:
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::DirPyrEqualizer:
    case RequestIntent::Sharpening:
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::RgbCurve:
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::Retinex:
        f.spot = true;
        f.retinex = true;
        f.init = true;
        f.lineDenoise = true;
        f.hdr = true;
        f.transform = true;
        f.blurmap = true;
        f.autoExposure = true;
        f.rgbCurve = true;
        f.lumaCurve = true;
        f.luminance = true;
        f.color = true;
        break;
    case RequestIntent::Crop:
        f.crop = true;
        break;
    case RequestIntent::Monitor:
        f.monitor = true;
        break;
    case RequestIntent::NoRequest:
        break;
    case RequestIntent::HighQuality:
        f.highQuality = true;
        break;
    }

    return f;
}

/* Whether the family set names every base family. */
bool namesEveryFamily(const RequestFamilies& f)
{
    return f.preprocess && f.raw && f.init && f.spot && f.lineDenoise
        && f.hdr && f.transform && f.blurmap && f.autoExposure
        && f.rgbCurve && f.lumaCurve && f.luminance && f.color;
}

/* Whether the family set names anything outside the control operation. */
bool namesRenderWork(const RequestFamilies& f)
{
    return f.repaintOnly || f.highQuality || f.whiteBalance || f.spot
        || f.captureSharpen || f.monitor || f.retinex || f.crop
        || f.preprocess || f.raw || f.init || f.lineDenoise || f.hdr
        || f.transform || f.blurmap || f.autoExposure || f.rgbCurve
        || f.lumaCurve || f.luminance || f.color;
}

/* Build the typed request of one family set. */
EditorRequest requestFromFamilies(const RequestFamilies& f)
{
    EditorRequest request;

    request.present = f.control || namesRenderWork(f);
    request.controlOnly = !namesRenderWork(f);
    request.controlBitRequest = f.control;
    request.repaintRequest = f.repaintOnly;
    request.spotPending = f.spot;
    request.highDetailIntent = f.highQuality;
    request.fullFrameIntent = f.init || f.lineDenoise || f.hdr;
    request.fullFrameAdmission = namesEveryFamily(f) || f.monitor;
    request.colorHlrWbRequest = f.whiteBalance;
    request.monitorIntent = f.monitor;
    request.rawIntent = f.raw;
    request.captureSharpenIntent = f.captureSharpen;
    request.transformFamilyIntent =
        f.hdr || f.transform || f.rgbCurve || f.init || f.lineDenoise;
    request.hdrIntent = f.hdr;
    request.rgbProcIntent = f.rgbCurve;
    request.labChainIntent = f.luminance || f.color || f.autoExposure;
    request.autoExposureIntent = f.autoExposure;
    request.localAdjustmentsIntent =
        f.autoExposure || f.rgbCurve || f.crop;
    request.baseChainIntent = f.preprocess || f.raw || f.whiteBalance
        || f.init || f.lineDenoise || f.monitor || f.transform
        || f.captureSharpen || f.retinex || f.blurmap;
    request.cropIntent = f.crop;
    request.lumaCurveIntent = f.lumaCurve;
    request.labPixelIntent = f.luminance || f.color;
    request.luminanceIntent = f.luminance;
    request.colorIntent = f.color;
    request.transformRequest = f.transform;

    /*
     * The intent table only ever sets repaintOnly for a bare repaint, so the
     * imperative flag follows it directly.
     */
    request.imperativeRepaint = f.repaintOnly;

    if (request.imperativeRepaint) {
        request.kind = EditorRequest::RequestKind::RepaintOnly;
    } else if (request.highDetailIntent) {
        request.kind = EditorRequest::RequestKind::HighQuality;
    } else if (request.fullFrameIntent) {
        request.kind = EditorRequest::RequestKind::FullPreview;
    } else {
        request.kind = EditorRequest::RequestKind::ParamsOnly;
    }

    request.monitorOutputRequest = !request.suppressesMonitorOutput();

    return request;
}

/*
 * Merge two typed requests.
 *
 * The union keeps every input fact. The derived facts that are not monotone
 * are recomputed from the union, exactly as the retired derivation recomputed
 * them from the union of two masks:
 *
 * - a bare repaint survives only when both inputs were bare repaints;
 * - a crop-only or repaint-only shape suppresses monitor.output, so two
 *   suppressing inputs produce a rendering union;
 * - the kind follows HighQuality, then the full-frame intent, then a surviving
 *   bare repaint.
 */
EditorRequest mergeRequests(const EditorRequest& first,
                            const EditorRequest& second)
{
    if (!first.present) {
        return second;
    }
    if (!second.present) {
        return first;
    }

    EditorRequest merged;

    merged.present = true;
    merged.controlOnly = first.controlOnly && second.controlOnly;
    merged.controlBitRequest =
        first.controlBitRequest || second.controlBitRequest;
    merged.repaintRequest = first.repaintRequest || second.repaintRequest;
    merged.spotPending = first.spotPending || second.spotPending;
    merged.viewportMoved = first.viewportMoved || second.viewportMoved;
    merged.highDetailIntent =
        first.highDetailIntent || second.highDetailIntent;
    merged.fullFrameIntent = first.fullFrameIntent || second.fullFrameIntent;
    merged.fullFrameAdmission =
        first.fullFrameAdmission || second.fullFrameAdmission;
    merged.colorHlrWbRequest =
        first.colorHlrWbRequest || second.colorHlrWbRequest;
    merged.monitorIntent = first.monitorIntent || second.monitorIntent;
    merged.rawIntent = first.rawIntent || second.rawIntent;
    merged.captureSharpenIntent =
        first.captureSharpenIntent || second.captureSharpenIntent;
    merged.transformFamilyIntent =
        first.transformFamilyIntent || second.transformFamilyIntent;
    merged.hdrIntent = first.hdrIntent || second.hdrIntent;
    merged.rgbProcIntent = first.rgbProcIntent || second.rgbProcIntent;
    merged.labChainIntent = first.labChainIntent || second.labChainIntent;
    merged.autoExposureIntent =
        first.autoExposureIntent || second.autoExposureIntent;
    merged.localAdjustmentsIntent =
        first.localAdjustmentsIntent || second.localAdjustmentsIntent;
    merged.baseChainIntent = first.baseChainIntent || second.baseChainIntent;
    merged.cropIntent = first.cropIntent || second.cropIntent;
    merged.lumaCurveIntent = first.lumaCurveIntent || second.lumaCurveIntent;
    merged.labPixelIntent = first.labPixelIntent || second.labPixelIntent;
    merged.luminanceIntent = first.luminanceIntent || second.luminanceIntent;
    merged.colorIntent = first.colorIntent || second.colorIntent;
    merged.transformRequest = first.transformRequest || second.transformRequest;

    merged.imperativeRepaint =
        first.imperativeRepaint && second.imperativeRepaint;

    if (merged.imperativeRepaint) {
        merged.kind = EditorRequest::RequestKind::RepaintOnly;
    } else if (merged.highDetailIntent) {
        merged.kind = EditorRequest::RequestKind::HighQuality;
    } else if (merged.fullFrameIntent) {
        merged.kind = EditorRequest::RequestKind::FullPreview;
    } else {
        merged.kind = EditorRequest::RequestKind::ParamsOnly;
    }

    merged.monitorOutputRequest = !merged.suppressesMonitorOutput();

    const bool firstProvided =
        first.dirtySource == EditorRequest::DirtySource::ProducerProvided;
    const bool secondProvided =
        second.dirtySource == EditorRequest::DirtySource::ProducerProvided;

    if (firstProvided && secondProvided) {
        merged.dirty = stage::mergeDirtyBlocks(first.dirty, second.dirty);
        merged.dirtySource = EditorRequest::DirtySource::ProducerProvided;
    } else if (firstProvided) {
        merged.dirty = first.dirty;
        merged.dirtySource = EditorRequest::DirtySource::ProducerProvided;
    } else if (secondProvided) {
        merged.dirty = second.dirty;
        merged.dirtySource = EditorRequest::DirtySource::ProducerProvided;
    }

    return merged;
}

uint64_t identityOf(const EditorRequest& request)
{
    uint64_t hash = 14695981039346656037ULL;

    hash = hashCombine(hash, static_cast<uint64_t>(request.kind));
    hash = hashCombine(hash, static_cast<uint64_t>(request.dirtySource));
    hash = hashCombine(hash, request.present ? 1 : 0);
    hash = hashCombine(hash, request.controlOnly ? 1 : 0);
    hash = hashCombine(hash, request.spotPending ? 1 : 0);
    hash = hashCombine(hash, request.viewportMoved ? 1 : 0);
    hash = hashCombine(hash, request.highDetailIntent ? 1 : 0);
    hash = hashCombine(hash, request.fullFrameIntent ? 1 : 0);
    hash = hashCombine(hash, request.fullFrameAdmission ? 1 : 0);
    hash = hashCombine(hash, request.colorHlrWbRequest ? 1 : 0);
    hash = hashCombine(hash, request.monitorOutputRequest ? 1 : 0);
    hash = hashCombine(hash, request.monitorIntent ? 1 : 0);
    hash = hashCombine(hash, request.rawIntent ? 1 : 0);
    hash = hashCombine(hash, request.captureSharpenIntent ? 1 : 0);
    hash = hashCombine(hash, request.imperativeRepaint ? 1 : 0);
    hash = hashCombine(hash, request.transformFamilyIntent ? 1 : 0);
    hash = hashCombine(hash, request.hdrIntent ? 1 : 0);
    hash = hashCombine(hash, request.rgbProcIntent ? 1 : 0);
    hash = hashCombine(hash, request.labChainIntent ? 1 : 0);
    hash = hashCombine(hash, request.autoExposureIntent ? 1 : 0);
    hash = hashCombine(hash, request.localAdjustmentsIntent ? 1 : 0);
    hash = hashCombine(hash, request.baseChainIntent ? 1 : 0);
    hash = hashCombine(hash, request.cropIntent ? 1 : 0);
    hash = hashCombine(hash, request.controlBitRequest ? 1 : 0);
    hash = hashCombine(hash, request.repaintRequest ? 1 : 0);
    hash = hashCombine(hash, request.lumaCurveIntent ? 1 : 0);
    hash = hashCombine(hash, request.labPixelIntent ? 1 : 0);
    hash = hashCombine(hash, request.luminanceIntent ? 1 : 0);
    hash = hashCombine(hash, request.colorIntent ? 1 : 0);
    hash = hashCombine(hash, request.transformRequest ? 1 : 0);
    hash = hashCombine(hash, request.dirty.valid ? 1 : 0);
    hash = hashCombine(hash, request.dirty.forced ? 1 : 0);

    /*
     * `ids` is a std::set, so the iteration order is stable and the identity
     * does not depend on the order the producer named the blocks.
     */
    for (const std::string& id : request.dirty.ids) {
        hash = hashCombine(hash, std::hash<std::string>{}(id));
    }

    return hash;
}

} // namespace

uint64_t EditorRequest::identity() const
{
    return identityOf(*this);
}

bool EditorRequest::suppressesMonitorOutput() const
{
    /*
     * A request suppresses monitor.output only when it is exactly a crop
     * repaint or exactly a bare repaint. `localAdjustmentsIntent` is true for
     * a crop request, so it is not part of the "work outside the shape" test.
     */
    const bool otherWork = spotPending || highDetailIntent || fullFrameIntent
        || colorHlrWbRequest || monitorIntent || rawIntent
        || captureSharpenIntent || transformFamilyIntent || hdrIntent
        || rgbProcIntent || labChainIntent || autoExposureIntent
        || baseChainIntent || controlBitRequest || lumaCurveIntent
        || labPixelIntent || transformRequest || viewportMoved
        || fullFrameAdmission;

    if (cropIntent && !repaintRequest && !otherWork) {
        return true;
    }
    if (repaintRequest && !cropIntent && !otherWork) {
        return true;
    }

    return false;
}

void EditorRequest::markCropTransform()
{
    *this = mergeRequests(*this, make_request(RequestIntent::Transform));
}

void EditorRequest::markViewportMoved()
{
    /*
     * The panning requeue widened the payload without refreshing the kind:
     * the caller patches RepaintOnly to ParamsOnly itself. Keep that shape:
     * only the display facts and the imperative flag change.
     */
    monitorIntent = true;
    monitorOutputRequest = true;
    fullFrameAdmission = true;
    baseChainIntent = true;
    imperativeRepaint = false;
    viewportMoved = true;
}

void EditorRequest::markBaseRefresh(bool spotChanged)
{
    RequestFamilies f;
    f.init = true;
    f.hdr = true;
    f.monitor = true;
    f.spot = spotChanged;

    *this = mergeRequests(*this, requestFromFamilies(f));
}

void EditorRequest::markColorHlrDemosaic()
{
    *this = mergeRequests(*this, make_request(RequestIntent::Demosaic));
}

void EditorRequest::markDemosaicRebuilt()
{
    RequestFamilies f;
    f.init = true;
    f.captureSharpen = true;

    *this = mergeRequests(*this, requestFromFamilies(f));
}

void EditorRequest::markLabPixels(bool luminance, bool color)
{
    /*
     * Only the Lab pixel facts change, and the caller asserts that the
     * planning fields still agree; do not re-derive the kind, because a bare
     * repaint keeps its RepaintOnly classification on the fast path.
     */
    luminanceIntent = luminanceIntent || luminance;
    colorIntent = colorIntent || color;
    labPixelIntent = luminanceIntent || colorIntent;
}

void EditorRequest::absorbDeferredExecution(const stage::DirtyBlocks& blocks)
{
    /*
     * The deferred record carries typed stage ids. Apply the same facts the
     * old execution payload implied, without re-deriving the kind: the caller
     * patches a repaint to ParamsOnly and asserts that it is no longer
     * imperative.
     */
    bool monitorStage = false;
    bool rgbStage = false;
    bool labStage = false;
    bool localStage = false;

    for (const std::string& id : blocks.ids) {
        if (id == "monitor.output") {
            monitorStage = true;
        } else if (id == "rgb.proc") {
            rgbStage = true;
        } else if (id == "lab.tools" || id == "lab.wavelet"
                   || id == "working.trc" || id == "ciecam"
                   || id == "gamut") {
            labStage = true;
        } else if (id == "local.adjustments") {
            localStage = true;
        }
    }

    if (monitorStage) {
        monitorIntent = true;
        monitorOutputRequest = true;
        fullFrameAdmission = true;
        baseChainIntent = true;
    }
    if (rgbStage) {
        rgbProcIntent = true;
        transformFamilyIntent = true;
        localAdjustmentsIntent = true;
    }
    if (labStage) {
        lumaCurveIntent = true;
        labChainIntent = true;
        labPixelIntent = true;
        luminanceIntent = true;
        colorIntent = true;
    }
    if (localStage) {
        autoExposureIntent = true;
        labChainIntent = true;
        localAdjustmentsIntent = true;
    }

    imperativeRepaint = false;
    monitorOutputRequest = !suppressesMonitorOutput();
}

EditorRequest make_request(RequestIntent intent,
                           std::initializer_list<const char*> blocks,
                           stage::StagePath path)
{
    EditorRequest request = requestFromFamilies(familiesFor(intent));

    if (blocks.size() == 0) {
        return request;
    }

    /*
     * A producer that names its blocks knows the seeds, not the execution
     * set. Close over the pipeline edges and add the path residents, exactly
     * as derive_dirty_blocks() does for the parameter form, so the coordinator
     * can use the set as an execution set.
     */
    stage::DirtyBlocks seed;
    seed.valid = true;
    for (const char* id : blocks) {
        seed.ids.insert(id);
        seed.blocks.push_back(id);
    }

    stage::DirtyBlocks closed = stage::closeOverGraph(seed, path);

    for (const std::string& id : stage::resident_blocks(path)) {
        closed.ids.insert(id);
        if (std::find(closed.blocks.begin(), closed.blocks.end(), id)
                == closed.blocks.end()) {
            closed.blocks.push_back(id);
        }
    }

    closed.reason.clear();
    closed.forced = false;
    request.dirty = closed;
    request.dirtySource = EditorRequest::DirtySource::ProducerProvided;
    return request;
}

EditorRequest make_request(RequestIntent intent)
{
    return make_request(intent, {}, stage::StagePath::Preview);
}

std::string producerEventKey(const EditorRequest& request,
                             const std::string& historyMsg)
{
    return "typed-" + std::to_string(request.identity()) + "-" + historyMsg;
}

stage::RequestKind crop_request_kind(EditorRequest::RequestKind previewKind,
                                     bool reallocation)
{
    if (reallocation) {
        return stage::RequestKind::FullCrop;
    }

    if (previewKind == stage::RequestKind::HighQuality) {
        return stage::RequestKind::HighQuality;
    }
    if (previewKind == stage::RequestKind::FullPreview) {
        return stage::RequestKind::FullCrop;
    }

    return previewKind;
}

bool sameHighlightRecovery(const procparams::ToneCurveParams& a,
                           const procparams::ToneCurveParams& b)
{
    return a.hrenabled == b.hrenabled
        && a.method == b.method
        && a.hlbl == b.hlbl
        && a.hlth == b.hlth
        && a.clampOOG == b.clampOOG;
}

ViewportEligibility viewportEligibility(const EditorRequest& request)
{
    /*
     * An absent request never reaches admission: the coordinator's loop stops
     * when the accumulator is empty. Classify it as unsupported rather than
     * admitting a request that names no work.
     */
    if (!request.present) {
        return ViewportEligibility::Unsupported;
    }

    /*
     * The base chain and the accurate path need the full-frame rebuild, and
     * they take precedence over the unsupported classification so the reason
     * stays stable.
     */
    if (request.baseChainIntent || request.highDetailIntent) {
        return ViewportEligibility::Base;
    }

    /*
     * A producer that reports its own dirty blocks is held to the same
     * envelope the request families describe. The set is an execution set:
     * residents and forward closure are included, so a delta seed also names
     * its downstream nodes.
     */
    if (!request.derivesFromParameters()) {
        if (!request.dirty.valid) {
            return ViewportEligibility::Unsupported;
        }
        for (const std::string& id : request.dirty.ids) {
            if (viewportDeltaBlock(id)) {
                continue;
            }
            return viewportBaseBlock(id) ? ViewportEligibility::Base
                                         : ViewportEligibility::Unsupported;
        }
    }

    /* A control-only request updates parameters without rendering. */
    if (request.controlBitRequest) {
        return ViewportEligibility::Unsupported;
    }

    return ViewportEligibility::Admit;
}

const char* viewportEligibilityReason(ViewportEligibility eligibility)
{
    switch (eligibility) {
    case ViewportEligibility::Admit:
        return nullptr;
    case ViewportEligibility::Base:
        return "base-request";
    case ViewportEligibility::Unsupported:
        return "unsupported-request";
    }
    assert(false && "unknown viewport eligibility");
    return "unsupported-request";
}

namespace
{

/*
 * The parameter vetoes of viewport admission, in the order the hand-written
 * gate applied them so a profile log keeps its reason.
 */
const char* viewportParamFallback(const ViewportAdmissionInputs& inputs,
                                  const EditorRequest& request)
{
    const procparams::ProcParams& current = inputs.current;
    const procparams::ProcParams& last = inputs.lastFullPass;

    if (current.toneCurve.autoexp || last.toneCurve.autoexp) {
        return "auto-exposure";
    }
    if (current.toneCurve.histmatching || last.toneCurve.histmatching
            || current.toneCurve.fromHistMatching
            || last.toneCurve.fromHistMatching) {
        return "histogram-matching";
    }
    if ((current.blackwhite.enabled && current.blackwhite.autoc)
            || (last.blackwhite.enabled && last.blackwhite.autoc)) {
        return "auto-black-white";
    }
    if ((current.colorToning.enabled && current.colorToning.autosat)
            || (last.colorToning.enabled && last.colorToning.autosat)) {
        return "auto-colour-toning";
    }
    if (current.colorappearance.enabled) {
        return "cie";
    }
    if (current.locallab.enabled) {
        return "local-lab";
    }
    if (current.labCurve.contrast != 0) {
        return "lab-contrast";
    }

    // Active full-frame filters would either be skipped here or re-applied
    // differently from the normal path, so leave them to the full pipeline.
    if (current.fattal.enabled || current.dehaze.enabled) {
        return "hdr-processing";
    }
    if (current.spot.enabled && !current.spot.entries.empty()) {
        return "spot-removal";
    }

    if (current.raw != last.raw) {
        return "raw";
    }
    if (current.wb != last.wb) {
        return "white-balance";
    }
    if (current.icm != last.icm) {
        return "color-management";
    }
    if (current.coarse != last.coarse) {
        return "coarse-transform";
    }
    // The transform-stage parameters stay rejected until the ROI parity of
    // Crop::update() with a live transform change is validated on the GUI.
    if (current.commonTrans != last.commonTrans) {
        return "common-transform";
    }
    if (current.rotate != last.rotate) {
        return "rotate";
    }
    if (current.distortion != last.distortion) {
        return "distortion";
    }
    if (current.lensProf != last.lensProf) {
        return "lens-profile";
    }
    if (current.perspective != last.perspective) {
        return "perspective";
    }
    if (current.gradient != last.gradient) {
        return "gradient";
    }
    if (current.pcvignette != last.pcvignette) {
        return "post-crop-vignette";
    }
    if (current.cacorrection != last.cacorrection) {
        return "ca-correction";
    }
    if (current.vignetting != last.vignetting) {
        return "vignetting";
    }
    if (current.spot != last.spot) {
        return "spot";
    }
    if (current.filmNegative != last.filmNegative) {
        return "film-negative";
    }
    if (current.dehaze != last.dehaze) {
        return "dehaze";
    }
    if (current.fattal != last.fattal) {
        return "fattal";
    }
    if (current.retinex != last.retinex) {
        return "retinex";
    }
    if (current.pdsharpening != last.pdsharpening) {
        return "capture-sharpening";
    }
    if (current.dirpyrDenoise != last.dirpyrDenoise) {
        return "denoise";
    }
    if (current.cg != last.cg) {
        return "gamut-compression";
    }
    if (current.locallab != last.locallab) {
        return "local-lab-params";
    }
    if (!sameHighlightRecovery(current.toneCurve, last.toneCurve)) {
        return "highlight-recovery";
    }
    if (current.toneCurve.autoexp != last.toneCurve.autoexp
            || current.toneCurve.histmatching
                != last.toneCurve.histmatching
            || current.blackwhite.autoc != last.blackwhite.autoc
            || current.colorToning.autosat != last.colorToning.autosat) {
        return "auto-flags";
    }

    // The fast path rebuilds only the parent LUTs the request asks for. If a
    // LUT input changed without its family, fall back instead of rendering
    // with stale curves.
    const procparams::ProcParams& lastLut = inputs.lastLutPass;

    if ((current.toneCurve != lastLut.toneCurve
            || current.rgbCurves != lastLut.rgbCurves
            || current.colorToning != lastLut.colorToning
            || current.blackwhite != lastLut.blackwhite)
            && !request.rgbProcIntent) {
        return "tone-curve-mask";
    }
    if (current.labCurve != lastLut.labCurve && !request.lumaCurveIntent) {
        return "lab-curve-mask";
    }
    if (current.wavelet != lastLut.wavelet && !request.labPixelIntent) {
        return "wavelet-mask";
    }

    return nullptr;
}

} // namespace

const char* viewportFastPathFallback(const EditorRequest& request,
                                     const ViewportAdmissionInputs& inputs)
{
    if (!inputs.state.fastPathEnabled) {
        return "switch-off";
    }
    if (!inputs.state.fullStateComplete) {
        return "no-complete-full-state";
    }
    if (inputs.state.highDetailNeeded != inputs.state.highDetailComplete) {
        return "high-detail-transition";
    }
    if (inputs.state.highDetailNeeded
            && (!inputs.state.highDetailPreprocessComputed
                || !inputs.state.highDetailRawComputed)) {
        return "high-detail-pending";
    }

    const ViewportEligibility eligibility = viewportEligibility(request);
    if (eligibility != ViewportEligibility::Admit) {
        return viewportEligibilityReason(eligibility);
    }

    return viewportParamFallback(inputs, request);
}

ViewportLutSelection viewportLutSelection(const EditorRequest& request)
{
    ViewportLutSelection selection;

    /*
     * A crop repaint rebuilds the tone LUTs and lumacurve from the zero
     * histogram, because Crop::update() consumes both.
     */
    selection.toneCurves = request.rgbProcIntent || request.cropIntent;
    selection.labCurves = request.lumaCurveIntent;
    selection.lumaCurveFromCrop = request.cropIntent;

    /* The wavelet LUT is shared by the Lab pixel family and auto exposure. */
    selection.wavelet = request.labPixelIntent || request.autoExposureIntent;

    return selection;
}

ViewportCropSelection viewportCropSelection(const EditorRequest& request)
{
    ViewportCropSelection selection;

    selection.crop = request.cropIntent;
    selection.repaint = request.repaintRequest;
    selection.autoExposure = request.autoExposureIntent;
    selection.transform = request.transformRequest;
    selection.rgbProc = request.rgbProcIntent;
    selection.lumaCurve = request.lumaCurveIntent;
    selection.luminance = request.luminanceIntent;
    selection.color = request.colorIntent;

    return selection;
}

void resolveCropDependencies(ViewportCropSelection& selection)
{
    if (selection.lumaCurve || selection.autoExposure) {
        selection.luminance = true;
        selection.color = true;
    }
}

EditorRequest coalesce(const EditorRequest& first, const EditorRequest& second)
{
    return mergeRequests(first, second);
}

} // namespace rtengine
