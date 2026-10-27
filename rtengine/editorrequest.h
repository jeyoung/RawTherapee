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

#pragma once

#include "stage/stage_dirty_blocks.h"
#include "stage/stage_render_state.h"

#include <cstdint>
#include <initializer_list>
#include <string>

namespace rtengine::procparams
{
struct ToneCurveParams;
class ProcParams;
} // namespace rtengine::procparams

namespace rtengine
{

/*
 * Typed request from an editor to the image pipeline.
 *
 * This is the coordinator's planning and execution input. A producer names an
 * intent, or reports the blocks it dirtied, and the request carries the
 * resulting facts. No integer mask exists between a producer and the plan.
 *
 * The type has these parts:
 *
 * - `kind`: the request kind the preview plan consumes. HighQuality wins,
 *   then a whole-path rebuild, then RepaintOnly, otherwise ParamsOnly.
 * - dirty blocks: either supplied by a producer that already knows the typed
 *   block set, or an explicit "derive from the parameters" state.
 * - request facts: the boolean inputs the render state, the work order, the
 *   plan and the stage bodies need and that are not fields of `ProcParams`.
 * - `identity()`: a content hash of every planning and payload field, used to
 *   decide whether two coalesced requests are indistinguishable.
 *
 * The struct is a plain value and is safe to copy and compare.
 */
struct EditorRequest {
    /* Namespace alias so callers can write EditorRequest::RequestKind. */
    using RequestKind = stage::RequestKind;

    /*
     * Where the dirty-block set comes from.
     *
     * DeriveFromParameters is the default: the coordinator compares the
     * parameter fingerprints and the render state against the last completed
     * pass. ProducerProvided is for a producer that reports the blocks it
     * dirtied directly; the coordinator then merges that set with the
     * render-state delta instead of deriving it from the parameters.
     */
    enum class DirtySource : int32_t {
        DeriveFromParameters = 0,
        ProducerProvided = 1
    };

    /* What the request asks for; the plan consumes this field. */
    RequestKind kind = RequestKind::ParamsOnly;

    /* Source of the dirty-block set, see DirtySource. */
    DirtySource dirtySource = DirtySource::DeriveFromParameters;

    /*
     * Blocks the producer reported. Meaningful only when dirtySource is
     * ProducerProvided.
     */
    stage::DirtyBlocks dirty;

    /*
     * Spot work is requested. The coordinator narrows it at pass time with the
     * live spot parameters (enabled and at least one entry).
     */
    bool spotPending = false;

    /*
     * The viewport moved since the last pass. This is the coordinator's
     * panning heuristic, not a request family: it says buffers may be reused,
     * not that a repaint must happen now.
     */
    bool viewportMoved = false;

    /* The request asks for the accurate high-detail chain. */
    bool highDetailIntent = false;

    /*
     * The request asks for the whole path to be rebuilt rather than a
     * parameter delta. The coordinator also sets it when it widens a request
     * to a base refresh.
     */
    bool fullFrameIntent = false;

    /*
     * The request alone admits the full-frame path: it names every base
     * family, or it names the display sink.
     */
    bool fullFrameAdmission = false;

    /*
     * The pass carries a white-balance request. The Color/Coloropp
     * highlight-recovery path forces DEMOSAIC for it; the work order consumes
     * this fact directly.
     */
    bool colorHlrWbRequest = false;

    /*
     * The pass renders `monitor.output`. A request that is exactly a crop
     * repaint or exactly a bare repaint suppresses the render.
     */
    bool monitorOutputRequest = true;

    /*
     * The request names the display/transform family. This is narrower than
     * monitorOutputRequest: a spot or white-balance request renders
     * monitor.output but does not name the monitor transform, and the crop
     * render gate and the output-profile refresh follow this fact.
     */
    bool monitorIntent = false;

    /*
     * The request names the sensor demosaic sub-step. The raw body runs as one
     * node for demosaic, capture sharpening and retinex buffer preparation;
     * this fact decides whether the demosaic itself is re-run, independently of
     * whether the node was entered.
     */
    bool rawIntent = false;

