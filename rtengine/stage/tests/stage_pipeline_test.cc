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
 * Specification test for the canonical stage pipeline inventory.
 *
 * The test checks the shape of the four path specifications: unique ids,
 * known edge endpoints, roots and sinks, reachability in both directions,
 * acyclicity and the presence of the expected blocks. It does not run any
 * image processing.
 */

#include "stage_pipeline.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace rtengine::stage;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

const std::vector<StagePath>& allPaths()
{
    static const std::vector<StagePath> paths = {
        StagePath::Export,
        StagePath::FastExport,
        StagePath::Preview,
        StagePath::Crop,
        StagePath::Thumbnail
    };
    return paths;
}

std::set<std::string> nodeIds(const StagePipelineSpec& spec)
{
    std::set<std::string> ids;
    for (const auto& node : spec.nodes) {
        ids.insert(node.id);
    }
    return ids;
}

bool contains(const std::set<std::string>& ids, const std::string& id)
{
    return ids.find(id) != ids.end();
}

using Adjacency = std::map<std::string, std::vector<std::string>>;

Adjacency adjacency(const StagePipelineSpec& spec)
{
    Adjacency adj;
    for (const auto& node : spec.nodes) {
        adj[node.id];
    }
    for (const auto& edge : spec.edges) {
        adj[edge.first].push_back(edge.second);
    }
    return adj;
}

Adjacency reverseAdjacency(const StagePipelineSpec& spec)
{
    Adjacency adj;
    for (const auto& node : spec.nodes) {
        adj[node.id];
    }
    for (const auto& edge : spec.edges) {
        adj[edge.second].push_back(edge.first);
    }
    return adj;
}

bool visitCycle(const std::string& id, const Adjacency& adj,
                std::map<std::string, int>& state)
{
    state[id] = 1;
    for (const auto& next : adj.at(id)) {
        if (state[next] == 1) {
            return true;
        }
        if (state[next] == 0 && visitCycle(next, adj, state)) {
            return true;
        }
    }
    state[id] = 2;
    return false;
}

bool hasCycle(const StagePipelineSpec& spec)
{
    const Adjacency adj = adjacency(spec);
    std::map<std::string, int> state;
    for (const auto& node : spec.nodes) {
        state[node.id] = 0;
    }
    for (const auto& node : spec.nodes) {
        if (state[node.id] == 0 && visitCycle(node.id, adj, state)) {
            return true;
        }
    }
    return false;
}

std::set<std::string> reachableFrom(const std::vector<std::string>& starts,
                                    const Adjacency& adj)
{
    std::vector<std::string> queue(starts.begin(), starts.end());
    std::set<std::string> seen(starts.begin(), starts.end());
    for (size_t i = 0; i < queue.size(); ++i) {
        for (const auto& next : adj.at(queue[i])) {
            if (seen.insert(next).second) {
                queue.push_back(next);
            }
        }
    }
    return seen;
}

// ProcParams members named by the specification. A parameter name that is not
// in this list has been invented and must not pass review.
const std::set<std::string>& knownParamFields()
{
    static const std::set<std::string> fields = {
        "toneCurve", "labCurve", "retinex", "localContrast", "rgbCurves",
        "colorToning", "sharpening", "prsharpening", "pdsharpening",
        "sharpenEdge", "sharpenMicro", "vibrance", "wb", "colorappearance",
        "defringe", "impulseDenoise", "dirpyrDenoise", "epd", "fattal", "sh",
        "cg", "toneEqualizer", "crop", "cropGuide", "coarse", "commonTrans",
        "rotate", "distortion", "lensProf", "perspective", "gradient",
        "locallab", "pcvignette", "cacorrection", "vignetting", "chmixer",
        "blackwhite", "resize", "framing", "spot", "icm", "raw", "wavelet",
        "dirpyrequalizer", "hsvequalizer", "filmSimulation", "softlight",
        "dehaze", "filmNegative", "metadata"
    };
    return fields;
}

