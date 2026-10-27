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
 * This is the coordinator's planning input. It replaces the legacy `M_*`
 * mask as the thing that decides *what kind of work* a preview request asks
 * for. The mask itself is still carried, as an opaque execution gate for the
 * block bodies that have not been converted yet; no planning decision reads
 * the raw mask. The adapter derives the mask-shaped planning facts below, so
 * every consumer reads a named fact instead.
 *
 * The type has these parts:
 *
 * - `kind`: the request kind the preview plan consumes. It has the same
 *   precedence the coordinator used to derive from the mask: HighQuality,
 *   then a whole-path rebuild, then RepaintOnly, otherwise ParamsOnly.
 * - dirty blocks: either supplied by a producer that already knows the typed
 *   block set, or an explicit "derive from the parameters" state. The legacy
 *   adapter always asks for the derivation, because the GUI has no typed
 *   block information yet.
 * - request facts: the boolean inputs the render state, the work order and
 *   the plan need and that are not fields of `ProcParams`. Facts that depend
 *   on the legacy mask are named and derived in one place; the coordinator
 *   never re-reads the mask for them.
 * - `identity()`: a content hash of every planning field above, used to
 *   decide whether two coalesced requests are planning-equivalent. The raw
 *   legacy mask is excluded, but every planning fact derived from it is
 *   included, so two masks that plan differently never share an identity.
 *
 * The struct is a plain value and is safe to copy and compare.
 */
struct EditorRequest {
    /* Namespace alias so callers can write EditorRequest::RequestKind. */
    using RequestKind = stage::RequestKind;

    /*
     * Where the dirty-block set comes from.
     *
     * DeriveFromParameters is the only source in this phase: the coordinator
     * compares the parameter fingerprints and the render state against the
     * last completed pass. ProducerProvided is for a future producer that
     * reports the blocks it dirtied directly; the coordinator then merges
     * that set with the render-state delta instead of deriving it from the
     * parameters.
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
     * ProducerProvided; the legacy adapter leaves this empty.
     */
    stage::DirtyBlocks dirty;

    /*
     * Spot work is requested. The adapter sets this from `M_SPOT`; the
     * coordinator narrows it at pass time with the live spot parameters
     * (enabled and at least one entry), because the adapter sees `nextParams`
     * before it has been published as the live `params`.
     */
    bool spotPending = false;

    /*
     * The viewport moved since the last pass. This is the coordinator's
     * panning heuristic, not a mask bit: it says buffers may be reused, not
     * that a repaint must happen now.
     */
    bool viewportMoved = false;

    /*
     * The request asks for the accurate high-detail chain (`M_HIGHQUAL`).
     * It is kept as a fact as well as a kind because the plan and the work
     * order both consult it.
     */
    bool highDetailIntent = false;

    /*
     * The request asks for the whole path to be rebuilt rather than a
     * parameter delta. The adapter sets this from the absolute reload bits
     * `M_INIT`, `M_LINDENOISE` and `M_HDR`; the coordinator also sets it when
     * it widens a request to a base refresh.
     */
    bool fullFrameIntent = false;

    /*
     * The mask alone admits the full-frame path: either every `ALL` bit is
     * set or `M_MONITOR` is present. The coordinator ORs this with its
     * panning and high-detail inputs; it is one of the mask-shaped planning
     * facts and is recomputed by setLegacyGateMask().
     */
    bool fullFrameAdmission = false;

    /*
     * The pass carries a white-balance request (`M_WB`). The Color/Coloropp
     * highlight-recovery path forces DEMOSAIC for it; the work order consumes
     * this fact directly. Recompute it with setLegacyGateMask().
     */
    bool colorHlrWbRequest = false;

    /*
     * The pass renders `monitor.output`. The legacy predicate is "the mask is
     * neither exactly `M_CROP` nor exactly `M_MINUPDATE`, or it carries
     * `M_MONITOR`". Recompute it with setLegacyGateMask().
     */
    bool monitorOutputRequest = false;

