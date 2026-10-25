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
 * Dirty-block derivation for a stage path.
 *
 * The unit reads the canonical specification (stage_pipeline.h) for the path
 * and its edges, the parameter map (stage_param_map.h) for the per-block
 * fingerprints, and the render state key (stage_render_state.h) for the inputs
 * that are not parameters. It owns no state and executes nothing, so the crop
 * and preview callers derive identical sets from identical inputs.
 *
 * Ordering rules, which the test pins:
 *
 * - every returned id is a canonical descriptor of the requested path;
 * - results are in that path's pipeline order, not in the order the seeds
 *   arrived, so two sets can be compared with ==;
 * - no duplicates, even when a block is both directly dirty and a closure
 *   member.
 *
 * The derivation is deliberately conservative. A block whose fingerprint
 * changed is dirty; a block named by a changed render-state aspect is dirty;
 * every consumer of a dirty block is dirty because the path carries one
 * in-place image; and the caller's unconditional blocks are always dirty. A
 * false positive costs one inert stage call while the legacy gates remain in
 * the block bodies, while a false negative would publish a stale image.
 */

#include "stage_dirty_blocks.h"

#include "stage_param_map.h"

#include "procparams.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace rtengine::stage
{

namespace
{

using BlockList = std::vector<std::string>;

/* Canonical ids of one path, in pipeline order. */
const BlockList& pathIds(StagePath path)
{
    static const BlockList exportIds = [] {
        BlockList ids;
        for (const auto& node : buildPipelineSpec(StagePath::Export).nodes) {
            ids.push_back(node.id);
        }
        return ids;
    }();
    static const BlockList previewIds = [] {
        BlockList ids;
        for (const auto& node : buildPipelineSpec(StagePath::Preview).nodes) {
            ids.push_back(node.id);
        }
        return ids;
    }();
    static const BlockList cropIds = [] {
        BlockList ids;
        for (const auto& node : buildPipelineSpec(StagePath::Crop).nodes) {
            ids.push_back(node.id);
        }
        return ids;
    }();
    static const BlockList thumbnailIds = [] {
        BlockList ids;
        for (const auto& node : buildPipelineSpec(StagePath::Thumbnail).nodes) {
            ids.push_back(node.id);
        }
        return ids;
    }();

    switch (path) {
    case StagePath::Export:
        return exportIds;
    case StagePath::Preview:
        return previewIds;
    case StagePath::Crop:
        return cropIds;
    case StagePath::Thumbnail:
        return thumbnailIds;
    }

    assert(false && "unknown stage path");
    return exportIds;
}

bool onPath(StagePath path, const std::string& id)
{
    const BlockList& ids = pathIds(path);
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/*
 * Reduce a bag of ids to the path's canonical order.
 *
 * Unknown ids are a programming error: the assertion fires in debug builds and
 * the id is dropped in release builds, so a typo can never widen an execution
 * set with a block that will never run.
 */
BlockList ordered(StagePath path, const std::set<std::string>& blocks)
{
    BlockList result;
    result.reserve(blocks.size());
    for (const std::string& id : pathIds(path)) {
        if (blocks.find(id) != blocks.end()) {
            result.push_back(id);
        }
    }
    assert(result.size() == blocks.size()
           && "dirty block set names a block outside the path");
    return result;
}

/* Pick the ids of `candidates` that belong to `path`. */
BlockList select(StagePath path,
                 std::initializer_list<const char*> candidates)
{
    std::set<std::string> wanted;
    for (const char* id : candidates) {
        if (onPath(path, id)) {
            wanted.insert(id);
        }
    }
    return ordered(path, wanted);
}

/* Full-frame source blocks; never part of a crop ROI recompute. */
bool isSourceBlock(const std::string& id)
{
    return id == "source.select" || id == "raw.preprocess"
           || id == "raw.demosaic";
}

/*
 * Aspect comparison helpers. Each group is a pure predicate over the two keys,
 * so a new RenderStateKey field must be added to exactly one group here and to
 * the RenderStateKey test at the same time.
 */
bool geometryChanged(const RenderStateKey& a, const RenderStateKey& b)
{
    return std::tie(a.previewScale, a.previewWidth, a.previewHeight,
                    a.fullWidth, a.fullHeight, a.cropX, a.cropY, a.cropWidth,
                    a.cropHeight, a.cropSkip, a.borderRequested, a.borderLeft,
                    a.borderTop)
           != std::tie(b.previewScale, b.previewWidth, b.previewHeight,
                       b.fullWidth, b.fullHeight, b.cropX, b.cropY, b.cropWidth,
                       b.cropHeight, b.cropSkip, b.borderRequested, b.borderLeft,
                       b.borderTop);
}

bool sourceChanged(const RenderStateKey& a, const RenderStateKey& b)
{
    return std::tie(a.sourceGeneration, a.rgbSourceModified)
           != std::tie(b.sourceGeneration, b.rgbSourceModified);
}

bool highDetailChanged(const RenderStateKey& a, const RenderStateKey& b)
{
    return std::tie(a.highDetailNeeded, a.highDetailPreprocessComputed,
                    a.highDetailRawComputed, a.highDetailStateKnown)
           != std::tie(b.highDetailNeeded, b.highDetailPreprocessComputed,
                       b.highDetailRawComputed, b.highDetailStateKnown);
}

bool displayChanged(const RenderStateKey& a, const RenderStateKey& b)
{
    return std::tie(a.monitorProfile, a.monitorIntent, a.softProof,
                    a.gamutCheck)
           != std::tie(b.monitorProfile, b.monitorIntent, b.softProof,
                       b.gamutCheck);
}

bool editorChanged(const RenderStateKey& a, const RenderStateKey& b)
{
    return std::tie(a.sharpMask, a.fastPreviewEnabled,
                    a.mainPreviewFullResolution, a.initialRoughPreview)
           != std::tie(b.sharpMask, b.fastPreviewEnabled,
                       b.mainPreviewFullResolution, b.initialRoughPreview);
}

bool requestChanged(const RenderStateKey& a, const RenderStateKey& b)
{
    return std::tie(a.requestKind, a.previousDemoBase)
           != std::tie(b.requestKind, b.previousDemoBase);
}

using FingerprintFn =
    ParamFingerprint (*)(const std::string&, const procparams::ProcParams&);

/*
 * Fingerprint function per path, or null when the path has none.
 *
 * The thumbnail path has no fingerprints: it reduces and converts in one pass
 * and its cache keys are derived elsewhere, so it degrades to the full set.
 */
FingerprintFn fingerprintFor(StagePath path)
{
    switch (path) {
    case StagePath::Export:
        return export_fingerprint;
    case StagePath::Preview:
        return preview_fingerprint;
    case StagePath::Crop:
        return crop_fingerprint;
    case StagePath::Thumbnail:
        break;
    }

    return nullptr;
}

} // namespace

const char* renderStateAspectName(RenderStateAspect aspect)
{
    switch (aspect) {
    case RenderStateAspect::Geometry:
        return "geometry";
    case RenderStateAspect::Source:
        return "source";
    case RenderStateAspect::HighDetail:
        return "high-detail";
    case RenderStateAspect::Display:
        return "display";
    case RenderStateAspect::Editor:
        return "editor";
    case RenderStateAspect::Request:
        return "request";
    case RenderStateAspect::Count:
        break;
    }

    return "unknown";
}

std::vector<RenderStateAspect> changed_render_state_aspects(
    const RenderStateKey& previous, const RenderStateKey& current)
{
    std::vector<RenderStateAspect> aspects;

    if (geometryChanged(previous, current)) {
        aspects.push_back(RenderStateAspect::Geometry);
    }
    if (sourceChanged(previous, current)) {
        aspects.push_back(RenderStateAspect::Source);
    }
    if (highDetailChanged(previous, current)) {
        aspects.push_back(RenderStateAspect::HighDetail);
    }
    if (displayChanged(previous, current)) {
        aspects.push_back(RenderStateAspect::Display);
    }
    if (editorChanged(previous, current)) {
        aspects.push_back(RenderStateAspect::Editor);
    }
    if (requestChanged(previous, current)) {
        aspects.push_back(RenderStateAspect::Request);
    }

    return aspects;
}

const std::vector<std::vector<std::string>>& coupled_block_groups(
    StagePath path)
{
    /*
     * The Lab chain is seeded once per pass from laboCrop and consumed in
     * order. The crop runs six blocks on it, the preview five (the preview
     * carries no sharpening block). Every member reads the seed, so a set that
     * names one member must name them all.
     */
    static const std::vector<std::vector<std::string>> cropGroups = {
        {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
         "gamut"}
    };
    static const std::vector<std::vector<std::string>> previewGroups = {
        {"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"}
    };
    static const std::vector<std::vector<std::string>> noGroups;

    switch (path) {
    case StagePath::Crop:
        return cropGroups;
    case StagePath::Preview:
        return previewGroups;
    case StagePath::Export:
    case StagePath::Thumbnail:
        break;
    }

    return noGroups;
}

std::vector<std::string> blocks_for_render_state(StagePath path,
                                                RenderStateAspect aspect)
{
    /*
     * Export and thumbnail do not carry a RenderStateKey. Answering with the
     * whole path is the conservative choice: a caller that passes one anyway
     * recomputes too much instead of reading a stale buffer.
     */
    if (path != StagePath::Preview && path != StagePath::Crop) {
        return pathIds(path);
    }

    switch (aspect) {
    case RenderStateAspect::Geometry: {
        if (path == StagePath::Preview) {
            /* A scale or preview-size change re-renders the whole pass. */
            return pathIds(path);
        }
        /* Crop: every ROI block is invalid, the full-frame source is not. */
        BlockList blocks;
        for (const std::string& id : pathIds(path)) {
            if (!isSourceBlock(id)) {
                blocks.push_back(id);
            }
        }
        return blocks;
    }
    case RenderStateAspect::Source:
        /* A new or modified source reloads the raw core. */
        return select(path, {"source.select", "raw.preprocess",
                             "raw.demosaic", "raw.base"});
    case RenderStateAspect::HighDetail:
        /* The high-detail pass feeds the raw core and the analysis. */
        return select(path, {"raw.preprocess", "raw.demosaic", "raw.base",
                             "analysis", "transform"});
    case RenderStateAspect::Display:
        return path == StagePath::Preview
                   ? select(path, {"monitor.output"})
                   : select(path, {"viewport.output"});
    case RenderStateAspect::Editor:
        /*
         * The sharpening mask changes the sharpening block. The fast-preview
         * family only exists on the preview path, where it re-renders the raw
         * core, the analysis and the transform before the reduced pass.
         */
        return path == StagePath::Preview
                   ? select(path, {"raw.base", "analysis", "transform"})
                   : select(path, {"lab.sharpening"});
    case RenderStateAspect::Request:
        /* A changed request kind is a repaint until a full request is made. */
        return path == StagePath::Preview
                   ? select(path, {"monitor.output"})
                   : select(path, {"viewport.output"});
    case RenderStateAspect::Count:
        break;
    }

    assert(false && "unknown render state aspect");
    return pathIds(path);
}

bool request_rebuilds_path(RequestKind kind)
{
    switch (kind) {
    case RequestKind::FullCrop:
    case RequestKind::FullPreview:
    case RequestKind::HighQuality:
        return true;
    case RequestKind::ParamsOnly:
    case RequestKind::RepaintOnly:
        return false;
    }

    assert(false && "unknown request kind");
    return true;
}

const std::vector<std::string>& resident_blocks(StagePath path)
{
    /*
     * The unconditional steps of the legacy sequences. The crop always runs
     * its base finish, its geometry transform and its viewport sink
     * (dcrop.cc: runs[CROP_RAW_BASE], runs[CROP_TRANSFORM] and
     * runs[CROP_VIEWPORT_OUTPUT] are set without a mask test). The preview
     * always publishes the monitor sink after the pass. Other paths have no
     * unconditional node inside the scope of this unit.
     */
    static const BlockList cropResidents = {"raw.base", "transform",
                                            "viewport.output"};
    static const BlockList previewResidents = {"monitor.output"};
    static const BlockList noResidents;

    switch (path) {
    case StagePath::Crop:
        return cropResidents;
    case StagePath::Preview:
        return previewResidents;
    case StagePath::Export:
    case StagePath::Thumbnail:
        break;
    }

    return noResidents;
}

BlockFingerprints stage_fingerprints(StagePath path,
                                     const procparams::ProcParams& params)
{
    BlockFingerprints result;
    const FingerprintFn fingerprint = fingerprintFor(path);
    if (fingerprint == nullptr) {
        return result;
    }

    const BlockList& ids = pathIds(path);
    result.ids = ids;
    result.values.reserve(ids.size());
    for (const std::string& id : ids) {
        result.values.push_back(fingerprint(id, params));
    }
    return result;
}

std::vector<std::string> close_dirty_blocks(
    StagePath path, const std::vector<std::string>& blocks)
{
    const StagePipelineSpec spec = buildPipelineSpec(path);
    std::map<std::string, size_t> index;
    std::vector<std::vector<size_t>> consumers(spec.nodes.size());
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        index[spec.nodes[i].id] = i;
    }
    for (const auto& edge : spec.edges) {
        const auto from = index.find(edge.first);
        const auto to = index.find(edge.second);
        if (from == index.end() || to == index.end()) {
            assert(false && "pipeline edge names an unknown block");
            continue;
        }
        consumers[from->second].push_back(to->second);
    }

    std::set<std::string> closed;
    std::deque<size_t> queue;
    for (const std::string& id : blocks) {
        const auto found = index.find(id);
        if (found == index.end()) {
            assert(false && "dirty block set names an unknown block");
            continue;
        }
        if (closed.insert(id).second) {
            queue.push_back(found->second);
        }
    }

    while (!queue.empty()) {
        const size_t current = queue.front();
        queue.pop_front();
        for (size_t consumer : consumers[current]) {
            const std::string& id = spec.nodes[consumer].id;
            if (closed.insert(id).second) {
                queue.push_back(consumer);
            }
        }
    }

    return ordered(path, closed);
}

DirtyBlocks derive_dirty_blocks(const DirtyBlockInputs& inputs)
{
    DirtyBlocks result;

    assert(inputs.currentFingerprints != nullptr);
    if (inputs.currentFingerprints == nullptr) {
        result.forced = true;
        result.blocks = pathIds(inputs.path);
        return result;
    }

    const BlockFingerprints& current = *inputs.currentFingerprints;
    const BlockFingerprints* previous = inputs.previousFingerprints;

    /*
     * History is usable only when both snapshots describe the same path in the
     * same order. Anything else is treated as a first update: a caller that
     * switched path or downgraded its fingerprint table must not compare
     * unrelated values.
     */
    const bool haveHistory =
        previous != nullptr && inputs.previousState != nullptr
        && inputs.currentState != nullptr && !previous->empty()
        && previous->ids == current.ids
        && previous->values.size() == current.values.size();

    /*
     * A path without fingerprints cannot be derived from parameters. The
     * conservative answer is the whole path, and it is marked forced so a
     * caller does not mistake it for a derivation.
     */
    if (inputs.forceFull || !haveHistory || current.empty()) {
        result.forced = true;
        result.blocks = pathIds(inputs.path);
        return result;
    }

    std::set<std::string> fromParams;
    for (size_t i = 0; i < current.ids.size(); ++i) {
        if (previous->values[i] != current.values[i]) {
            fromParams.insert(current.ids[i]);
        }
    }

    std::set<std::string> fromState;
    for (RenderStateAspect aspect : changed_render_state_aspects(
             *inputs.previousState, *inputs.currentState)) {
        for (const std::string& id : blocks_for_render_state(inputs.path,
                                                             aspect)) {
            fromState.insert(id);
        }
    }

    /*
     * A rebuild request is absolute, not a delta: this pass has to rebuild the
     * path even when no fingerprint moved. The crop reaches this with the raw
     * reload bits of the legacy mask (M_INIT, M_LINDENOISE, M_HDR), which no
     * parameter fingerprint can express because the raw stage is reloaded
     * instead of updated in place.
     */
    if (request_rebuilds_path(inputs.currentState->requestKind)) {
        for (const std::string& id : pathIds(inputs.path)) {
            fromState.insert(id);
        }
    }

    std::set<std::string> seeds = fromParams;
    seeds.insert(fromState.begin(), fromState.end());

    /*
     * Expand a seeded coupled group to all of its members. The group is a
     * buffer dependency: every member consumes the same seed, so running a
     * subset would leave the rest reading a stale one.
     */
    std::set<std::string> grouped;
    for (const std::vector<std::string>& group :
         coupled_block_groups(inputs.path)) {
        const bool seeded = std::any_of(
            group.begin(), group.end(),
            [&seeds](const std::string& id) {
                return seeds.find(id) != seeds.end();
            });
        if (!seeded) {
            continue;
        }
        for (const std::string& id : group) {
            if (!onPath(inputs.path, id)) {
                assert(false && "coupled group names a block outside the path");
                continue;
            }
            if (seeds.insert(id).second) {
                grouped.insert(id);
            }
        }
    }

    std::set<std::string> execution = seeds;
    for (const std::string& id : resident_blocks(inputs.path)) {
        if (!onPath(inputs.path, id)) {
            assert(false && "resident block is not part of the path");
            continue;
        }
        execution.insert(id);
    }

    result.fromParams = ordered(inputs.path, fromParams);
    result.fromState = ordered(inputs.path, fromState);
    result.fromGroups = ordered(inputs.path, grouped);
    result.seeds = ordered(inputs.path, seeds);

    /*
     * Only the directly dirty blocks seed the closure. A resident block runs
     * on every update, but its body is still mask-gated, so a resident alone
     * does not regenerate its output and must not pull its consumers in.
     */
    const BlockList closed = close_dirty_blocks(
        inputs.path, BlockList(seeds.begin(), seeds.end()));
    for (const std::string& id : closed) {
        if (execution.insert(id).second) {
            result.fromClosure.push_back(id);
        }
    }

    result.blocks = ordered(inputs.path, execution);
    return result;
}

DirtyShadow compare_dirty_shadow(const std::vector<std::string>& legacy,
                                 const std::vector<std::string>& derived)
{
    const std::set<std::string> legacySet(legacy.begin(), legacy.end());
    const std::set<std::string> derivedSet(derived.begin(), derived.end());

    DirtyShadow shadow;
    for (const std::string& id : legacy) {
        if (derivedSet.find(id) == derivedSet.end()) {
            shadow.missing.push_back(id);
        }
    }
    for (const std::string& id : derived) {
        if (legacySet.find(id) == legacySet.end()) {
            shadow.extra.push_back(id);
        }
    }
    return shadow;
}

std::string format_block_ids(const std::vector<std::string>& blocks)
{
    std::string text = "[";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i != 0) {
            text += ' ';
        }
        text += blocks[i];
    }
    text += ']';
    return text;
}

} // namespace rtengine::stage
