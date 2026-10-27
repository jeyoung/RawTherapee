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

/*
 * Node-output cache of the preview raw.base tier.
 *
 * The test pins six things: the key covers every byte-affecting input it
 * names and rejects a different image generation, profile, geometry, work
 * order, backend, node or output size; the reuse decision follows the typed
 * work order and refuses an unstable spot state; the budget, true LRU order,
 * the counters and the disabled no-allocation path behave; a repeated
 * identical pass restores the exact bytes (AE=0) with a hit; a white-balance
 * sentinel round-trips without clamping; and a malformed auxiliary payload is
 * detected rather than used.
 *
 * The image is synthetic. It stands in for the Imagefloat tier so the test
 * needs no raw file and no coordinator.
 */

#include "stage_cache_integration.h"

#include "colortemp.h"
#include "stage_param_hash.h"
#include "stage_render_state.h"
#include "stage_work.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using namespace rtengine::stage;

ParamFingerprint fingerprintOf(const char* label, double value)
{
    ParamHasher hasher;
    hasher.addSectionLabel(label);
    hasher.add(value);
    return hasher.finish();
}

RenderStateKey baseRenderState()
{
    RenderStateKey state;
    state.previewScale = 4;
    state.previewWidth = 800;
    state.previewHeight = 600;
    state.fullWidth = 3200;
    state.fullHeight = 2400;
    state.sourceGeneration = 11;
    state.requestKind = RequestKind::ParamsOnly;
    state.previousDemoBase = static_cast<int32_t>(PreviousDemoBase::Fast);
    return state;
}

NodeCacheKey baseKey(const RenderStateKey& state)
{
    NodeCacheKey key;
    key.node = "preview.raw.base";
    key.render = state.fingerprint();
    key.block_ids = {"raw.preprocess", "raw.demosaic", "raw.base"};
    key.block_values = {
        fingerprintOf("raw.preprocess", 1.0),
        fingerprintOf("raw.demosaic", 2.0),
        fingerprintOf("raw.base", 3.0),
    };
    WorkOrder work;
    work.set(WorkItem::rawBaseRebuild);
    key.work = nodeCacheWorkBits(work) & nodeCacheTierMask();
    key.backend = fingerprintOf("backend", 5.0);
    key.width = state.previewWidth;
    key.height = state.previewHeight;
    key.planes = 3;
    key.format = RT_STAGE_F32;
    return key;
}

RtStageBuffer makeBuffer(std::vector<float>& data, int width, int height)
{
    RtStageBuffer buffer{};
    buffer.size = sizeof(buffer);
    buffer.data = data.data();
    buffer.width = width;
    buffer.height = height;
    buffer.row_pitch = static_cast<size_t>(width) * sizeof(float);
    buffer.plane_pitch = buffer.row_pitch * static_cast<size_t>(height);
    buffer.planes = 3;
    buffer.format = RT_STAGE_F32;
    buffer.flags = RT_STAGE_BUFFER_READ;
    return buffer;
}

void fillSynthetic(std::vector<float>& data)
{
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>((i * 37u + 11u) % 251u) / 251.0f;
    }
}

/* Absolute-error count between two float images, in elements. */
size_t absoluteError(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) {
        return a.size() + b.size();
    }
    size_t errors = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++errors;
        }
    }
    return errors;
}

void testSwitchParser()
{
    check(!nodeCacheValueEnables(nullptr), "unset disables the node cache");
    check(nodeCacheValueEnables(""), "empty value enables the node cache");
    check(nodeCacheValueEnables("1"), "1 enables the node cache");
    check(nodeCacheValueEnables("yes"), "any other value enables the node cache");
    check(!nodeCacheValueEnables("0"), "0 disables the node cache");
    check(!nodeCacheValueEnables("off"), "off disables the node cache");
    check(!nodeCacheValueEnables("OFF"), "OFF disables the node cache");

    const size_t budget = nodeCacheBudgetMiB();
    check(budget >= 1 && budget <= 8192,
          "the budget is inside the documented range");
}

