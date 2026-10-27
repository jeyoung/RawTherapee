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
 * Render state fingerprint test.
 *
 * The test is specification-only: it builds keys, fingerprints them, and never
 * touches an image or a coordinator. The mutation table proves that every
 * declared field participates in the fingerprint. The canonical-stream check
 * rebuilds the same field sequence with the typed ParamHasher overloads, so a
 * change to raw struct hashing, padding or host byte order would fail instead
 * of silently changing stored keys.
 */

#include "stage_render_state.h"

#include "rtengine/editorrequest.h"
#include "rtengine/viewportdefer.h"

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using rtengine::stage::ParamFingerprint;
using rtengine::stage::PreviousDemoBase;
using rtengine::stage::RenderStateKey;
using rtengine::stage::RequestKind;

/*
 * A fully populated key. Every field carries a value that can be changed
 * without touching a neighbour, so the mutation table is unambiguous.
 */
RenderStateKey sample()
{
    RenderStateKey key;
    key.previewScale = 2;
    key.previewWidth = 600;
    key.previewHeight = 400;
    key.fullWidth = 6000;
    key.fullHeight = 4000;

    key.cropX = 100;
    key.cropY = 200;
    key.cropWidth = 800;
    key.cropHeight = 600;
    key.cropSkip = 2;
    key.borderRequested = 32;
    key.borderLeft = 32;
    key.borderTop = 16;

    key.highDetailNeeded = true;
    key.highDetailPreprocessComputed = true;
    key.highDetailRawComputed = false;
    key.highDetailStateKnown = true;

    key.sourceGeneration = 42;
    key.rgbSourceModified = false;

    key.monitorProfile = "sRGB-v4.icc";
    key.monitorIntent = 1;
    key.softProof = false;
    key.gamutCheck = false;

    key.sharpMask = false;
    key.fastPreviewEnabled = true;
    key.mainPreviewFullResolution = false;
    key.initialRoughPreview = false;

    key.requestKind = RequestKind::FullPreview;
    key.previousDemoBase =
        static_cast<int32_t>(PreviousDemoBase::Fast);

    key.sourceRotateDegree = 90;
    key.sourceSensorType = 2;
    key.sourceFullWidth = 6048;
    key.sourceFullHeight = 4024;
    key.sourceFrameCount = 3;
    key.dirPyrDenoiseExpComp = 0.5;
    key.denoiseAnalysisValid = true;
    key.spotPending = true;
    key.spotBufferLive = true;
    key.cropGuideFingerprint = UINT64_C(0x0123456789abcdef);
    key.viewportMoved = true;
    return key;
}

/*
 * The canonical field stream, written out independently of the struct layout.
 *
 * ParamHasher encodes each typed value as a fixed-width little-endian bit
 * pattern. Matching this stream therefore shows that the implementation hashes
 * values, not object representation, so padding and host byte order are
 * irrelevant.
 */
ParamFingerprint canonicalFingerprint(const RenderStateKey& key)
{
    rtengine::stage::ParamHasher hasher;
    hasher.addSectionLabel("render-state");

    hasher.add(key.previewScale);
    hasher.add(key.previewWidth);
    hasher.add(key.previewHeight);
    hasher.add(key.fullWidth);
    hasher.add(key.fullHeight);

    hasher.add(key.cropX);
    hasher.add(key.cropY);
    hasher.add(key.cropWidth);
    hasher.add(key.cropHeight);
    hasher.add(key.cropSkip);
    hasher.add(key.borderRequested);
    hasher.add(key.borderLeft);
    hasher.add(key.borderTop);

    hasher.add(key.highDetailNeeded);
    hasher.add(key.highDetailPreprocessComputed);
    hasher.add(key.highDetailRawComputed);
    hasher.add(key.highDetailStateKnown);

    hasher.add(key.sourceGeneration);
    hasher.add(key.rgbSourceModified);

    hasher.addString(key.monitorProfile);
    hasher.add(key.monitorIntent);
    hasher.add(key.softProof);
    hasher.add(key.gamutCheck);

    hasher.add(key.sharpMask);
    hasher.add(key.fastPreviewEnabled);
    hasher.add(key.mainPreviewFullResolution);
    hasher.add(key.initialRoughPreview);

    hasher.add(static_cast<int32_t>(key.requestKind));
    hasher.add(key.previousDemoBase);

    hasher.add(key.sourceRotateDegree);
    hasher.add(key.sourceSensorType);
    hasher.add(key.sourceFullWidth);
    hasher.add(key.sourceFullHeight);
    hasher.add(key.sourceFrameCount);
    hasher.add(key.dirPyrDenoiseExpComp);
    hasher.add(key.denoiseAnalysisValid);
    hasher.add(key.spotPending);
    hasher.add(key.spotBufferLive);

    hasher.add(key.cropGuideFingerprint);
    hasher.add(key.viewportMoved);

    return hasher.finish();
}

