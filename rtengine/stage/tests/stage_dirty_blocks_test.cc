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
 * Specification test for the dirty-block derivation.
 *
 * The test proves the contract the crop and preview paths rely on:
 *
 * - fingerprints are per path, in canonical order, deterministic and equal for
 *   equal parameters;
 * - a parameter change dirties exactly the blocks whose fingerprint lists the
 *   changed member, plus their consumers and the caller's residents;
 * - a render-state change dirties the blocks its aspect names, and every
 *   RenderStateKey field belongs to exactly one aspect;
 * - the forward closure reaches every consumer and stops at the sinks;
 * - the shadow comparison separates the unsafe direction (missing) from the
 *   documented over-approximation (extra).
 *
 * It does not run image processing. End-to-end equivalence of the crop
 * execution set is measured by tools/perf/crop-stage-test.cc.
 */

#include "stage_dirty_blocks.h"

#include "stage_mask_map.h"
#include "stage_pipeline.h"

#include "rtengine/procparams.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{

using namespace rtengine;
using namespace rtengine::stage;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::vector<std::string> pipelineIds(StagePath path)
{
    std::vector<std::string> ids;
    for (const auto& node : buildPipelineSpec(path).nodes) {
        ids.push_back(node.id);
    }
    return ids;
}

bool isSubsequenceOfPath(StagePath path, const std::vector<std::string>& ids)
{
    const std::vector<std::string> canonical = pipelineIds(path);
    size_t position = 0;
    for (const std::string& id : ids) {
        const auto found = std::find(canonical.begin() + position,
                                     canonical.end(), id);
        if (found == canonical.end()) {
            return false;
        }
        position = static_cast<size_t>(found - canonical.begin()) + 1;
    }
    return true;
}

bool orderedAndUnique(StagePath path, const std::vector<std::string>& ids)
{
    const std::set<std::string> unique(ids.begin(), ids.end());
    return unique.size() == ids.size() && isSubsequenceOfPath(path, ids);
}

