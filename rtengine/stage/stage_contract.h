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

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

/*
 * Canonical stage contract.
 *
 * This unit is the single definition of the processor's block vocabulary. It
 * describes every canonical block once and every path once; nothing else may
 * restate a block id, a reuse mode, a parameter field list, a fold, a resident
 * step, a work item or a working-set class. The four planning authorities read
 * it instead of carrying their own copies:
 *
 * - the parameter map (stage_param_map.cc) publishes the contract's parameter
 *   fields and binds them to the fingerprint hashers;
 * - the dirty-block derivation (stage_dirty_blocks.cc) reads the residents,
 *   the coupled groups and the raw-core flag;
 * - the work order (stage_work.cc) reads the WorkItem vocabulary and the
 *   path-applicable item set;
 * - the planner (stage_pipeline.cc) builds every path specification from the
 *   block and path records, including the fold table the executors consume.
 *
 * The records are data only: they execute nothing, own no buffer and do not
 * depend on ProcParams, the coordinator or a crop. A change to a block's
 * inputs, ownership, side effects, folds, work items or working set is made
 * here and observed by every authority in the same build.
 */

/* Canonical paths. The values are diagnostic and log vocabulary. */
enum class StagePath {
    Export,
    FastExport,
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

/*
 * One destructive body job.
 *
 * The names are the diagnostic vocabulary and must stay stable: the work order
 * is logged and compared between passes, so renumbering or renaming an item
 * breaks the log contract. `Count` is the sentinel and is never a member of an
 * order.
 *
 * The enumerator spelling follows the job name used by the gates rather than
 * the UpperCamelCase used by other stage enums, so a log line can be matched
 * to the gate without a translation table. The vocabulary lives in the
 * contract because the block records declare which bodies implement each item.
 */
enum class WorkItem : uint8_t {
    /* Select the frame and, on the crop, reload the ROI bookkeeping. */
    sourceSelect = 0,
    /* Black/white levels, flat field and the pre-interpolation pass. */
    rawPreprocess = 1,
    /* Demosaic, capture sharpening and the retinex buffer preparation. */
    rawDemosaic = 2,
    /* The destructive raw.base body: getImage, spot, denoise, film negative. */
    rawBaseRebuild = 3,
    /* The retinex pre-pass that rewrites the shared raw planes. */
    retinexPrepare = 4,
    /* Spot removal into the base image. */
    spotApply = 5,
    /* Spot buffer restore or copy-back. */
    spotRestore = 6,
    /* The analysis body, including the automatic-denoise analysis. */
    analysisRun = 7,
    /* The directional-pyramid denoise body. */
    denoiseRun = 8,
    Count = 9
};

/* Number of real WorkItem values, excluding the sentinel. */
constexpr size_t workItemCount = static_cast<size_t>(WorkItem::Count);

/* Stable lower-case name of one item, for diagnostics. Never null. */
const char* workItemName(WorkItem item);

/*
 * Who owns the buffer a block writes.
 *
 * The planner uses this to reject a path whose structure contradicts the
 * buffer model: a consumer has no output buffer, an in-place block needs one
 * image input, and a producer may seed one.
 */
enum class StageOwnership : uint8_t {
    /* Allocates or seeds the image buffer. */
    Produces = 0,
    /* Consumes the in-place image buffer and writes it. */
    InPlace = 1,
    /* Reads the image but owns no output buffer (display or attachment). */
    Consumes = 2
};

const char* stageOwnershipName(StageOwnership ownership);

/*
 * Side effects that outlive the block's own output buffer.
 *
 * These are the facts the dirty-block derivation and the work order need in
 * order to keep the raw core consistent: a block that rewrites the raw planes
 * invalidates the base, and a block that writes a side channel must be
 * scheduled with the body that owns that channel.
 */
enum class StageEffect : uint32_t {
    None = 0,
    /* Seeds the frame identity; a change reloads the raw core. */
    SourceFrame = 1u << 0,
    /* Rewrites the shared raw planes (preprocess, demosaic, retinex). */
    RawPlanes = 1u << 1,
    /* Rewrites the base image or the spot buffer. */
    BaseImage = 1u << 2,
    /* Writes the spot buffer that a later pass restores. */
    SpotBuffer = 1u << 3,
    /* Writes the analysis side channel consumed by the base. */
    Analysis = 1u << 4,
    /* Writes a display sink, which owns no processing buffer. */
    Display = 1u << 5,
    /* Writes output metadata or attaches a profile. */
    Metadata = 1u << 6
};

inline StageEffect operator|(StageEffect a, StageEffect b)
{
    return static_cast<StageEffect>(static_cast<uint32_t>(a)
                                    | static_cast<uint32_t>(b));
}

inline StageEffect operator&(StageEffect a, StageEffect b)
{
    return static_cast<StageEffect>(static_cast<uint32_t>(a)
                                    & static_cast<uint32_t>(b));
}

inline bool hasEffect(StageEffect value, StageEffect flag)
{
    return (value & flag) != StageEffect::None;
}

/* Stable lower-case rendering of an effect set, for diagnostics. */
std::string stageEffectName(StageEffect effect);

/*
 * Device working-memory classes.
 *
 * The class names the buffers a node needs beyond its cached image plane.
 * `stage_opencl_device.cc` enforces the byte budget itself; the contract
 * declares the class so the planner can report a block's working set and a
 * future launcher can admit it before allocating.
 */
enum class StageWorkingSet : uint32_t {
    None = 0,
    /* One image plane, held by the residency cache. */
    Image = 1u << 0,
    /* One or more full-frame scratch planes beside the image. */
    ScratchPlanes = 1u << 1,
    /* The four-float-per-pixel geometry map. */
    TransformMap = 1u << 2,
    /* The Lanczos tap-weight tables. */
    TapTables = 1u << 3,
    /* The double-precision local-contrast state plane. */
    DoubleState = 1u << 4
};

inline StageWorkingSet operator|(StageWorkingSet a, StageWorkingSet b)
{
    return static_cast<StageWorkingSet>(static_cast<uint32_t>(a)
                                        | static_cast<uint32_t>(b));
}

inline StageWorkingSet operator&(StageWorkingSet a, StageWorkingSet b)
{
    return static_cast<StageWorkingSet>(static_cast<uint32_t>(a)
                                        & static_cast<uint32_t>(b));
}

inline bool hasWorkingSet(StageWorkingSet value, StageWorkingSet flag)
{
    return (value & flag) != StageWorkingSet::None;
}

/* Stable lower-case rendering of a working-set set, for diagnostics. */
std::string stageWorkingSetName(StageWorkingSet workingSet);

/*
 * One fold: on `path`, the block does not run as its own executor step and its
 * body is carried by `host`.
 *
 * A folded block stays in the path specification so the dirty derivation and
 * the planner can mark it dirty; the executor skips it and runs the host body.
 */
struct StageFoldRule {
    StagePath path = StagePath::Preview;
    const char* host = nullptr;
};

/* One canonical block, defined once for every path that carries it. */
struct StageBlockContract {
    const char* id = "";
    const char* name = "";
    RtReuseMode reuse = RT_STAGE_RECOMPUTE;
    StageOwnership ownership = StageOwnership::InPlace;
    StageEffect effects = StageEffect::None;
    StageWorkingSet workingSet = StageWorkingSet::None;
    /* ProcParams members the block reads; dotted names are sub-fields. */
    std::vector<const char*> paramFields;
    /* Destructive bodies this block implements. */
    std::vector<WorkItem> workItems;
    /* Paths on which another block carries this body. */
    std::vector<StageFoldRule> folds;
    /* Paths on which the block runs on every update. */
    std::vector<StagePath> residents;
    /*
     * Whether the block loads the full-frame raw source. A path on an ROI or a
     * reduction still runs this prefix at full frame; every other block runs
     * in the path's domain. The flag is the planner's only source for that
     * split, so the three raw-core ids are not repeated in stage_pipeline.cc.
     */
    bool rawCore = false;
};

/*
 * One path, defined once. The node list is the execution order; the edges are
 * the buffer dependencies; roots and sinks are the boundary markers. The
 * coupled groups are path-level because membership differs between preview
 * and crop.
 */
struct StagePathContract {
    StagePath path = StagePath::Export;
    std::vector<const char*> nodes;
    std::vector<std::pair<const char*, const char*>> edges;
    std::vector<const char*> roots;
    std::vector<const char*> sinks;
    std::vector<std::vector<const char*>> coupledGroups;
};

/* The block catalogue, in stable review order. Immutable for the process. */
const std::vector<StageBlockContract>& stageBlockContracts();

/* The definition of one path. Asserts on an unknown path. */
const StagePathContract& stagePathContract(StagePath path);

/* The block record for `id`, or null when the id is not canonical. */
const StageBlockContract* findStageBlockContract(const std::string& id);

/* Whether `id` is a canonical block. */
bool isStageBlockId(const std::string& id);

/*
 * Folded blocks on `path`, in pipeline order. A folded block is present in the
 * path specification but does not run as its own executor step.
 */
std::vector<std::string> foldedBlockIds(StagePath path);

/* The host block that carries `id` on `path`, or null when it is not folded. */
const char* foldHost(StagePath path, const std::string& id);

/* WorkItem values a path can schedule, in enumeration order. */
std::vector<WorkItem> pathWorkItems(StagePath path);

/* Whether `path` carries a block that implements `item`. */
bool pathSchedulesWorkItem(StagePath path, WorkItem item);

/* Blocks on `path` that implement `item`, in pipeline order. */
std::vector<std::string> workItemBlocks(StagePath path, WorkItem item);

/* Stable lower-case names for diagnostics and the dump format. */
const char* stagePathName(StagePath path);
const char* stageDomainName(StageDomain domain);
const char* stageConsumerName(StageConsumer consumer);
const char* stageReuseName(RtReuseMode reuse);

} // namespace rtengine::stage