/* Sub-fields of ProcParams members with per-consumer hashing. */
const std::set<std::string>& knownParamSubFields()
{
    static const std::set<std::string> fields = {
        "raw.bayersensor.imageNum", "raw.bayersensor.black0",
        "raw.bayersensor.black1", "raw.bayersensor.black2",
        "raw.bayersensor.black3", "raw.bayersensor.twogreen",
        "raw.bayersensor.Dehablack", "raw.bayersensor.greenthresh",
        "raw.bayersensor.linenoise", "raw.bayersensor.linenoiseDirection",
        "raw.bayersensor.pdafLinesFilter", "raw.bayersensor.border",
        "raw.bayersensor.method", "raw.bayersensor.dcb_iterations",
        "raw.bayersensor.dcb_enhance", "raw.bayersensor.lmmse_iterations",
        "raw.bayersensor.dualDemosaicAutoContrast",
        "raw.bayersensor.dualDemosaicContrast",
        "raw.bayersensor.pixelShiftMotionCorrectionMethod",
        "raw.bayersensor.pixelShiftEperIso",
        "raw.bayersensor.pixelShiftSigma",
        "raw.bayersensor.pixelShiftShowMotion",
        "raw.bayersensor.pixelShiftShowMotionMaskOnly",
        "raw.bayersensor.pixelShiftHoleFill",
        "raw.bayersensor.pixelShiftMedian",
        "raw.bayersensor.pixelShiftAverage",
        "raw.bayersensor.pixelShiftGreen",
        "raw.bayersensor.pixelShiftBlur",
        "raw.bayersensor.pixelShiftSmoothFactor",
        "raw.bayersensor.pixelShiftEqualBright",
        "raw.bayersensor.pixelShiftEqualBrightChannel",
        "raw.bayersensor.pixelShiftNonGreenCross",
        "raw.bayersensor.pixelShiftDemosaicMethod",
        "raw.xtranssensor.blackred", "raw.xtranssensor.blackgreen",
        "raw.xtranssensor.blackblue", "raw.xtranssensor.Dehablackx",
        "raw.xtranssensor.method", "raw.xtranssensor.border",
        "raw.xtranssensor.dualDemosaicAutoContrast",
        "raw.xtranssensor.dualDemosaicContrast", "raw.dark_frame",
        "raw.df_autoselect", "raw.ff_file", "raw.ff_AutoSelect",
        "raw.ff_FromMetaData", "raw.ff_BlurRadius", "raw.ff_BlurType",
        "raw.ff_AutoClipControl", "raw.ff_clipControl",
        "raw.ca_autocorrect", "raw.ca_avoidcolourshift",
        "raw.caautoiterations", "raw.cared", "raw.cablue", "raw.expos",
        "raw.preprocessWB", "raw.hotPixelFilter", "raw.deadPixelFilter",
        "raw.hotdeadpix_thresh", "lensProf.lcMode", "lensProf.lcpFile",
        "lensProf.useVign", "lensProf.lfCameraMake",
        "lensProf.lfCameraModel", "lensProf.lfLens"
    };
    return fields;
}

bool isKnownParamField(const std::string& field)
{
    return knownParamFields().count(field) != 0
        || knownParamSubFields().count(field) != 0;
}