void testKeyCollisions()
{
    const RenderStateKey state = baseRenderState();
    const NodeCacheKey base = baseKey(state);
    const StageKey base_stage = nodeCacheStageKey(base);

    check(nodeCacheStageKey(base) == base_stage, "a key equals itself");

    // Different image generation.
    {
        RenderStateKey other = state;
        other.sourceGeneration = 12;
        check(nodeCacheStageKey(baseKey(other)) != base_stage,
              "a different source generation is a different key");
    }

    // Different geometry: preview size and crop rectangle.
    {
        RenderStateKey other = state;
        other.previewWidth = 801;
        check(nodeCacheStageKey(baseKey(other)) != base_stage,
              "a different preview width is a different key");
    }
    {
        RenderStateKey other = state;
        other.cropX = 16;
        other.cropWidth = 400;
        check(nodeCacheStageKey(baseKey(other)) != base_stage,
              "a different crop rectangle is a different key");
    }
    {
        RenderStateKey other = state;
        other.previewScale = 2;
        check(nodeCacheStageKey(baseKey(other)) != base_stage,
              "a different preview scale is a different key");
    }
    {
        RenderStateKey other = state;
        other.spotPending = true;
        check(nodeCacheStageKey(baseKey(other)) != base_stage,
              "a pending spot request is a different key");
    }
    {
        RenderStateKey other = state;
        other.spotBufferLive = true;
        check(nodeCacheStageKey(baseKey(other)) != base_stage,
              "a live spot buffer is a different key");
    }

    // Different profile: one block fingerprint changes.
    {
        NodeCacheKey other = base;
        other.block_values[2] = fingerprintOf("raw.base", 4.0);
        check(nodeCacheStageKey(other) != base_stage,
              "a different raw.base profile is a different key");
    }
    {
        NodeCacheKey other = base;
        other.block_ids[1] = "raw.preprocess";
        check(nodeCacheStageKey(other) != base_stage,
              "a different block identity is a different key");
    }

    // Different work order: the producing item set changes.
    {
        NodeCacheKey other = base;
        WorkOrder work;
        work.set(WorkItem::spotApply);
        other.work = nodeCacheWorkBits(work) & nodeCacheTierMask();
        check(nodeCacheStageKey(other) != base_stage,
              "a different producing work order is a different key");
    }

    // Different compute backend.
    {
        NodeCacheKey other = base;
        other.backend = fingerprintOf("backend", 6.0);
        check(nodeCacheStageKey(other) != base_stage,
              "a different backend is a different key");
    }

    // Different node.
    {
        NodeCacheKey other = base;
        other.node = "preview.raw.core";
        check(nodeCacheStageKey(other) != base_stage,
              "a different node id is a different key");
    }

    // Different output geometry.
    {
        NodeCacheKey other = base;
        other.width = 400;
        check(nodeCacheStageKey(other) != base_stage,
              "a different output width is a different key");
    }
    {
        NodeCacheKey other = base;
        other.format = RT_STAGE_U16;
        check(nodeCacheStageKey(other) != base_stage,
              "a different output format is a different key");
    }
}

