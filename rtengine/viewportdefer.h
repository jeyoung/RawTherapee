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
#include <cassert>
#include <cstdint>
#include <type_traits>

#include <glib.h>

#include "stage/stage_dirty_blocks.h"

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

/*
 * Graph nodes a deferred analysis pass may have to run.
 *
 * The retired request mask kept the same set implicitly: the monitor family
 * selected monitor.output, the RGB curve family selected rgb.proc, and the Lab
 * families selected the Lab chain. Spelling the nodes out here makes the
 * mapping a typed fact that
 * the coordinator applies once; no request family decides the schedule.
 */
enum class DeferredStage : uint32_t {
    MonitorOutput = 1u << 0,
    RgbProc = 1u << 1,
    LabTools = 1u << 2,
    LabWavelet = 1u << 3,
    WorkingTrc = 1u << 4,
    CieCam = 1u << 5,
    Gamut = 1u << 6,
    LocalAdjustments = 1u << 7
};

/* The stages a deferred pass always runs: the full-frame analysis chain. */
constexpr uint32_t deferredForcedStageBits =
    static_cast<uint32_t>(DeferredStage::MonitorOutput)
    | static_cast<uint32_t>(DeferredStage::RgbProc)
    | static_cast<uint32_t>(DeferredStage::LabTools)
    | static_cast<uint32_t>(DeferredStage::LabWavelet)
    | static_cast<uint32_t>(DeferredStage::WorkingTrc)
    | static_cast<uint32_t>(DeferredStage::CieCam)
    | static_cast<uint32_t>(DeferredStage::Gamut);

/* Stable lower-case node id of one deferred stage. */
inline const char* deferredStageId(DeferredStage stage)
{
    switch (stage) {
    case DeferredStage::MonitorOutput:
        return "monitor.output";
    case DeferredStage::RgbProc:
        return "rgb.proc";
    case DeferredStage::LabTools:
        return "lab.tools";
    case DeferredStage::LabWavelet:
        return "lab.wavelet";
    case DeferredStage::WorkingTrc:
        return "working.trc";
    case DeferredStage::CieCam:
        return "ciecam";
    case DeferredStage::Gamut:
        return "gamut";
    case DeferredStage::LocalAdjustments:
        return "local.adjustments";
    }

    assert(false && "unknown deferred stage");
    return "";
}

/* Stage bit named by a block id; unknown ids contribute none. */
inline uint32_t deferredStageBitForId(const std::string& id)
{
    if (id == "monitor.output") {
        return static_cast<uint32_t>(DeferredStage::MonitorOutput);
    }
    if (id == "rgb.proc") {
        return static_cast<uint32_t>(DeferredStage::RgbProc);
    }
    if (id == "lab.tools") {
        return static_cast<uint32_t>(DeferredStage::LabTools);
    }
    if (id == "lab.wavelet") {
        return static_cast<uint32_t>(DeferredStage::LabWavelet);
    }
    if (id == "working.trc") {
        return static_cast<uint32_t>(DeferredStage::WorkingTrc);
    }
    if (id == "ciecam") {
        return static_cast<uint32_t>(DeferredStage::CieCam);
    }
    if (id == "gamut") {
        return static_cast<uint32_t>(DeferredStage::Gamut);
    }
    if (id == "local.adjustments") {
        return static_cast<uint32_t>(DeferredStage::LocalAdjustments);
    }
    return 0;
}

/* Typed stage bits named by a dirty-block set; unknown ids contribute none. */
inline uint32_t deferredStageBits(const stage::DirtyBlocks& blocks)
{
    uint32_t bits = 0;
    for (const std::string& id : blocks.ids) {
        bits |= deferredStageBitForId(id);
    }
    return bits;
}

/* Dirty-block set for the stages a deferred pass always runs. */
inline stage::DirtyBlocks deferredForcedBlocks()
{
    stage::DirtyBlocks blocks;
    blocks.valid = true;
    const DeferredStage stages[] = {
        DeferredStage::MonitorOutput,
        DeferredStage::RgbProc,
        DeferredStage::LabTools,
        DeferredStage::LabWavelet,
        DeferredStage::WorkingTrc,
        DeferredStage::CieCam,
        DeferredStage::Gamut
    };
    for (DeferredStage stage : stages) {
        blocks.ids.insert(deferredStageId(stage));
    }
    blocks.blocks.assign(blocks.ids.begin(), blocks.ids.end());
    return blocks;
}

/**
 * @brief Lock-free snapshot of the deferred record.
 *
 * The value is a single 64-bit word so it is always lock-free: the render path
 * loads it with one atomic read and never takes `paramsUpdateMutex`. The typed
 * DeferredStage bit set, the explicit forced-analysis flag and the hit count
 * are packed; the settle window timestamps stay in the authoritative record
 * because only the watcher, which already holds the mutex, reads them.
 */
struct DeferredAnalysisSnapshot {
    static constexpr uint64_t stageMask = 0xFFu;
    static constexpr uint64_t forcedBit = 1ull << 8;
    static constexpr int hitsShift = 32;

    uint64_t packed = 0;

    static DeferredAnalysisSnapshot make(uint32_t stageBits, bool forced,
                                         int32_t hits)
    {
        DeferredAnalysisSnapshot snapshot;
        snapshot.packed =
            (static_cast<uint64_t>(stageBits) & stageMask)
            | (forced ? forcedBit : 0)
            | (static_cast<uint64_t>(
                   static_cast<uint32_t>(hits)) << hitsShift);
        return snapshot;
    }

    uint32_t stageBits() const
    {
        return static_cast<uint32_t>(packed & stageMask);
    }

    bool forcedAnalysis() const
    {
        return (packed & forcedBit) != 0;
    }

    int32_t hits() const
    {
        return static_cast<int32_t>(packed >> hitsShift);
    }

    bool pending() const
    {
        return forcedAnalysis() || stageBits() != 0;
    }
};

static_assert(std::is_trivially_copyable_v<DeferredAnalysisSnapshot>
                  && sizeof(DeferredAnalysisSnapshot) == sizeof(uint64_t),
              "the deferred snapshot must be a lock-free 64-bit value");

/**
 * @brief Authoritative deferred record, guarded by `paramsUpdateMutex`.
 *
 * The block set is the typed record the coordinator accumulates and absorbs.
 * The hit count and the two timestamps are the existing settle-window
 * bookkeeping; the snapshot below is derived from this record after every
 * mutation and published atomically for lock-free readers.
 */
struct DeferredRecord {
    stage::DirtyBlocks blocks;
    bool forcedAnalysis = false;
    int32_t hits = 0;
    gint64 firstHitUs = 0;
    gint64 deadlineUs = 0;

    bool pending() const
    {
        return forcedAnalysis || !blocks.ids.empty();
    }
};

/* Derive the lock-free snapshot from the authoritative record. */
inline DeferredAnalysisSnapshot deferredSnapshot(const DeferredRecord& record)
{
    return DeferredAnalysisSnapshot::make(
        deferredStageBits(record.blocks), record.forcedAnalysis, record.hits);
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
