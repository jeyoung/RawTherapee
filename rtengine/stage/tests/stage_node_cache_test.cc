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
 * The test pins four things: the key covers every byte-affecting input it
 * names and rejects a different image generation, profile, geometry, work
 * order, node or output size; the reuse decision follows the typed work order;
 * the budget, the eviction and the counters behave; and a repeated identical
 * pass restores the exact bytes (AE=0) with a hit. The disabled path is a
 * no-op that allocates nothing.
 *
 * The image is synthetic. It stands in for the Imagefloat tier so the test
 * needs no raw file and no coordinator.
 */

#include "stage_cache_integration.h"

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
    check(nodeCacheKeyDigest(base) == nodeCacheKeyDigest(base),
          "the digest is stable");

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
        check(nodeCacheKeyDigest(other) != nodeCacheKeyDigest(base),
              "a different work order has a different digest");
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
    WorkOrder source;
    source.set(WorkItem::sourceSelect);
    check(!nodeCacheReuseAllowed(source),
          "a source selection refuses the tier");

    WorkOrder preprocess;
    preprocess.set(WorkItem::rawPreprocess);
    check(!nodeCacheReuseAllowed(preprocess),
          "a preprocess run refuses the tier");

    WorkOrder demosaic;
    demosaic.set(WorkItem::rawDemosaic);
    check(!nodeCacheReuseAllowed(demosaic),
          "a demosaic run refuses the tier");

    WorkOrder retinex;
    retinex.set(WorkItem::retinexPrepare);
    check(!nodeCacheReuseAllowed(retinex),
          "a retinex preparation refuses the tier");

    WorkOrder rebuild;
    rebuild.set(WorkItem::rawBaseRebuild);
    check(nodeCacheReuseAllowed(rebuild),
          "a base rebuild with unchanged source inputs is reusable");
    check(nodeCacheSubstitutionAllowed(rebuild),
          "a scheduled base rebuild permits the substitution");

    WorkOrder empty;
    check(nodeCacheReuseAllowed(empty),
          "an empty order permits reuse");
    check(!nodeCacheSubstitutionAllowed(empty),
          "an empty order never substitutes: downstream keeps its buffer");

    WorkOrder spot;
    spot.set(WorkItem::spotApply);
    check(nodeCacheReuseAllowed(spot),
          "a spot-only pass permits reuse");
    check(!nodeCacheSubstitutionAllowed(spot),
          "a spot-only pass never substitutes");

    WorkOrder rebuildAndSpot;
    rebuildAndSpot.set(WorkItem::rawBaseRebuild);
    rebuildAndSpot.set(WorkItem::spotApply);
    check(nodeCacheReuseAllowed(rebuildAndSpot),
          "a rebuild that also applies spots is reusable");
    check(nodeCacheSubstitutionAllowed(rebuildAndSpot),
          "a rebuild that also applies spots substitutes the whole tier");
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
    check(!cache.store(key, source), "the disabled cache never stores");
    check(!cache.probe(key), "the disabled cache never hits");
    check(!cache.contains(key), "the disabled cache contains nothing");
    check(cache.used() == 0, "the disabled cache uses no bytes");
    check(cache.budget() == 0, "the disabled cache has no budget");
    const StageCacheStats stats = cache.stats();
    check(stats.hits == 0 && stats.misses == 0 && stats.inserts == 0
              && stats.evictions == 0 && stats.bytes_used == 0,
          "the disabled cache counts nothing");

    cache.note_refused();
    check(cache.stats().refused == 0, "the disabled cache counts no refusal");
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
}

} // namespace

int main()
{
    testSwitchParser();
    testKeyCollisions();
    testReuseDecision();
    testRepeatedPassParity();
    testBudgetAndEviction();
    testDisabledIsNoOp();
    testAuxRejectsCorruption();

    if (failures == 0) {
        std::puts("stage-node-cache-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
