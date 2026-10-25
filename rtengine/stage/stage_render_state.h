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

namespace rtengine::stage
{

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
 *
 * All fields are plain values or an owning string: there are no pointers, no
 * ImageSource, and no coordinator state. The struct is therefore safe to copy,
 * compare, persist and hash, and two states with equal field values are
 * interchangeable regardless of where they were built.
 *
 * Field order in fingerprint() is part of the persisted format. Keep the
 * member order and the hashing order in step, and never reuse a field slot for
 * a different meaning without changing the section label.
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
        previousDemoBase))
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
            previousDemoBase);
    }
};

} // namespace rtengine::stage