    /*
     * The mask is exactly `M_MINUPDATE`, the only form in which a repaint is
     * imperative. A widened mask turns a RepaintOnly request into ParamsOnly.
     * Recompute it with setLegacyGateMask().
     */
    bool imperativeRepaint = false;

    /*
     * The request carries the transform family (`M_HDR`, `M_TRANSFORM`,
     * `M_RGBCURVE`, `M_INIT` or `M_LINDENOISE`). The transform node applies
     * the pre-RGB directional-pyramid equaliser for this family only, exactly
     * as the hand-written pass did. Recompute it with setLegacyGateMask().
     */
    bool transformFamilyIntent = false;

    /* The request asks for the HDR/dehaze sub-path (`M_HDR`). */
    bool hdrIntent = false;

    /* The request applies the RGB processing chain (`M_RGBCURVE`). */
    bool rgbProcIntent = false;

    /*
     * The request applies the Lab chain (`M_LUMINANCE`, `M_COLOR` or
     * `M_AUTOEXP`). The Lab node bodies decide whether to touch pixels with
     * this fact; the graph plan decides whether the node is entered.
     */
    bool labChainIntent = false;

    /* The request recomputes auto exposure (`M_AUTOEXP`). */
    bool autoExposureIntent = false;

    /*
     * The request may recompute the local-adjustment regions (`M_AUTOEXP`,
     * `M_RGBCURVE` or `M_CROP`).
     */
    bool localAdjustmentsIntent = false;

    /*
     * The request carries a bit of the base chain: `M_PREPROC`, `M_RAW`,
     * `M_WB`, `M_INIT`, `M_LINDENOISE`, `M_MONITOR`, `M_TRANSFORM`,
     * `M_CSHARP`, `M_RETINEX` or `M_BLURMAP`. These are the bits that ask for
     * the full-frame base or a display sink and therefore cannot be served by
     * the viewport fast path. `M_HDR` is deliberately not included: it is one
     * of the over-approximated bits a GUI event may carry even when the HDR
     * inputs did not change. Recompute it with setLegacyGateMask().
     */
    bool baseChainIntent = false;

    /* The request repaints the visible crop (`M_CROP`). */
    bool cropIntent = false;

    /*
     * The request carries the control bit `M_VOID`, with or without other
     * work. The fast path has nothing to render for a request that asks to
     * update parameters without a repaint.
     */
    bool controlBitRequest = false;

    /*
     * The request carries `M_MINUPDATE`. It is the imperative repaint only
     * when it is the whole mask (see imperativeRepaint), but a coalesced
     * request can carry it beside other work, and the crop subset follows the
     * bit rather than the imperative.
     */
    bool repaintRequest = false;

    /* The request rebuilds the Lab tone curve (`M_LUMACURVE`). */
    bool lumaCurveIntent = false;

    /* The request applies the Lab pixel tools (`M_LUMINANCE` or `M_COLOR`). */
    bool labPixelIntent = false;

    /*
     * The exact Lab pixel bits. The crop bodies read M_LUMINANCE and M_COLOR
     * separately, so the crop payload preserves which one the request named.
     * They are payload-shaping facts like the mask itself and are therefore
     * excluded from identity(); every planning decision uses their union,
     * labPixelIntent.
     */
    bool luminanceIntent = false;
    bool colorIntent = false;

    /*
     * The request asks for the geometry transform (`M_TRANSFORM`) without the
     * other transform-family bits. The viewport crop subset carries it when
     * post-crop vignetting derives a transform, and it is a dependency of the
     * crop render rather than a request kind. Recompute it with
     * setLegacyGateMask().
     */
    bool transformRequest = false;

    /*
     * The request carries no bit outside the control bit `M_VOID`, so it
     * updates parameters without rendering. A request with an empty legacy
     * mask is treated the same way.
     */
    bool controlOnly = true;

    /*
     * Whether this value is part of the accumulated request. An empty
     * accumulator has `present == false`; the coordinator's loop stops when
     * the accumulator is empty, exactly as the integer mask loop stopped on
     * zero.
     */
    bool present = false;