    /*
     * The request names the capture-sharpening sub-step. Capture sharpening
     * rewrites the demosaiced planes in place, so the raw body consults this
     * beside rawIntent.
     */
    bool captureSharpenIntent = false;

    /*
     * The request is exactly a bare repaint. A widened request turns a
     * RepaintOnly request into ParamsOnly.
     */
    bool imperativeRepaint = false;

    /*
     * The request carries the transform family. The transform node applies the
     * pre-RGB directional-pyramid equaliser for this family only.
     */
    bool transformFamilyIntent = false;

    /* The request asks for the HDR/dehaze sub-path. */
    bool hdrIntent = false;

    /* The request applies the RGB processing chain. */
    bool rgbProcIntent = false;

    /* The request applies the Lab chain. */
    bool labChainIntent = false;

    /* The request recomputes auto exposure. */
    bool autoExposureIntent = false;

    /* The request may recompute the local-adjustment regions. */
    bool localAdjustmentsIntent = false;

    /*
     * The request asks for the full-frame base or a display sink and
     * therefore cannot be served by the viewport fast path.
     */
    bool baseChainIntent = false;

    /* The request repaints the visible crop. */
    bool cropIntent = false;

    /*
     * The request carries the control operation, with or without other work.
     * A control-only request updates parameters without rendering.
     */
    bool controlBitRequest = false;

    /* The request carries an imperative repaint bit. */
    bool repaintRequest = false;

    /* The request rebuilds the Lab tone curve. */
    bool lumaCurveIntent = false;

    /* The request applies the Lab pixel tools. */
    bool labPixelIntent = false;

    /*
     * The exact Lab pixel families. The crop bodies read luminance and colour
     * separately, so the crop payload preserves which one the request named.
     */
    bool luminanceIntent = false;
    bool colorIntent = false;

    /*
     * The request asks for the geometry transform without the other
     * transform-family families. The viewport crop subset carries it when
     * post-crop vignetting derives a transform.
     */
    bool transformRequest = false;

    /*
     * The request updates parameters without rendering. An absent request is
     * treated the same way.
     */
    bool controlOnly = true;

    /*
     * Whether this value is part of the accumulated request. An empty
     * accumulator has `present == false`; the coordinator's loop stops when
     * the accumulator is empty.
     */
    bool present = false;

    /* Stable content identity of every planning and payload field. */
    uint64_t identity() const;

    /* Whether the dirty set comes from the producer rather than parameters. */
    bool derivesFromParameters() const
    {
        return dirtySource == DirtySource::DeriveFromParameters;
    }

    /*
     * Whether this request is exactly a crop repaint or exactly a bare
     * repaint, the two shapes that suppress monitor.output.
     */
    bool suppressesMonitorOutput() const;

    /*
     * Execution-payload augmentations of one preview pass.
     *
     * Each method names the intent of a widening the hand-written coordinator
     * applied to its `todo` integer. They set the same facts the widening
     * implied, so no execution site manipulates a request family by hand.
     */

    /* A post-crop-vignetting change needs the transform family. */
    void markCropTransform();

    /* The viewport moved; the pass may reuse buffers. */
    void markViewportMoved();

    /* Refresh the whole-frame base after a viewport-state mismatch. */
    void markBaseRefresh(bool spotChanged);

    /* A Color/Coloropp white-balance request needs fresh demosaic planes. */
    void markColorHlrDemosaic();

    /* The demosaic body re-ran during this pass. */
    void markDemosaicRebuilt();

    /* A viewport crop dependency needs the Lab pixel tools. */
    void markLabPixels(bool luminance, bool color);

