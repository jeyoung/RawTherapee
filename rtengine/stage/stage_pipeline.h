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

#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

/*
 * Canonical description of the four image pipelines in the engine.
 *
 * The specification is data only: it names the blocks each pipeline runs, the
 * order they run in, the spatial domain of the buffer they consume and the
 * consumer they serve. It does not execute anything. The thumbnail path
 * consumes it behind RT_STAGE_GRAPH=1; see devnotes/perf/STAGE_PIPELINE.md for
 * the inventory and the blocks that could not be mapped.
 */

enum class StagePath {
    Export,
    Preview,
    Crop,
    Thumbnail
};

/* Spatial domain of the buffer a block consumes. */
enum class StageDomain {
    Full,     // full processed frame
    Roi,      // crop/viewport region of the frame
    Reduced   // thumbnail-sized reduction
};

/* Whether a consumer requires exact output or tolerates interactive reuse. */
enum class StageConsumer {
    Exact,
    Interactive
};

struct StageDescriptorSpec {
    std::string id;
    std::string name;
    StageDomain domain = StageDomain::Full;
    StageConsumer consumer = StageConsumer::Exact;
    RtReuseMode reuse = RT_STAGE_RECOMPUTE;
    /* Buffer ids this block consumes; populated from the pipeline edges. */
    std::vector<std::string> inputs;
    /* Buffer ids this block produces; always the block id itself. */
    std::vector<std::string> outputs;
    /*
     * ProcParams sub-structs read by the block. Provisional: the names are the
     * ProcParams member names, not the individual fields, and they are a
     * lower bound until each block is implemented against the spec.
     */
    std::vector<std::string> paramFields;
};

struct StagePipelineSpec {
    StagePath path = StagePath::Export;
    std::vector<StageDescriptorSpec> nodes;
    /* (producer id, consumer id) pairs. */
    std::vector<std::pair<std::string, std::string>> edges;
    std::vector<std::string> roots;
    std::vector<std::string> sinks;
};

/* Stable lower-case names for diagnostics and the dump format. */
const char* stagePathName(StagePath path);
const char* stageDomainName(StageDomain domain);
const char* stageConsumerName(StageConsumer consumer);
const char* stageReuseName(RtReuseMode reuse);

/*
 * Build the rooted subgraph for one pipeline. The returned spec only contains
 * nodes on that pipeline: preview does not carry export-only blocks, and the
 * thumbnail path carries both the embedded source branch and the raw fallback.
 */
StagePipelineSpec buildPipelineSpec(StagePath path);

/* Deterministic Graphviz DOT rendering of buildPipelineSpec(path). */
std::string dumpPipelineSpec(StagePath path);

} // namespace rtengine::stage