/* Change one field, then require both equality and the fingerprint to move. */
void expectChange(const char* field,
                  const std::function<void(RenderStateKey&)>& mutate)
{
    const RenderStateKey base = sample();
    RenderStateKey changed = base;
    mutate(changed);

    const std::string message =
        std::string("changing ") + field + " changes the fingerprint";
    check(changed != base, message.c_str());
    if (changed.fingerprint() == base.fingerprint()) {
        std::fprintf(stderr,
                     "FAIL: changing %s leaves the fingerprint at %s\n",
                     field, base.fingerprint().hexString().c_str());
        ++failures;
    }
}

/*
 * A pointer field would make the fingerprint depend on allocation, and an
 * unsupported field type would bypass ParamHasher's typed encoding.
 */
template <typename... T>
constexpr bool noPointers()
{
    return (!std::is_pointer_v<T> && ...);
}

static_assert(noPointers<
                  decltype(RenderStateKey::previewScale),
                  decltype(RenderStateKey::previewWidth),
                  decltype(RenderStateKey::previewHeight),
                  decltype(RenderStateKey::fullWidth),
                  decltype(RenderStateKey::fullHeight),
                  decltype(RenderStateKey::cropX),
                  decltype(RenderStateKey::cropY),
                  decltype(RenderStateKey::cropWidth),
                  decltype(RenderStateKey::cropHeight),
                  decltype(RenderStateKey::cropSkip),
                  decltype(RenderStateKey::borderRequested),
                  decltype(RenderStateKey::borderLeft),
                  decltype(RenderStateKey::borderTop),
                  decltype(RenderStateKey::highDetailNeeded),
                  decltype(RenderStateKey::highDetailPreprocessComputed),
                  decltype(RenderStateKey::highDetailRawComputed),
                  decltype(RenderStateKey::highDetailStateKnown),
                  decltype(RenderStateKey::sourceGeneration),
                  decltype(RenderStateKey::rgbSourceModified),
                  decltype(RenderStateKey::monitorProfile),
                  decltype(RenderStateKey::monitorIntent),
                  decltype(RenderStateKey::softProof),
                  decltype(RenderStateKey::gamutCheck),
                  decltype(RenderStateKey::sharpMask),
                  decltype(RenderStateKey::fastPreviewEnabled),
                  decltype(RenderStateKey::mainPreviewFullResolution),
                  decltype(RenderStateKey::initialRoughPreview),
                  decltype(RenderStateKey::requestKind),
                  decltype(RenderStateKey::previousDemoBase),
                  decltype(RenderStateKey::sourceRotateDegree),
                  decltype(RenderStateKey::sourceSensorType),
                  decltype(RenderStateKey::sourceFullWidth),
                  decltype(RenderStateKey::sourceFullHeight),
                  decltype(RenderStateKey::sourceFrameCount),
                  decltype(RenderStateKey::dirPyrDenoiseExpComp),
                  decltype(RenderStateKey::denoiseAnalysisValid),
                  decltype(RenderStateKey::spotPending),
                  decltype(RenderStateKey::spotBufferLive),
                  decltype(RenderStateKey::cropGuideFingerprint),
                  decltype(RenderStateKey::viewportMoved)>(),
              "RenderStateKey must not contain pointers");

/*
 * Recorded base-cache decisions.
 *
 * Each row is the exact input set and reason the coordinator must derive for
 * one producer intent. The table is the oracle; no live request family is
 * interpreted here.
 */
struct BaseCacheDecision {
    const char* name;
    std::initializer_list<const char*> blocks;
    bool highDetail;
    const char* reason;
};

