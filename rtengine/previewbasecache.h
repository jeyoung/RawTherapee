/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2026 Gabor Horvath <hgabor@rawtherapee.com>
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

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <tuple>

#include <glib.h>
#include <glibmm/ustring.h>

#include "colortemp.h"
#include "imagefloat.h"

namespace rtengine
{

class ImageSource;

/**
 * Experimental cache for the display preview's base image, i.e. the output of
 * ImageSource::getImage() before spot removal, colour conversion, film
 * negative processing and the rest of the preview pipeline.
 *
 * The cache is enabled by default. Set RT_PREVIEW_BASE_CACHE=0 (or false/off/no)
 * to disable it for a process. It holds at most one deep copy, so no pointer
 * into orig_prev/oprevi is ever retained. A cache hit restores the snapshot and
 * lets the normal pipeline run from there.
 *
 * The coordinator serialises all cache access with mProcessing. The internal
 * mutex additionally makes each operation atomic for future callers and keeps
 * a failed probe/restore from leaving a partially valid entry behind. It does
 * not protect the ImageSource itself: callers must still respect the
 * coordinator's processing lock.
 */
class PreviewBaseCache
{
public:
    static constexpr std::size_t MAX_BYTES = 256u * 1024u * 1024u;

    struct Key {
        const ImageSource* source = nullptr;
        std::uint64_t sourceGeneration = 0;
        int frame = 0;
        int sensorType = 0;
        int rotateDegree = 0;
        bool raw = false;

        double wbTemp = 0.0;
        double wbGreen = 0.0;
        double wbEqual = 0.0;
        StandardObserver wbObserver = StandardObserver::TWO_DEGREES;

        bool highDetailNeeded = false;
        bool highDetailPreprocessComputed = false;
        bool highDetailRawComputed = false;

        Glib::ustring bayerMethod;
        int bayerCcSteps = 0;
        Glib::ustring xtransMethod;
        int xtransCcSteps = 0;
        double rawExposure = 0.0;

        bool hrEnabled = false;
        Glib::ustring hrMethod;
        int hrBlend = 0;
        double hrThreshold = 0.0;
        bool clampOOG = false;

        bool rgbSourceModified = false;

        int transform = 0;
        int scale = 0;
        int fullWidth = 0;
        int fullHeight = 0;
        int width = 0;
        int height = 0;

        bool operator==(const Key& other) const
        {
            return tie() == other.tie();
        }

    private:
        auto tie() const -> decltype(std::tie(
            source,
            sourceGeneration,
            frame,
            sensorType,
            rotateDegree,
            raw,
            wbTemp,
            wbGreen,
            wbEqual,
            wbObserver,
            highDetailNeeded,
            highDetailPreprocessComputed,
            highDetailRawComputed,
            bayerMethod,
            bayerCcSteps,
            xtransMethod,
            xtransCcSteps,
            rawExposure,
            hrEnabled,
            hrMethod,
            hrBlend,
            hrThreshold,
            clampOOG,
            rgbSourceModified,
            transform,
            scale,
            fullWidth,
            fullHeight,
            width,
            height))
        {
            return std::tie(
                source,
                sourceGeneration,
                frame,
                sensorType,
                rotateDegree,
                raw,
                wbTemp,
                wbGreen,
                wbEqual,
                wbObserver,
                highDetailNeeded,
                highDetailPreprocessComputed,
                highDetailRawComputed,
                bayerMethod,
                bayerCcSteps,
                xtransMethod,
                xtransCcSteps,
                rawExposure,
                hrEnabled,
                hrMethod,
                hrBlend,
                hrThreshold,
                clampOOG,
                rgbSourceModified,
                transform,
                scale,
                fullWidth,
                fullHeight,
                width,
                height);
        }
    };

    PreviewBaseCache()
        : enabled_(!envDisabled(PREVIEW_BASE_CACHE_ENV)),
          profileEnabled_(profileEnvironmentEnabled())
    {
    }

    // Used by the standalone regression test to exercise both modes without
    // depending on the process-wide environment snapshot.
    PreviewBaseCache(bool enabled, bool profileEnabled)
        : enabled_(enabled),
          profileEnabled_(profileEnabled)
    {
    }

