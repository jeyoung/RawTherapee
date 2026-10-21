/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 Eddy Young <jeyoung@priscimon.com>
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include <glib.h>

namespace rtengine
{

/**
 * Opt-in wall-clock profiling of the initial image load and first preview.
 *
 * Set RT_LOAD_PROFILE=1 in the environment to enable it. When disabled every
 * instrumentation point is a single cached environment check, so the cost is
 * negligible. The original RT_PREVIEW_PROFILE timer remains responsible for
 * per-request preview timings.
 */
namespace LoadProfile
{

using Clock = std::chrono::steady_clock;

inline bool isEnabled()
{
    static const bool enabled = []() {
        const char* value = g_getenv("RT_LOAD_PROFILE");
        return value != nullptr && value[0] != '\0'
            && g_strcmp0(value, "0") != 0;
    }();
    return enabled;
}

struct State {
    std::atomic<bool> loadActive{false};
    std::atomic<long long> loadStartNs{0};

    // One-shot flags, reset by beginLoad(), so that only the first occurrence
    // after an initial image load is reported.
    std::atomic<bool> preprocessSeen{false};
    std::atomic<bool> demosaicSeen{false};
    std::atomic<bool> getImageSeen{false};
    std::atomic<bool> pipelineSeen{false};
    std::atomic<bool> setImageSeen{false};
    std::atomic<bool> imageReadySeen{false};
};

inline State& state()
{
    static State instance;
    return instance;
}

inline long long nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

inline double msBetween(long long startNs, long long endNs)
{
    return static_cast<double>(endNs - startNs) / 1e6;
}

inline void report(const char* phase, long long startNs,
                   const char* detail = nullptr, bool isMark = false)
{
    const long long endNs = nowNs();
    const long long loadStartNs =
        state().loadStartNs.load(std::memory_order_relaxed);

    std::string line = "load-profile: ";
    line += phase;

    if (detail != nullptr && detail[0] != '\0') {
        line += " [";
        line += detail;
        line += ']';
    }

    if (!isMark) {
        char duration[32];
        std::snprintf(duration, sizeof duration, " %.1f ms",
                      msBetween(startNs, endNs));
        line += duration;
    }

    if (loadStartNs > 0) {
        char sinceLoad[40];
        std::snprintf(sinceLoad, sizeof sinceLoad, " (t+%.1f ms)",
                      msBetween(loadStartNs, endNs));
        line += sinceLoad;
    }

    line += '\n';
    std::fputs(line.c_str(), stderr);
}

/**
 * Marks the start of an initial image load. Resets the first-preview flags and
 * the reference time used by the "t+" column.
 */
inline void beginLoad()
{
    if (!isEnabled()) {
        return;
    }

    State& s = state();
    s.loadStartNs.store(nowNs(), std::memory_order_relaxed);
    s.preprocessSeen.store(false, std::memory_order_relaxed);
    s.demosaicSeen.store(false, std::memory_order_relaxed);
    s.getImageSeen.store(false, std::memory_order_relaxed);
    s.pipelineSeen.store(false, std::memory_order_relaxed);
    s.setImageSeen.store(false, std::memory_order_relaxed);
    s.imageReadySeen.store(false, std::memory_order_relaxed);
    s.loadActive.store(true, std::memory_order_relaxed);
}

inline void endLoad()
{
    if (!isEnabled()) {
        return;
    }

    state().loadActive.store(false, std::memory_order_relaxed);
}

class Session
{
public:
    Session()
    {
        beginLoad();
    }

    ~Session()
    {
        endLoad();
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

/**
 * Reports the wall time of the enclosing scope.
 *
 * @param phase       phase name printed on the profiling line
 * @param firstOnly   if set, only the first occurrence after beginLoad() is
 *                    reported and the flag is claimed
 * @param detail      optional short qualifier, e.g. the demosaic method
 * @param onlyInLoad  if true, do nothing outside an InitialImage::load call
 */
class Timer
{
public:
    explicit Timer(const char* phase, std::atomic<bool>* firstOnly = nullptr,
                   const char* detail = nullptr, bool onlyInLoad = false)
        : phase_(phase), startNs_(0), active_(false), detail_()
    {
        if (!isEnabled()) {
            return;
        }

        if (onlyInLoad
                && !state().loadActive.load(std::memory_order_relaxed)) {
            return;
        }

        if (firstOnly != nullptr
                && firstOnly->exchange(true, std::memory_order_relaxed)) {
            return;
        }

        startNs_ = nowNs();

        if (detail != nullptr) {
            detail_ = detail;
        }

        active_ = true;
    }

    ~Timer()
    {
        stop();
    }

    void stop()
    {
        if (!active_) {
            return;
        }

        active_ = false;
        report(phase_, startNs_, detail_.empty() ? nullptr : detail_.c_str());
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    const char* phase_;
    long long startNs_;
    bool active_;
    std::string detail_;
};

/**
 * Reports an instantaneous event, relative to the start of the load.
 */
inline void markFirst(const char* phase, std::atomic<bool>& firstOnly,
                      const char* detail = nullptr)
{
    if (!isEnabled()) {
        return;
    }

    if (firstOnly.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    report(phase, 0, detail, true);
}

} // namespace LoadProfile

} // namespace rtengine
