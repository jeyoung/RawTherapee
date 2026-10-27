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
 * Typed editor request and the single legacy-mask adapter.
 *
 * The mask layout lives in refreshmap.h. This unit maps every bit the
 * coordinator consumes and refuses an unmapped bit, so a future mask addition
 * cannot silently lose its request meaning. The mapping and the coalescing
 * rules are documented in editorrequest.h.
 */

#include "editorrequest.h"

#include "refreshmap.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <string>

namespace rtengine
{

namespace
{

/*
 * Bits that name a parameter family or an output request. They all degrade to
 * ParamsOnly: they dirty work, but they are not a request kind.
 */
constexpr int PARAMETER_AND_OUTPUT_BITS =
    M_PREPROC | M_RAW | M_WB | M_SPOT | M_CSHARP | M_MONITOR | M_RETINEX
    | M_CROP | M_INIT | M_LINDENOISE | M_HDR | M_TRANSFORM | M_BLURMAP
    | M_AUTOEXP | M_RGBCURVE | M_LUMACURVE | M_LUMINANCE | M_COLOR;

/* Control and request-kind bits, none of which names a parameter family. */
constexpr int REQUEST_BITS = M_VOID | M_MINUPDATE | M_HIGHQUAL;

/* Every bit the coordinator consumes. */
constexpr int MAPPED_BITS = PARAMETER_AND_OUTPUT_BITS | REQUEST_BITS;

/* The absolute reload bits, which ask for the whole path. */
constexpr int FULL_FRAME_BITS = M_INIT | M_LINDENOISE | M_HDR;

/*
 * Fail fast on a contract violation.
 *
 * The assert documents the invariant in debug builds; the abort makes the
 * contract hold in release builds too, where NDEBUG removes the assert. An
 * unmapped bit is a mapping gap and a combined M_MINUPDATE is a producer bug;
 * both would otherwise degrade silently to ParamsOnly.
 */
[[noreturn]] void failContract(const char* message)
{
    assert(false && message);
    (void)message;
    std::abort();
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

uint64_t identityOf(const EditorRequest& request)
{
    uint64_t hash = 14695981039346656037ULL;

    hash = hashCombine(hash, static_cast<uint64_t>(request.kind));
    hash = hashCombine(
        hash, static_cast<uint64_t>(request.dirtySource));
    hash = hashCombine(hash, request.controlOnly ? 1 : 0);
    hash = hashCombine(hash, request.spotPending ? 1 : 0);
    hash = hashCombine(hash, request.viewportMoved ? 1 : 0);
    hash = hashCombine(hash, request.highDetailIntent ? 1 : 0);
    hash = hashCombine(hash, request.fullFrameIntent ? 1 : 0);
    /*
     * The mask-shaped facts are planning inputs, so they belong to the
     * identity even though the raw mask does not. Two masks that plan
     * differently must never share an identity.
     */
    hash = hashCombine(hash, request.fullFrameAdmission ? 1 : 0);
    hash = hashCombine(hash, request.colorHlrWbRequest ? 1 : 0);
    hash = hashCombine(hash, request.monitorOutputRequest ? 1 : 0);
    hash = hashCombine(hash, request.imperativeRepaint ? 1 : 0);
    hash = hashCombine(hash, request.transformFamilyIntent ? 1 : 0);
    hash = hashCombine(hash, request.hdrIntent ? 1 : 0);
    hash = hashCombine(hash, request.rgbProcIntent ? 1 : 0);
    hash = hashCombine(hash, request.labChainIntent ? 1 : 0);
    hash = hashCombine(hash, request.autoExposureIntent ? 1 : 0);
    hash = hashCombine(hash, request.localAdjustmentsIntent ? 1 : 0);
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

/* The mask alone admits the full-frame path. */
bool fullFrameAdmissionFor(int mask)
{
    return (mask & ALL) == ALL || (mask & M_MONITOR) != 0;
}

/* The pass carries a white-balance request. */
bool colorHlrWbRequestFor(int mask)
{
    return (mask & M_WB) != 0;
}

/*
 * The legacy monitor render gate: every request except a bare crop or a bare
 * repaint renders monitor.output, and M_MONITOR always does.
 */
bool monitorOutputRequestFor(int mask)
{
    return (mask != CROP && mask != MINUPDATE) || (mask & M_MONITOR) != 0;
}

/* The mask exactly in its imperative-repaint form. */
bool imperativeRepaintFor(int mask)
{
    return mask == M_MINUPDATE;
}

/*
 * Kind the request bits imply, in the precedence the coordinator used before
 * the typed request. Deliberately total and non-aborting: the coordinator's
 * debug cross-check evaluates widened masks that the adapter would reject.
 */
EditorRequest::RequestKind kindForLegacyMask(int mask)
{
    if (mask == M_MINUPDATE) {
        return EditorRequest::RequestKind::RepaintOnly;
    }
    if ((mask & M_HIGHQUAL) != 0) {
        return EditorRequest::RequestKind::HighQuality;
    }
    if ((mask & FULL_FRAME_BITS) != 0) {
        return EditorRequest::RequestKind::FullPreview;
    }
    return EditorRequest::RequestKind::ParamsOnly;
}

} // namespace

void EditorRequest::setLegacyGateMask(int mask)
{
    legacyGateMask = mask;
    fullFrameAdmission = fullFrameAdmissionFor(mask);
    colorHlrWbRequest = colorHlrWbRequestFor(mask);
    monitorOutputRequest = monitorOutputRequestFor(mask);
    imperativeRepaint = imperativeRepaintFor(mask);
    transformFamilyIntent =
        (mask & (M_HDR | M_TRANSFORM | M_RGBCURVE | M_INIT | M_LINDENOISE)) != 0;
    hdrIntent = (mask & M_HDR) != 0;
    rgbProcIntent = (mask & M_RGBCURVE) != 0;
    labChainIntent = (mask & (M_LUMINANCE | M_COLOR | M_AUTOEXP)) != 0;
    autoExposureIntent = (mask & M_AUTOEXP) != 0;
    localAdjustmentsIntent =
        (mask & (M_AUTOEXP | M_RGBCURVE | M_CROP)) != 0;
}

uint64_t EditorRequest::identity() const
{
    return identityOf(*this);
}

void EditorRequest::refreshFromLegacyMask()
{
    present = legacyGateMask != 0;
    controlOnly = (legacyGateMask & ~M_VOID) == 0;
    spotPending = (legacyGateMask & M_SPOT) != 0;
    highDetailIntent = (legacyGateMask & M_HIGHQUAL) != 0;
    fullFrameIntent = (legacyGateMask & FULL_FRAME_BITS) != 0;
    setLegacyGateMask(legacyGateMask);
    kind = kindForLegacyMask(legacyGateMask);
}

bool planningAgreesWithLegacyMask(const EditorRequest& request)
{
    return request.kind == kindForLegacyMask(request.legacyGateMask)
        && request.controlOnly == ((request.legacyGateMask & ~M_VOID) == 0)
        && request.spotPending
            == ((request.legacyGateMask & M_SPOT) != 0)
        && request.highDetailIntent
            == ((request.legacyGateMask & M_HIGHQUAL) != 0)
        && request.fullFrameIntent
            == ((request.legacyGateMask & FULL_FRAME_BITS) != 0)
        && request.fullFrameAdmission
            == fullFrameAdmissionFor(request.legacyGateMask)
        && request.colorHlrWbRequest
            == colorHlrWbRequestFor(request.legacyGateMask)
        && request.monitorOutputRequest
            == monitorOutputRequestFor(request.legacyGateMask)
        && request.imperativeRepaint
            == imperativeRepaintFor(request.legacyGateMask)
        && request.transformFamilyIntent
            == ((request.legacyGateMask
                 & (M_HDR | M_TRANSFORM | M_RGBCURVE | M_INIT
                    | M_LINDENOISE))
                != 0)
        && request.hdrIntent
            == ((request.legacyGateMask & M_HDR) != 0)
        && request.rgbProcIntent
            == ((request.legacyGateMask & M_RGBCURVE) != 0)
        && request.labChainIntent
            == ((request.legacyGateMask & (M_LUMINANCE | M_COLOR | M_AUTOEXP))
                != 0)
        && request.autoExposureIntent
            == ((request.legacyGateMask & M_AUTOEXP) != 0)
        && request.localAdjustmentsIntent
            == ((request.legacyGateMask
                 & (M_AUTOEXP | M_RGBCURVE | M_CROP))
                != 0);
}

EditorRequest from_legacy_mask(int mask, const LegacyMaskContext& context)
{
    assert(mask >= 0);

    const int unknown = mask & ~MAPPED_BITS;

    if (unknown != 0) {
        failContract("unknown bit in a legacy editor mask");
    }

    const bool repaint = (mask & M_MINUPDATE) != 0;

    /*
     * refreshmap.h declares that M_MINUPDATE must not be combined with any
     * other event. The old code only recognised it when the whole mask was
     * exactly M_MINUPDATE; treat any other combination as a contract
     * violation instead of silently planning a parameter update.
     */
    if (repaint && mask != M_MINUPDATE) {
        failContract("M_MINUPDATE must be used alone");
    }

    EditorRequest request;

    request.present = mask != 0;
    request.setLegacyGateMask(mask);
    request.controlOnly = (mask & ~M_VOID) == 0;
    request.viewportMoved = context.viewportMoved;
    request.spotPending = (mask & M_SPOT) != 0;
    request.highDetailIntent = (mask & M_HIGHQUAL) != 0;
    request.fullFrameIntent = (mask & FULL_FRAME_BITS) != 0;
    request.dirtySource = EditorRequest::DirtySource::DeriveFromParameters;

    /*
     * Kind precedence is the one the coordinator used to derive from the
     * coalesced mask, kept here so the mask is interpreted in exactly one
     * place.
     */
    request.kind = kindForLegacyMask(mask);

    return request;
}

EditorRequest coalesce(const EditorRequest& first, const EditorRequest& second)
{
    if (!first.present) {
        return second;
    }
    if (!second.present) {
        return first;
    }

    EditorRequest merged;

    merged.present = true;
    merged.setLegacyGateMask(
        first.legacyGateMask | second.legacyGateMask);
    merged.controlOnly = first.controlOnly && second.controlOnly;
    merged.spotPending = first.spotPending || second.spotPending;
    merged.viewportMoved = first.viewportMoved || second.viewportMoved;
    merged.highDetailIntent =
        first.highDetailIntent || second.highDetailIntent;
    merged.fullFrameIntent = first.fullFrameIntent || second.fullFrameIntent;
    merged.transformFamilyIntent =
        first.transformFamilyIntent || second.transformFamilyIntent;
    merged.hdrIntent = first.hdrIntent || second.hdrIntent;
    merged.rgbProcIntent = first.rgbProcIntent || second.rgbProcIntent;
    merged.labChainIntent = first.labChainIntent || second.labChainIntent;
    merged.autoExposureIntent =
        first.autoExposureIntent || second.autoExposureIntent;
    merged.localAdjustmentsIntent =
        first.localAdjustmentsIntent || second.localAdjustmentsIntent;

    /*
     * Reproduce the old derivation over the ORed mask. A bare repaint stays a
     * repaint only when every contributing request is a bare repaint; any
     * other request (including a control-only M_VOID) makes the combination a
     * parameter update.
     */
    if (merged.highDetailIntent) {
        merged.kind = EditorRequest::RequestKind::HighQuality;
    } else if (merged.fullFrameIntent) {
        merged.kind = EditorRequest::RequestKind::FullPreview;
    } else if (!merged.controlOnly
               && first.kind == EditorRequest::RequestKind::RepaintOnly
               && second.kind == EditorRequest::RequestKind::RepaintOnly) {
        merged.kind = EditorRequest::RequestKind::RepaintOnly;
    } else {
        merged.kind = EditorRequest::RequestKind::ParamsOnly;
    }

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

int compatibility_mask(RequestIntent intent)
{
    switch (intent) {
    case RequestIntent::AllNoRaw:
        return ALLNORAW;
    case RequestIntent::Demosaic:
        return DEMOSAIC;
    case RequestIntent::Hdr:
        return HDR;
    case RequestIntent::SpotAdjust:
        return SPOTADJUST;
    case RequestIntent::Transform:
        return TRANSFORM;
    case RequestIntent::WhiteBalance:
        return WB;
    case RequestIntent::RepaintOnly:
        return M_MINUPDATE;
    case RequestIntent::ControlOnly:
        return M_VOID;
    case RequestIntent::Compr:
        return COMPR;
    case RequestIntent::CaptureSharpen:
        return CAPTURESHARPEN;
    case RequestIntent::DarkFrame:
        return DARKFRAME;
    case RequestIntent::First:
        return FIRST;
    case RequestIntent::AutoExposure:
        return AUTOEXP;
    case RequestIntent::LuminanceCurve:
        return LUMINANCECURVE;
    case RequestIntent::DirPyrEqualizer:
        return DIRPYREQUALIZER;
    case RequestIntent::RgbCurve:
        return RGBCURVE;
    case RequestIntent::Sharpening:
        return SHARPENING;
    }

    assert(false && "unknown request intent");
    std::abort();
}

EditorRequest make_request(RequestIntent intent,
                           std::initializer_list<const char*> blocks,
                           stage::StagePath path)
{
    EditorRequest request = from_legacy_mask(compatibility_mask(intent));

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
    return "typed-" + std::to_string(request.identity()) + "-"
        + std::to_string(request.legacyGateMask) + historyMsg;
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

} // namespace rtengine
