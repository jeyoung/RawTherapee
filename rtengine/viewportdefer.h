/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2026 Gabor Horvath
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

#include <algorithm>

#include <glib.h>

#include "refreshmap.h"

namespace rtengine
{

/**
 * @brief Delay before the deferred full-frame analysis pass runs.
 *
 * Unit: milliseconds. Range: 0 to VIEWPORT_DEFERRED_SETTLE_MS_MAX. Zero
 * reproduces the pre-deferral behaviour (the analysis is queued immediately).
 *
 * The default must stay above the adjuster's maximum debounce interval
 * (Options::adjusterMaxDelay, 200 ms by default). A shorter settle would fire
 * between two debounced slider updates and coalesce nothing: the drag would
 * still cost one analysis pass per update, just later.
 */
constexpr int VIEWPORT_DEFERRED_SETTLE_MS_DEFAULT = 250;
constexpr int VIEWPORT_DEFERRED_SETTLE_MS_MIN = 0;
constexpr int VIEWPORT_DEFERRED_SETTLE_MS_MAX = 1000;

/**
 * @brief Upper bound on how long the analysis may be postponed while the user
 * keeps interacting.
 *
 * Unit: milliseconds, measured from the oldest deferred hit. Without it a
 * continuous drag would never refresh the navigator, histograms and scopes.
 */
constexpr int VIEWPORT_DEFERRED_MAX_WAIT_MS_DEFAULT = 500;

/**
 * @brief Granularity of the deferred worker's sleep.
 *
 * Unit: milliseconds. Only bounds how late a deadline can fire; it does not
 * affect the settle window itself.
 */
constexpr int VIEWPORT_DEFERRED_POLL_MS = 10;

/**
 * @brief Watcher sleep when no analysis is pending.
 *
 * Unit: milliseconds. The watcher thread lives for the coordinator's lifetime
 * after the first deferred hit, so it must not wake at the active rate while
 * idle.
 */
constexpr int VIEWPORT_DEFERRED_IDLE_POLL_MS = 100;

/**
 * @brief Build the full-frame analysis mask that a viewport fast-path hit
 * skipped.
 *
 * The forced bits make the deferred pass rebuild the full-frame analysis even
 * when the original request only carried one downstream stage. Base bits are
 * stripped: the fast path never changes the base, and a request that does
 * takes the normal path instead.
 */
constexpr int viewportDeferredMask(int todo)
{
    constexpr int forced =
        M_MONITOR | M_RGBCURVE | M_LUMACURVE | M_LUMINANCE | M_COLOR;
    constexpr int base =
        M_HDR | M_SPOT | M_PREPROC | M_RAW | M_WB | M_INIT | M_LINDENOISE
        | M_CSHARP | M_RETINEX | M_TRANSFORM | M_HIGHQUAL | M_BLURMAP;
    return (todo | forced) & ~base;
}

/**
 * @brief Deadline for the next deferred analysis pass.
 *
 * Each hit re-arms the settle window, but the result never exceeds the
 * max-wait cap measured from the first hit of the burst.
 *
 * All time values are microseconds from g_get_monotonic_time().
 */
inline gint64 viewportDeferredDeadline(gint64 firstHitUs, gint64 nowUs,
                                       int settleMs, int maxWaitMs)
{
    const gint64 settle = nowUs
        + static_cast<gint64>(std::max(settleMs, 0)) * 1000;
    const gint64 cap = firstHitUs
        + static_cast<gint64>(std::max(maxWaitMs, 0)) * 1000;
    return std::min(settle, cap);
}

/**
 * @brief Whether the deferred pass may reuse the crop the fast path rendered.
 *
 * A single hit rendered the crop from the same parameters the deferred pass
 * will use, which is the case the pre-deferral code already suppressed. Once
 * hits accumulate, the last crop render may not cover the earlier stages, so
 * the crops are rendered again. The extra render happens once per settle.
 */
constexpr bool viewportCropRenderReusable(int hitCount)
{
    return hitCount == 1;
}

} // namespace rtengine
