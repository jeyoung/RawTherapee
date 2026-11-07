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

#include "stage_contract.h"

#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

/*
 * Canonical description of the image pipelines in the engine.
 *
 * The specification is data only: it names the blocks each pipeline runs, the
 * order they run in, the spatial domain of the buffer they consume and the
 * consumer they serve. It does not execute anything. Every path consumes it;
 * see devnotes/perf/STAGE_PIPELINE.md for the inventory and the blocks that
 * could not be mapped. Fast export is the `-f` variant of export: it keeps the
 * export consumer contract but runs denoise after its early resize.
 *
 * The block vocabulary, the path node lists, the parameter fields, the fold
 * table, the work items, the working-set classes, the host bodies and the
 * device coverage come from the stage contract (stage_contract.h). This unit
 * projects them into the graph shape the planner needs; it does not restate
 * them.
 */

struct StageDescriptorSpec {
    std::string id;
    std::string name;
    StageDomain domain = StageDomain::Full;
    StageConsumer consumer = StageConsumer::Exact;
    RtReuseMode reuse = RT_STAGE_RECOMPUTE;
    StageOwnership ownership = StageOwnership::InPlace;
    /* Buffer ids this block consumes; populated from the pipeline edges. */
    std::vector<std::string> inputs;
    /* Buffer ids this block produces; always the block id itself. */
    std::vector<std::string> outputs;
    /*
     * ProcParams sub-structs read by the block, from the stage contract. The
     * names are ProcParams member names or dotted sub-fields, not individual
     * scalars.
     */
    std::vector<std::string> paramFields;
    /*
     * Host executor body id, device coverage and the device node names, from
     * the stage contract. The fields are exposed here so the planner can dump
     * the implementation map without reading the block record again.
     */
    std::string hostBody;
    StageDeviceCoverage coverage = StageDeviceCoverage::NotApplicable;
    std::vector<std::string> deviceStages;
    /*
     * When non-empty, another block carries this block's body on this path.
     * The node stays in the specification so the dirty derivation can mark it,
     * but the executor skips it.
     */
    std::string foldedInto;
};

struct StagePipelineSpec {
    StagePath path = StagePath::Export;
    std::vector<StageDescriptorSpec> nodes;
    /* (producer id, consumer id) pairs. */
    std::vector<std::pair<std::string, std::string>> edges;
    std::vector<std::string> roots;
    std::vector<std::string> sinks;
};

/*
 * Build the rooted subgraph for one pipeline. The returned spec only contains
 * nodes on that pipeline: preview does not carry export-only blocks, and the
 * thumbnail path carries both the embedded source branch and the raw fallback.
 */
StagePipelineSpec buildPipelineSpec(StagePath path);

/* Deterministic Graphviz DOT rendering of buildPipelineSpec(path). */
std::string dumpPipelineSpec(StagePath path);

} // namespace rtengine::stage