void testReuseDecision()
{
    const RenderStateKey stable = baseRenderState();

    WorkOrder source;
    source.set(WorkItem::sourceSelect);
    check(!nodeCacheReuseAllowed(source, stable),
          "a source selection refuses the tier");

    WorkOrder preprocess;
    preprocess.set(WorkItem::rawPreprocess);
    check(!nodeCacheReuseAllowed(preprocess, stable),
          "a preprocess run refuses the tier");

    WorkOrder demosaic;
    demosaic.set(WorkItem::rawDemosaic);
    check(!nodeCacheReuseAllowed(demosaic, stable),
          "a demosaic run refuses the tier");

    WorkOrder retinex;
    retinex.set(WorkItem::retinexPrepare);
    check(!nodeCacheReuseAllowed(retinex, stable),
          "a retinex preparation refuses the tier");

    WorkOrder rebuild;
    rebuild.set(WorkItem::rawBaseRebuild);
    check(nodeCacheReuseAllowed(rebuild, stable),
          "a base rebuild with unchanged source inputs is reusable");
    check(nodeCacheSubstitutionAllowed(rebuild),
          "a scheduled base rebuild permits the substitution");

    WorkOrder empty;
    check(nodeCacheReuseAllowed(empty, stable),
          "an empty order permits reuse");
    check(!nodeCacheSubstitutionAllowed(empty),
          "an empty order never substitutes: downstream keeps its buffer");

    WorkOrder spot;
    spot.set(WorkItem::spotApply);
    check(nodeCacheReuseAllowed(spot, stable),
          "a spot-only pass with a stable buffer permits reuse");
    check(!nodeCacheSubstitutionAllowed(spot),
          "a spot-only pass never substitutes");

    WorkOrder rebuildAndSpot;
    rebuildAndSpot.set(WorkItem::rawBaseRebuild);
    rebuildAndSpot.set(WorkItem::spotApply);
    check(nodeCacheReuseAllowed(rebuildAndSpot, stable),
          "a rebuild that also applies spots is reusable while stable");
    check(nodeCacheSubstitutionAllowed(rebuildAndSpot),
          "a rebuild that also applies spots substitutes the whole tier");

    /*
     * The spot-state guard: the live buffer contributes pixels that the key
     * does not carry, so either fact refuses both the store and the probe.
     */
    RenderStateKey pending = stable;
    pending.spotPending = true;
    check(!nodeCacheReuseAllowed(rebuild, pending),
          "a pending spot request refuses the tier");

    RenderStateKey live = stable;
    live.spotBufferLive = true;
    check(!nodeCacheReuseAllowed(rebuild, live),
          "a live spot buffer refuses the tier");
    check(!nodeCacheReuseAllowed(rebuildAndSpot, live),
          "a live spot buffer refuses even a rebuilding spot pass");

    RenderStateKey both = stable;
    both.spotPending = true;
    both.spotBufferLive = true;
    check(!nodeCacheReuseAllowed(rebuild, both),
          "both spot facts together refuse the tier");
}

/*
 * The call site in stageRawBase combines the work-order and spot-state guard
 * with the substitution rule before it probes or stores. This test runs that
 * exact expression: a live-buffer pass is counted as refused and never
 * touches the cache, while the stable pass stores and hits.
 */
void testCallSiteGuard()
{
    constexpr int width = 32;
    constexpr int height = 24;
    std::vector<float> data(width * height * 3);
    fillSynthetic(data);
    const RtStageBuffer source = makeBuffer(data, width, height);

    WorkOrder work;
    work.set(WorkItem::rawBaseRebuild);

    const RenderStateKey stable = baseRenderState();
    RenderStateKey live = stable;
    live.spotBufferLive = true;

    NodeCache cache(true, 64u * 1024u);
    const NodeCacheKey stable_key = baseKey(stable);
    const NodeCacheKey live_key = baseKey(live);

    // The stable pass is admitted exactly as the call site admits it.
    bool admitted = nodeCacheReuseAllowed(work, stable)
                 && nodeCacheSubstitutionAllowed(work);
    check(admitted, "the call-site guard admits a stable rebuild");
    if (admitted) {
        check(!cache.probe(stable_key), "the stable pass misses first");
        check(cache.store(stable_key, source), "the stable pass stores");
        check(cache.probe(stable_key), "the repeated stable pass hits");
    }

    // The live-buffer pass is refused exactly as the call site refuses it.
    admitted = nodeCacheReuseAllowed(work, live)
            && nodeCacheSubstitutionAllowed(work);
    check(!admitted, "the call-site guard refuses a live spot buffer");
    if (!admitted) {
        cache.note_refused();
    }
    check(cache.stats().refused == 1, "the refused probe is counted");
    check(cache.contains(stable_key), "the refused pass left the cache alone");
    check(!cache.contains(live_key), "the refused pass stored nothing");
}

