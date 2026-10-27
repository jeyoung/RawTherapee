/*
 *  This file is part of RawTherapee.
 *
 *  Regression test for the 100% FAST preview decision. Build it with:
 *
 *    cmake --build <build-dir> --target fast-preview-test
 *    <build-dir>/rtengine/fast-preview-test
 */
#include <iostream>

#include "rtengine/previewquality.h"
#include "rtengine/viewportdefer.h"

namespace
{

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::cerr << __FILE__ << ":" << __LINE__                      \
                      << ": CHECK failed: " #expr << "\n";                \
            return false;                                                  \
        }                                                                  \
    } while (false)

bool testTruthTable()
{
    // At 100% and above the per-tab FAST toggle decides.
    CHECK(!rtengine::needFullDemosaic(false, true, true));
    CHECK(!rtengine::needFullDemosaic(true, true, true));
    CHECK(rtengine::needFullDemosaic(false, false, true));
    CHECK(rtengine::needFullDemosaic(true, false, true));

    // Below 100% the Preview Demosaic Method preference is authoritative.
    CHECK(!rtengine::needFullDemosaic(false, true, false));
    CHECK(!rtengine::needFullDemosaic(false, false, false));
    CHECK(rtengine::needFullDemosaic(true, true, false));
    CHECK(rtengine::needFullDemosaic(true, false, false));

    return true;
}

bool testCrossingRefresh()
{
    // Preference unchanged and the crossing leaves the decision unchanged.
    CHECK(!rtengine::mainPreviewCrossingNeedsRefresh(true, false, false, true,
                                                     true, true));
    CHECK(!rtengine::mainPreviewCrossingNeedsRefresh(false, true, false, true,
                                                     true, false));

    // Preference unchanged and the crossing changes the decision.
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(true, true, false, true,
                                                    true, true));
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(false, false, false, true,
                                                    true, false));

    // Both directions of the crossing are symmetric.
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(false, false, true, false,
                                                    true, false));
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(true, true, true, false,
                                                    true, true));

    // The base was built with a different Preview Demosaic Method.
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(true, false, false, true,
                                                    true, false));
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(false, true, false, true,
                                                    true, true));

    // No pass has published the preference yet.
    CHECK(rtengine::mainPreviewCrossingNeedsRefresh(true, false, false, true,
                                                    false, false));

    return true;
}

bool testDeferredMask()
{
    using namespace rtengine;

    // The forced stages are typed: monitor.output, rgb.proc and the Lab chain.
    const stage::DirtyBlocks forced = deferredForcedBlocks();
    CHECK(forced.ids.count("monitor.output") == 1);
    CHECK(forced.ids.count("rgb.proc") == 1);
    CHECK(forced.ids.count("lab.tools") == 1);
    CHECK(forced.ids.count("lab.wavelet") == 1);
    CHECK(forced.ids.count("working.trc") == 1);
    CHECK(forced.ids.count("ciecam") == 1);
    CHECK(forced.ids.count("gamut") == 1);

    // No source or raw block can reach the deferred schedule.
    CHECK(forced.ids.count("source.select") == 0);
    CHECK(forced.ids.count("raw.preprocess") == 0);
    CHECK(forced.ids.count("raw.demosaic") == 0);
    CHECK(forced.ids.count("raw.base") == 0);
    CHECK(forced.ids.count("analysis") == 0);

    const uint32_t bits = deferredStageBits(forced);
    CHECK(bits == deferredForcedStageBits);
    CHECK((bits & static_cast<uint32_t>(DeferredStage::LocalAdjustments)) == 0);

    // The opaque block-body payload covers the same downstream stages.
    const int payload = deferredExecutionPayload(bits);
    CHECK((payload & M_MONITOR) != 0);
    CHECK((payload & M_RGBCURVE) != 0);
    CHECK((payload & (M_LUMACURVE | M_LUMINANCE | M_COLOR))
          == (M_LUMACURVE | M_LUMINANCE | M_COLOR));
    CHECK((payload & (M_PREPROC | M_RAW | M_WB | M_INIT | M_HDR)) == 0);

    // A request-stage addition stays typed and adds only its own payload bit.
    stage::DirtyBlocks withLocal = forced;
    withLocal.ids.insert("local.adjustments");
    const uint32_t withLocalBits = deferredStageBits(withLocal);
    CHECK((withLocalBits
           & static_cast<uint32_t>(DeferredStage::LocalAdjustments)) != 0);
    CHECK((deferredExecutionPayload(withLocalBits) & M_AUTOEXP) != 0);

    // The atomic snapshot is a plain value derived from the record.
    DeferredRecord record;
    record.blocks = forced;
    record.forcedAnalysis = true;
    record.hits = 2;
    record.firstHitUs = 1000;
    record.deadlineUs = 1500;
    const DeferredAnalysisSnapshot snapshot = deferredSnapshot(record);
    CHECK(snapshot.pending());
    CHECK(snapshot.stageBits() == bits);
    CHECK(snapshot.forcedAnalysis());
    CHECK(snapshot.hits() == 2);

    return true;
}

bool testDeferredDeadline()
{
    using namespace rtengine;

    constexpr gint64 ms = 1000;
    const gint64 first = 1000 * ms;
    const gint64 now = first + 20 * ms;

    // The settle window is measured from the latest hit.
    CHECK(viewportDeferredDeadline(first, now, 150, 500)
          == now + 150 * ms);

    // The max-wait cap is measured from the first hit.
    CHECK(viewportDeferredDeadline(first, first + 400 * ms, 150, 500)
          == first + 500 * ms);

    // A non-positive settle fires immediately.
    CHECK(viewportDeferredDeadline(first, now, 0, 500) == now);

    // Negative values are clamped instead of moving the deadline backwards.
    CHECK(viewportDeferredDeadline(first, now, -10, -10)
          == first);

    return true;
}

bool testCropReuse()
{
    using namespace rtengine;

    CHECK(!viewportCropRenderReusable(0));
    CHECK(viewportCropRenderReusable(1));
    CHECK(!viewportCropRenderReusable(2));

    return true;
}

} // namespace

int main()
{
    if (!testTruthTable()) {
        return 1;
    }

    if (!testCrossingRefresh()) {
        return 1;
    }

    if (!testDeferredMask()) {
        return 1;
    }

    if (!testDeferredDeadline()) {
        return 1;
    }

    if (!testCropReuse()) {
        return 1;
    }

    std::cout << "fast-preview-test: PASS\n";
    return 0;
}