void checkShape(const StagePipelineSpec& spec, StagePath requested)
{
    const std::string label = stagePathName(spec.path);
    check(spec.path == requested, label + ": spec carries the requested path");

    const std::set<std::string> ids = nodeIds(spec);
    check(ids.size() == spec.nodes.size(), label + ": node ids are unique");
    check(!spec.nodes.empty(), label + ": has nodes");

    for (const auto& node : spec.nodes) {
        check(!node.id.empty(), label + ": node id is not empty");
        check(node.outputs.size() == 1 && node.outputs[0] == node.id,
              label + ": node " + node.id + " produces its own buffer id");
        for (const auto& field : node.paramFields) {
            check(isKnownParamField(field),
                  label + ": node " + node.id + " uses a known ProcParams member: " +
                      field);
        }
    }

    std::set<std::pair<std::string, std::string>> uniqueEdges;
    for (const auto& edge : spec.edges) {
        check(contains(ids, edge.first),
              label + ": edge source is a known node: " + edge.first);
        check(contains(ids, edge.second),
              label + ": edge target is a known node: " + edge.second);
        check(uniqueEdges.insert(edge).second,
              label + ": edge is not duplicated: " + edge.first + " -> " +
                  edge.second);
    }

    // inputs must mirror the incoming edges exactly
    std::map<std::string, std::vector<std::string>> incoming;
    for (const auto& node : spec.nodes) {
        incoming[node.id];
    }
    for (const auto& edge : spec.edges) {
        incoming[edge.second].push_back(edge.first);
    }
    for (const auto& node : spec.nodes) {
        check(node.inputs == incoming[node.id],
              label + ": node " + node.id + " lists its incoming edges");
    }

    check(spec.roots.size() == 1 || spec.path == StagePath::Thumbnail,
          label + ": has one root except the thumbnail path");
    check(!spec.sinks.empty(), label + ": has at least one sink");

    for (const auto& root : spec.roots) {
        check(contains(ids, root), label + ": root is a known node: " + root);
        check(incoming[root].empty(), label + ": root " + root + " has no input");
    }
    const Adjacency adj = adjacency(spec);
    for (const auto& sink : spec.sinks) {
        check(contains(ids, sink), label + ": sink is a known node: " + sink);
        check(adj.at(sink).empty(), label + ": sink " + sink + " has no output");
    }

    check(!hasCycle(spec), label + ": graph is acyclic");

    const std::set<std::string> reachable = reachableFrom(spec.roots, adj);
    check(reachable.size() == ids.size(),
          label + ": every node is reachable from a root");

    const std::set<std::string> coReachable =
        reachableFrom(spec.sinks, reverseAdjacency(spec));
    check(coReachable.size() == ids.size(),
          label + ": every node can reach a sink");

    for (const auto& node : spec.nodes) {
        check(node.domain == StageDomain::Full || node.domain == StageDomain::Roi ||
                  node.domain == StageDomain::Reduced,
              label + ": node " + node.id + " has a defined domain");
        const bool exact = spec.path == StagePath::Export ||
                           spec.path == StagePath::FastExport;
        const StageConsumer expected =
            exact ? StageConsumer::Exact : StageConsumer::Interactive;
        check(node.consumer == expected,
              label + ": node " + node.id + " serves the path consumer");
    }

    // Deterministic dump: identical input, identical text, and every node and
    // edge appears in it.
    const std::string first = dumpPipelineSpec(spec.path);
    const std::string second = dumpPipelineSpec(spec.path);
    check(first == second, label + ": dump is deterministic");
    check(first.rfind(std::string("digraph stage_pipeline_") + label + " {", 0) == 0,
          label + ": dump is a named DOT graph");
    for (const auto& node : spec.nodes) {
        check(first.find("\"" + node.id + "\"") != std::string::npos,
              label + ": dump contains node " + node.id);
    }
    for (const auto& edge : spec.edges) {
        check(first.find("\"" + edge.first + "\" -> \"" + edge.second + "\"") !=
                  std::string::npos,
              label + ": dump contains edge " + edge.first + " -> " + edge.second);
    }
}

void checkExport()
{
    const StagePipelineSpec spec = buildPipelineSpec(StagePath::Export);
    const std::set<std::string> ids = nodeIds(spec);

    const std::vector<std::string> blocks = {
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base.source",
        "raw.denoise", "raw.color", "analysis", "transform",
        "local.adjustments", "rgb.proc", "lab.tools", "lab.sharpening",
        "lab.wavelet", "working.trc", "ciecam", "gamut", "output.convert",
        "crop", "resize", "output.sharpen", "framing", "output.icc"
    };
    for (const auto& block : blocks) {
        check(contains(ids, block), "export: missing block " + block);
    }
    check(spec.nodes.size() == blocks.size(),
          "export: node count matches the canonical block list");
    check(spec.roots.size() == 1 && spec.roots[0] == "source.select",
          "export: source.select is the single root");
    check(spec.sinks.size() == 1 && spec.sinks[0] == "output.icc",
          "export: output.icc is the single sink");
}

