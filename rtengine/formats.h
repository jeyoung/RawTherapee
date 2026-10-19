/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 The RawTherapee Team
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

#include <string>
#include <vector>

namespace rtengine
{

/**
 * @brief The input file extensions the build supports.
 *
 * This is the single source of truth for file browser filters, command line
 * format classification and the format coverage tests. Entries are lower case
 * and do not include the leading dot.
 */
const std::vector<std::string>& getKnownInputExtensions();

/**
 * @brief True for raster formats decoded by StdImageSource.
 */
bool isRasterExtension(const std::string& extension);

/**
 * @brief True for raw formats decoded by RawImageSource.
 *
 * Unknown extensions return false. Callers that must preserve the historical
 * behaviour of attempting unknown files through the raw path should use
 * !isRasterExtension() instead.
 */
bool isRawExtension(const std::string& extension);

/**
 * @brief True if the extension is in getKnownInputExtensions().
 */
bool isSupportedExtension(const std::string& extension);

} // namespace rtengine
