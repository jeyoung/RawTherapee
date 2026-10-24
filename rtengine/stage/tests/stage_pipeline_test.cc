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
            check(contains(knownParamFields(), field),
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
        const StageConsumer expected = spec.path == StagePath::Export
                                           ? StageConsumer::Exact
                                           : StageConsumer::Interactive;
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
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
        "analysis", "transform", "local.adjustments", "rgb.proc", "lab.tools",
        "lab.sharpening", "lab.wavelet", "working.trc", "ciecam", "gamut",
        "output.convert", "crop", "resize", "output.sharpen", "framing",
        "output.icc"
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
        "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
        "analysis", "transform"
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

} // namespace

int main()
{
    check(std::string(stagePathName(StagePath::Export)) == "export",
          "path name: export");
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
    checkPreview();
    checkCrop();
    checkSharedCore();
    checkThumbnail();

    if (failures == 0) {
        std::puts("stage-pipeline-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