bool contains(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::vector<std::string> setOf(std::initializer_list<const char*> ids)
{
    return std::vector<std::string>(ids.begin(), ids.end());
}

bool equal(const std::vector<std::string>& a,
           const std::vector<std::string>& b)
{
    return a == b;
}

void checkOrdered(StagePath path, const std::vector<std::string>& ids,
                  const std::string& label)
{
    check(orderedAndUnique(path, ids),
          label + ": ids are canonical, ordered and unique");
    for (const std::string& id : ids) {
        check(contains(pipelineIds(path), id),
              label + ": id is a canonical block: " + id);
    }
}

/* A key with every field distinct from the default one. */
RenderStateKey changedKey()
{
    RenderStateKey key;
    key.previewScale = 3;
    key.previewWidth = 100;
    key.previewHeight = 90;
    key.fullWidth = 4000;
    key.fullHeight = 3000;
    key.cropX = 10;
    key.cropY = 11;
    key.cropWidth = 200;
    key.cropHeight = 180;
    key.cropSkip = 2;
    key.borderRequested = 32;
    key.borderLeft = 8;
    key.borderTop = 8;
    key.highDetailNeeded = true;
    key.highDetailPreprocessComputed = true;
    key.highDetailRawComputed = true;
    key.highDetailStateKnown = true;
    key.sourceGeneration = 7;
    key.rgbSourceModified = true;
    key.monitorProfile = "sRGB";
    key.monitorIntent = 1;
    key.softProof = true;
    key.gamutCheck = true;
    key.sharpMask = true;
    key.fastPreviewEnabled = true;
    key.mainPreviewFullResolution = true;
    key.initialRoughPreview = true;
    key.requestKind = RequestKind::FullPreview;
    key.previousDemoBase = 2;
    return key;
}

/*
 * One representative mutation per render-state aspect.
 *
 * Every mutation starts from the default key and changes only that aspect. The
 * test then requires the changed-aspect list to be exactly {aspect}, which
 * pins that no field is compared by two aspects and that no field is missed.
 */
struct AspectMutation {
    RenderStateAspect aspect;
    void (*mutate)(RenderStateKey&);
};

const std::vector<AspectMutation>& aspectMutations()
{
    static const std::vector<AspectMutation> mutations = {
        {RenderStateAspect::Geometry,
         [](RenderStateKey& key) { key.cropWidth += 1; }},
        {RenderStateAspect::Source,
         [](RenderStateKey& key) { key.sourceGeneration += 1; }},
        {RenderStateAspect::HighDetail,
         [](RenderStateKey& key) { key.highDetailNeeded = true; }},
        {RenderStateAspect::Display,
         [](RenderStateKey& key) { key.monitorProfile = "Adobe RGB"; }},
        {RenderStateAspect::Editor,
         [](RenderStateKey& key) { key.sharpMask = true; }},
        {RenderStateAspect::Request,
         [](RenderStateKey& key) {
             key.requestKind = RequestKind::RepaintOnly;
         }}
    };
    return mutations;
}

/* One representative change per parameter family, and the block it must hit. */
struct ParamMutation {
    const char* name;
    void (*mutate)(procparams::ProcParams&);
    std::vector<std::string> expectedParams;
};

const std::vector<ParamMutation>& paramMutations()
{
    static const std::vector<ParamMutation> mutations = {
        {"wb", [](procparams::ProcParams& p) { p.wb.temperature += 500; },
         {"raw.base", "local.adjustments"}},
        {"pdsharpening",
         [](procparams::ProcParams& p) { p.pdsharpening.enabled = true; },
         {"raw.demosaic"}},
        {"rotate", [](procparams::ProcParams& p) { p.rotate.degree = 1.5; },
         {"transform"}},
        {"locallab",
         [](procparams::ProcParams& p) {
             p.locallab.enabled = true;
             p.locallab.spots.emplace_back();
             p.locallab.spots.back().struc += 5.0;
         },
         {"local.adjustments"}},
        {"sharpening",
         [](procparams::ProcParams& p) { p.sharpening.amount += 50; },
         {"lab.sharpening"}},
        {"wavelet", [](procparams::ProcParams& p) { p.wavelet.enabled = true; },
         {"lab.wavelet", "working.trc"}},
        {"ciecam",
         [](procparams::ProcParams& p) { p.colorappearance.enabled = true; },
         {"ciecam"}},
        {"icm", [](procparams::ProcParams& p) { p.icm.workingProfile = "ProPhoto"; },
         {"raw.base", "analysis", "local.adjustments", "rgb.proc", "lab.tools",
          "lab.sharpening", "lab.wavelet", "working.trc", "ciecam", "gamut",
          "viewport.output"}}
    };
    return mutations;
}

DirtyBlocks deriveCrop(const BlockFingerprints& previous,
                       const BlockFingerprints& current,
                       const RenderStateKey& previousState,
                       const RenderStateKey& currentState,
                       bool forceFull = false)
{
    DirtyBlockInputs inputs;
    inputs.path = StagePath::Crop;
    inputs.previousFingerprints = &previous;
    inputs.currentFingerprints = &current;
    inputs.previousState = &previousState;
    inputs.currentState = &currentState;
    inputs.forceFull = forceFull;
    return derive_dirty_blocks(inputs);
}

void testFingerprints()
{
    procparams::ProcParams params;

    for (StagePath path : {StagePath::Export, StagePath::Preview,
                           StagePath::Crop}) {
        const BlockFingerprints fingerprints = stage_fingerprints(path, params);
        check(fingerprints.ids == pipelineIds(path),
              std::string(stagePathName(path))
                  + ": fingerprints follow the canonical order");
        check(fingerprints.values.size() == fingerprints.ids.size(),
              std::string(stagePathName(path))
                  + ": one fingerprint per canonical block");
        check(!fingerprints.empty(),
              std::string(stagePathName(path)) + ": fingerprints exist");

        procparams::ProcParams copy = params;
        const BlockFingerprints again = stage_fingerprints(path, copy);
        check(again.ids == fingerprints.ids && again.values == fingerprints.values,
              std::string(stagePathName(path))
                  + ": equal parameters give equal fingerprints");
    }

    /* The thumbnail path has no parameter fingerprints by design. */
    const BlockFingerprints thumbnail =
        stage_fingerprints(StagePath::Thumbnail, params);
    check(thumbnail.empty(), "thumbnail: no fingerprints");
}

void testNoHistory()
{
    procparams::ProcParams params;
    const BlockFingerprints current = stage_fingerprints(StagePath::Crop, params);
    const RenderStateKey state;

    DirtyBlockInputs inputs;
    inputs.path = StagePath::Crop;
    inputs.currentFingerprints = &current;
    inputs.currentState = &state;
    const DirtyBlocks forced = derive_dirty_blocks(inputs);

    check(forced.forced, "no history: the result is forced");
    check(forced.blocks == pipelineIds(StagePath::Crop),
          "no history: the whole path is dirty");
    checkOrdered(StagePath::Crop, forced.blocks, "no history");

    /* A previous fingerprint table of another path is not usable history. */
    const BlockFingerprints preview =
        stage_fingerprints(StagePath::Preview, params);
    const DirtyBlocks mismatched = deriveCrop(preview, current, state, state);
    check(mismatched.forced, "mismatched history: the result is forced");
    check(mismatched.blocks == pipelineIds(StagePath::Crop),
          "mismatched history: the whole path is dirty");

    /* An explicit full request is forced even with usable history. */
    const DirtyBlocks highQuality =
        deriveCrop(current, current, state, state, true);
    check(highQuality.forced, "forceFull: the result is forced");
    check(highQuality.blocks == pipelineIds(StagePath::Crop),
          "forceFull: the whole path is dirty");
}

void testStableState()
{
    procparams::ProcParams params;
    const BlockFingerprints fingerprints =
        stage_fingerprints(StagePath::Crop, params);
    const RenderStateKey state;

    const DirtyBlocks derived = deriveCrop(fingerprints, fingerprints, state,
                                           state);
    check(!derived.forced, "stable: not forced");
    check(derived.fromParams.empty(), "stable: no parameter block is dirty");
    check(derived.fromState.empty(), "stable: no render-state block is dirty");
    check(derived.fromClosure.empty(), "stable: the closure adds nothing");
    check(derived.seeds.empty(), "stable: no seeds");
    check(derived.blocks == resident_blocks(StagePath::Crop),
          "stable: only the resident blocks are dirty");
    check(derived.blocks == setOf({"raw.base", "transform", "viewport.output"}),
          "stable: the crop residents are the unconditional blocks");
    checkOrdered(StagePath::Crop, derived.blocks, "stable");
}

void testParamMutation()
{
    for (const ParamMutation& mutation : paramMutations()) {
        procparams::ProcParams params;
        const BlockFingerprints previous =
            stage_fingerprints(StagePath::Crop, params);

        procparams::ProcParams changed = params;
        mutation.mutate(changed);
        const BlockFingerprints current =
            stage_fingerprints(StagePath::Crop, changed);

        check(previous.values != current.values,
              std::string(mutation.name) + ": the mutation changes a fingerprint");

        const RenderStateKey state;
        const DirtyBlocks derived =
            deriveCrop(previous, current, state, state);

        const std::string label = std::string("param ") + mutation.name;
        check(!derived.forced, label + ": not forced");
        check(equal(derived.fromParams, mutation.expectedParams),
              label + ": dirties the blocks that list the member: "
                  + format_block_ids(derived.fromParams));
        checkOrdered(StagePath::Crop, derived.fromParams, label + " params");
        checkOrdered(StagePath::Crop, derived.blocks, label + " blocks");

        /*
         * Every resident is dirty and every directly dirty block is in the
         * execution set; the closure only ever adds blocks.
         */
        for (const std::string& id : resident_blocks(StagePath::Crop)) {
            check(contains(derived.blocks, id),
                  label + ": keeps the resident block " + id);
        }
        for (const std::string& id : derived.seeds) {
            check(contains(derived.blocks, id),
                  label + ": keeps the seed " + id);
        }
        for (const std::string& id : derived.fromClosure) {
            check(!contains(derived.seeds, id),
                  label + ": the closure only reports additions");
            check(contains(derived.blocks, id),
                  label + ": the closure output is in the set");
        }
        for (const std::string& id : derived.fromGroups) {
            check(contains(derived.seeds, id),
                  label + ": a coupled-group addition is a seed");
            check(!contains(derived.fromParams, id),
                  label + ": a coupled-group addition is not a direct param");
            check(contains(derived.blocks, id),
                  label + ": the coupled group is in the set");
        }
    }
}

void testCoupledGroups()
{
    const auto& cropGroups = coupled_block_groups(StagePath::Crop);
    check(cropGroups.size() == 1, "groups: the crop has one coupled group");
    check(cropGroups[0]
              == setOf({"lab.tools", "lab.sharpening", "lab.wavelet",
                        "working.trc", "ciecam", "gamut"}),
          "groups: the crop group is the Lab chain");
    for (const std::string& id : cropGroups[0]) {
        check(contains(pipelineIds(StagePath::Crop), id),
              "groups: crop member is canonical: " + id);
    }

    const auto& previewGroups = coupled_block_groups(StagePath::Preview);
    check(previewGroups.size() == 1,
          "groups: the preview has one coupled group");
    check(previewGroups[0]
              == setOf({"lab.tools", "lab.wavelet", "working.trc", "ciecam",
                        "gamut"}),
          "groups: the preview group has no sharpening block");
    check(coupled_block_groups(StagePath::Export).empty(),
          "groups: the export path has none");
    check(coupled_block_groups(StagePath::Thumbnail).empty(),
          "groups: the thumbnail path has none");

    /*
     * A sharpening change names one member; the whole chain must be dirty so
     * no member reads a stale Lab seed.
     */
    procparams::ProcParams params;
    const BlockFingerprints previous =
        stage_fingerprints(StagePath::Crop, params);
    procparams::ProcParams changed = params;
    changed.sharpening.amount += 50;
    const BlockFingerprints current =
        stage_fingerprints(StagePath::Crop, changed);

    const RenderStateKey state;
    const DirtyBlocks derived = deriveCrop(previous, current, state, state);
    check(derived.fromParams == setOf({"lab.sharpening"}),
          "groups: the sharpening block is the only direct param");
    check(derived.fromGroups
              == setOf({"lab.tools", "lab.wavelet", "working.trc", "ciecam",
                        "gamut"}),
          "groups: the group adds the rest of the Lab chain");
    check(derived.blocks
              == setOf({"raw.base", "transform", "lab.tools", "lab.sharpening",
                        "lab.wavelet", "working.trc", "ciecam", "gamut",
                        "viewport.output"}),
          "groups: the Lab chain runs as a unit with the residents");
    check(!contains(derived.blocks, "rgb.proc"),
          "groups: the group does not reach a producer");
    checkOrdered(StagePath::Crop, derived.blocks, "groups");

    /* A base change does not seed the Lab group directly. */
    procparams::ProcParams baseChanged = params;
    baseChanged.wb.temperature += 500;
    const DirtyBlocks fromBase = deriveCrop(
        previous, stage_fingerprints(StagePath::Crop, baseChanged), state,
        state);
    check(fromBase.fromGroups.empty(),
          "groups: a base change is not a coupled-group seed");
    check(contains(fromBase.blocks, "lab.tools"),
          "groups: the closure still reaches the chain from above");
}

void testClosure()
{
    /* The Lab chain: a Lab block forces its consumers, not its producers. */
    const std::vector<std::string> closed =
        close_dirty_blocks(StagePath::Crop, {"lab.tools"});
    check(contains(closed, "lab.tools"), "closure: keeps the seed");
    check(contains(closed, "lab.sharpening"),
          "closure: reaches the next Lab block");
    check(contains(closed, "viewport.output"),
          "closure: reaches the sink");
    check(!contains(closed, "rgb.proc"),
          "closure: does not reach a producer");
    checkOrdered(StagePath::Crop, closed, "closure");

    /* The closure is idempotent and keeps an already closed set unchanged. */
    check(close_dirty_blocks(StagePath::Crop, closed) == closed,
          "closure: idempotent");

    /* A root reaches the whole path. */
    const std::vector<std::string> fromRoot =
        close_dirty_blocks(StagePath::Crop, {"source.select"});
    check(fromRoot == pipelineIds(StagePath::Crop),
          "closure: the source reaches every node");

    /* The sink has no consumers. */
    check(close_dirty_blocks(StagePath::Crop, {"viewport.output"})
              == setOf({"viewport.output"}),
          "closure: the sink is a fixed point");
}

void testAspects()
{
    const RenderStateKey base;
    const RenderStateKey full = changedKey();
    const std::vector<RenderStateAspect> all =
        changed_render_state_aspects(base, full);
    check(all.size() == static_cast<size_t>(RenderStateAspect::Count),
          "aspects: a key that changes every group reports every aspect");
    check(changed_render_state_aspects(base, base).empty(),
          "aspects: equal keys report no aspect");

    for (const AspectMutation& mutation : aspectMutations()) {
        RenderStateKey changed;
        mutation.mutate(changed);
        const std::vector<RenderStateAspect> aspects =
            changed_render_state_aspects(base, changed);
        check(aspects.size() == 1 && aspects[0] == mutation.aspect,
              std::string("aspect ") + renderStateAspectName(mutation.aspect)
                  + ": exactly one group changes");

        /* The aspect name is stable and used in logs. */
        check(std::string(renderStateAspectName(mutation.aspect)) != "unknown",
              "aspect: name is defined");
    }
}

void testAspectBlocks()
{
    struct Expected {
        StagePath path;
        RenderStateAspect aspect;
        std::vector<std::string> blocks;
    };

    const std::vector<Expected> expected = {
        /* Crop: ROI blocks follow the allocation, the raw core does not. */
        {StagePath::Crop, RenderStateAspect::Geometry,
         setOf({"raw.base", "analysis", "transform", "local.adjustments",
                "rgb.proc", "lab.tools", "lab.sharpening", "lab.wavelet",
                "working.trc", "ciecam", "gamut", "viewport.output"})},
        {StagePath::Crop, RenderStateAspect::Source,
         setOf({"source.select", "raw.preprocess", "raw.demosaic", "raw.base"})},
        {StagePath::Crop, RenderStateAspect::HighDetail,
         setOf({"raw.preprocess", "raw.demosaic", "raw.base", "analysis",
                "transform"})},
        {StagePath::Crop, RenderStateAspect::Display,
         setOf({"viewport.output"})},
        {StagePath::Crop, RenderStateAspect::Editor,
         setOf({"lab.sharpening"})},
        {StagePath::Crop, RenderStateAspect::Request,
         setOf({"viewport.output"})},
        /* Preview: a scale change re-renders the whole pass, the display
           aspect only its sink. */
        {StagePath::Preview, RenderStateAspect::Geometry,
         pipelineIds(StagePath::Preview)},
        {StagePath::Preview, RenderStateAspect::Source,
         setOf({"source.select", "raw.preprocess", "raw.demosaic", "raw.base"})},
        {StagePath::Preview, RenderStateAspect::Display,
         setOf({"monitor.output"})},
        {StagePath::Preview, RenderStateAspect::Editor,
         setOf({"raw.base", "analysis", "transform"})},
        {StagePath::Preview, RenderStateAspect::Request,
         setOf({"monitor.output"})}
    };

    for (const Expected& entry : expected) {
        const std::vector<std::string> blocks =
            blocks_for_render_state(entry.path, entry.aspect);
        const std::string label =
            std::string(stagePathName(entry.path)) + " "
            + renderStateAspectName(entry.aspect);
        check(blocks == entry.blocks,
              label + ": blocks are " + format_block_ids(entry.blocks)
                  + ", got " + format_block_ids(blocks));
        checkOrdered(entry.path, blocks, label);
    }

    /* Export and thumbnail do not carry a render state: full path. */
    check(blocks_for_render_state(StagePath::Export,
                                  RenderStateAspect::Display)
              == pipelineIds(StagePath::Export),
          "export: an aspect conservatively returns the whole path");
}

void testStateMutation()
{
    procparams::ProcParams params;
    const BlockFingerprints fingerprints =
        stage_fingerprints(StagePath::Crop, params);
    const RenderStateKey base;

    /* Display: only the viewport sink, nothing else is dirty. */
    RenderStateKey display;
    display.monitorProfile = "Adobe RGB";
    const DirtyBlocks fromDisplay =
        deriveCrop(fingerprints, fingerprints, base, display);
    check(fromDisplay.fromState == setOf({"viewport.output"}),
          "state display: dirties the viewport sink");
    check(fromDisplay.blocks == setOf({"raw.base", "transform",
                                       "viewport.output"}),
          "state display: keeps the residents only");
    check(!fromDisplay.forced, "state display: not forced");

    /* Source: the raw core reloads and the whole path follows. */
    RenderStateKey source;
    source.sourceGeneration = 3;
    const DirtyBlocks fromSource =
        deriveCrop(fingerprints, fingerprints, base, source);
    check(fromSource.fromState
              == setOf({"source.select", "raw.preprocess", "raw.demosaic",
                        "raw.base"}),
          "state source: dirties the raw core");
    check(fromSource.blocks == pipelineIds(StagePath::Crop),
          "state source: the closure reaches every crop block");

    /* Geometry: the ROI blocks, not the full-frame source. */
    RenderStateKey geometry;
    geometry.cropX += 1;
    const DirtyBlocks fromGeometry =
        deriveCrop(fingerprints, fingerprints, base, geometry);
    check(!contains(fromGeometry.blocks, "source.select"),
          "state geometry: does not reload the source");
    check(contains(fromGeometry.blocks, "viewport.output"),
          "state geometry: reaches the sink");
    checkOrdered(StagePath::Crop, fromGeometry.blocks, "state geometry");
}

void testShadow()
{
    const DirtyShadow clean = compare_dirty_shadow(
        setOf({"raw.base", "transform"}), setOf({"raw.base", "transform"}));
    check(clean.equivalent(), "shadow: equal sets are equivalent");
    check(clean.missing.empty() && clean.extra.empty(),
          "shadow: equal sets have no mismatch");

    const DirtyShadow over = compare_dirty_shadow(
        setOf({"raw.base", "transform"}),
        setOf({"raw.base", "transform", "lab.tools"}));
    check(over.equivalent(), "shadow: extra dirtiness is safe");
    check(over.extra == setOf({"lab.tools"}), "shadow: extra is reported");
    check(over.missing.empty(), "shadow: extra does not produce a missing id");

    const DirtyShadow under = compare_dirty_shadow(
        setOf({"raw.base", "transform", "lab.tools"}),
        setOf({"raw.base", "transform"}));
    check(!under.equivalent(), "shadow: a missing block is not equivalent");
    check(under.missing == setOf({"lab.tools"}),
          "shadow: the missing block is reported");
    check(under.extra.empty(), "shadow: a subset produces no extra id");

    check(format_block_ids({}) == "[]", "format: empty set");
    check(format_block_ids(setOf({"a", "b"})) == "[a b]", "format: two ids");
}

void testResidents()
{
    check(resident_blocks(StagePath::Crop)
              == setOf({"raw.base", "transform", "viewport.output"}),
          "residents: crop unconditional blocks");
    check(resident_blocks(StagePath::Preview) == setOf({"monitor.output"}),
          "residents: preview display sink");
    check(resident_blocks(StagePath::Export).empty(),
          "residents: export has none");
    check(resident_blocks(StagePath::Thumbnail).empty(),
          "residents: thumbnail has none");

    for (const std::string& id : resident_blocks(StagePath::Crop)) {
        check(contains(pipelineIds(StagePath::Crop), id),
              "residents: crop resident is a canonical block: " + id);
    }
    for (const std::string& id : resident_blocks(StagePath::Preview)) {
        check(contains(pipelineIds(StagePath::Preview), id),
              "residents: preview resident is a canonical block: " + id);
    }
}

/*
 * The legacy mask map is a parallel description, not the crop execution list,
 * and it documents its own over-approximations. The one property the derived
 * set must never break is that it is expressed in the same vocabulary: every
 * id is canonical and appears in mask_for_blocks()/nonMaskInputs() for the
 * path, so a shadow mismatch can always be attributed to a mask row.
 */
void testRequestKind()
{
    check(request_rebuilds_path(RequestKind::FullCrop),
          "request: FullCrop rebuilds the path");
    check(request_rebuilds_path(RequestKind::FullPreview),
          "request: FullPreview rebuilds the path");
    check(request_rebuilds_path(RequestKind::HighQuality),
          "request: HighQuality rebuilds the path");
    check(!request_rebuilds_path(RequestKind::ParamsOnly),
          "request: ParamsOnly is a delta");
    check(!request_rebuilds_path(RequestKind::RepaintOnly),
          "request: RepaintOnly is a repaint");

    procparams::ProcParams params;
    const BlockFingerprints fingerprints =
        stage_fingerprints(StagePath::Crop, params);
    RenderStateKey base;
    RenderStateKey rebuild;
    rebuild.requestKind = RequestKind::FullCrop;

    const DirtyBlocks derived = deriveCrop(fingerprints, fingerprints, base,
                                           rebuild);
    check(!derived.forced,
          "request: a rebuild request is a derivation, not a forced result");
    check(derived.blocks == pipelineIds(StagePath::Crop),
          "request: a rebuild request dirties the whole path");
    check(derived.fromState == pipelineIds(StagePath::Crop),
          "request: the rebuild is attributed to the request state");
    checkOrdered(StagePath::Crop, derived.blocks, "request");

    /* A repaint request reaches the sink and nothing else. */
    RenderStateKey repaint;
    repaint.requestKind = RequestKind::RepaintOnly;
    const DirtyBlocks fromRepaint =
        deriveCrop(fingerprints, fingerprints, base, repaint);
    check(fromRepaint.fromState == setOf({"viewport.output"}),
          "request: a repaint dirties the viewport sink");
    check(!contains(fromRepaint.blocks, "lab.tools"),
          "request: a repaint does not rebuild the Lab chain");
}

void testVocabulary()
{
    std::set<std::string> maskIds;
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        for (const std::string& id : row.cropBlocks) {
            maskIds.insert(id);
        }
    }
    for (const StageInputEntry& entry : nonMaskInputs(StagePath::Crop)) {
        maskIds.insert(entry.block);
    }

    procparams::ProcParams params;
    params.wb.temperature += 500;
    params.sharpening.amount += 50;
    procparams::ProcParams changed = params;
    changed.locallab.enabled = true;
    changed.locallab.spots.emplace_back();

    const DirtyBlocks derived = deriveCrop(
        stage_fingerprints(StagePath::Crop, params),
        stage_fingerprints(StagePath::Crop, changed),
        RenderStateKey(), changedKey());

    for (const std::string& id : derived.blocks) {
        check(maskIds.find(id) != maskIds.end(),
              "vocabulary: derived block is known to the mask map: " + id);
    }
}

} // namespace

int main()
{
    testFingerprints();
    testNoHistory();
    testStableState();
    testParamMutation();
    testClosure();
    testAspects();
    testAspectBlocks();
    testStateMutation();
    testShadow();
    testResidents();
    testCoupledGroups();
    testRequestKind();
    testVocabulary();

    if (failures != 0) {
        std::fprintf(stderr, "stage-dirty-blocks-test: %d failure(s)\n",
                     failures);
        return 1;
    }

    std::printf("stage-dirty-blocks-test: PASS\n");
    return 0;
}
