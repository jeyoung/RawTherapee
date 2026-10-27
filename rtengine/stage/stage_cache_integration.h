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

#include "stage_abi.h"
#include "stage_cache.h"
#include "stage_param_hash.h"
#include "stage_render_state.h"
#include "stage_work.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rtengine::stage
{

/*
 * Identity of one node output tier.
 *
 * The key is an exact value: two keys compare equal only when every component
 * below is equal. It carries no pointer and no process-local value, so it can
 * be logged, compared between passes and, later, persisted.
 *
 * Components, and the input each one covers:
 *
 * - `node`: the canonical node id plus the output tier (for example
 *   "preview.raw.base"). It stops two nodes that happen to share their other
 *   inputs from sharing an entry.
 * - `render`: the 128-bit `RenderStateKey::fingerprint()`. It covers every
 *   input that is not in ProcParams: preview scale and allocated dimensions,
 *   the request rectangle and skip, the high-detail decision and the work
 *   already done for it, source identity (generation, rotation, sensor type,
 *   unrotated size, frame count), a modified RGB source, the monitor and
 *   soft-proof configuration, the editor switches, the request kind, the base
 *   the preceding pass used, the denoise and spot request facts, the deferred
 *   analysis mask, the crop-guide fingerprint and the viewport-moved fact.
 *   Including the whole fingerprint rather than a subset costs a few misses
 *   and can never produce a false hit.
 * - `blocks`: every canonical block fingerprint of the pass, in pipeline
 *   order. It covers every ProcParams member that any block reads. The list
 *   is the snapshot the pass derived its work order from, not a hand-picked
 *   subset, so a parameter that a future block starts reading cannot silently
 *   fall outside the key.
 * - `work`: the work-order items that produced the output, restricted to the
 *   items that rewrite the raw.base tier (`rawBaseRebuild`, `retinexPrepare`,
 *   `spotApply`, `spotRestore`, `analysisRun`). Two passes that ran different
 *   bodies are different producers even when their parameters are equal, so
 *   the entry records which bodies its bytes already contain.
 * - `width`, `height`, `planes`, `format`: the output geometry and layout. A
 *   scale change is already in `render`, but the explicit dimensions make the
 *   key self-describing and let the buffer check agree with the key.
 *
 * Deliberately excluded, and why it is safe:
 *
 * - The live `Imagefloat` pointers and allocation addresses. They change on
 *   every reallocation without changing the pixels.
 * - `currWB`, `vhist16`, `denoiseInfoStore.valid` and the spot buffer. They
 *   are derived from the parameters and the source already in the key; they
 *   are cached as an auxiliary payload beside the pixels instead of being
 *   hashed, because their bytes are an output of the same node.
 * - The wall clock, thread count and OpenCL device. The tier is only reused
 *   inside one process; a later cross-process store must add them.
 */
struct NodeCacheKey {
    std::string node;
    ParamFingerprint render;
    std::vector<std::string> block_ids;
    std::vector<ParamFingerprint> block_values;
    uint64_t work = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint16_t planes = 0;
    uint8_t format = 0;

    bool operator==(const NodeCacheKey& other) const;
    bool operator!=(const NodeCacheKey& other) const
    {
        return !(*this == other);
    }
};

/* Stable 64-bit digest of a key, used for the coordinator's resident stamp. */
uint64_t nodeCacheKeyDigest(const NodeCacheKey& key);

/* The key translated to the flat StageCache key. Equality stays exact. */
StageKey nodeCacheStageKey(const NodeCacheKey& key);

/* Bit per WorkItem, in enumeration order; the tier's producer mask. */
uint64_t nodeCacheWorkBits(const WorkOrder& work);

/*
 * Work-order items whose execution rewrites the raw.base output tier.
 *
 * `rawBaseRebuild` rebuilds the pixels; `retinexPrepare` rewrites the raw
 * planes before that; `spotApply` writes the spots into the base;
 * `spotRestore` copies a live spot buffer back; `analysisRun` closes the base
 * with its histogram. The mask is what the key records and what the reuse
 * decision compares.
 */
constexpr uint64_t nodeCacheTierMask()
{
    return (1ULL << static_cast<uint64_t>(WorkItem::rawBaseRebuild))
         | (1ULL << static_cast<uint64_t>(WorkItem::retinexPrepare))
         | (1ULL << static_cast<uint64_t>(WorkItem::spotApply))
         | (1ULL << static_cast<uint64_t>(WorkItem::spotRestore))
         | (1ULL << static_cast<uint64_t>(WorkItem::analysisRun));
}

/*
 * Auxiliary payload stored beside the pixel tier.
 *
 * These are outputs of the same raw.base node. They are serialised with their
 * own version and field order so a change invalidates old blobs instead of
 * being read as a different layout. `histogram` is `vhist16`.
 */
struct RawBaseTierState {
    double wb_temp = 0.0;
    double wb_green = 0.0;
    double wb_equal = 1.0;
    int32_t wb_observer = 0;
    std::string wb_method;
    bool denoise_valid = false;
    std::vector<uint32_t> histogram;
};

std::vector<unsigned char> encodeRawBaseTierState(const RawBaseTierState& state);
bool decodeRawBaseTierState(const unsigned char* data, size_t size,
                            RawBaseTierState& state);

/*
 * Parse one `RT_NODE_CACHE` value.
 *
 * Unset (`nullptr`) is the default and disables the cache: this commit is
 * opt-in. "0" and "off" (case-insensitive) also disable it. Every other
 * value, including the empty string, enables it. The parser is exposed so a
 * unit test can pin the rule without a child process.
 */
bool nodeCacheValueEnables(const char* value);

/* Whether the process-wide switch is on; read once. */
bool nodeCacheEnabled();

/*
 * Budget in MiB, from `RT_NODE_CACHE_MB`.
 *
 * Default 256 MiB, valid range 1..8192 MiB. A missing, empty, malformed or
 * out-of-range value falls back to the default rather than silently producing
 * a zero or unbounded cache. The value is read once per process and the budget
 * applies to each NodeCache instance (one per editor coordinator).
 */
size_t nodeCacheBudgetMiB();

/* Budget in bytes, from nodeCacheBudgetMiB(). */
size_t nodeCacheBudgetBytes();

/*
 * Whether the typed work order permits serving the raw.base tier from the
 * cache.
 *
 * The tier's inputs are the raw source bodies. When any of them is named, the
 * pass regenerates the planes the base is built from, so the cache is refused
 * regardless of the key. When none is named, the base body's own inputs are
 * unchanged and the exact key decides whether the scheduled rebuild would
 * reproduce the same bytes. The work order therefore gates the path, and the
 * key gates the bytes.
 */
bool nodeCacheReuseAllowed(const WorkOrder& work);

/*
 * Whether a cache hit may be substituted for the rebuild body.
 *
 * A substitution replaces the end-of-node tier, so every downstream node must
 * rerun from it. The typed order names `rawBaseRebuild` exactly when the
 * dirty-block derivation seeds the base, and that same seed closes forward
 * over every consumer; a pass that does not name the rebuild may still carry
 * downstream output in the shared buffer, so a substitution there would
 * corrupt the in-place chain. This predicate keeps the two cases apart.
 */
bool nodeCacheSubstitutionAllowed(const WorkOrder& work);

/*
 * Budgeted store of node output tiers.
 *
 * The pixel tier is copied into a `StageCache`; the auxiliary state shares the
 * entry and the budget. The switch and the budget are fixed when the object is
 * constructed, so a process that starts without `RT_NODE_CACHE` never
 * allocates anything.
 *
 * Counters: `hits`/`misses` are probes that found or missed an entry, `refused`
 * counts probes the work-order rule declined, `inserts`/`rejects` the store
 * attempts, `evictions` the budget removals, and the byte counters report the
 * live budget and the bytes copied in or evicted.
 */
class NodeCache
{
public:
    NodeCache();
    /* Test seam: explicit switch and budget without reading the environment. */
    NodeCache(bool enabled, size_t budget_bytes);

    bool enabled() const
    {
        return enabled_;
    }

    bool contains(const NodeCacheKey& key) const;
    bool probe(const NodeCacheKey& key);
    bool restore(const NodeCacheKey& key, RtStageBuffer& destination,
                 std::vector<unsigned char>* aux = nullptr) const;
    bool store(const NodeCacheKey& key, const RtStageBuffer& source,
               const void* aux = nullptr, size_t aux_bytes = 0);

    void note_refused();

    size_t used() const;
    size_t budget() const;
    StageCacheStats stats() const;
    void reset_stats();
    void clear();

private:
    bool enabled_ = false;
    StageCache cache_;
};

} // namespace rtengine::stage
