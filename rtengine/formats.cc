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
#include "formats.h"

#include <algorithm>
#include <cctype>

namespace rtengine
{

namespace
{

const std::vector<std::string>& extensionList()
{
    static const std::vector<std::string> extensions = {
        "3fr", "arw", "arq", "cr2", "cr3", "crf", "crw", "dcr", "dng",
        "fff", "iiq", "jpg", "jpeg",
#ifdef LIBJXL
        "jxl",
#endif
        "kdc", "mef", "mos", "mrw", "nef", "nrw", "orf", "ori", "pef",
        "png", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf", "srw",
        "tif", "tiff", "x3f"
    };
    return extensions;
}

std::string normalise(const std::string& extension)
{
    std::string result;
    result.reserve(extension.size());

    for (const char c : extension) {
        if (c != '.') {
            result.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(c))));
        }
    }

    return result;
}

bool isRaster(const std::string& extension)
{
    return extension == "jpg" || extension == "jpeg"
        || extension == "png" || extension == "tif" || extension == "tiff"
        || extension == "jxl";
}

} // namespace

const std::vector<std::string>& getKnownInputExtensions()
{
    return extensionList();
}

bool isRasterExtension(const std::string& extension)
{
    return isRaster(normalise(extension));
}

bool isRawExtension(const std::string& extension)
{
    const std::string normalised = normalise(extension);
    return isSupportedExtension(normalised) && !isRaster(normalised);
}

bool isSupportedExtension(const std::string& extension)
{
    const std::string normalised = normalise(extension);
    const auto& extensions = extensionList();
    return std::find(extensions.begin(), extensions.end(), normalised)
        != extensions.end();
}

} // namespace rtengine
