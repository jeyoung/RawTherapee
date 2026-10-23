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

#include <glib.h>

namespace rtengine
{

/**
 * @brief Return true when the interactive preview needs the full demosaic.
 *
 * The main preview below 100% follows the Preview Demosaic Method preference.
 * At 100% and above the per-editor-tab FAST toggle decides. Detail windows use
 * the same shared base as the main preview and do not override this choice.
 */
inline bool needFullDemosaic(bool prevdemoFull,
                             bool fastPreviewEffective,
                             bool mainPreviewFullResolution)
{
    if (mainPreviewFullResolution) {
        return !fastPreviewEffective;
    }

    return prevdemoFull;
}

/**
 * @brief Return false when the FAST preview is disabled by the environment.
 *
 * RT_FAST_PREVIEW=0 is the kill switch. Any other value, and an unset
 * variable, leave the feature enabled.
 */
inline bool fastPreviewAllowed()
{
    static const bool allowed = []() {
        const char* value = g_getenv("RT_FAST_PREVIEW");
        return value == nullptr || value[0] == '\0'
            || g_strcmp0(value, "0") != 0;
    }();
    return allowed;
}

} // namespace rtengine
