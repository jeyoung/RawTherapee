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

#include "stage_param_hash.h"

#include <cstdint>
#include <string>
#include <tuple>

namespace rtengine::procparams
{
struct CropGuideParams;
}

namespace rtengine::stage
{

struct DirtyBlocks;

/*
 * Reason the preview base cache is invalidated for one request.
 *
 * The values keep the strings the mask-based coordinator published, so a
 * profile log stays comparable across the retirement. The precedence is the
 * old one: a source preprocess wins over a demosaic, which wins over a base
 * finish, which wins over the high-detail request. `None` means the request
 * leaves the cached base reusable.
 */
enum class BaseCacheInvalidation : int32_t {
    None = 0,
    Preprocess = 1,
    Raw = 2,
    WhiteBalance = 3,
    HighDetailRequest = 4
};

/* Stable reason string, or null for None. */
const char* baseCacheInvalidationReason(BaseCacheInvalidation invalidation);

/*
 * Map a typed dirty-block set and the high-detail request fact to the base
 * cache invalidation.
 *
 * The set is the narrow "base cache inputs" set, not the pass execution set:
 * `raw.preprocess` is a source preprocess change, `raw.demosaic` a demosaiced
 * plane change, and `raw.base` the base finish where white balance is applied.
 * A caller must not pass the full execution set, because the base block also
 * covers spot, film-negative, retinex and tone-curve inputs that the cache
 * key or its bypass rules handle without invalidating the stored snapshot.
 */
BaseCacheInvalidation baseCacheInvalidationFor(const DirtyBlocks& blocks,
                                               bool highDetailIntent);

/*
 * Kind of work one preview or crop request asks for.
 *
 * The values are part of the fingerprint format, so never renumber them.
 * ParamsOnly means the request changes processing parameters but does not
 * need a repaint; RepaintOnly reuses the current buffers; FullCrop repaints
 * one ROI; FullPreview repaints the whole preview; HighQuality forces the
 * accurate path even when a fast approximation is available.
 */
enum class RequestKind : int32_t {
    ParamsOnly = 0,
    RepaintOnly = 1,
    FullCrop = 2,
    FullPreview = 3,
    HighQuality = 4
};

/*
 * Preview base the preceding pass actually used.
 *
 * This mirrors ImProcCoordinator::PrevdemoBase. It is kept as a separate
 * enumeration so the render state has no dependency on a coordinator header,
 * and the field itself is stored as an int for serialisability.
 */
enum class PreviousDemoBase : int32_t {
    Unknown = 0,
    Fast = 1,
    Sidecar = 2
};

/*
 * Explicit render state that is not represented in ProcParams.
 *
 * The ProcParams bitset records parameter changes only. Several inputs that
 * decide which pixels a pass computes and how it converts them live outside
 * ProcParams: the preview scale and its allocated dimensions, the crop or ROI
 * rectangle, the high-detail decision and what has already been computed for
 * it, the source generation, a modified RGB source, the monitor and
 * soft-proofing configuration, the sharpening mask toggle, the fast-preview
 * switches and the request kind. This struct carries all of them so a stage
 * cache key or an invalidation decision can be derived without the bitset.
 * The request-facts section at the end adds the source geometry and sensor
 * facts, the denoise and spot facts, the deferred analysis the coordinator had
 * queued, the crop-guide state and the shared viewport-moved flag, which the
 * work flags will need and which live in the coordinator or the crop rather
 * than in ProcParams.
 *
 * All fields are plain values or an owning string: there are no pointers, no
 * ImageSource, and no coordinator state. The struct is therefore safe to copy,
 * compare, persist and hash, and two states with equal field values are
 * interchangeable regardless of where they were built.
 *
 * Field order in fingerprint() is part of the persisted format. Keep the
 * member order and the hashing order in step, and never reuse a field slot for
 * a different meaning without changing the section label. Adding the
 * request-facts fields changed the format: a key persisted before that change
 * hashes to a different value than the same key after it, so stored render
 * states must be discarded rather than compared across the change.
 */
struct RenderStateKey {
    /* Preview geometry, in pixels at `previewScale`. */
    int32_t previewScale = 1;
    int32_t previewWidth = 0;
    int32_t previewHeight = 0;
    int32_t fullWidth = 0;
    int32_t fullHeight = 0;