void testRepeatedPassParity()
{
    const RenderStateKey state = baseRenderState();
    const NodeCacheKey key = baseKey(state);

    constexpr int width = 32;
    constexpr int height = 24;
    std::vector<float> pass_a(width * height * 3);
    fillSynthetic(pass_a);
    RtStageBuffer source = makeBuffer(pass_a, width, height);

    RawBaseTierState tier_state;
    tier_state.wb_temp = 6500.0;
    tier_state.wb_green = 1.03;
    tier_state.wb_equal = 1.0;
    tier_state.wb_observer = 2;
    tier_state.wb_method = "Custom";
    tier_state.denoise_valid = false;
    tier_state.histogram = {1u, 2u, 3u, 65536u};
    const std::vector<unsigned char> aux =
        encodeRawBaseTierState(tier_state);

    // A practical budget: two 9 KiB tiers plus a little slack.
    NodeCache cache(true, 64u * 1024u);

    // First pass: miss, compute, store.
    check(!cache.probe(key), "the first pass misses");
    check(cache.store(key, source, aux.data(), aux.size()),
          "the first pass stores the tier");

    // Second, identical pass: hit, restore into a zeroed tier.
    std::vector<float> pass_b(pass_a.size(), 0.0f);
    RtStageBuffer restored = makeBuffer(pass_b, width, height);
    std::vector<unsigned char> restored_aux;

    check(cache.probe(key), "the repeated pass hits");
    check(cache.restore(key, restored, &restored_aux),
          "the repeated pass restores the tier");
    const size_t ae = absoluteError(pass_a, pass_b);
    check(ae == 0, "the repeated pass is pixel-exact (AE=0)");

    RawBaseTierState decoded;
    check(decodeRawBaseTierState(restored_aux.data(), restored_aux.size(),
                                 decoded),
          "the auxiliary state decodes");
    check(decoded.wb_method == tier_state.wb_method
              && decoded.wb_temp == tier_state.wb_temp
              && decoded.histogram == tier_state.histogram
              && decoded.denoise_valid == tier_state.denoise_valid,
          "the auxiliary state round-trips");

    const StageCacheStats stats = cache.stats();
    check(stats.hits == 1, "one hit is counted");
    check(stats.misses == 1, "one miss is counted");
    check(stats.inserts == 1, "one insert is counted");
    check(stats.bytes_used == pass_a.size() * sizeof(float) + aux.size(),
          "the auxiliary bytes share the budget");

    std::printf("repeated-pass: hits=%llu misses=%llu AE=%zu\n",
                static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.misses), ae);
}