void checkBaseCacheReplay()
{
    const BaseCacheDecision rows[] = {
        {"AllNoRaw", {}, false, nullptr},
        {"Demosaic", {"raw.demosaic"}, false, "raw"},
        {"Hdr", {}, false, nullptr},
        {"SpotAdjust", {}, false, nullptr},
        {"Transform", {}, false, nullptr},
        {"WhiteBalance", {"raw.base"}, false, "white-balance"},
        {"RepaintOnly", {}, false, nullptr},
        {"ControlOnly", {}, false, nullptr},
        {"Compr", {}, false, nullptr},
        {"CaptureSharpen", {}, false, nullptr},
        {"DarkFrame", {"raw.preprocess", "raw.demosaic"}, false,
         "preprocess"},
        {"First", {"raw.preprocess", "raw.demosaic"}, false, "preprocess"},
        {"AutoExposure", {}, false, nullptr},
        {"LuminanceCurve", {}, false, nullptr},
        {"DirPyrEqualizer", {}, false, nullptr},
        {"RgbCurve", {}, false, nullptr},
        {"Sharpening", {}, false, nullptr},
        {"Retinex", {}, false, nullptr},
        {"Crop", {}, false, nullptr},
        {"Monitor", {}, false, nullptr},
        {"HighQuality", {}, true, "high-detail-request"},
        {"Demosaic+WhiteBalance", {"raw.demosaic", "raw.base"}, false, "raw"},
        {"DarkFrame+HighQuality",
         {"raw.preprocess", "raw.demosaic"}, true, "preprocess"},
        {"WhiteBalance+HighQuality", {"raw.base"}, true, "white-balance"},
    };

    long long comparisons = 0;
    for (const BaseCacheDecision& row : rows) {
        rtengine::stage::DirtyBlocks blocks;
        blocks.valid = true;
        for (const char* id : row.blocks) {
            blocks.ids.insert(id);
        }

        const char* actual = rtengine::stage::baseCacheInvalidationReason(
            rtengine::stage::baseCacheInvalidationFor(blocks,
                                                      row.highDetail));
        const bool equal = (row.reason == nullptr && actual == nullptr)
            || (row.reason != nullptr && actual != nullptr
                && std::string(row.reason) == actual);
        if (!equal) {
            std::fprintf(stderr,
                         "FAIL: base-cache replay %s: expected=%s typed=%s\n",
                         row.name,
                         row.reason != nullptr ? row.reason : "(none)",
                         actual != nullptr ? actual : "(none)");
            ++failures;
        }
        ++comparisons;
    }

    // The narrow typed set maps the base finish to white balance only.
    {
        rtengine::stage::DirtyBlocks broad;
        broad.valid = true;
        broad.ids.insert("raw.base");
        broad.ids.insert("analysis");
        broad.ids.insert("local.adjustments");
        check(rtengine::stage::baseCacheInvalidationFor(broad, false)
                  == rtengine::stage::BaseCacheInvalidation::WhiteBalance,
              "raw.base maps to white-balance");
    }

    std::printf("stage-render-state-test: base-cache replay: %lld decisions\n",
                comparisons);
}

/*
 * The deferred record is typed and can never schedule the base.
 */
void checkDeferredReplay()
{
    using namespace rtengine;

    const stage::DirtyBlocks forced = deferredForcedBlocks();
    check(forced.ids.count("monitor.output") == 1,
          "deferred forced set names monitor.output");
    check(forced.ids.count("rgb.proc") == 1,
          "deferred forced set names rgb.proc");
    check(forced.ids.count("lab.tools") == 1,
          "deferred forced set names lab.tools");
    for (const char* base : {"source.select", "raw.preprocess", "raw.demosaic",
                             "raw.base", "analysis"}) {
        check(forced.ids.count(base) == 0,
              "deferred forced set never names a source or raw block");
    }

    const uint32_t bits = deferredStageBits(forced);
    check(bits == deferredForcedStageBits,
          "deferred stage bits are the canonical forced set");
    rtengine::EditorRequest request;
    request.present = true;
    request.controlOnly = false;
    request.kind = RequestKind::ParamsOnly;
    request.absorbDeferredExecution(forced);
    check(!request.rawIntent && !request.captureSharpenIntent
              && !request.colorHlrWbRequest && !request.fullFrameIntent,
          "the deferred request never carries a base family");
    check(request.monitorIntent && request.rgbProcIntent
              && request.lumaCurveIntent,
          "the deferred request names its typed downstream families");

    DeferredRecord record;
    record.blocks = forced;
    record.forcedAnalysis = true;
    record.hits = 3;
    record.firstHitUs = 100;
    record.deadlineUs = 600;
    const DeferredAnalysisSnapshot snapshot = deferredSnapshot(record);
    check(snapshot.pending(), "deferred snapshot reports pending work");
    check(snapshot.forcedAnalysis(),
          "deferred snapshot carries the forced flag");
    check(snapshot.stageBits() == bits,
          "deferred snapshot carries the typed bits");
    check(snapshot.hits() == 3, "deferred snapshot carries the hit count");

    DeferredRecord empty;
    check(!empty.pending(), "an empty deferred record is not pending");
    check(!deferredSnapshot(empty).pending(),
          "an empty deferred snapshot is not pending");
}

} // namespace