    /*
     * Crop or ROI request. `borderRequested`, `borderLeft` and `borderTop`
     * are the requested and actually allocated processing border in pixels;
     * zero means no border on that side.
     */
    int32_t cropX = 0;
    int32_t cropY = 0;
    int32_t cropWidth = 0;
    int32_t cropHeight = 0;
    int32_t cropSkip = 1;
    int32_t borderRequested = 0;
    int32_t borderLeft = 0;
    int32_t borderTop = 0;

    /* High-detail decision and the work already done for it. */
    bool highDetailNeeded = false;
    bool highDetailPreprocessComputed = false;
    bool highDetailRawComputed = false;
    bool highDetailStateKnown = false;

    /* Source identity beyond the parameter set. */
    uint64_t sourceGeneration = 0;
    bool rgbSourceModified = false;

    /* Display configuration, which is application state, not ProcParams. */
    std::string monitorProfile;
    int32_t monitorIntent = 0;
    bool softProof = false;
    bool gamutCheck = false;

    /* Editor-only processing switches. */
    bool sharpMask = false;
    bool fastPreviewEnabled = false;
    bool mainPreviewFullResolution = false;
    bool initialRoughPreview = false;

    /* Request and the base the preceding preview pass used. */
    RequestKind requestKind = RequestKind::ParamsOnly;
    int32_t previousDemoBase = static_cast<int32_t>(PreviousDemoBase::Unknown);

    /*
     * Request facts.
     *
     * These are the inputs the exact preview and crop work flags need and that
     * neither ProcParams nor an earlier field carries. Eight of the eleven
     * are mapped to a RenderStateAspect by stage_dirty_blocks.cc: the five
     * source facts join Source, and `dirPyrDenoiseExpComp`,
     * `denoiseAnalysisValid` and `spotBufferLive` form Derived.
     *
     * The other three are deliberately exempt, each for its own reason:
     * `spotPending`, whose false->true transition is carried by the spot
     * parameter delta; `cropGuideFingerprint`, an overlay value whose later
     * job is to suppress a repaint; and `viewportMoved`, which
     * RequestKind::RepaintOnly already represents on the crop.
     *
     * `sourceRotateDegree` is the source rotation in degrees, `sourceFullWidth`
     * and `sourceFullHeight` are its unrotated full size in pixels, which the
     * preview dimensions do not express once a rotation swaps the axes,
     * `sourceSensorType` is an eSensorType value and `sourceFrameCount` the
     * number of frames the source offers. `dirPyrDenoiseExpComp` is the
     * exposure compensation the source contributes to denoise; its infinity
     * sentinel means the source has not measured it yet, and the fingerprint
     * hashes its bit pattern like every other typed value. The source is read
     * at its current frame, so a frame change that only alters the metadata
     * does not appear here.
     *
     * `denoiseAnalysisValid` is true when the coordinator holds a usable
     * automatic-denoise analysis for this request; only the crop builds it.
     * `spotPending` is true when the request asked for spot removal and the
     * parameters still name spots; `spotBufferLive` is true when a spot buffer
     * exists, because restoring it changes the next base pass.
     *
     * The deferred analysis mask was removed from this struct: deferred analysis is
     * scheduler state that the coordinator now carries as a typed dirty-block
     * record before it builds the key, so it is not a render-state field and
     * does not reach this struct at all.
     *
     * `cropGuideFingerprint` is a 64-bit fingerprint of CropGuideParams.
     * Guides are an editor overlay and no stage consumes them, so they stay
     * out of the parameter fingerprint; the value is carried here so a later
     * phase can distinguish a guide-only change from a geometry change.
     * `viewportMoved` is the one shared panning/repaint fact: true when the
     * viewport moved and the existing buffers may be reused, not when a repaint
     * must happen now. The preview reports the `panningRelatedChange`
     * heuristic, a ProcParams diff that has no mask bit, and the crop reports
     * a bare repaint request, which is imperative; both
     * are reported under this one name and the repaint imperative itself stays
     * out of scope.
     *
     * Every field defaults to the value an unpopulated key had before the
     * field existed, so an old and a new key differ only through these fields.
     */
    int32_t sourceRotateDegree = 0;
    int32_t sourceSensorType = 0;
    int32_t sourceFullWidth = 0;
    int32_t sourceFullHeight = 0;
    int32_t sourceFrameCount = 0;
    double dirPyrDenoiseExpComp = 0.0;
    bool denoiseAnalysisValid = false;
    bool spotPending = false;
    bool spotBufferLive = false;
    uint64_t cropGuideFingerprint = 0;
    bool viewportMoved = false;

