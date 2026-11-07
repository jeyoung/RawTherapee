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

#include "stage_pipeline.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

namespace
{

StageDomain domainFor(StagePath path, const StageBlockContract& block)
{
    switch (path) {
    case StagePath::Export:
    case StagePath::FastExport:
    case StagePath::Preview:
        return StageDomain::Full;
    case StagePath::Crop:
        // Raw loading is always full-frame; everything from getImage onwards
        // runs on the crop region.
        return block.rawCore ? StageDomain::Full : StageDomain::Roi;
    case StagePath::Thumbnail:
        return block.rawCore ? StageDomain::Full : StageDomain::Reduced;
    }
    return StageDomain::Full;
}

StageConsumer consumerFor(StagePath path)
{
    // Only the export pipelines have to be bit-exact; the others feed a viewer
    // or a thumbnail cache and may reuse approximate intermediate results.
    return path == StagePath::Export || path == StagePath::FastExport
               ? StageConsumer::Exact
               : StageConsumer::Interactive;
}

StageDescriptorSpec makeDescriptor(const std::string& id, StagePath path)
{
    const StageBlockContract* block = findStageBlockContract(id);
    assert(block != nullptr && "a path names an unknown stage block");
    if (block == nullptr) {
        StageDescriptorSpec descriptor;
        descriptor.id = id;
        descriptor.name = id;
        descriptor.domain = StageDomain::Full;
        descriptor.consumer = consumerFor(path);
        descriptor.hostBody = id;
        descriptor.outputs.push_back(id);
        return descriptor;
    }

    StageDescriptorSpec descriptor;
    descriptor.id = block->id;
    descriptor.name = block->name;
    descriptor.domain = domainFor(path, *block);
    descriptor.consumer = consumerFor(path);
    descriptor.reuse = block->reuse;
    descriptor.ownership = block->ownership;
    descriptor.hostBody = block->hostBody;
    descriptor.coverage = block->coverage;
    descriptor.outputs.push_back(descriptor.id);
    for (const char* param : block->paramFields) {
        descriptor.paramFields.emplace_back(param);
    }
    for (const char* deviceStage : block->deviceStages) {
        descriptor.deviceStages.emplace_back(deviceStage);
    }
    const char* host = foldHost(path, descriptor.id);
    if (host != nullptr) {
        descriptor.foldedInto = host;
    }
    return descriptor;
}

void populateInputs(StagePipelineSpec& spec)
{
    for (auto& node : spec.nodes) {
        node.inputs.clear();
    }
    for (const auto& edge : spec.edges) {
        for (auto& node : spec.nodes) {
            if (node.id == edge.second) {
                node.inputs.push_back(edge.first);
                break;
            }
        }
    }
}

/*
 * Check that the contract's structural columns agree with the path edges.
 *
 * This is the planner's side of the contract: a path whose node structure
 * contradicts a block's ownership, effects or working-set class is a
 * programming error, not a runtime fallback. The checks abort in release
 * builds too, because a rejected plan is safer than a partly ordered one.
 */
[[noreturn]] void invalidSpec(const std::string& reason)
{
    std::fprintf(stderr, "stage-pipeline: fatal: %s\n", reason.c_str());
    std::abort();
}

bool hasWorkItem(StagePath path, WorkItem item)
{
    return pathSchedulesWorkItem(path, item);
}

void validateSpec(const StagePipelineSpec& spec)
{
    std::set<std::string> ids;
    for (const StageDescriptorSpec& node : spec.nodes) {
        if (!ids.insert(node.id).second) {
            invalidSpec("a path names a block twice: " + node.id);
        }
        if (node.id.empty()) {
            invalidSpec("a path names an empty block id");
        }
    }

    std::set<std::pair<std::string, std::string>> uniqueEdges;
    std::set<std::string> produced;
    std::set<std::string> consumed;
    for (const auto& edge : spec.edges) {
        if (ids.count(edge.first) == 0 ||
            ids.count(edge.second) == 0) {
            invalidSpec("an edge names a block outside the path");
        }
        if (!uniqueEdges.insert(edge).second) {
            invalidSpec("duplicate path edge");
        }
        produced.insert(edge.first);
        consumed.insert(edge.second);
    }

    const bool workOrdered = spec.path != StagePath::Thumbnail;

    for (const StageDescriptorSpec& node : spec.nodes) {
        if (!node.foldedInto.empty()) {
            if (ids.count(node.foldedInto) == 0) {
                invalidSpec("a fold host is not on the path: " +
                            node.foldedInto);
            }
            if (foldHost(spec.path, node.foldedInto) != nullptr) {
                invalidSpec("a fold host is itself folded: " +
                            node.foldedInto);
            }
        }

        switch (node.ownership) {
        case StageOwnership::Consumes:
            if (produced.count(node.id) != 0) {
                invalidSpec("a consumer produces an output buffer: " +
                            node.id);
            }
            break;
        case StageOwnership::InPlace:
            if (node.inputs.size() != 1) {
                invalidSpec("an in-place block needs one buffer input: " +
                            node.id);
            }
            break;
        case StageOwnership::Produces:
            break;
        }

        const StageBlockContract* block =
            findStageBlockContract(node.id);
        if (block == nullptr) {
            invalidSpec("a path names an unknown block: " + node.id);
        }

        /*
         * A folded block runs its host's body, so its effects and work items
         * belong to that body on this path. The thumbnail path has no work
         * order, so its effects are not checked against one.
         */
        if (block == nullptr || !workOrdered ||
            !node.foldedInto.empty()) {
            continue;
        }

        if (hasEffect(block->effects, StageEffect::Display) ||
            hasEffect(block->effects, StageEffect::Metadata)) {
            if (node.ownership != StageOwnership::Consumes) {
                invalidSpec("a sink effect needs a consumer: " + node.id);
            }
        }
        if (hasEffect(block->effects, StageEffect::SpotBuffer) &&
            !hasWorkItem(spec.path, WorkItem::spotApply) &&
            !hasWorkItem(spec.path, WorkItem::spotRestore)) {
            invalidSpec("a spot effect has no spot work item: " + node.id);
        }
        if (hasEffect(block->effects, StageEffect::BaseImage) &&
            !hasWorkItem(spec.path, WorkItem::rawBaseRebuild) &&
            !hasWorkItem(spec.path, WorkItem::denoiseRun)) {
            invalidSpec("a base effect has no base work item: " + node.id);
        }
        if (hasEffect(block->effects, StageEffect::Analysis) &&
            !hasWorkItem(spec.path, WorkItem::analysisRun)) {
            invalidSpec("an analysis effect has no analysis item: " + node.id);
        }
        if (hasEffect(block->effects, StageEffect::RawPlanes) &&
            !hasWorkItem(spec.path, WorkItem::rawPreprocess) &&
            !hasWorkItem(spec.path, WorkItem::rawDemosaic) &&
            !hasWorkItem(spec.path, WorkItem::retinexPrepare)) {
            invalidSpec("a raw-plane effect has no raw work item: " + node.id);
        }
        if (node.ownership == StageOwnership::Consumes &&
            (hasWorkingSet(block->workingSet, StageWorkingSet::ScratchPlanes) ||
             hasWorkingSet(block->workingSet, StageWorkingSet::TransformMap) ||
             hasWorkingSet(block->workingSet, StageWorkingSet::TapTables) ||
             hasWorkingSet(block->workingSet, StageWorkingSet::DoubleState))) {
            invalidSpec("a consumer declares device scratch: " + node.id);
        }
    }

    for (const std::string& root : spec.roots) {
        if (ids.count(root) == 0 || consumed.count(root) != 0) {
            invalidSpec("a root is not a root: " + root);
        }
    }
    for (const std::string& sink : spec.sinks) {
        if (ids.count(sink) == 0 || produced.count(sink) != 0) {
            invalidSpec("a sink is not a sink: " + sink);
        }
    }
}

} // namespace