    /*
     * Opaque legacy execution payload.
     *
     * This is the mask the adapter received, ORed across coalesced requests
     * and widened whenever the coordinator adds an execution bit. It is
     * consumed only by the unconverted block bodies, by cache invalidation and
     * by the deferred-analysis queue. It is not read by any planning
     * decision, and it is excluded from identity(); the planning facts derived
     * from it are included.
     */
    int legacyGateMask = 0;

    /*
     * Stable content identity of the planning fields, excluding the raw
     * legacy mask. Equal values mean the two requests are indistinguishable to
     * the planner and may be coalesced without losing planning information.
     */
    uint64_t identity() const;

    /*
     * Set the legacy execution payload and recompute every mask-shaped
     * planning fact from it.
     *
     * This is the only place the coordinator may change `legacyGateMask`: an
     * augmentation such as a base refresh adds execution bits, and the named
     * facts must follow. The raw mask interpretation stays in this unit.
     */
    void setLegacyGateMask(int mask);

    /* Whether the dirty set comes from the producer rather than parameters. */
    bool derivesFromParameters() const
    {
        return dirtySource == DirtySource::DeriveFromParameters;
    }

    /*
     * Recompute every mask-derived planning field from the current legacy
     * gate mask, exactly as from_legacy_mask() would. Used after an
     * execution-payload augmentation: the post-crop-vignetting transform
     * augmentation adds M_HDR and M_SPOT through TRANSFORM, so kind,
     * fullFrameIntent and spotPending change too. Sites that deliberately
     * narrow a repaint patch the kind themselves and must not call this.
     */
    void refreshFromLegacyMask();

    /*
     * The opaque execution payload, for the block bodies, cache invalidation
     * and the deferred-analysis queue. Viewport admission does not read it.
     */
    int executionPayload() const
    {
        return legacyGateMask;
    }
};

/*
 * What the viewport fast path can do with a request, from the typed request
 * alone.
 *
 * The classification is the request-family half of admission; the parameter
 * vetoes are applied separately by viewportFastPathFallback(). The values keep
 * the legacy fallback reasons so a profile log stays comparable across the
 * retirement.
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

/*
 * Fallback reason for a non-admitted classification, or nullptr for Admit.
 * The strings are the ones the mask-based gate reported.
 */
const char* viewportEligibilityReason(ViewportEligibility eligibility);

/*
 * Coordinator state that takes part in admission but is not request data.
 *
 * The high-detail fields are the decision and what has been computed for it;
 * they decide whether the cached full-frame analysis can serve the request.
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
 *
 * `current` is the request's parameters, `lastFullPass` the inputs of the last
 * completed full-frame pass (`viewportFullParams`) and `lastLutPass` the
 * parameters whose parent LUTs were last rebuilt (`viewportLutParams`).
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
 *
 * The request family is classified from `request` (see viewportEligibility());
 * the parameter vetoes below reproduce the hand-written gate exactly. They
 * stay because each names an input the cached full-frame analysis cannot
 * serve: auto exposure and histogram matching change the tone LUT's histogram,
 * auto black-and-white and auto colour toning recompute their own LUTs from
 * the image, CIE and Local Lab are full-frame chains, non-zero Lab contrast
 * needs the cached Lab histogram, the full-frame filters would be re-applied
 * differently, and a base input that differs from the snapshot invalidates the
 * cached analysis. A LUT input that changed without a request for its family
 * would leave a stale parent LUT, so that is vetoed too.
 */
const char* viewportFastPathFallback(const EditorRequest& request,
                                     const ViewportAdmissionInputs& inputs);

/*
 * Whether the highlight-recovery subset of the tone curve is equal.
 *
 * Only these fields feed the highlight reconstruction that the fast path
 * reproduces from the cached base; the rest of the tone curve is a parent LUT.
 */
bool sameHighlightRecovery(const procparams::ToneCurveParams& a,
                           const procparams::ToneCurveParams& b);