    bool operator==(const RenderStateKey& other) const
    {
        return tie() == other.tie();
    }

    bool operator!=(const RenderStateKey& other) const
    {
        return !(*this == other);
    }

    /*
     * Stable 128-bit fingerprint of every field, in the order declared above.
     *
     * The stream is domain-separated with the "render-state" section label and
     * uses only ParamHasher's typed overloads, never raw struct bytes, so
     * padding, host byte order and pointer values cannot affect the result.
     * The value is persisted, so changing the label, the field order or a
     * field's type invalidates every stored render state key.
     */
    ParamFingerprint fingerprint() const;

private:
    auto tie() const -> decltype(std::tie(
        previewScale,
        previewWidth,
        previewHeight,
        fullWidth,
        fullHeight,
        cropX,
        cropY,
        cropWidth,
        cropHeight,
        cropSkip,
        borderRequested,
        borderLeft,
        borderTop,
        highDetailNeeded,
        highDetailPreprocessComputed,
        highDetailRawComputed,
        highDetailStateKnown,
        sourceGeneration,
        rgbSourceModified,
        monitorProfile,
        monitorIntent,
        softProof,
        gamutCheck,
        sharpMask,
        fastPreviewEnabled,
        mainPreviewFullResolution,
        initialRoughPreview,
        requestKind,
        previousDemoBase,
        sourceRotateDegree,
        sourceSensorType,
        sourceFullWidth,
        sourceFullHeight,
        sourceFrameCount,
        dirPyrDenoiseExpComp,
        denoiseAnalysisValid,
        spotPending,
        spotBufferLive,
        cropGuideFingerprint,
        viewportMoved))
    {
        return std::tie(
            previewScale,
            previewWidth,
            previewHeight,
            fullWidth,
            fullHeight,
            cropX,
            cropY,
            cropWidth,
            cropHeight,
            cropSkip,
            borderRequested,
            borderLeft,
            borderTop,
            highDetailNeeded,
            highDetailPreprocessComputed,
            highDetailRawComputed,
            highDetailStateKnown,
            sourceGeneration,
            rgbSourceModified,
            monitorProfile,
            monitorIntent,
            softProof,
            gamutCheck,
            sharpMask,
            fastPreviewEnabled,
            mainPreviewFullResolution,
            initialRoughPreview,
            requestKind,
            previousDemoBase,
            sourceRotateDegree,
            sourceSensorType,
            sourceFullWidth,
            sourceFullHeight,
            sourceFrameCount,
            dirPyrDenoiseExpComp,
            denoiseAnalysisValid,
            spotPending,
            spotBufferLive,
            cropGuideFingerprint,
            viewportMoved);
    }
};

/*
 * Stable 64-bit fingerprint of the crop-guide overlay state.
 *
 * CropGuideParams has no ProcParams fingerprint: guides are an editor overlay
 * that no stage consumes. This function hashes the state with ParamHasher's
 * typed overloads under its own section label, so the result is portable and
 * independent of struct padding and of the process that computes it. It is
 * used for the RenderStateKey::cropGuideFingerprint field; the 64-bit width is
 * a compact change detector, not a collision-resistant identifier.
 */
uint64_t cropGuideFingerprint(const procparams::CropGuideParams& guide);

} // namespace rtengine::stage
