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

#include "rtengine/opencl/openclengine.h"

namespace rtengine
{

namespace opencl
{

/**
 * @brief Stable key for a compiled program.
 *
 * The key covers the kernel source, build options, device name, driver and
 * device version, so a driver update invalidates the cache automatically.
 */
std::string programCacheKey(const std::string& source,
                            const std::string& buildOptions,
                            const DeviceInfo& device);

/** @brief Directory containing cached program binaries. */
std::string programCacheDirectory();

/** @brief Load a cached binary. Returns false when missing or invalid. */
bool loadProgramBinary(const std::string& key, std::vector<unsigned char>& binary);

/** @brief Store a program binary. Returns false when the cache is unwritable. */
bool storeProgramBinary(const std::string& key,
                        const std::vector<unsigned char>& binary);

} // namespace opencl

} // namespace rtengine
