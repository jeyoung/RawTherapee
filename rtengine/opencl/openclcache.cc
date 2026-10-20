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
#include "rtengine/opencl/openclcache.h"

#include "rtengine/settings.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/miscutils.h>

#include <cstdint>
#include <cstring>

namespace rtengine
{

namespace opencl
{

namespace
{

constexpr unsigned char cacheMagic[4] = {'R', 'T', 'C', 'L'};
constexpr std::uint32_t cacheVersion = 1;
constexpr const char* cacheFormat = "rt-opencl-cache-v1";

std::string configuredDirectory;

void appendUint32(std::vector<unsigned char>& data, std::uint32_t value)
{
    data.push_back(static_cast<unsigned char>(value & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

std::uint32_t readUint32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::string fileNameForKey(const std::string& key)
{
    return key + ".bin";
}

} // namespace

std::string programCacheKey(const std::string& source,
                            const std::string& buildOptions,
                            const DeviceInfo& device)
{
    std::string material = cacheFormat;
    material += '\0';
    material += source;
    material += '\0';
    material += buildOptions;
    material += '\0';
    material += device.name;
    material += '\0';
    material += device.vendor;
    material += '\0';
    material += device.driver;
    material += '\0';
    material += device.version;

    gchar* checksum = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar*>(material.data()),
        material.size());

    if (checksum == nullptr) {
        return {};
    }

    std::string key(checksum);
    g_free(checksum);
    return key;
}

std::string programCacheDirectory()
{
    const char* override = g_getenv("RT_OPENCL_CACHE_DIR");

    if (override != nullptr && *override != '\0') {
        return override;
    }

    if (!configuredDirectory.empty()) {
        return configuredDirectory;
    }

    if (settings != nullptr && !settings->cacheDir.empty()) {
        return Glib::build_filename(settings->cacheDir, "opencl");
    }

    return Glib::build_filename(g_get_user_cache_dir(), "rawtherapee", "opencl");
}

void setProgramCacheDirectory(const std::string& directory)
{
    configuredDirectory = directory;
}

bool loadProgramBinary(const std::string& key, std::vector<unsigned char>& binary)
{
    binary.clear();

    if (key.empty()) {
        return false;
    }

    const std::string path = Glib::build_filename(
        programCacheDirectory(), fileNameForKey(key));
    gchar* contents = nullptr;
    gsize length = 0;

    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)
            || contents == nullptr) {
        g_free(contents);
        return false;
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(contents);
    constexpr gsize headerSize = sizeof(cacheMagic) + sizeof(std::uint32_t);

    if (length <= headerSize
            || std::memcmp(bytes, cacheMagic, sizeof(cacheMagic)) != 0
            || readUint32(bytes + sizeof(cacheMagic)) != cacheVersion) {
        g_free(contents);
        return false;
    }

    binary.assign(bytes + headerSize, bytes + length);
    g_free(contents);
    return !binary.empty();
}

bool storeProgramBinary(const std::string& key,
                        const std::vector<unsigned char>& binary)
{
    if (key.empty() || binary.empty()) {
        return false;
    }

    const std::string directory = programCacheDirectory();

    if (g_mkdir_with_parents(directory.c_str(), 0700) != 0) {
        return false;
    }

    std::vector<unsigned char> contents;
    contents.reserve(sizeof(cacheMagic) + sizeof(std::uint32_t) + binary.size());
    contents.insert(contents.end(), cacheMagic, cacheMagic + sizeof(cacheMagic));
    appendUint32(contents, cacheVersion);
    contents.insert(contents.end(), binary.begin(), binary.end());

    const std::string path = Glib::build_filename(directory, fileNameForKey(key));
    const std::string temporary = path + "."
        + std::to_string(g_get_monotonic_time()) + "-"
        + std::to_string(g_random_int()) + ".tmp";
    GError* error = nullptr;

    if (!g_file_set_contents(temporary.c_str(),
                             reinterpret_cast<const gchar*>(contents.data()),
                             static_cast<gssize>(contents.size()), &error)) {
        g_clear_error(&error);
        g_remove(temporary.c_str());
        return false;
    }

    if (g_rename(temporary.c_str(), path.c_str()) != 0) {
        // g_rename does not replace an existing file on Windows.
        if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
            g_remove(path.c_str());
        }

        if (g_rename(temporary.c_str(), path.c_str()) != 0) {
            g_remove(temporary.c_str());
            return false;
        }
    }

    return true;
}

} // namespace opencl

} // namespace rtengine