    /* Merge the typed deferred-analysis stages into this request. */
    void absorbDeferredExecution(const stage::DirtyBlocks& blocks);
};

/*
 * What the viewport fast path can do with a request, from the typed request
 * alone.
 */
enum class ViewportEligibility : int32_t {
    /* The request touches only inputs the fast path rebuilds. */
    Admit = 0,
    /* The request needs the full-frame base chain or the display sink. */
    Base = 1,
    /* The request names no rendering family the fast path supports. */
    Unsupported = 2
};

/* Classify a request for viewport admission. See ViewportEligibility. */
ViewportEligibility viewportEligibility(const EditorRequest& request);

/* Fallback reason for a non-admitted classification, or nullptr for Admit. */
const char* viewportEligibilityReason(ViewportEligibility eligibility);

/*
 * Coordinator state that takes part in admission but is not request data.
 */
struct ViewportAdmissionState {
    /* The RT_VIEWPORT_FAST kill switch is on. */
    bool fastPathEnabled = true;
    /* A complete full-frame pass has published its snapshot. */
    bool fullStateComplete = false;
    /* The request needs the high-detail chain. */
    bool highDetailNeeded = false;
    /* The high-detail decision of the last completed full-frame pass. */
    bool highDetailComplete = false;
    /* The high-detail preprocess and demosaic buffers are current. */
    bool highDetailPreprocessComputed = false;
    bool highDetailRawComputed = false;
};

/*
 * The parameter sets admission compares.
 */
struct ViewportAdmissionInputs {
    const procparams::ProcParams& current;
    const procparams::ProcParams& lastFullPass;
    const procparams::ProcParams& lastLutPass;
    ViewportAdmissionState state;
};

/*
 * Viewport fast-path admission: nullptr when the request may be served by the
 * fast path, otherwise the stable fallback reason.
 */
const char* viewportFastPathFallback(const EditorRequest& request,
                                     const ViewportAdmissionInputs& inputs);

/*
 * Whether the highlight-recovery subset of the tone curve is equal.
 */
bool sameHighlightRecovery(const procparams::ToneCurveParams& a,
                           const procparams::ToneCurveParams& b);

/*
 * The parent LUTs a viewport request rebuilds.
 */
struct ViewportLutSelection {
    /* complexCurve, the RGB curves, colour toning and the B&W curves. */
    bool toneCurves = false;
    /* Lumacurve, clcurve and the signed Lab curves. */
    bool labCurves = false;
    /* Lumacurve only, from the zero histogram. */
    bool lumaCurveFromCrop = false;
    /* wavclCurve. */
    bool wavelet = false;

    /* Whether complexLCurve must run. */
    bool rebuildsLumaCurve() const
    {
        return labCurves || lumaCurveFromCrop;
    }

    /* Whether any parent LUT must be rebuilt. */
    bool any() const
    {
        return toneCurves || rebuildsLumaCurve() || wavelet;
    }

    /* The tone/RGB/toning/B&W family alone. */
    static ViewportLutSelection toneOnly()
    {
        ViewportLutSelection selection;
        selection.toneCurves = true;
        return selection;
    }

    /* The whole Lab curve family alone. */
    static ViewportLutSelection labOnly()
    {
        ViewportLutSelection selection;
        selection.labCurves = true;
        return selection;
    }

    /* The wavelet curve alone. */
    static ViewportLutSelection waveletOnly()
    {
        ViewportLutSelection selection;
        selection.wavelet = true;
        return selection;
    }
};

/* The parent LUTs one request needs rebuilt, from its typed request data. */
ViewportLutSelection viewportLutSelection(const EditorRequest& request);

/*
 * The work one viewport fast-path hit renders on the visible crops.
 */
struct ViewportCropSelection {
    /* The crop rectangle changed. */
    bool crop = false;
    /* Repaint without new image work. */
    bool repaint = false;
    /* Auto exposure. */
    bool autoExposure = false;
    /* A derived or requested geometry transform. */
    bool transform = false;
    /* The RGB curve family. */
    bool rgbProc = false;
    /* The Lab tone curve. */
    bool lumaCurve = false;
    /* The Lab pixel tools, kept apart because the crop carries the exact
     * families the request named. */
    bool luminance = false;
    bool color = false;

    /* Whether the request names either Lab pixel tool. */
    bool labPixels() const
    {
        return luminance || color;
    }