    bool enabled() const
    {
        return enabled_;
    }

    bool profileEnabled() const
    {
        return profileEnabled_;
    }

    static bool fits(int width, int height)
    {
        if (width <= 0 || height <= 0) {
            return false;
        }

        // Imagefloat uses 128-bit aligned rows. Account for the padding
        // instead of assuming width * sizeof(float) per row.
        const std::size_t rowBytes =
            (static_cast<std::size_t>(width) * sizeof(float) + 15u) / 16u * 16u;
        const std::size_t planes = 3u;
        const std::size_t rows = static_cast<std::size_t>(height);

        if (rowBytes == 0 || rows > MAX_BYTES / planes / rowBytes) {
            return false;
        }

        return rowBytes * planes * rows <= MAX_BYTES;
    }

    /**
     * Atomically validate the key, check the destination size and restore the
     * snapshot. Returns true on a hit. A key mismatch retains the old entry
     * because it may still be valid for an older key; a broken entry or
     * destination size invalidates the cache instead.
     */
    bool tryRestore(const Key& key, Imagefloat* destination,
                    std::string& reason)
    {
        if (!enabled_) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (!valid_ || !entry_) {
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            ++misses_;
            if (dirty_) {
                reason = "invalidated:" + dirtyReason_;
                dirty_ = false;
                dirtyReason_.clear();
            } else {
                reason = "no-entry";
            }
            noteMiss(reason);
            return false;
        }

        if (!(key_ == key)) {
            ++misses_;
            reason = "key-mismatch";
            noteMiss(reason);
            return false;
        }

        if (entry_->getWidth() != key_.width
                || entry_->getHeight() != key_.height) {
            ++misses_;
            reason = "entry-size-mismatch";
            noteMiss(reason);
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            return false;
        }

        if (!destination
                || destination->getWidth() != key.width
                || destination->getHeight() != key.height) {
            ++misses_;
            reason = "destination-mismatch";
            noteMiss(reason);
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            return false;
        }

        entry_->copyData(destination);

        if (destination->getWidth() != key.width
                || destination->getHeight() != key.height
                || destination->getWidth() == -1) {
            ++misses_;
            reason = "restore-failed";
            noteMiss(reason);
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            return false;
        }

        ++hits_;
        logProbe("hit", std::string());
        return true;
    }

