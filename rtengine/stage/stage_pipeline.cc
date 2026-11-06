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

#include <cassert>
#include <cstddef>
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
    descriptor.effects = block->effects;
    descriptor.workingSet = block->workingSet;
    descriptor.workItems = block->workItems;
    descriptor.outputs.push_back(descriptor.id);
    for (const char* param : block->paramFields) {
        descriptor.paramFields.emplace_back(param);
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
 * contradicts a block's ownership is a programming error, not a runtime
 * fallback. The checks are assertions so a release build still plans the path,
 * while every debug build and every test fails at the source of the drift.
 */
void validateSpec(const StagePipelineSpec& spec)
{
    std::set<std::string> ids;
    for (const StageDescriptorSpec& node : spec.nodes) {
        const bool inserted = ids.insert(node.id).second;
        assert(inserted && "a path names a block twice");
        (void)inserted;
        assert(!node.id.empty() && "a path names an empty block id");
    }

    std::set<std::pair<std::string, std::string>> uniqueEdges;
    std::set<std::string> produced;
    std::set<std::string> consumed;
    for (const auto& edge : spec.edges) {
        assert(ids.count(edge.first) != 0 && "edge source is not on the path");
        assert(ids.count(edge.second) != 0 && "edge target is not on the path");
        assert(uniqueEdges.insert(edge).second && "duplicate path edge");
        produced.insert(edge.first);
        consumed.insert(edge.second);
    }

    for (const StageDescriptorSpec& node : spec.nodes) {
        if (!node.foldedInto.empty()) {
            assert(ids.count(node.foldedInto) != 0
                   && "a fold host is not on the path");
            assert(foldHost(spec.path, node.foldedInto) == nullptr
                   && "a fold host is itself folded");
        }

        switch (node.ownership) {
        case StageOwnership::Consumes:
            assert(produced.count(node.id) == 0
                   && "a consumer may not produce an output buffer");
            break;
        case StageOwnership::InPlace:
            assert(node.inputs.size() == 1
                   && "an in-place block needs exactly one buffer input");
            break;
        case StageOwnership::Produces:
            break;
        }
    }

    for (const std::string& root : spec.roots) {
        (void)root;
        assert(ids.count(root) != 0 && "root is not on the path");
        assert(consumed.count(root) == 0 && "root has an input edge");
    }
    for (const std::string& sink : spec.sinks) {
        (void)sink;
        assert(ids.count(sink) != 0 && "sink is not on the path");
        assert(produced.count(sink) == 0 && "sink has an output edge");
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
        out << "  // node: id=" << node.id << " name=\"" << node.name
            << "\" domain=" << stageDomainName(node.domain)
            << " consumer=" << stageConsumerName(node.consumer)
            << " reuse=" << stageReuseName(node.reuse)
            << " ownership=" << stageOwnershipName(node.ownership)
            << " effects=" << stageEffectName(node.effects)
            << " working-set=" << stageWorkingSetName(node.workingSet)
            << " work-items=";
        for (size_t i = 0; i < node.workItems.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << workItemName(node.workItems[i]);
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
