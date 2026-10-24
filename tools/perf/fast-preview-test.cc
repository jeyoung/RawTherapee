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

    constexpr int forced =
        M_MONITOR | M_RGBCURVE | M_LUMACURVE | M_LUMINANCE | M_COLOR;

    // An empty request still rebuilds the analysis.
    CHECK(viewportDeferredMask(0) == forced);

    // Downstream stages are preserved.
    CHECK((viewportDeferredMask(M_COLOR) & M_COLOR) != 0);

    // Base bits never reach the deferred pass.
    CHECK((viewportDeferredMask(M_RAW | M_WB | M_INIT | M_COLOR)
           & (M_RAW | M_WB | M_INIT)) == 0);
    CHECK((viewportDeferredMask(M_PREPROC | M_RGBCURVE) & M_PREPROC) == 0);
    CHECK((viewportDeferredMask(M_TRANSFORM | M_COLOR) & M_TRANSFORM) == 0);

    // Accumulating two hits keeps both stages and the forced bits.
    const int merged = viewportDeferredMask(M_RGBCURVE)
        | viewportDeferredMask(M_LUMACURVE);
    CHECK((merged & M_RGBCURVE) != 0);
    CHECK((merged & M_LUMACURVE) != 0);
    CHECK((merged & forced) == forced);

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