void testWhiteBalanceSentinel()
{
    /*
     * The body assigns ColorTemp() when white balance is disabled. Its
     * temperature and green are -1, which the clipping constructor turns into
     * 1500 K / 0.02. RawImageSource::getImage() branches on `getTemp() < 0`
     * to restore the raw pre-multipliers, so restoring the clamped value
     * changes the rendered bytes.
     */
    const rtengine::ColorTemp live;
    check(live.getTemp() < 0.0,
          "the default ColorTemp is the no-balance sentinel");

    RawBaseTierState state;
    state.wb_temp = live.getTemp();
    state.wb_green = live.getGreen();
    state.wb_equal = live.getEqual();
    state.wb_observer = static_cast<int32_t>(live.getObserver());
    state.wb_method = live.getMethod();
    state.wb_no_balance = live.getTemp() < 0.0;
    state.histogram = {9u, 10u};

    const std::vector<unsigned char> blob = encodeRawBaseTierState(state);
    RawBaseTierState decoded;
    check(decodeRawBaseTierState(blob.data(), blob.size(), decoded),
          "the no-balance payload decodes");
    check(decoded.wb_no_balance, "the no-balance flag survives the round trip");

    rtengine::ColorTemp restored;
    check(rawBaseTierWhiteBalance(decoded, restored),
          "the no-balance state is restored exactly");
    check(restored.getTemp() == live.getTemp()
              && restored.getGreen() == live.getGreen()
              && restored.getEqual() == live.getEqual()
              && restored.getMethod() == live.getMethod()
              && restored.getObserver() == live.getObserver(),
          "the sentinel is restored field for field");

    // A state that the constructor would clip is refused, not approximated.
    RawBaseTierState unrepresentable = decoded;
    unrepresentable.wb_no_balance = false;
    unrepresentable.wb_temp = rtengine::MINTEMP - 1.0;
    unrepresentable.wb_green = 1.0;
    unrepresentable.wb_equal = 1.0;
    unrepresentable.wb_method = "Custom";
    rtengine::ColorTemp refused;
    check(!rawBaseTierWhiteBalance(unrepresentable, refused),
          "a state that would be clipped is refused");

    const rtengine::ColorTemp naive(
        decoded.wb_temp, decoded.wb_green, decoded.wb_equal,
        decoded.wb_method,
        static_cast<rtengine::StandardObserver>(decoded.wb_observer));
    check(naive.getTemp() != live.getTemp(),
          "the clipping constructor changes the sentinel temperature");
    check(naive.getTemp() >= rtengine::MINTEMP,
          "the clipped sentinel is inside the temperature range");

    /*
     * Byte-for-byte: reproduce the branch the base body takes. The no-balance
     * path uses the raw pre-multipliers; every other path uses the white
     * balance multipliers (RawImageSource::getImage() branches on
     * `getTemp() < 0`). The constants stand in for the two multiplier sets,
     * which is enough to expose the sentinel-vs-clamped difference without a
     * global Settings object.
     */
    constexpr size_t pixel_values = 32u * 24u * 3u;
    constexpr double pre_mul_r = 1.25;
    constexpr double pre_mul_g = 1.0;
    constexpr double pre_mul_b = 0.75;
    constexpr double wb_mul_r = 0.80;
    constexpr double wb_mul_g = 1.10;
    constexpr double wb_mul_b = 1.35;
    const auto apply = [=](const rtengine::ColorTemp& wb,
                           std::vector<float>& pixels) {
        double r = pre_mul_r;
        double g = pre_mul_g;
        double b = pre_mul_b;
        if (wb.getTemp() >= 0.0) {
            r = wb_mul_r;
            g = wb_mul_g;
            b = wb_mul_b;
        }
        for (size_t i = 0; i + 2 < pixels.size(); i += 3) {
            pixels[i] *= static_cast<float>(r);
            pixels[i + 1] *= static_cast<float>(g);
            pixels[i + 2] *= static_cast<float>(b);
        }
    };

    std::vector<float> uncached(pixel_values);
    fillSynthetic(uncached);
    std::vector<float> cached = uncached;
    std::vector<float> clamped = uncached;
    apply(live, uncached);
    apply(restored, cached);
    apply(naive, clamped);
    check(absoluteError(uncached, cached) == 0,
          "a cached no-WB pass is byte-identical to an uncached one");
    check(absoluteError(uncached, clamped) != 0,
          "the clamped sentinel would render different bytes");

    // A normal white balance still round-trips exactly.
    RawBaseTierState normal;
    normal.wb_temp = 6500.0;
    normal.wb_green = 1.03;
    normal.wb_equal = 1.0;
    normal.wb_observer =
        static_cast<int32_t>(rtengine::StandardObserver::TWO_DEGREES);
    normal.wb_method = "Custom";
    const std::vector<unsigned char> normal_blob =
        encodeRawBaseTierState(normal);
    RawBaseTierState normal_decoded;
    check(decodeRawBaseTierState(normal_blob.data(), normal_blob.size(),
                                 normal_decoded),
          "the normal white-balance payload decodes");
    rtengine::ColorTemp normal_restored;
    check(rawBaseTierWhiteBalance(normal_decoded, normal_restored),
          "the normal state is restored exactly");
    check(normal_restored.getTemp() == normal.wb_temp
              && normal_restored.getGreen() == normal.wb_green
              && normal_restored.getEqual() == normal.wb_equal
              && normal_restored.getMethod() == normal.wb_method,
          "a normal white balance round-trips without clamping");
}

void testTrueLru()
{
    constexpr int width = 32;
    constexpr int height = 24;
    std::vector<float> data(width * height * 3);
    fillSynthetic(data);
    const RtStageBuffer source = makeBuffer(data, width, height);
    const size_t tier_bytes = data.size() * sizeof(float);

    NodeCacheKey a = baseKey(baseRenderState());
    RenderStateKey state_b = baseRenderState();
    state_b.sourceGeneration = 21;
    NodeCacheKey b = baseKey(state_b);
    RenderStateKey state_c = baseRenderState();
    state_c.sourceGeneration = 22;
    NodeCacheKey c = baseKey(state_c);

    // Room for two entries.
    NodeCache cache(true, 2 * tier_bytes + tier_bytes / 2);

    check(cache.store(a, source), "LRU: the first tier stores");
    check(cache.store(b, source), "LRU: the second tier stores");

    // A probe refreshes recency. Under FIFO, inserting c would evict a.
    check(cache.probe(a), "LRU: the first tier is probed");
    check(cache.store(c, source), "LRU: the third tier stores");
    check(cache.contains(a), "LRU: the probed tier survives eviction");
    check(!cache.contains(b), "LRU: the untouched tier is evicted");
    check(cache.contains(c), "LRU: the newest tier is kept");
    check(cache.stats().evictions == 1, "LRU: one capacity eviction");

    // restore() refreshes recency too: store a and b, fetch a, then c.
    NodeCache fetch_cache(true, 2 * tier_bytes + tier_bytes / 2);
    check(fetch_cache.store(a, source), "LRU: fetch case first tier stores");
    check(fetch_cache.store(b, source), "LRU: fetch case second tier stores");
    std::vector<float> destination_data(data.size(), 0.0f);
    RtStageBuffer destination = makeBuffer(destination_data, width, height);
    check(fetch_cache.restore(a, destination),
          "LRU: the first tier restores");
    check(fetch_cache.store(c, source), "LRU: fetch case third tier stores");
    check(fetch_cache.contains(a), "LRU: the restored tier survives");
    check(!fetch_cache.contains(b), "LRU: the never-accessed tier is evicted");

    std::printf("lru: evictions=%llu\n",
                static_cast<unsigned long long>(cache.stats().evictions));
}