int main()
{
    /* Identical states, including separately allocated strings, agree. */
    {
        const RenderStateKey first = sample();
        const RenderStateKey second = sample();
        check(first == second, "identical states compare equal");
        check(!(first != second), "identical states are not unequal");
        check(first.fingerprint() == second.fingerprint(),
              "identical states produce identical fingerprints");
        check(first.fingerprint().hexString() ==
                  second.fingerprint().hexString(),
              "identical states produce the same hexadecimal form");
        check(first.fingerprint() == canonicalFingerprint(first),
              "the fingerprint is the canonical typed field stream");
    }

    /*
     * The persisted format needs a published value. Update it only together
     * with a deliberate format change.
     */
    {
        const ParamFingerprint golden = sample().fingerprint();
        const std::string expected = "e607b13944b921db473300421f0da1b7";
        if (golden.hexString() != expected) {
            std::fprintf(stderr, "golden fingerprint is %s\n",
                         golden.hexString().c_str());
        }
        check(golden.hexString() == expected,
              "the published render-state fingerprint is stable");
    }

    /* Criterion: every field, changed alone, changes the fingerprint. */
    expectChange("previewScale",
                 [](RenderStateKey& key) { ++key.previewScale; });
    expectChange("previewWidth",
                 [](RenderStateKey& key) { ++key.previewWidth; });
    expectChange("previewHeight",
                 [](RenderStateKey& key) { ++key.previewHeight; });
    expectChange("fullWidth", [](RenderStateKey& key) { ++key.fullWidth; });
    expectChange("fullHeight", [](RenderStateKey& key) { ++key.fullHeight; });

    expectChange("cropX", [](RenderStateKey& key) { ++key.cropX; });
    expectChange("cropY", [](RenderStateKey& key) { ++key.cropY; });
    expectChange("cropWidth", [](RenderStateKey& key) { ++key.cropWidth; });
    expectChange("cropHeight", [](RenderStateKey& key) { ++key.cropHeight; });
    expectChange("cropSkip", [](RenderStateKey& key) { --key.cropSkip; });
    expectChange("borderRequested",
                 [](RenderStateKey& key) { ++key.borderRequested; });
    expectChange("borderLeft",
                 [](RenderStateKey& key) { ++key.borderLeft; });
    expectChange("borderTop",
                 [](RenderStateKey& key) { ++key.borderTop; });

    expectChange("highDetailNeeded",
                 [](RenderStateKey& key) { key.highDetailNeeded = false; });
    expectChange("highDetailPreprocessComputed",
                 [](RenderStateKey& key) {
                     key.highDetailPreprocessComputed = false;
                 });
    expectChange("highDetailRawComputed", [](RenderStateKey& key) {
        key.highDetailRawComputed = true;
    });
    expectChange("highDetailStateKnown",
                 [](RenderStateKey& key) { key.highDetailStateKnown = false; });

    expectChange("sourceGeneration",
                 [](RenderStateKey& key) { ++key.sourceGeneration; });
    expectChange("rgbSourceModified",
                 [](RenderStateKey& key) { key.rgbSourceModified = true; });

    expectChange("monitorProfile", [](RenderStateKey& key) {
        key.monitorProfile += "-softproof";
    });
    expectChange("monitorIntent",
                 [](RenderStateKey& key) { ++key.monitorIntent; });
    expectChange("softProof",
                 [](RenderStateKey& key) { key.softProof = true; });
    expectChange("gamutCheck",
                 [](RenderStateKey& key) { key.gamutCheck = true; });

    expectChange("sharpMask",
                 [](RenderStateKey& key) { key.sharpMask = true; });
    expectChange("fastPreviewEnabled", [](RenderStateKey& key) {
        key.fastPreviewEnabled = false;
    });
    expectChange("mainPreviewFullResolution", [](RenderStateKey& key) {
        key.mainPreviewFullResolution = true;
    });
    expectChange("initialRoughPreview",
                 [](RenderStateKey& key) { key.initialRoughPreview = true; });

    expectChange("requestKind", [](RenderStateKey& key) {
        key.requestKind = RequestKind::HighQuality;
    });
    expectChange("previousDemoBase", [](RenderStateKey& key) {
        key.previousDemoBase =
            static_cast<int32_t>(PreviousDemoBase::Sidecar);
    });

    /* Criterion: every request fact participates in the fingerprint. */
    expectChange("sourceRotateDegree",
                 [](RenderStateKey& key) { key.sourceRotateDegree = 180; });
    expectChange("sourceSensorType",
                 [](RenderStateKey& key) { ++key.sourceSensorType; });
    expectChange("sourceFullWidth",
                 [](RenderStateKey& key) { ++key.sourceFullWidth; });
    expectChange("sourceFullHeight",
                 [](RenderStateKey& key) { ++key.sourceFullHeight; });
    expectChange("sourceFrameCount",
                 [](RenderStateKey& key) { --key.sourceFrameCount; });
    expectChange("dirPyrDenoiseExpComp",
                 [](RenderStateKey& key) { key.dirPyrDenoiseExpComp = -0.5; });
    expectChange("denoiseAnalysisValid",
                 [](RenderStateKey& key) { key.denoiseAnalysisValid = false; });
    expectChange("spotPending",
                 [](RenderStateKey& key) { key.spotPending = false; });
    expectChange("spotBufferLive",
                 [](RenderStateKey& key) { key.spotBufferLive = false; });
    expectChange("cropGuideFingerprint",
                 [](RenderStateKey& key) { key.cropGuideFingerprint = 0; });
    expectChange("viewportMoved",
                 [](RenderStateKey& key) { key.viewportMoved = false; });

    /* Criterion: sourceGeneration is significant at both ends. */
    {
        RenderStateKey first = sample();
        RenderStateKey second = sample();
        first.sourceGeneration = 0;
        second.sourceGeneration = UINT64_C(0xFFFFFFFFFFFFFFFF);
        check(first.fingerprint() != second.fingerprint(),
              "sourceGeneration is part of the fingerprint");
    }

    /* Criterion: every request kind is distinct. */
    {
        const RequestKind kinds[] = {
            RequestKind::ParamsOnly,
            RequestKind::RepaintOnly,
            RequestKind::FullCrop,
            RequestKind::FullPreview,
            RequestKind::HighQuality,
        };
        bool distinct = true;
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
            for (size_t j = i + 1; j < sizeof(kinds) / sizeof(kinds[0]);
                 ++j) {
                RenderStateKey first = sample();
                RenderStateKey second = sample();
                first.requestKind = kinds[i];
                second.requestKind = kinds[j];
                if (first.fingerprint() == second.fingerprint()) {
                    distinct = false;
                    std::fprintf(stderr,
                                 "FAIL: request kinds %zu and %zu collide\n",
                                 i, j);
                }
            }
        }
        check(distinct, "request-kind changes are significant");
    }

    /*
     * Criterion: no pointer values. Two equal strings at different addresses
     * must produce the same fingerprint, so the string is hashed by content.
     */
    {
        RenderStateKey first = sample();
        RenderStateKey second = sample();
        first.monitorProfile = std::string(128, 'm');
        second.monitorProfile = std::string(128, 'm');
        check(first.monitorProfile.data() != second.monitorProfile.data(),
              "the pointer-independence sample has distinct buffers");
        check(first.fingerprint() == second.fingerprint(),
              "equal string contents at different addresses agree");
    }

    /*
     * Criterion: no raw struct hashing. A change in a field must be visible
     * even when the canonical stream is rebuilt by hand, and the canonical
     * stream must match for a copy.
     */
    {
        RenderStateKey changed = sample();
        changed.fullWidth += 1;
        check(canonicalFingerprint(changed) != canonicalFingerprint(sample()),
              "the canonical stream tracks a field change");
        check(changed.fingerprint() == canonicalFingerprint(changed),
              "a changed state still uses the canonical stream");
    }

    checkBaseCacheReplay();
    checkDeferredReplay();

    if (failures == 0) {
        std::puts("stage-render-state-test: PASS");
        return 0;
    }

    std::fprintf(stderr, "stage-render-state-test: %d failure(s)\n", failures);
    return 1;
}
