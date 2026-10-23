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

} // namespace

int main()
{
    if (!testTruthTable()) {
        return 1;
    }

    if (!testCrossingRefresh()) {
        return 1;
    }

    std::cout << "fast-preview-test: PASS\n";
    return 0;
}