void checkFastExport()
{
    const StagePipelineSpec spec = buildPipelineSpec(StagePath::FastExport);

    const std::vector<std::string> order = {
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base.source",
        "raw.color", "analysis", "transform", "early.resize", "denoise",
        "finish"
    };

    check(spec.nodes.size() == order.size(),
          "fast export: node count matches the fixed fast order");
    for (size_t i = 0; i < order.size() && i < spec.nodes.size(); ++i) {
        check(spec.nodes[i].id == order[i],
              "fast export: node " + std::to_string(i) + " is " + order[i]);
    }

    check(spec.edges.size() == order.size() - 1,
          "fast export: linear graph has one edge per adjacent pair");
    for (size_t i = 1; i < order.size(); ++i) {
        check(spec.edges[i - 1] ==
                  std::make_pair(order[i - 1], order[i]),
              "fast export: edge " + std::to_string(i - 1) + " is " +
                  order[i - 1] + " -> " + order[i]);
    }

    check(spec.roots.size() == 1 && spec.roots[0] == order.front(),
          "fast export: source.select is the single root");
    check(spec.sinks.size() == 1 && spec.sinks[0] == order.back(),
          "fast export: finish is the single sink");
}

void checkPreview()
{
    const StagePipelineSpec spec = buildPipelineSpec(StagePath::Preview);
    const std::set<std::string> ids = nodeIds(spec);

    const std::vector<std::string> blocks = {
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
        "analysis", "transform", "local.adjustments", "rgb.proc", "lab.tools",
        "lab.wavelet", "working.trc", "ciecam", "gamut", "monitor.output"
    };
    for (const auto& block : blocks) {
        check(contains(ids, block), "preview: missing block " + block);
    }
    check(spec.nodes.size() == blocks.size(),
          "preview: node count matches the canonical block list");
    check(!contains(ids, "crop") && !contains(ids, "resize") &&
              !contains(ids, "framing") && !contains(ids, "output.icc") &&
              !contains(ids, "output.convert"),
          "preview: export-only output blocks are absent");
    check(spec.sinks.size() == 1 && spec.sinks[0] == "monitor.output",
          "preview: monitor.output is the single sink");
}

void checkCrop()
{
    const StagePipelineSpec spec = buildPipelineSpec(StagePath::Crop);
    const std::set<std::string> ids = nodeIds(spec);

    const std::vector<std::string> blocks = {
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
        "analysis", "transform", "local.adjustments", "rgb.proc", "lab.tools",
        "lab.sharpening", "lab.wavelet", "working.trc", "ciecam", "gamut",
        "viewport.output"
    };
    for (const auto& block : blocks) {
        check(contains(ids, block), "crop: missing block " + block);
    }
    check(spec.nodes.size() == blocks.size(),
          "crop: node count matches the canonical block list");
    check(spec.sinks.size() == 1 && spec.sinks[0] == "viewport.output",
          "crop: viewport.output is the single sink");

    for (const auto& node : spec.nodes) {
        if (node.id == "source.select" || node.id == "raw.preprocess" ||
            node.id == "raw.demosaic") {
            check(node.domain == StageDomain::Full,
                  "crop: " + node.id + " is full-frame");
        } else {
            check(node.domain == StageDomain::Roi,
                  "crop: " + node.id + " is a region of interest");
        }
    }
}

void checkSharedCore()
{
    const std::vector<std::string> core = {
        "source.select", "raw.preprocess", "raw.demosaic", "analysis",
        "transform"
    };
    const std::vector<StagePath> paths = {
        StagePath::Export, StagePath::Preview, StagePath::Crop
    };
    for (StagePath path : paths) {
        const std::set<std::string> ids = nodeIds(buildPipelineSpec(path));
        for (const auto& id : core) {
            check(contains(ids, id),
                  std::string("shared core: ") + stagePathName(path) +
                      " is missing " + id);
        }

        /* The export names the raw.base sub-blocks; the viewers keep the
         * composite node. */
        const std::vector<std::string> base =
            path == StagePath::Export
                ? std::vector<std::string>{"raw.base.source", "raw.denoise",
                                           "raw.color"}
                : std::vector<std::string>{"raw.base"};
        for (const auto& id : base) {
            check(contains(ids, id),
                  std::string("shared core: ") + stagePathName(path) +
                      " is missing " + id);
        }
    }
}