void testBudgetAndEviction()
{
    constexpr int width = 32;
    constexpr int height = 24;
    std::vector<float> data(width * height * 3);
    fillSynthetic(data);
    const RtStageBuffer source = makeBuffer(data, width, height);
    const size_t tier_bytes = data.size() * sizeof(float);

    NodeCache cache(true, tier_bytes + tier_bytes / 2);

    NodeCacheKey first = baseKey(baseRenderState());
    RenderStateKey other = baseRenderState();
    other.sourceGeneration = 99;
    NodeCacheKey second = baseKey(other);

    check(cache.store(first, source), "the first tier fits");
    check(cache.store(second, source), "the second tier fits after eviction");
    const StageCacheStats stats = cache.stats();
    check(stats.evictions == 1, "the budget evicts one entry");
    check(stats.bytes_evicted == tier_bytes, "the evicted bytes are counted");
    check(!cache.contains(first), "the oldest tier is gone");
    check(cache.contains(second), "the newest tier is kept");
    check(cache.used() <= cache.budget(), "usage stays inside the budget");

    // A tier larger than the whole budget is refused.
    NodeCache tiny(true, tier_bytes / 2);
    check(!tiny.store(first, source), "an oversized tier is refused");
    check(tiny.stats().rejects == 1, "the refusal is counted");
    check(tiny.used() == 0, "a refused tier uses no budget");
}

void testDisabledIsNoOp()
{
    constexpr int width = 32;
    constexpr int height = 24;
    std::vector<float> data(width * height * 3);
    fillSynthetic(data);
    const RtStageBuffer source = makeBuffer(data, width, height);
    const NodeCacheKey key = baseKey(baseRenderState());

    NodeCache cache(false, 8u * 1024u * 1024u);
    check(!cache.enabled(), "the disabled cache reports itself off");
    check(cache.allocationCount() == 0,
          "the disabled cache starts with no allocations");
    check(!cache.store(key, source), "the disabled cache never stores");
    check(!cache.probe(key), "the disabled cache never hits");
    check(!cache.contains(key), "the disabled cache contains nothing");
    check(cache.used() == 0, "the disabled cache uses no bytes");
    check(cache.budget() == 0, "the disabled cache has no budget");
    check(cache.allocationCount() == 0,
          "the disabled path allocates no tier or auxiliary buffer");
    const StageCacheStats stats = cache.stats();
    check(stats.hits == 0 && stats.misses == 0 && stats.inserts == 0
              && stats.evictions == 0 && stats.bytes_used == 0,
          "the disabled cache counts nothing");

    cache.note_refused();
    check(cache.stats().refused == 0, "the disabled cache counts no refusal");

    // Control: the same call sequence on an enabled cache does allocate, so
    // the counter above is not vacuous.
    NodeCache enabled(true, 64u * 1024u);
    check(enabled.allocationCount() == 0, "the enabled cache starts empty");
    check(enabled.store(key, source), "the enabled cache stores");
    check(enabled.allocationCount() > 0,
          "the enabled store allocates a buffer");
}