    void store(const Key& key, const Imagefloat* source)
    {
        if (!enabled_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        assert(source != nullptr);
        assert(source != entry_.get());
        assert(key.width == source->getWidth());
        assert(key.height == source->getHeight());

        if (!source
                || source == entry_.get()
                || key.width != source->getWidth()
                || key.height != source->getHeight()) {
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            ++misses_;
            noteMiss("store-failed");
            return;
        }

        if (!fits(source->getWidth(), source->getHeight())) {
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            ++misses_;
            noteMiss("store-failed");
            return;
        }

        // A miss invalidates the previous snapshot. Release it before
        // allocating the replacement so a 256 MiB entry does not briefly
        // become two snapshots plus the source image.
        entry_.reset();
        key_ = Key{};
        valid_ = false;

        std::unique_ptr<Imagefloat> next;
        try {
            next = std::make_unique<Imagefloat>();
            source->copyData(next.get());
        } catch (const std::bad_alloc&) {
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            ++misses_;
            noteMiss("store-failed");
            return;
        }

        if (next->getWidth() != source->getWidth()
                || next->getHeight() != source->getHeight()
                || next->getWidth() == -1) {
            entry_.reset();
            key_ = Key{};
            valid_ = false;
            ++misses_;
            noteMiss("store-failed");
            return;
        }

        entry_ = std::move(next);
        key_ = key;
        valid_ = true;
        dirty_ = false;
        dirtyReason_.clear();
        ++stores_;

        if (profileEnabled_) {
            std::fprintf(stderr,
                         "preview-base-cache: store %dx%d scale=%d gen=%llu\n",
                         key.width, key.height, key.scale,
                         static_cast<unsigned long long>(key.sourceGeneration));
        }
    }

    void noteBypass(const std::string& reason)
    {
        if (!enabled_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ++bypasses_;
        ++bypassReasons_[reason];
        logProbe("bypass", reason);
    }

    void markDirty(const char* reason)
    {
        if (!enabled_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (dirty_) {
            return;
        }

        dirty_ = true;
        dirtyReason_ = reason ? reason : "unknown";
        entry_.reset();
        valid_ = false;
        ++invalidations_;
        ++invalidationReasons_[dirtyReason_];

        if (profileEnabled_) {
            std::fprintf(stderr, "preview-base-cache: invalidate [%s]\n",
                         dirtyReason_.c_str());
        }
    }

    void clear(const char* reason)
    {
        if (!enabled_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (!entry_ && !valid_ && !dirty_) {
            return;
        }

        entry_.reset();
        valid_ = false;
        dirty_ = false;
        dirtyReason_.clear();
        ++invalidations_;

        const std::string text = reason ? reason : "unknown";
        ++invalidationReasons_[text];

        if (profileEnabled_) {
            std::fprintf(stderr, "preview-base-cache: clear [%s]\n",
                         text.c_str());
        }
    }

    void printSummary() const
    {
        if (!enabled_ || !profileEnabled_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        std::fprintf(
            stderr,
            "preview-base-cache: summary hits=%llu misses=%llu "
            "bypasses=%llu stores=%llu invalidations=%llu\n",
            static_cast<unsigned long long>(hits_),
            static_cast<unsigned long long>(misses_),
            static_cast<unsigned long long>(bypasses_),
            static_cast<unsigned long long>(stores_),
            static_cast<unsigned long long>(invalidations_));

        printReasons("miss", missReasons_);
        printReasons("bypass", bypassReasons_);
        printReasons("invalidate", invalidationReasons_);
    }

private:
    static constexpr const char* PREVIEW_BASE_CACHE_ENV =
        "RT_PREVIEW_BASE_CACHE";

    static bool envEnabled(const char* name)
    {
        const char* value = g_getenv(name);
        return value != nullptr && value[0] != '\0'
            && g_strcmp0(value, "0") != 0;
    }

    static bool envDisabled(const char* name)
    {
        const char* value = g_getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return false;
        }

        return g_ascii_strcasecmp(value, "0") == 0
            || g_ascii_strcasecmp(value, "false") == 0
            || g_ascii_strcasecmp(value, "off") == 0
            || g_ascii_strcasecmp(value, "no") == 0;
    }

    static bool profileEnvironmentEnabled()
    {
        return envEnabled("RT_LOAD_PROFILE")
            || envEnabled("RT_PREVIEW_PROFILE");
    }

    void noteMiss(const std::string& reason)
    {
        ++missReasons_[reason];
        logProbe("miss", reason);
    }

    void logProbe(const char* result, const std::string& reason) const
    {
        if (!profileEnabled_) {
            return;
        }

        if (reason.empty()) {
            std::fprintf(stderr, "preview-base-cache: %s\n", result);
        } else {
            std::fprintf(stderr, "preview-base-cache: %s [%s]\n",
                         result, reason.c_str());
        }
    }

    static void printReasons(
        const char* label,
        const std::map<std::string, unsigned long long>& reasons)
    {
        for (const auto& item : reasons) {
            std::fprintf(stderr, "preview-base-cache: %s [%s]=%llu\n",
                         label, item.first.c_str(),
                         static_cast<unsigned long long>(item.second));
        }
    }

    const bool enabled_;
    const bool profileEnabled_;

    mutable std::mutex mutex_;
    std::unique_ptr<Imagefloat> entry_;
    Key key_;
    bool valid_ = false;
    bool dirty_ = false;
    std::string dirtyReason_;

    unsigned long long hits_ = 0;
    unsigned long long misses_ = 0;
    unsigned long long bypasses_ = 0;
    unsigned long long stores_ = 0;
    unsigned long long invalidations_ = 0;

    std::map<std::string, unsigned long long> missReasons_;
    std::map<std::string, unsigned long long> bypassReasons_;
    std::map<std::string, unsigned long long> invalidationReasons_;
};

} // namespace rtengine
