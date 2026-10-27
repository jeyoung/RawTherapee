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

namespace rtengine
{

/*
 * Typed request from an editor to the image pipeline.
 *
 * This is the coordinator's planning input. It replaces the legacy `M_*`
 * mask as the thing that decides *what kind of work* a preview request asks
 * for. The mask itself is still carried, as an opaque execution gate for the
 * block bodies that have not been converted yet; no planning decision reads
 * it.
 *
 * The type has four parts:
 *
 * - `kind`: the request kind the preview plan consumes. It has the same
 *   precedence the coordinator used to derive from the mask: HighQuality,
 *   then a whole-path rebuild, then RepaintOnly, otherwise ParamsOnly.
 * - dirty blocks: either supplied by a producer that already knows the typed
 *   block set, or an explicit "derive from the parameters" state. The legacy
 *   adapter always asks for the derivation, because the GUI has no typed
 *   block information yet.
 * - request facts: the four boolean inputs the render state and the work
 *   order need and that are not fields of `ProcParams`.
 * - `identity()`: a content hash of the fields above, used to decide whether
 *   two coalesced requests are planning-equivalent. It deliberately excludes
 *   the legacy mask, so it does not change when a producer switches from a
 *   mask to typed information.
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
     * This is the mask the adapter received, ORed across coalesced requests.
     * It is consumed only by the legacy block gates, by cache invalidation
     * and by the documented viewport fast-path admission site. It is not a
     * planning input and is excluded from identity().
     */
    int legacyGateMask = 0;

    /*
     * Stable content identity of the planning fields, excluding the legacy
     * mask. Equal values mean the two requests are indistinguishable to the
     * planner and may be coalesced without losing planning information.
     */
    uint64_t identity() const;

    /* Whether the dirty set comes from the producer rather than parameters. */
    bool derivesFromParameters() const
    {
        return dirtySource == DirtySource::DeriveFromParameters;
    }

    /*
     * The legacy mask for the viewport fast-path admission test.
     *
     * This is the single accessor the fast path uses; see
     * ImProcCoordinator::updatePreviewImage() for the documented limitation.
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
 * | every other execution   | ParamsOnly  | none                         |
 * |   or output bit         |             |                              |
 *
 * `M_VOID` is pure control: a mask made only of it suppresses the render but
 * may still publish parameters. `M_MINUPDATE` is imperative: it means
 * "repaint without new image work" and is only valid on its own, as declared
 * in refreshmap.h. A combined `M_MINUPDATE` is a contract violation and
 * fails fast rather than silently degrading to ParamsOnly. An unknown bit is
 * a mapping gap and also fails fast.
 *
 * `legacyGateMask` always receives the input mask unchanged, so the caller's
 * legacy block gates see exactly what they saw before.
 */
EditorRequest from_legacy_mask(int mask, const LegacyMaskContext& context = {});

/*
 * Coalesce two requests into one.
 *
 * The result keeps the legacy-mask union (so block gates are unchanged), the
 * union of every fact, and the strongest kind. Kind precedence reproduces the
 * old derivation over the ORed mask: HighQuality, then FullPreview, then
 * RepaintOnly only when every contributing request was a bare repaint, then
 * ParamsOnly. A control-only request therefore turns a bare repaint into a
 * parameter update, exactly as forming `M_MINUPDATE | M_VOID` did.
 *
 * Producer-provided dirty sets are unioned; otherwise both inputs derive from
 * the parameters.
 */
EditorRequest coalesce(const EditorRequest& first, const EditorRequest& second);

} // namespace rtengine