StagePipelineSpec buildPipelineSpec(StagePath path)
{
    const StagePathContract& contract = stagePathContract(path);

    StagePipelineSpec spec;
    spec.path = path;

    spec.nodes.reserve(contract.nodes.size());
    for (const char* id : contract.nodes) {
        spec.nodes.push_back(makeDescriptor(id, path));
    }

    spec.edges.reserve(contract.edges.size());
    for (const auto& edge : contract.edges) {
        spec.edges.emplace_back(edge.first, edge.second);
    }

    for (const char* root : contract.roots) {
        spec.roots.emplace_back(root);
    }
    for (const char* sink : contract.sinks) {
        spec.sinks.emplace_back(sink);
    }

    populateInputs(spec);
    validateSpec(spec);
    return spec;
}

std::string dumpPipelineSpec(StagePath path)
{
    const StagePipelineSpec spec = buildPipelineSpec(path);

    std::ostringstream out;
    out << "digraph stage_pipeline_" << stagePathName(path) << " {\n";
    out << "  rankdir=LR;\n";
    out << "  // path: " << stagePathName(path) << "\n";

    for (const auto& node : spec.nodes) {
        const StageBlockContract* block =
            findStageBlockContract(node.id);
        assert(block != nullptr && "a spec node has no contract record");
        out << "  // node: id=" << node.id << " name=\"" << node.name
            << "\" domain=" << stageDomainName(node.domain)
            << " consumer=" << stageConsumerName(node.consumer)
            << " reuse=" << stageReuseName(node.reuse)
            << " ownership=" << stageOwnershipName(node.ownership)
            << " host-body=" << node.hostBody
            << " device-coverage="
            << stageDeviceCoverageName(node.coverage)
            << " device-stages=";
        for (size_t i = 0; i < node.deviceStages.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << node.deviceStages[i];
        }
        if (block != nullptr) {
            out << " effects=" << stageEffectName(block->effects)
                << " working-set=" << stageWorkingSetName(block->workingSet)
                << " work-items=";
            for (size_t i = 0; i < block->workItems.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << workItemName(block->workItems[i].item);
            }
        }
        if (!node.foldedInto.empty()) {
            out << " folded-into=" << node.foldedInto;
        }
        out << " params=";
        for (size_t i = 0; i < node.paramFields.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << node.paramFields[i];
        }
        out << "\n";
        out << "  \"" << node.id << "\" [label=\"" << node.id << "\\n"
            << node.name << "\\n" << stageDomainName(node.domain) << "/"
            << stageConsumerName(node.consumer) << "\"];\n";
    }

    for (const auto& edge : spec.edges) {
        out << "  // edge: " << edge.first << " -> " << edge.second << "\n";
        out << "  \"" << edge.first << "\" -> \"" << edge.second << "\";\n";
    }

    out << "  // roots:";
    for (const auto& root : spec.roots) {
        out << " " << root;
    }
    out << "\n  // sinks:";
    for (const auto& sink : spec.sinks) {
        out << " " << sink;
    }
    out << "\n}\n";

    return out.str();
}

} // namespace rtengine::stage