void checkThumbnail()
{
    const StagePipelineSpec spec = buildPipelineSpec(StagePath::Thumbnail);
    const std::set<std::string> ids = nodeIds(spec);

    const std::vector<std::string> blocks = {
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
        "source.embedded", "thumbnail.decode", "thumbnail.scale",
        "thumbnail.transform", "thumbnail.whitebalance", "thumbnail.rgb",
        "thumbnail.lab", "thumbnail.output"
    };
    for (const auto& block : blocks) {
        check(contains(ids, block), "thumbnail: missing block " + block);
    }
    check(spec.nodes.size() == blocks.size(),
          "thumbnail: node count matches the canonical block list");

    check(spec.roots.size() == 2, "thumbnail: has two roots");
    check(contains(ids, "source.embedded"),
          "thumbnail: has the embedded JPEG source root");
    check(contains(ids, "source.select"),
          "thumbnail: has the raw fallback root");
    check(contains(ids, "thumbnail.decode") && contains(ids, "thumbnail.scale"),
          "thumbnail: embedded decode joins the shared scale block");

    // The embedded branch feeds thumbnail.decode, and both roots feed the
    // shared scale block.
    const Adjacency adj = adjacency(spec);
    check(adj.at("source.embedded") ==
              std::vector<std::string>({"thumbnail.decode"}),
          "thumbnail: embedded source feeds only the decoder");
    check(adj.at("raw.base") ==
              std::vector<std::string>({"thumbnail.scale"}),
          "thumbnail: raw fallback feeds the shared scale block");

    for (const auto& node : spec.nodes) {
        const bool raw = node.id == "source.select" ||
                         node.id == "raw.preprocess" ||
                         node.id == "raw.demosaic";
        check(node.domain == (raw ? StageDomain::Full : StageDomain::Reduced),
              "thumbnail: " + node.id + " has the expected domain");
    }

    check(spec.sinks.size() == 1 && spec.sinks[0] == "thumbnail.output",
          "thumbnail: thumbnail.output is the single sink");
}

/*
 * The contract is the single definition of the block vocabulary. These checks
 * prove that every consumer can rely on it: the ids are unique, every path
 * node exists, every fold names a real host, every work item belongs to a
 * block on the path that schedules it, and the structural columns agree with
 * the path edges.
 */