void testAuxiliaryPayloadFailure()
{
    constexpr int width = 32;
    constexpr int height = 24;
    std::vector<float> data(width * height * 3);
    fillSynthetic(data);
    const RtStageBuffer source = makeBuffer(data, width, height);
    const NodeCacheKey key = baseKey(baseRenderState());

    // A size without a pointer is a store failure and allocates nothing.
    {
        NodeCache cache(true, 64u * 1024u);
        check(!cache.store(key, source, nullptr, 16),
              "auxiliary bytes without a pointer are refused");
        check(cache.stats().rejects == 1, "the malformed store is a reject");
        check(cache.used() == 0, "the malformed store uses no budget");
        check(cache.allocationCount() == 0,
              "the malformed store allocates nothing");
    }

    // A payload that makes the entry larger than the budget is refused
    // before any allocation.
    {
        NodeCache cache(true, data.size() * sizeof(float) + 64u);
        std::vector<unsigned char> oversized(128u, 0x5a);
        check(!cache.store(key, source, oversized.data(), oversized.size()),
              "an oversized payload is refused");
        check(cache.stats().rejects == 1,
              "the oversized payload is a reject");
        check(cache.allocationCount() == 0,
              "the rejected entry allocates nothing");
    }

    // The cache stores opaque bytes, so a corrupt blob restores but fails to
    // decode. The caller must fall back to a rebuild rather than use it.
    {
        NodeCache cache(true, 64u * 1024u);
        RawBaseTierState state;
        state.wb_method = "Custom";
        state.histogram = {1u, 2u};
        std::vector<unsigned char> blob = encodeRawBaseTierState(state);
        check(!blob.empty(), "the payload is encoded for the failure test");
        blob.pop_back();

        check(cache.store(key, source, blob.data(), blob.size()),
              "a corrupt payload is stored as opaque bytes");
        std::vector<float> destination_data(data.size(), 0.0f);
        RtStageBuffer destination = makeBuffer(destination_data, width, height);
        std::vector<unsigned char> aux;
        check(cache.probe(key), "the corrupt-payload entry is present");
        check(cache.restore(key, destination, &aux),
              "the cache copies the opaque payload back");
        RawBaseTierState decoded;
        check(!decodeRawBaseTierState(aux.data(), aux.size(), decoded),
              "the decoder refuses the corrupt payload");
        check(absoluteError(data, destination_data) == 0,
              "the pixel bytes are still delivered; the caller decides");

        // Replacing the entry with a valid payload recovers.
        blob = encodeRawBaseTierState(state);
        check(cache.store(key, source, blob.data(), blob.size()),
              "a valid payload replaces the corrupt entry");
        aux.clear();
        check(cache.probe(key), "the repaired entry is present");
        check(cache.restore(key, destination, &aux),
              "the repaired entry restores");
        check(decodeRawBaseTierState(aux.data(), aux.size(), decoded),
              "the repaired payload decodes");
    }
}

void testAuxRejectsCorruption()
{
    RawBaseTierState state;
    state.wb_method = "Custom";
    state.histogram = {7u, 8u};
    std::vector<unsigned char> blob = encodeRawBaseTierState(state);
    check(!blob.empty(), "the auxiliary blob is encoded");

    RawBaseTierState decoded;
    check(decodeRawBaseTierState(blob.data(), blob.size(), decoded),
          "a well-formed blob decodes");

    std::vector<unsigned char> truncated(blob.begin(), blob.end() - 1);
    check(!decodeRawBaseTierState(truncated.data(), truncated.size(), decoded),
          "a truncated blob is rejected");

    std::vector<unsigned char> wrong_version = blob;
    wrong_version[3] = 99;
    check(!decodeRawBaseTierState(wrong_version.data(), wrong_version.size(),
                                  decoded),
          "an unknown version is rejected");

    // Version 1 predates the no-balance flag and must not be read as version 2.
    std::vector<unsigned char> old_version = blob;
    old_version[3] = 1;
    check(!decodeRawBaseTierState(old_version.data(), old_version.size(),
                                  decoded),
          "a version-1 blob is rejected");
}

} // namespace

int main()
{
    testSwitchParser();
    testKeyCollisions();
    testReuseDecision();
    testCallSiteGuard();
    testRepeatedPassParity();
    testWhiteBalanceSentinel();
    testTrueLru();
    testBudgetAndEviction();
    testDisabledIsNoOp();
    testAuxiliaryPayloadFailure();
    testAuxRejectsCorruption();

    if (failures == 0) {
        std::puts("stage-node-cache-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
