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

namespace rtengine
{

/**
 * @brief Shortest session preview period the debounce and deferred analysis
 * accept.
 *
 * Unit: milliseconds. The same period is used for the adjuster debounce of the
 * active tab and for the settle window and max-wait cap of the deferred
 * full-frame analysis.
 */
constexpr int PREVIEW_REFRESH_MIN_MS = 100;

/**
 * @brief Longest session preview period the debounce and deferred analysis
 * accept.
 *
 * Unit: milliseconds. Heavy images can legitimately need more than the old
 * 200 ms preference ceiling, so the dynamic period is allowed to grow to one
 * second before it is clamped.
 */
constexpr int PREVIEW_REFRESH_MAX_MS = 1000;

/**
 * @brief Fallback session preview period.
 *
 * Unit: milliseconds. Used until a complete refresh has been measured, and by
 * adjusters that are not attached to an active measured session.
 */
constexpr int PREVIEW_REFRESH_DEFAULT_MS = 125;

/**
 * @brief Clamp a preview period to the supported range.
 *
 * @param ms Period in milliseconds; any value is accepted.
 * @return The period clamped to PREVIEW_REFRESH_MIN_MS..PREVIEW_REFRESH_MAX_MS.
 */
constexpr int previewRefreshClamp(int ms)
{
    return std::clamp(ms, PREVIEW_REFRESH_MIN_MS, PREVIEW_REFRESH_MAX_MS);
}

/**
 * @brief Resolve a possibly unmeasured session period.
 *
 * @param ms Measured or smoothed period in milliseconds, or -1 when the session
 * has no complete measurement yet.
 * @return The period when it is non-negative, PREVIEW_REFRESH_DEFAULT_MS
 * otherwise. The result is not clamped; callers pass it through
 * previewRefreshClamp() when a measurement is involved.
 */
constexpr int previewRefreshResolved(int ms)
{
    return ms >= 0 ? ms : PREVIEW_REFRESH_DEFAULT_MS;
}

/**
 * @brief Blend a new measurement into the smoothed session period.
 *
 * Unit: milliseconds. The measured refresh cost varies by a few milliseconds
 * between frames, so a three-quarter weight on the previous value keeps the
 * debounce and the deferred settle/cap stable while still tracking a real
 * change within a few refreshes. A negative previous value seeds the average
 * with the first measurement. The result is clamped to the supported range, so
 * jitter cannot push the period outside it.
 *
 * @param previousMs Current smoothed period, or -1 when unmeasured.
 * @param measuredMs Latest measured period in milliseconds.
 * @return The smoothed, clamped period.
 */
constexpr int previewRefreshSmooth(int previousMs, int measuredMs)
{
    const int measured = previewRefreshClamp(measuredMs);

    if (previousMs < 0) {
        return measured;
    }

    return previewRefreshClamp((3 * previousMs + measured) / 4);
}

} // namespace rtengine
