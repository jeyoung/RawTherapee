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
 * @brief Shortest debounce the Preview Refresh preference accepts.
 *
 * Unit: milliseconds. The value is used as both the adjuster debounce and the
 * settle window of the deferred full-frame analysis.
 */
constexpr int PREVIEW_REFRESH_MIN_MS = 100;

/**
 * @brief Longest debounce the Preview Refresh preference accepts.
 *
 * Unit: milliseconds.
 */
constexpr int PREVIEW_REFRESH_MAX_MS = 200;

/**
 * @brief Default Preview Refresh interval.
 *
 * Unit: milliseconds. The legacy adjuster defaults were a 100 ms minimum and
 * a 200 ms maximum; the single slider replaces both with one period.
 */
constexpr int PREVIEW_REFRESH_DEFAULT_MS = 125;

/**
 * @brief Clamp a refresh interval to the supported range.
 *
 * @param ms Interval in milliseconds; any value is accepted.
 * @return The interval clamped to PREVIEW_REFRESH_MIN_MS..PREVIEW_REFRESH_MAX_MS.
 */
constexpr int previewRefreshClamp(int ms)
{
    return std::clamp(ms, PREVIEW_REFRESH_MIN_MS, PREVIEW_REFRESH_MAX_MS);
}

} // namespace rtengine
