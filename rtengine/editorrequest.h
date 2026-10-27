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
     * consumed only by the legacy block gates, by cache invalidation and by
     * the documented viewport fast-path admission site. It is not read by any
     * planning decision, and it is excluded from identity(); the planning
     * facts derived from it are included.
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
     * The legacy mask for the documented viewport fast-path admission test.
     *
     * This is the single accessor the coordinator uses to obtain the opaque
     * execution payload; see ImProcCoordinator::updatePreviewImage() for the
     * documented admission site and the list of non-planning consumers.
     */
    int viewportAdmissionMask() const
    {
        return legacyGateMask;
    }
};

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
 * the viewport fast path, the deferred queue, cache invalidation and the block
 * bodies keep the payload they had. The compatibility mask is derived here,
 * next to the adapter, and never at the tool.
 *
 * A tool that knows the exact blocks it dirties passes them to the second
 * form. They replace the parameter-fingerprint comparison for that request;
 * the set is closed over the pipeline and joined with the path residents
 * before it is stored, so it is an execution set and not only a seed list.
 * Omitting the blocks leaves the derivation to the parameter fingerprints,
 * which is the safe default for a tool that only knows "this parameter
 * changed".
 *
 * Parameter-only tools that have no planning information to carry stay on the
 * integer `ProcEventMapper::newEvent(action, message)` compatibility path; no
 * `M_*` bit reaches a planning decision through it because the coordinator
 * adapter classifies it once.
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
    First = 11
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