    /* A luminance-curve or auto-exposure edit needs the Lab pixel tools. */
    bool labPixelsImplied() const
    {
        return labPixels() || lumaCurve || autoExposure;
    }
};

/* The crop-render selection of one request, from its typed request data. */
ViewportCropSelection viewportCropSelection(const EditorRequest& request);

/* Close the selection over the crop's Lab pixel dependency, in place. */
void resolveCropDependencies(ViewportCropSelection& selection);

/*
 * Typed producer API.
 *
 * A GUI tool that carries planning information names the operation it asks
 * for. `RequestIntent` is the closed set of operations the tools and the
 * static event registry express; `make_request()` turns one into an
 * `EditorRequest` whose kind, facts and dirty-block source are the planning
 * inputs.
 *
 * A tool that knows the exact blocks it dirties passes them to the second
 * form. They replace the parameter-fingerprint comparison for that request;
 * the set is closed over the pipeline and joined with the path residents
 * before it is stored.
 */
enum class RequestIntent : int32_t {
    /* A destructive raw reload without the raw preprocessing families. */
    AllNoRaw = 0,
    /* A demosaic family change. */
    Demosaic = 1,
    /* The HDR/dehaze family. */
    Hdr = 2,
    /* Spot add/remove inside the base pass. */
    SpotAdjust = 3,
    /* The geometry family. */
    Transform = 4,
    /* A white-balance change. */
    WhiteBalance = 5,
    /* Repaint the current image without new work. */
    RepaintOnly = 6,
    /* Update parameters without rendering. */
    ControlOnly = 7,
    /* Gamut compression family. */
    Compr = 8,
    /* Capture sharpening family. */
    CaptureSharpen = 9,
    /* Dark-frame/flat-field family. */
    DarkFrame = 10,
    /* Whole preview including the monitor sink. */
    First = 11,
    /* Auto-exposure family. */
    AutoExposure = 12,
    /* Luminance-curve family. */
    LuminanceCurve = 13,
    /* Directional-pyramid equaliser family. */
    DirPyrEqualizer = 14,
    /* RGB-curve family. */
    RgbCurve = 15,
    /* Sharpening family. */
    Sharpening = 16,
    /* Retinex family with the base path. */
    Retinex = 17,
    /* Repaint the visible crop. */
    Crop = 18,
    /* Render the monitor sink without any other family. */
    Monitor = 19,
    /* No request at all: the event carries no preview work. */
    NoRequest = 20,
    /* The accurate high-detail chain alone. */
    HighQuality = 21
};

/* Intent to typed request, with no producer-provided dirty blocks. */
EditorRequest make_request(RequestIntent intent);

/*
 * Intent to typed request with a producer-provided dirty-block seed.
 */
EditorRequest make_request(RequestIntent intent,
                           std::initializer_list<const char*> blocks,
                           stage::StagePath path = stage::StagePath::Preview);

/*
 * Dedup key of a typed producer event. Two events that differ in any typed
 * fact or history message stay distinct even when they plan alike.
 */
std::string producerEventKey(const EditorRequest& request,
                             const std::string& historyMsg);

/*
 * Request kind of one crop update.
 */
stage::RequestKind crop_request_kind(EditorRequest::RequestKind previewKind,
                                     bool reallocation);

/*
 * Coalesce two requests into one.
 *
 * The result keeps the union of every input fact and the strongest kind. The
 * derived facts are recomputed from the union so they describe the request as
 * a whole:
 *
 * - `fullFrameAdmission` is the union of the inputs: a request can only admit
 *   the full frame if one of its parts already did;
 * - `suppressesMonitorOutput()` is re-evaluated, so two suppressing inputs
 *   produce a rendering union;
 * - `imperativeRepaint` survives only when both inputs were bare repaints.
 *
 * Producer-provided dirty sets are unioned; otherwise both inputs derive from
 * the parameters.
 */
EditorRequest coalesce(const EditorRequest& first, const EditorRequest& second);

} // namespace rtengine