/*
 * The parent LUTs a viewport request rebuilds.
 *
 * The full path rebuilds a parent LUT when its parameter family is requested.
 * The fast path must reproduce exactly that, and one family needs two levels:
 * a crop repaint rebuilds the tone LUTs and lumacurve -- the crop's zero
 * histogram makes lumacurve independent of the cached full-frame histogram --
 * but not clcurve or the signed Lab curves, which need the real analysis. A
 * luminance-curve request rebuilds all three Lab LUTs. `labCurves` therefore
 * means the whole Lab family and `lumaCurveFromCrop` means lumacurve alone.
 *
 * The names are the dependencies, not the bits: `toneCurves` is the
 * complexCurve/RGBCurve/toning/B&W group and `wavelet` is wavclCurve. The two
 * named constructors serve the full-path sites, which rebuild one family for
 * their own block and only need the snapshot updated.
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

/*
 * The parent LUTs one request needs rebuilt, from its typed request data.
 *
 * `toneCurves` covers the tone/RGB/toning/B&W group, `labCurves` the whole Lab
 * family, `lumaCurveFromCrop` lumacurve from a crop repaint, and `wavelet`
 * wavclCurve. A crop repaint is a dependency of the tone and luma LUTs because
 * the crop consumes them; it is not a second request.
 */
ViewportLutSelection viewportLutSelection(const EditorRequest& request);

/*
 * The work one viewport fast-path hit renders on the visible crops.
 *
 * The crop bodies still read an execution mask; this struct is the typed
 * decision behind it. `labPixelsImplied()` is the dependency the legacy site
 * expressed as a mask OR: a luminance-curve or auto-exposure edit reaches the
 * crop's Lab pixel tools only through `M_LUMINANCE|M_COLOR`, so the crop
 * render needs them whenever either family is in the selection.
 */