void checkContract()
{
    std::set<std::string> ids;
    for (const StageBlockContract& block : stageBlockContracts()) {
        const std::string id(block.id);
        check(!id.empty(), "contract: block id is not empty");
        check(ids.insert(id).second, "contract: block id is unique: " + id);
        check(block.name != nullptr && block.name[0] != '\0',
              "contract: block has a display name: " + id);
        check(!block.paramFields.empty() || id == "source.embedded",
              "contract: block lists its parameter inputs: " + id);
        for (WorkItem item : block.workItems) {
            check(item != WorkItem::Count,
                  "contract: block names a real work item: " + id);
        }
        for (const StageFoldRule& fold : block.folds) {
            check(fold.host != nullptr && fold.host[0] != '\0',
                  "contract: fold has a host: " + id);
        }
        check(stageOwnershipName(block.ownership) != std::string("unknown"),
              "contract: block has an ownership: " + id);
        check(stageWorkingSetName(block.workingSet) != std::string("unknown"),
              "contract: block has a working-set class: " + id);
    }

    for (StagePath path : allPaths()) {
        const StagePathContract& contract = stagePathContract(path);
        const std::string label = stagePathName(path);
        check(contract.path == path, label + ": contract carries its path");
        check(!contract.nodes.empty(), label + ": contract has nodes");
        check(contract.roots.size() == 1
                  || path == StagePath::Thumbnail,
              label + ": contract has one root except the thumbnail path");

        std::set<std::string> pathIds;
        for (const char* id : contract.nodes) {
            check(ids.count(id) != 0,
                  label + ": contract node is a canonical block: "
                      + std::string(id));
            check(pathIds.insert(id).second,
                  label + ": contract node is unique: " + std::string(id));
        }
        for (const auto& edge : contract.edges) {
            check(pathIds.count(edge.first) != 0,
                  label + ": contract edge source is on the path");
            check(pathIds.count(edge.second) != 0,
                  label + ": contract edge target is on the path");
        }
        for (const char* root : contract.roots) {
            check(pathIds.count(root) != 0,
                  label + ": contract root is on the path");
        }
        for (const char* sink : contract.sinks) {
            check(pathIds.count(sink) != 0,
                  label + ": contract sink is on the path");
        }

        /* The fold host must be a real block on the same path. */
        for (const std::string& id : foldedBlockIds(path)) {
            const char* host = foldHost(path, id);
            check(host != nullptr, label + ": folded block has a host: " + id);
            check(pathIds.count(host) != 0,
                  label + ": fold host is on the path: " + id);
            check(std::string(host) != id,
                  label + ": a block cannot fold into itself: " + id);
        }

        /* Every item the path can schedule has at least one implementing
         * block, and every implementing block is on the path. */
        for (WorkItem item : pathWorkItems(path)) {
            const std::vector<std::string> blocks = workItemBlocks(path, item);
            check(!blocks.empty(),
                  label + ": work item has an implementing block: "
                      + workItemName(item));
            for (const std::string& id : blocks) {
                check(pathIds.count(id) != 0,
                      label + ": work item block is on the path: " + id);
            }
        }
    }

    /* The fold table is the one the executors consume. */
    check(foldHost(StagePath::Preview, "analysis")
              == std::string("raw.base"),
          "contract: preview analysis folds into raw.base");
    check(foldHost(StagePath::Crop, "raw.preprocess")
              == std::string("source.select"),
          "contract: crop preprocess folds into source.select");
    check(foldHost(StagePath::Crop, "raw.demosaic")
              == std::string("source.select"),
          "contract: crop demosaic folds into source.select");
    check(foldHost(StagePath::Export, "analysis") == nullptr,
          "contract: export runs analysis on its own");
    check(foldHost(StagePath::FastExport, "analysis") == nullptr,
          "contract: fast export runs analysis on its own");
    check(foldHost(StagePath::Thumbnail, "raw.base") == nullptr,
          "contract: thumbnail runs raw.base on its own");

    /* The raw-core flag is the only source of the full-frame prefix. */
    for (StagePath path : allPaths()) {
        if (path != StagePath::Crop && path != StagePath::Thumbnail) {
            continue;
        }
        const StagePipelineSpec spec = buildPipelineSpec(path);
        const StageDomain rest = path == StagePath::Crop
                                     ? StageDomain::Roi
                                     : StageDomain::Reduced;
        for (const StageDescriptorSpec& node : spec.nodes) {
            const StageBlockContract* block =
                findStageBlockContract(node.id);
            check(block != nullptr,
                  std::string(stagePathName(path))
                      + ": node has a contract record: " + node.id);
            if (block == nullptr) {
                continue;
            }
            check(node.domain ==
                      (block->rawCore ? StageDomain::Full : rest),
                  std::string(stagePathName(path)) + ": " + node.id
                      + " follows the raw-core flag");
        }
    }

    /* The contract's structural columns agree with the path edges. */
    for (StagePath path : allPaths()) {
        const StagePipelineSpec spec = buildPipelineSpec(path);
        for (const StageDescriptorSpec& node : spec.nodes) {
            if (node.ownership == StageOwnership::Consumes) {
                bool produces = false;
                for (const auto& edge : spec.edges) {
                    if (edge.first == node.id) {
                        produces = true;
                    }
                }
                check(!produces,
                      std::string(stagePathName(path))
                          + ": consumer owns no output buffer: " + node.id);
            }
            if (node.ownership == StageOwnership::InPlace) {
                check(node.inputs.size() == 1,
                      std::string(stagePathName(path))
                          + ": in-place block has one input: " + node.id);
            }
        }
    }
}

} // namespace

int main()
{
    check(std::string(stagePathName(StagePath::Export)) == "export",
          "path name: export");
    check(std::string(stagePathName(StagePath::FastExport)) == "fast_export",
          "path name: fast_export");
    check(std::string(stageDomainName(StageDomain::Reduced)) == "reduced",
          "domain name: reduced");
    check(std::string(stageConsumerName(StageConsumer::Interactive)) ==
              "interactive",
          "consumer name: interactive");
    check(std::string(stageReuseName(RT_STAGE_REUSE_OUTPUT)) == "reuse-output",
          "reuse name: reuse-output");

    for (StagePath path : allPaths()) {
        checkShape(buildPipelineSpec(path), path);
    }

    checkExport();
    checkFastExport();
    checkPreview();
    checkCrop();
    checkSharedCore();
    checkThumbnail();
    checkContract();

    if (failures == 0) {
        std::puts("stage-pipeline-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