struct ViewportCropSelection {
    /* M_CROP: the crop rectangle changed. */
    bool crop = false;
    /* M_MINUPDATE: repaint without new image work. */
    bool repaint = false;
    /* M_AUTOEXP. */
    bool autoExposure = false;
    /* M_TRANSFORM: a derived or requested geometry transform. */
    bool transform = false;
    /* M_RGBCURVE. */
    bool rgbProc = false;
    /* M_LUMACURVE. */
    bool lumaCurve = false;
    /* M_LUMINANCE and M_COLOR, kept apart because the crop carries the exact
     * bits the request named. */
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

/*
 * The crop-render selection of one request, from its typed request data.
 *
 * `request` must carry the execution facts of the pass, so a caller that
 * augmented the payload refreshes the request first. resolveCropDependencies()
 * is separate so the dependency is visible at the call site and can be
 * replayed.
 */
ViewportCropSelection viewportCropSelection(const EditorRequest& request);

/* Close the selection over the crop's Lab pixel dependency, in place. */
void resolveCropDependencies(ViewportCropSelection& selection);

/*
 * The `todo` argument Crop::update() reads for a selection, and the execution
 * mask it reads from the request. Both are payload materialisations for the
 * unconverted crop bodies, not planning decisions.
 */
int viewportCropTodo(const ViewportCropSelection& selection);
int viewportCropPayload(int requestMask,
                        const ViewportCropSelection& selection);

/*
 * Facts that reach a request without passing through an `M_*` bit.
 *
 * `viewportMoved` is the coordinator's panning heuristic. It is not encoded
 * in the mask and is supplied separately so the typed request can carry it.
 */
struct LegacyMaskContext {
    bool viewportMoved = false;
};

/*
 * Translate a legacy mask into a typed request.
 *
 * This is the only place the coordinator interprets the request-kind and
 * request-fact meaning of an `M_*` bit. Every bit the coordinator consumes is
 * mapped:
 *
 * | bit                     | kind        | facts                        |
 * |-------------------------|-------------|------------------------------|
 * | M_VOID                  | ParamsOnly  | controlOnly                  |
 * | M_MINUPDATE (alone)     | RepaintOnly | imperative repaint           |
 * | M_HIGHQUAL              | HighQuality | highDetailIntent             |
 * | M_INIT, M_LINDENOISE,   | FullPreview | fullFrameIntent              |
 * |   M_HDR                 |             |                              |
 * | M_SPOT                  | ParamsOnly  | spotPending                  |
 * | M_WB                    | ParamsOnly  | colorHlrWbRequest            |
 * | every other execution   | ParamsOnly  | none                         |
 * |   or output bit         |             |                              |
 *
 * Independently of the kind, every mask also yields `fullFrameAdmission` and
 * `monitorOutputRequest`; both are documented on the fields. The body facts
 * `transformFamilyIntent`, `hdrIntent`, `rgbProcIntent`, `labChainIntent`,
 * `autoExposureIntent` and `localAdjustmentsIntent` are derived here too.
 * They tell a scheduled node how much of its block the request allows; they
 * are not entry decisions, which come from the plan's dirty-block set.
 * `M_VOID` is pure
 * control: a mask made only of it suppresses the render but may still publish
 * parameters. `M_MINUPDATE` is imperative: it means "repaint without new image
 * work" and is only valid on its own, as declared in refreshmap.h. A combined
 * `M_MINUPDATE` is a contract violation and fails fast rather than silently
 * degrading to ParamsOnly. An unknown bit is a mapping gap and also fails
 * fast.
 *
 * `legacyGateMask` always receives the input mask unchanged, so the caller's
 * legacy block gates see exactly what they saw before.
 */
EditorRequest from_legacy_mask(int mask, const LegacyMaskContext& context = {});

/*
 * Typed producer API.
 *
 * A GUI tool that carries planning information names the operation it asks
 * for instead of building an `M_*` mask. `RequestIntent` is the closed set of
 * operations the tools currently express; `make_request()` turns one into an
 * `EditorRequest` whose kind, facts and dirty-block source are the planning
 * inputs. `legacyGateMask` is filled with the identical compatibility mask, so
 * the deferred queue, cache invalidation and the block bodies keep the payload
 * they had. The compatibility mask is derived here, next to the adapter, and
 * never at the tool.
 *
 * A tool that knows the exact blocks it dirties passes them to the second
 * form. They replace the parameter-fingerprint comparison for that request;
 * the set is closed over the pipeline and joined with the path residents
 * before it is stored, so it is an execution set and not only a seed list.
 * Omitting the blocks leaves the derivation to the parameter fingerprints,
 * which is the safe default for a tool that only knows "this parameter
 * changed".
 *
 * Every GUI producer now names an intent, so the typed API is the only
 * producer entry point. The integer
 * `ProcEventMapper::newEvent(action, message)` overload and the legacy
 * adapter remain in place for the later retirement step, but no producer
 * constructs a mask with them. No `M_*` bit reaches a planning decision
 * through either path because the coordinator adapter classifies each mask
 * once.
 */
enum class RequestIntent {
    /* A destructive raw reload, the legacy `ALLNORAW` composite. */
    AllNoRaw = 0,
    /* A demosaic family change, the legacy `DEMOSAIC` composite. */
    Demosaic = 1,
    /* The HDR/dehaze family, the legacy `HDR` composite. */
    Hdr = 2,
    /* Spot add/remove inside the base pass, the legacy `SPOTADJUST`. */
    SpotAdjust = 3,
    /* The geometry family, the legacy `TRANSFORM` composite. */
    Transform = 4,
    /* A white-balance change, the legacy `WB` composite. */
    WhiteBalance = 5,
    /* Repaint the current image without new work, the legacy `MINUPDATE`. */
    RepaintOnly = 6,
    /* Update parameters without rendering, the legacy `M_VOID`. */
    ControlOnly = 7,
    /* Gamut compression family, the legacy `COMPR` composite. */
    Compr = 8,
    /* Capture sharpening family, the legacy `CAPTURESHARPEN` composite. */
    CaptureSharpen = 9,
    /* Dark-frame/flat-field family, the legacy `DARKFRAME` composite. */
    DarkFrame = 10,
    /* Whole preview including the monitor sink, the legacy `FIRST`. */
    First = 11,
    /* Auto-exposure family, the legacy `AUTOEXP` composite. */
    AutoExposure = 12,
    /* Luminance-curve family, the legacy `LUMINANCECURVE` composite. */
    LuminanceCurve = 13,
    /* Directional-pyramid equaliser family, the legacy `DIRPYREQUALIZER`. */
    DirPyrEqualizer = 14,
    /* RGB-curve family, the legacy `RGBCURVE` composite. */
    RgbCurve = 15,
    /* Sharpening family, the legacy `SHARPENING` composite. */
    Sharpening = 16
};

/* Intent to typed request, with no producer-provided dirty blocks. */
EditorRequest make_request(RequestIntent intent);

/*
 * Intent to typed request with a producer-provided dirty-block seed.
 *
 * `blocks` are canonical ids of `path`; the result closes over the pipeline
 * edges and includes the path residents. An empty list is equivalent to the
 * one-argument form.
 */
EditorRequest make_request(RequestIntent intent,
                           std::initializer_list<const char*> blocks,
                           stage::StagePath path = stage::StagePath::Preview);

/* The compatibility mask an intent carries, exposed for tests and diagnostics. */
int compatibility_mask(RequestIntent intent);

/*
 * Dedup key of a typed producer event.
 *
 * Two producer actions must stay distinct even when their planning identity is
 * equal: `EvSpotEnabled` (ALLNORAW) and `EvSpotEnabledOPA` (SPOTADJUST) plan
 * the same today but carry different compatibility masks, and collapsing them
 * would lose the mask the rest of the coordinator consumes. The key therefore
 * includes the identity *and* the compatibility mask. The identity remains the
 * planning identity; this function is only the event registry key.
 */
std::string producerEventKey(const EditorRequest& request,
                             const std::string& historyMsg);

/*
 * Request kind of one crop update.
 *
 * A crop reallocation has to refill `origCrop`, but the preview request that
 * triggered it may be a plain parameter update. The render-state request kind
 * is therefore widened to FullCrop for that update, so the crop work order
 * names the base rebuild even when the editor request did not. HighQuality
 * keeps its own priority; the other kinds map one to one.
 */
stage::RequestKind crop_request_kind(EditorRequest::RequestKind previewKind,
                                     bool reallocation);

/*
 * Debug cross-check: whether a request still agrees with the planning a bare
 * interpretation of its legacy mask would give.
 *
 * The coordinator patches the typed kind and facts after it widens a legacy
 * mask. The base-refresh and Color/Coloropp patches must reproduce the old
 * derivation over the widened mask, and a debug build asserts that here. The
 * requeue and deferred-absorb patches only ever narrow a repaint, so they
 * assert the narrower `imperativeRepaint` relation instead. A unit test can
 * call this directly. It is never used by planning.
 */
bool planningAgreesWithLegacyMask(const EditorRequest& request);

/*
 * Coalesce two requests into one.
 *
 * The result keeps the legacy-mask union (so block gates are unchanged), the
 * union of every input fact, and the strongest kind. The mask-shaped facts are
 * recomputed from the union so they describe the payload as a whole, exactly
 * as the old code interpreted the ORed mask. That is required because some of
 * them are not monotone in the mask:
 *
 * - `fullFrameAdmission` becomes true only when the union accumulates every
 *   `ALL` bit, even though neither input had them all;
 * - `monitorOutputRequest` is false for exactly `M_CROP` and `M_MINUPDATE`, so
 *   two suppressing inputs can produce a rendering union;
 * - `imperativeRepaint` is true only for exactly `M_MINUPDATE`, so any
 *   widening clears it.
 *
 * `colorHlrWbRequest` is monotone in `M_WB`, but it is recomputed with the
 * others so every fact is derived once from the same payload and cannot drift
 * from the rest. Kind precedence reproduces the old derivation over the ORed
 * mask: HighQuality, then FullPreview, then RepaintOnly only when every
 * contributing request was a bare repaint, then ParamsOnly. A control-only
 * request therefore turns a bare repaint into a parameter update, exactly as
 * forming `M_MINUPDATE | M_VOID` did.
 *
 * Producer-provided dirty sets are unioned; otherwise both inputs derive from
 * the parameters.
 */
EditorRequest coalesce(const EditorRequest& first, const EditorRequest& second);

} // namespace rtengine
