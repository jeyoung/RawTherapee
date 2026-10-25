/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
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

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <glibmm/ustring.h>
#include <glibmm/timer.h>

#include "cieimage.h"
#include "color.h"
#include "curves.h"
#include "dcp.h"
#include "dcrop.h"
#include "guidedfilter.h"
#include "image8.h"
#include "imagefloat.h"
#include "improccoordinator.h"
#include "labimage.h"
#include "mytime.h"
#include "procparams.h"
#include "refreshmap.h"
#include "stage/stage_dirty_blocks.h"
#include "stage/stage_graph.h"
#include "stage/stage_pipeline.h"
#include "rt_math.h"
#include "utils.h"
#include "iccstore.h"

#include "rtgui/editcallbacks.h"

#pragma GCC diagnostic warning "-Wall"
#pragma GCC diagnostic warning "-Wextra"
namespace
{
// "ceil" rounding
template<typename T>
constexpr T skips(T a, T b)
{
    return a / b + static_cast<bool>(a % b);
}

}

namespace rtengine
{
using rtengine::TMatrix;

/*
 * Per-update state shared by the extracted crop stage blocks.
 *
 * The raw stage is one load call and an auto-noise analysis whose buffers are
 * allocated before the loads and released by the base finish, so the blocks
 * that the canonical graph names separately exchange this scratch through the
 * context. Nothing here changes an allocation, a rounding step or an ordering.
 */
struct CropStageContext {
    procparams::ProcParams& params;
    const int todo;
    Imagefloat* baseCrop;

    // Raw load and auto-denoise scratch.
    bool needsInitUpdate = false;
    int tr = 0;
    int widIm = 0;
    int heiIm = 0;
    float autoNR = 0.f;
    float autoNRmax = 0.f;
    NoiseCurve noiseLCurve;
    NoiseCurve noiseCCurve;
    int numtiles_W = 0;
    int numtiles_H = 0;
    int tileWskip = 0;
    int tileHskip = 0;
    float* min_b = nullptr;
    float* min_r = nullptr;
    float* lumL = nullptr;
    float* chromC = nullptr;
    float* ry = nullptr;
    float* sk = nullptr;
    float* pcsk = nullptr;
    int* centerTile_X = nullptr;
    int* centerTile_Y = nullptr;

    // Written by the transform block in its legacy position, read by the Lab
    // blocks.
    bool cam02 = false;

    // Shared sink for the two calls that take a dummy histogram output.
    // Nothing reads it, exactly as in the legacy sequence.
    LUTu dummy;

    CropStageContext(procparams::ProcParams& parameters, int actions,
                     Imagefloat* base)
        : params(parameters), todo(actions), baseCrop(base)
    {
    }

    ~CropStageContext()
    {
        releaseRawScratch();
    }

    CropStageContext(const CropStageContext&) = delete;
    CropStageContext& operator=(const CropStageContext&) = delete;

    void releaseRawScratch()
    {
        delete[] min_b;
        min_b = nullptr;
        delete[] min_r;
        min_r = nullptr;
        delete[] lumL;
        lumL = nullptr;
        delete[] chromC;
        chromC = nullptr;
        delete[] ry;
        ry = nullptr;
        delete[] sk;
        sk = nullptr;
        delete[] pcsk;
        pcsk = nullptr;
        delete[] centerTile_X;
        centerTile_X = nullptr;
        delete[] centerTile_Y;
        centerTile_Y = nullptr;
    }
};

/*
 * The inputs of the previous dirty-block derivation of one crop.
 *
 * The parameter side keeps one fingerprint per canonical crop block instead of
 * a copy of ProcParams: the next update only needs to know whether a block's
 * inputs moved, and hashing the current parameters once per update is enough.
 * The render state is kept whole because every aspect is compared by field.
 *
 * The snapshot is only meaningful after a derivation has run; `valid` is false
 * until then, which makes the next derivation a forced full recompute.
 */
struct CropDirtyState {
    bool valid = false;
    rtengine::stage::BlockFingerprints fingerprints;
    rtengine::stage::RenderStateKey renderState;
};

Crop::Crop(ImProcCoordinator* parent, EditDataProvider *editDataProvider, bool isDetailWindow)
    : PipetteBuffer(editDataProvider), origCrop(nullptr), spotCrop(nullptr), laboCrop(nullptr), labnCrop(nullptr),
      cropImg(nullptr), shbuf_real(nullptr), transCrop(nullptr), cieCrop(nullptr), shbuffer(nullptr),
      updating(false), newUpdatePending(false), skip(10),
      cropx(0), cropy(0), cropw(-1), croph(-1),
      trafx(0), trafy(0), trafw(-1), trafh(-1),
      rqcropx(0), rqcropy(0), rqcropw(-1), rqcroph(-1),
      borderRequested(32), upperBorder(0), leftBorder(0),
      cropAllocated(false),
      cropImageListener(nullptr), parent(parent), isDetailWindow(isDetailWindow)
{
    parent->crops.push_back(this);
}

Crop::~Crop()
{

    MyMutex::MyLock cropLock(cropMutex);

    std::vector<Crop*>::iterator i = std::find(parent->crops.begin(), parent->crops.end(), this);

    if (i != parent->crops.end()) {
        parent->crops.erase(i);
    }

    MyMutex::MyLock processingLock(parent->mProcessing);
    freeAll();
}

void Crop::destroy()
{
    MyMutex::MyLock lock(cropMutex);
    MyMutex::MyLock processingLock(parent->mProcessing);
    freeAll();
}

void Crop::setListener(DetailedCropListener* il)
{
    // We can make reads in the IF, because the mProcessing lock is only needed for change
    if (cropImageListener != il) {
        MyMutex::MyLock lock(cropMutex);
        cropImageListener = il;
    }
}

EditUniqueID Crop::getCurrEditID() const
{
    const EditSubscriber *subscriber = PipetteBuffer::dataProvider ? PipetteBuffer::dataProvider->getCurrSubscriber() : nullptr;
    return subscriber ? subscriber->getEditID() : EUID_None;
}

/*
 * Delete the edit image buffer if there's no subscriber anymore.
 * If allocation has to be done, it is deferred to Crop::update
 */
void Crop::setEditSubscriber(EditSubscriber* newSubscriber)
{
    MyMutex::MyLock lock(cropMutex);

    // At this point, editCrop.dataProvider->currSubscriber is the old subscriber
    const EditSubscriber *oldSubscriber = PipetteBuffer::dataProvider ? PipetteBuffer::dataProvider->getCurrSubscriber() : nullptr;

    if (newSubscriber == nullptr || (oldSubscriber != nullptr && oldSubscriber->getPipetteBufferType() != newSubscriber->getPipetteBufferType())) {
        if (PipetteBuffer::imgFloatBuffer != nullptr) {
            delete PipetteBuffer::imgFloatBuffer;
            PipetteBuffer::imgFloatBuffer = nullptr;
        }

        if (PipetteBuffer::LabBuffer != nullptr) {
            delete PipetteBuffer::LabBuffer;
            PipetteBuffer::LabBuffer = nullptr;
        }

        if (PipetteBuffer::singlePlaneBuffer.getWidth() != -1) {
            PipetteBuffer::singlePlaneBuffer.flushData();
        }
    }

    // If oldSubscriber == NULL && newSubscriber != NULL && newSubscriber->getEditingType() == ET_PIPETTE-> the image will be allocated when necessary
}

bool Crop::hasListener()
{
    MyMutex::MyLock cropLock(cropMutex);
    return cropImageListener;
}

namespace
{

/*
 * How the crop graph obtains its dirty block set (RT_CROP_DIRTY).
 *
 * - `legacy` executes the set the mask-derived gates have always produced and
 *   skips the derivation entirely. It is the A/B baseline for parity runs.
 * - `shadow` executes the legacy set and compares it with the derived set,
 *   which is how equivalence is established without changing pixels.
 * - `fingerprint`, the default once the shadow run is clean, executes the
 *   derived set. If the derived set would skip a block the legacy set runs, the
 *   update fails closed to the legacy set and the event is counted.
 *
 * The mask still drives GUI notification and coalescing in all three modes; it
 * just stops being the only source of block dirtiness. The modes choose the
 * dirty set only: every mode executes through the graph, which is the only
 * crop sequence.
 */
enum class CropDirtyMode {
    Legacy,
    Shadow,
    Fingerprint
};

CropDirtyMode cropDirtyMode()
{
    const char* value = g_getenv("RT_CROP_DIRTY");
    if (value == nullptr || value[0] == '\0') {
        return CropDirtyMode::Fingerprint;
    }
    if (g_ascii_strcasecmp(value, "legacy") == 0
            || g_ascii_strcasecmp(value, "mask") == 0) {
        return CropDirtyMode::Legacy;
    }
    if (g_ascii_strcasecmp(value, "shadow") == 0) {
        return CropDirtyMode::Shadow;
    }
    if (g_ascii_strcasecmp(value, "fingerprint") == 0
            || g_ascii_strcasecmp(value, "derived") == 0) {
        return CropDirtyMode::Fingerprint;
    }
    std::fprintf(stderr,
                 "crop-dirty: unknown RT_CROP_DIRTY value '%s', using "
                 "fingerprint\n",
                 value);
    return CropDirtyMode::Fingerprint;
}

const char* cropDirtyModeName(CropDirtyMode mode)
{
    switch (mode) {
    case CropDirtyMode::Legacy:
        return "legacy";
    case CropDirtyMode::Shadow:
        return "shadow";
    case CropDirtyMode::Fingerprint:
        return "fingerprint";
    }

    return "unknown";
}

/*
 * Execution order of the crop blocks.
 *
 * This is the order of the legacy sequence, not the order of the canonical
 * specification: the crop measures noise on the freshly loaded region before
 * the base finish completes, so `analysis` runs before the rest of `raw.base`.
 * The specification folds that interleaving; the graph decides whether a block
 * runs, never the order in which the blocks run.
 */
enum CropStage {
    CROP_SOURCE_SELECT = 0,
    CROP_ANALYSIS,
    CROP_RAW_BASE,
    CROP_TRANSFORM,
    CROP_LOCAL_ADJUSTMENTS,
    CROP_RGB_PROC,
    CROP_LAB_TOOLS,
    CROP_LAB_SHARPENING,
    CROP_LAB_WAVELET,
    CROP_WORKING_TRC,
    CROP_CIE_CAM,
    CROP_GAMUT,
    CROP_VIEWPORT_OUTPUT,
    CROP_STAGE_COUNT
};

constexpr const char* CROP_STAGE_IDS[CROP_STAGE_COUNT] = {
    "source.select",
    "analysis",
    "raw.base",
    "transform",
    "local.adjustments",
    "rgb.proc",
    "lab.tools",
    "lab.sharpening",
    "lab.wavelet",
    "working.trc",
    "ciecam",
    "gamut",
    "viewport.output"
};

/*
 * Canonical nodes this path does not execute as a block of its own. The crop
 * obtains its region through one opaque ImageSource::getImage() call, which
 * performs the raw select, preprocess and demosaic steps, so those two nodes
 * are carried by `source.select`.
 */
constexpr const char* CROP_FOLDED_NODES[] = {
    "raw.preprocess",
    "raw.demosaic"
};
constexpr size_t CROP_FOLDED_COUNT =
    sizeof(CROP_FOLDED_NODES) / sizeof(CROP_FOLDED_NODES[0]);

/*
 * The dirty-block set in the terms the crop execution list uses.
 *
 * The derivation works on canonical descriptor ids, while the crop sequence is
 * a list of stage blocks in its own order. These helpers translate between the
 * two and drop the folded nodes, which the crop never executes on their own.
 * They deliberately keep the canonical pipeline order, so two sets from
 * different sources can be printed and compared without a second sort.
 */

/* Stage-owned ids of `blocks`, in canonical crop pipeline order. */
std::vector<std::string> cropStageOrder(const std::vector<std::string>& blocks)
{
    const std::set<std::string> wanted(blocks.begin(), blocks.end());
    std::vector<std::string> ordered;
    for (const auto& node : rtengine::stage::buildPipelineSpec(
             rtengine::stage::StagePath::Crop).nodes) {
        if (wanted.find(node.id) == wanted.end()) {
            continue;
        }
        for (int stage = 0; stage < CROP_STAGE_COUNT; ++stage) {
            if (node.id == CROP_STAGE_IDS[stage]) {
                ordered.push_back(node.id);
                break;
            }
        }
    }
    return ordered;
}

/* The ids the given execution list names, in canonical order. */
std::vector<std::string> cropBlocksOf(const bool runs[CROP_STAGE_COUNT])
{
    std::vector<std::string> blocks;
    for (int stage = 0; stage < CROP_STAGE_COUNT; ++stage) {
        if (runs[stage]) {
            blocks.emplace_back(CROP_STAGE_IDS[stage]);
        }
    }
    return blocks;
}

/* Execution list implied by a set of canonical ids. */
void cropRunsOf(const std::vector<std::string>& blocks,
                bool runs[CROP_STAGE_COUNT])
{
    const std::set<std::string> dirty(blocks.begin(), blocks.end());
    for (int stage = 0; stage < CROP_STAGE_COUNT; ++stage) {
        runs[stage] = dirty.find(CROP_STAGE_IDS[stage]) != dirty.end();
    }
}

struct CropGraphPlan {
    bool valid = false;
    std::string reason;
    /* Blocks to run, in execution order. */
    std::vector<CropStage> execute;
};

/*
 * Build the canonical crop spec, connect it into a StageGraph and ask
 * StageGraph::plan() which blocks this invocation may run. Every node named by
 * the chosen dirty set is marked dirty; every other node is clean, either
 * because it is folded into a block that is already scheduled or because its
 * output is reused.
 *
 * `dirtyIds` is the execution set in canonical ids and must contain every
 * block in `runs`: the crop always runs what the execution list names. A plan
 * is rejected, and the caller aborts, when a block the execution list names
 * would be skipped (ReuseOutput): that is the only way the graph could change
 * the result, and there is no second sequence to run.
 */
CropGraphPlan planCropGraph(const std::vector<std::string>& dirtyIds,
                            const bool runs[CROP_STAGE_COUNT])
{
    using namespace rtengine::stage;

    CropGraphPlan result;
    const StagePipelineSpec spec = buildPipelineSpec(StagePath::Crop);
    const std::set<std::string> dirty(dirtyIds.begin(), dirtyIds.end());

    const auto indexOf = [&spec](const std::string& id) -> size_t {
        for (size_t i = 0; i < spec.nodes.size(); ++i) {
            if (spec.nodes[i].id == id) {
                return i;
            }
        }
        return spec.nodes.size();
    };

    StageGraph graph;
    std::vector<size_t> nodes;
    nodes.reserve(spec.nodes.size());
    for (const auto& node : spec.nodes) {
        nodes.push_back(graph.addNode(node.id, node.reuse));
    }
    for (const auto& edge : spec.edges) {
        const size_t from = indexOf(edge.first);
        const size_t to = indexOf(edge.second);
        if (from == spec.nodes.size() || to == spec.nodes.size()) {
            result.reason = "unknown-edge";
            return result;
        }
        graph.connect(nodes[from], nodes[to]);
    }

    std::vector<bool> active(spec.nodes.size(), false);
    size_t activeCount = 0;
    for (int stage = 0; stage < CROP_STAGE_COUNT; ++stage) {
        const size_t index = indexOf(CROP_STAGE_IDS[stage]);
        if (index == spec.nodes.size()) {
            result.reason = std::string("unknown-node:")
                            + CROP_STAGE_IDS[stage];
            return result;
        }
        if (runs[stage]) {
            active[index] = true;
            ++activeCount;
            /*
             * The execution list must be part of the dirty set; otherwise the
             * graph would be asked to run a block it believes is current.
             */
            if (dirty.find(CROP_STAGE_IDS[stage]) == dirty.end()) {
                result.reason = std::string("dirty-missing:")
                                + CROP_STAGE_IDS[stage];
                return result;
            }
        }
    }
    for (const std::string& id : dirty) {
        if (indexOf(id) == spec.nodes.size()) {
            result.reason = "unknown-dirty-node:" + id;
            return result;
        }
    }

    /*
     * Dirtiness comes from the chosen dirty set, not from the execution list.
     * The two normally agree on the stage-owned ids; the dirty set also names
     * the folded nodes, which are marked dirty so the plan sees the raw core as
     * invalid even though the crop carries them inside source.select.
     */
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        if (dirty.find(spec.nodes[i].id) != dirty.end()) {
            graph.markDirty(nodes[i]);
        } else {
            graph.markClean(nodes[i]);
        }
    }

    std::vector<size_t> order;
    if (!graph.topologicalOrder(order) || order.size() != spec.nodes.size()) {
        result.reason = "cycle";
        return result;
    }

    const std::vector<StageDecision> plan = graph.plan();
    if (plan.size() != spec.nodes.size()) {
        result.reason = "incomplete-plan";
        return result;
    }

    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        if (active[i] && plan[i].action == StageAction::ReuseOutput) {
            result.reason = spec.nodes[i].id;
            return result;
        }
    }

    for (int stage = 0; stage < CROP_STAGE_COUNT; ++stage) {
        if (runs[stage]) {
            result.execute.push_back(static_cast<CropStage>(stage));
        }
    }
    if (result.execute.size() != activeCount) {
        result.reason = "missing-node";
        return result;
    }

    result.valid = true;
    return result;
}

/* One diagnostic line per process, so a log stays readable. */
void logCropGraph(const CropGraphPlan& plan)
{
    static std::mutex mutex;
    static bool logged = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (logged) {
        return;
    }
    logged = true;

    std::fprintf(stderr,
                 "crop-graph: active, %zu canonical nodes executed, %zu "
                 "folded: ", plan.execute.size(), CROP_FOLDED_COUNT);
    for (CropStage stage : plan.execute) {
        std::fprintf(stderr, "%s ", CROP_STAGE_IDS[stage]);
    }
    std::fprintf(stderr, "| folded:");
    for (size_t i = 0; i < CROP_FOLDED_COUNT; ++i) {
        std::fprintf(stderr, " %s", CROP_FOLDED_NODES[i]);
    }
    std::fprintf(stderr, "\n");
}

/*
 * Report a rejected crop plan, and stop.
 *
 * The graph is the only crop sequence, so a plan that would skip a scheduled
 * block is a programming error: aborting keeps it visible instead of
 * publishing a partly processed viewport.
 */
[[noreturn]] void abortCropGraph(const CropGraphPlan& plan)
{
    std::fprintf(stderr, "crop-graph: fatal: plan rejected: %s\n",
                 plan.reason.c_str());
    std::fprintf(stderr,
                 "crop-graph: the graph is the only crop sequence, so there is "
                 "nothing to fall back to\n");
    std::abort();
}

std::atomic<unsigned> cropGraphRuns{0};
/*
 * Rejected plans. A rejection now aborts the process, so a passing run always
 * reads zero; the counter and its accessor remain for the crop harness, which
 * fails the gate if either the graph did not run or a plan was rejected.
 */
std::atomic<unsigned> cropGraphFallbacks{0};

std::atomic<unsigned> cropDirtyChecks{0};
std::atomic<unsigned> cropDirtyEquivalent{0};
std::atomic<unsigned> cropDirtyMissing{0};
std::atomic<unsigned> cropDirtyExtra{0};
std::atomic<unsigned> cropDirtyFallbacks{0};

/*
 * One diagnostic line per mode and mismatch signature.
 *
 * A clean comparison is logged once per mode, so a long run does not repeat it
 * per keystroke. A mismatch logs the two block sets once per distinct
 * signature: the missing set is the one that must be empty before the derived
 * set can be trusted, and the extra set is the documented over-approximation.
 */
void logCropDirty(CropDirtyMode mode,
                  const rtengine::stage::DirtyBlocks& derived,
                  const rtengine::stage::DirtyShadow& shadow,
                  bool executedDerived)
{
    static std::mutex mutex;
    static std::set<std::string> logged;

    std::lock_guard<std::mutex> lock(mutex);

    const std::string signature =
        std::string(cropDirtyModeName(mode)) + "|"
        + rtengine::stage::format_block_ids(shadow.missing) + "|"
        + rtengine::stage::format_block_ids(shadow.extra) + "|"
        + (executedDerived ? "derived" : "legacy");
    if (!logged.insert(signature).second) {
        return;
    }

    std::fprintf(stderr,
                 "crop-dirty: mode=%s executed=%s derived=%s forced=%d "
                 "params=%s state=%s groups=%s closure=%s\n",
                 cropDirtyModeName(mode),
                 executedDerived ? "derived" : "legacy",
                 rtengine::stage::format_block_ids(derived.blocks).c_str(),
                 derived.forced ? 1 : 0,
                 rtengine::stage::format_block_ids(derived.fromParams).c_str(),
                 rtengine::stage::format_block_ids(derived.fromState).c_str(),
                 rtengine::stage::format_block_ids(derived.fromGroups).c_str(),
                 rtengine::stage::format_block_ids(derived.fromClosure).c_str());
    if (shadow.equivalent() && shadow.extra.empty()) {
        std::fprintf(stderr, "crop-dirty: shadow equivalent\n");
        return;
    }
    std::fprintf(stderr,
                 "crop-dirty: shadow missing=%s extra=%s\n",
                 rtengine::stage::format_block_ids(shadow.missing).c_str(),
                 rtengine::stage::format_block_ids(shadow.extra).c_str());
}

} // namespace

namespace crop_graph
{

unsigned runs()
{
    return cropGraphRuns.load();
}

unsigned fallbacks()
{
    return cropGraphFallbacks.load();
}

unsigned dirtyChecks()
{
    return cropDirtyChecks.load();
}

unsigned dirtyEquivalent()
{
    return cropDirtyEquivalent.load();
}

unsigned dirtyMissing()
{
    return cropDirtyMissing.load();
}

unsigned dirtyExtra()
{
    return cropDirtyExtra.load();
}

unsigned dirtyFallbacks()
{
    return cropDirtyFallbacks.load();
}

} // namespace crop_graph

void Crop::update(int todo)
{
    MyMutex::MyLock cropLock(cropMutex);

    ProcParams& params = *parent->params;
//       CropGUIListener* cropgl;

    // No need to update todo here, since it has already been changed in ImprocCoordinator::updatePreviewImage,
    // and Crop::update ask to do ALL anyway

    // give possibility to the listener to modify crop window (as the full image dimensions are already known at this point)
    int wx, wy, ww, wh, ws;
    const bool overrideWindow = cropImageListener;

    if (overrideWindow) {
        cropImageListener->getWindow(wx, wy, ww, wh, ws);
    }

    // re-allocate sub-images and arrays if their dimensions changed
    bool needsinitupdate = false;

    if (!overrideWindow) {
        needsinitupdate = setCropSizes(rqcropx, rqcropy, rqcropw, rqcroph, skip, true);
    } else {
        needsinitupdate = setCropSizes(wx, wy, ww, wh, ws, true);     // this set skip=ws
    }

    // it something has been reallocated, all processing steps have to be performed
    if (needsinitupdate || (todo & M_HIGHQUAL)) {
        todo = ALL;
    }

    // Tells to the ImProcFunctions' tool what is the preview scale, which may lead to some simplifications
    parent->ipf.setScale(skip);

    Imagefloat* baseCrop = origCrop;
    int widIm = parent->fw;//full image
    int heiIm = parent->fh;


    /*
     * The blocks below are shared by both sequences: the legacy path runs every
     * block in order, and the opt-in graph path asks StageGraph::plan() which
     * of them this invocation may run. Both call the same methods, so the graph
     * cannot change an arithmetic step or an ordering.
     */
    CropStageContext ctx(*parent->params, todo, baseCrop);
    ctx.needsInitUpdate = needsinitupdate;
    ctx.widIm = widIm;
    ctx.heiIm = heiIm;

    /*
     * What the legacy mask-derived gates execute for this request. A folded
     * node is not listed: it is carried by the block that owns its
     * implementation. This stays the fail-closed set: it is what the graph ran
     * before the dirty-block derivation existed, and what it still runs when
     * the derivation would skip a block.
     */
    bool runsMask[CROP_STAGE_COUNT] = {};
    runsMask[CROP_SOURCE_SELECT] = (todo & (M_INIT | M_LINDENOISE | M_HDR)) != 0;
    runsMask[CROP_ANALYSIS] = runsMask[CROP_SOURCE_SELECT];
    runsMask[CROP_RAW_BASE] = true;
    runsMask[CROP_TRANSFORM] = true;
    runsMask[CROP_LOCAL_ADJUSTMENTS] = (todo & (M_AUTOEXP | M_RGBCURVE)) != 0
                                       && params.locallab.enabled
                                       && !params.locallab.spots.empty();
    runsMask[CROP_RGB_PROC] = (todo & M_RGBCURVE) != 0;
    const bool labRuns = (todo & (M_LUMINANCE + M_COLOR)) != 0;
    runsMask[CROP_LAB_TOOLS] = labRuns;
    runsMask[CROP_LAB_SHARPENING] = labRuns;
    runsMask[CROP_LAB_WAVELET] = labRuns;
    runsMask[CROP_WORKING_TRC] = labRuns;
    runsMask[CROP_CIE_CAM] = labRuns;
    runsMask[CROP_GAMUT] = labRuns;
    runsMask[CROP_VIEWPORT_OUTPUT] = true;

    /*
     * The dirty-block set the graph will use, and the execution list it
     * implies. The mask-derived list stays available as the fail-closed
     * answer, so the two are computed together and compared in every mode that
     * derives.
     */
    bool runs[CROP_STAGE_COUNT];
    std::copy(std::begin(runsMask), std::end(runsMask), std::begin(runs));
    std::vector<std::string> graphDirty = cropStageOrder(cropBlocksOf(runsMask));

    /*
     * Render state of one crop pass.
     *
     * The crop owns the ROI geometry and reads the coordinator's source and
     * display state, so this is the complete set of inputs that are not in
     * ProcParams. It is built once before the derivation and once after the
     * pass, because the stage bodies can update the source generation and the
     * high-detail flags while they run.
     */
    const auto cropRenderState = [this, &todo]() {
        rtengine::stage::RenderStateKey state;
        state.previewScale = skip;
        state.previewWidth = cropw;
        state.previewHeight = croph;
        state.fullWidth = parent->fw;
        state.fullHeight = parent->fh;
        state.cropX = rqcropx;
        state.cropY = rqcropy;
        state.cropWidth = rqcropw;
        state.cropHeight = rqcroph;
        state.cropSkip = skip;
        state.borderRequested = borderRequested;
        state.borderLeft = leftBorder;
        state.borderTop = upperBorder;
        /*
         * The high-detail decision is the coordinator's, but it decides which
         * raw pixels this crop receives, so it is part of the crop state.
         */
        state.highDetailNeeded = parent->lastHighDetailNeeded;
        state.highDetailPreprocessComputed =
            parent->highDetailPreprocessComputed;
        state.highDetailRawComputed = parent->highDetailRawComputed;
        state.highDetailStateKnown = parent->highDetailStateKnown;
        state.sourceGeneration = parent->imgsrc
                                     ? parent->imgsrc->getSourceGeneration()
                                     : 0;
        state.rgbSourceModified = parent->imgsrc
                                      ? parent->imgsrc->isRGBSourceModified()
                                      : false;
        state.monitorProfile = parent->monitorProfile.raw();
        state.monitorIntent = static_cast<int32_t>(parent->monitorIntent);
        state.softProof = parent->softProof;
        state.gamutCheck = parent->gamutCheck;
        state.sharpMask = parent->sharpMask;
        /*
         * The fast-preview and main-preview switches are preview request
         * state; the crop viewport does not consult them.
         *
         * The request kind is the one thing the crop takes from the legacy
         * mask rather than from ProcParams: M_INIT, M_LINDENOISE and M_HDR ask
         * for the destructive raw stage to be reloaded, which is a request,
         * not a parameter delta. It is carried as FullCrop so the derivation
         * treats the path as a rebuild instead of silently skipping the
         * reload.
         */
        const bool reloadRequested = (todo & (M_INIT | M_LINDENOISE | M_HDR)) != 0;
        state.requestKind = (todo & M_HIGHQUAL) != 0
                                ? rtengine::stage::RequestKind::HighQuality
                                : reloadRequested
                                      ? rtengine::stage::RequestKind::FullCrop
                                      : todo == M_MINUPDATE
                                            ? rtengine::stage::RequestKind::RepaintOnly
                                            : rtengine::stage::RequestKind::ParamsOnly;
        return state;
    };

    const CropDirtyMode dirtyMode = cropDirtyMode();
    if (dirtyMode != CropDirtyMode::Legacy) {
        /*
         * The derivation reads the parameters before any stage body can touch
         * them, and the render state the crop itself owns. The fingerprint
         * vector is kept as the next update's history instead of a ProcParams
         * copy; it is stored again after the pass, because several stage bodies
         * write corrected values back into the parameters (auto white balance,
         * automatic denoise) and those corrections are not new inputs on the
         * next update.
         */
        const rtengine::stage::BlockFingerprints current =
            rtengine::stage::stage_fingerprints(
                rtengine::stage::StagePath::Crop, params);
        const rtengine::stage::RenderStateKey state = cropRenderState();

        if (!dirtyState) {
            dirtyState.reset(new CropDirtyState());
        }

        rtengine::stage::DirtyBlockInputs inputs;
        inputs.path = rtengine::stage::StagePath::Crop;
        inputs.currentFingerprints = &current;
        inputs.previousFingerprints =
            dirtyState->valid ? &dirtyState->fingerprints : nullptr;
        inputs.previousState = dirtyState->valid ? &dirtyState->renderState
                                                 : nullptr;
        inputs.currentState = &state;
        inputs.forceFull = needsinitupdate || (todo & M_HIGHQUAL) != 0;

        const rtengine::stage::DirtyBlocks derived =
            rtengine::stage::derive_dirty_blocks(inputs);

        if (g_getenv("RT_CROP_DIRTY_TRACE") != nullptr) {
            std::fprintf(stderr,
                         "crop-dirty-trace: todo=0x%x init=%d forced=%d "
                         "wb=%d sharp=%d cie=%d spots=%zu crop=%d "
                         "params=%s\n",
                         todo, needsinitupdate ? 1 : 0, derived.forced ? 1 : 0,
                         params.wb.temperature, params.sharpening.amount,
                         params.colorappearance.enabled ? 1 : 0,
                         params.locallab.spots.size(),
                         params.crop.enabled ? 1 : 0,
                         rtengine::stage::format_block_ids(derived.fromParams)
                             .c_str());
        }

        dirtyState->fingerprints = current;
        dirtyState->renderState = state;
        dirtyState->valid = true;

        const std::vector<std::string> legacy = graphDirty;
        const std::vector<std::string> stageDerived =
            cropStageOrder(derived.blocks);
        const rtengine::stage::DirtyShadow shadow =
            rtengine::stage::compare_dirty_shadow(legacy, stageDerived);

        ++cropDirtyChecks;
        if (shadow.equivalent()) {
            ++cropDirtyEquivalent;
        }
        cropDirtyMissing += static_cast<unsigned>(shadow.missing.size());
        cropDirtyExtra += static_cast<unsigned>(shadow.extra.size());

        const bool executeDerived =
            dirtyMode == CropDirtyMode::Fingerprint && shadow.equivalent();
        if (dirtyMode == CropDirtyMode::Fingerprint && !executeDerived) {
            ++cropDirtyFallbacks;
        }
        logCropDirty(dirtyMode, derived, shadow, executeDerived);

        if (executeDerived) {
            graphDirty = derived.blocks;
            cropRunsOf(cropStageOrder(derived.blocks), runs);
        }
    }

    const auto runStage = [this, &ctx](CropStage stage) {
        switch (stage) {
        case CROP_SOURCE_SELECT:
            stageSourceSelect(ctx);
            break;
        case CROP_ANALYSIS:
            stageAnalysis(ctx);
            break;
        case CROP_RAW_BASE:
            stageRawBase(ctx);
            break;
        case CROP_TRANSFORM:
            stageTransform(ctx);
            break;
        case CROP_LOCAL_ADJUSTMENTS:
            stageLocalAdjustments(ctx);
            break;
        case CROP_RGB_PROC:
            stageRgbProc(ctx);
            break;
        case CROP_LAB_TOOLS:
            stageLabTools(ctx);
            break;
        case CROP_LAB_SHARPENING:
            stageLabSharpening(ctx);
            break;
        case CROP_LAB_WAVELET:
            stageLabWavelet(ctx);
            break;
        case CROP_WORKING_TRC:
            stageWorkingTrc(ctx);
            break;
        case CROP_CIE_CAM:
            stageCieCam(ctx);
            break;
        case CROP_GAMUT:
            stageGamut(ctx);
            break;
        case CROP_VIEWPORT_OUTPUT:
            stageViewportOutput(ctx);
            break;
        case CROP_STAGE_COUNT:
            break;
        }
    };

    const CropGraphPlan plan = planCropGraph(graphDirty, runs);
    if (!plan.valid) {
        ++cropGraphFallbacks;
        abortCropGraph(plan);
    }
    logCropGraph(plan);

    ++cropGraphRuns;
    for (CropStage stage : plan.execute) {
        runStage(stage);
    }

    if (cropImageListener) {
        // Computing the internal image for analysis, i.e. conversion from lab->Output profile (rtSettings.HistogramWorking disabled) or lab->WCS (rtSettings.HistogramWorking enabled)

        // internal image in output color space for analysis
        Image8 *cropImgtrue = parent->ipf.lab2rgb(labnCrop, 0, 0, cropw, croph, params.icm);

        int finalW = rqcropw;

        if (cropImg->getWidth() - leftBorder < finalW) {
            finalW = cropImg->getWidth() - leftBorder;
        }

        int finalH = rqcroph;

        if (cropImg->getHeight() - upperBorder < finalH) {
            finalH = cropImg->getHeight() - upperBorder;
        }

        Image8* final = new Image8(finalW, finalH);
        Image8* finaltrue = new Image8(finalW, finalH);

        for (int i = 0; i < finalH; i++) {
            memcpy(final->data + 3 * i * finalW, cropImg->data + 3 * (i + upperBorder)*cropw + 3 * leftBorder, 3 * finalW);
            memcpy(finaltrue->data + 3 * i * finalW, cropImgtrue->data + 3 * (i + upperBorder)*cropw + 3 * leftBorder, 3 * finalW);
        }

        cropImageListener->setDetailedCrop(
            final, finaltrue, params.icm, params.crop, params.cropGuide,
            rqcropx, rqcropy, rqcropw, rqcroph, skip);
        delete final;
        delete finaltrue;
        delete cropImgtrue;
    }

    /*
     * Store the inputs as this pass left them.
     *
     * Several stage bodies write corrected values back into the parameters -
     * auto white balance replaces the temperature, the automatic denoise
     * method replaces its strengths, auto exposure replaces the curve - and a
     * source mutation bumps the generation. Those corrections were consumed by
     * this pass, so they are part of the state the buffers hold and must not
     * read as new inputs on the next update.
     */
    if (dirtyMode != CropDirtyMode::Legacy && dirtyState) {
        dirtyState->fingerprints = rtengine::stage::stage_fingerprints(
            rtengine::stage::StagePath::Crop, params);
        dirtyState->renderState = cropRenderState();
        dirtyState->valid = true;
    }
}

void Crop::stageSourceSelect(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    int& tr = ctx.tr;
    int& widIm = ctx.widIm;
    int& heiIm = ctx.heiIm;
    float& autoNR = ctx.autoNR;
    float& autoNRmax = ctx.autoNRmax;
    NoiseCurve& noiseLCurve = ctx.noiseLCurve;
    NoiseCurve& noiseCCurve = ctx.noiseCCurve;
    int& numtiles_W = ctx.numtiles_W;
    int& numtiles_H = ctx.numtiles_H;
    int& tileWskip = ctx.tileWskip;
    int& tileHskip = ctx.tileHskip;
    float*& min_b = ctx.min_b;
    float*& min_r = ctx.min_r;
    float*& lumL = ctx.lumL;
    float*& chromC = ctx.chromC;
    float*& ry = ctx.ry;
    float*& sk = ctx.sk;
    float*& pcsk = ctx.pcsk;
    int*& centerTile_X = ctx.centerTile_X;
    int*& centerTile_Y = ctx.centerTile_Y;
    const bool needsinitupdate = ctx.needsInitUpdate;

    if ((todo & (M_INIT | M_LINDENOISE | M_HDR)) == 0) {
        return;
    }
    tr = getCoarseBitMask(params.coarse);

    if (!needsinitupdate) {
        setCropSizes(rqcropx, rqcropy, rqcropw, rqcroph, skip, true);
    }


    //       printf("x=%d y=%d crow=%d croh=%d skip=%d\n",rqcropx, rqcropy, rqcropw, rqcroph, skip);
    //      printf("trafx=%d trafyy=%d trafwsk=%d trafHs=%d \n",trafx, trafy, trafw*skip, trafh*skip);

    autoNR = (float) settings->nrauto;//
    autoNRmax = (float) settings->nrautomax;//

    params.dirpyrDenoise.getCurves(noiseLCurve, noiseCCurve);

    const int tilesize = settings->leveldnti == 0 ? 1024 : 768;
    const int overlap = settings->leveldnti == 0 ? 128 : 96;

    int tilewidth, tileheight;

    parent->ipf.Tile_calc(tilesize, overlap, 2, widIm, heiIm, numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip);

    min_b = new float[9];
    min_r = new float[9];
    lumL = new float[9];
    chromC = new float[9];
    ry = new float[9];
    sk = new float[9];
    pcsk = new float[9];
    centerTile_X = new int[numtiles_W];
    centerTile_Y = new int[numtiles_H];

    for (int cX = 0; cX < numtiles_W; cX++) {
        centerTile_X[cX] = tileWskip / 2 + tileWskip * cX;
    }

    for (int cY = 0; cY < numtiles_H; cY++) {
        centerTile_Y[cY] = tileHskip / 2 + tileHskip * cY;
    }

    if (settings->leveldnautsimpl == 1) {
        if (params.dirpyrDenoise.Cmethod == "MAN" || params.dirpyrDenoise.Cmethod == "PON")  {
            PreviewProps pp(trafx, trafy, trafw * skip, trafh * skip, skip);
            parent->imgsrc->getImage(parent->currWB, tr, origCrop, pp, params.toneCurve, params.raw);
        }
    } else {
        if (params.dirpyrDenoise.C2method == "MANU")  {
            PreviewProps pp(trafx, trafy, trafw * skip, trafh * skip, skip);
            parent->imgsrc->getImage(parent->currWB, tr, origCrop, pp, params.toneCurve, params.raw);
        }
    }
}

void Crop::stageAnalysis(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    int& tr = ctx.tr;
    int& widIm = ctx.widIm;
    int& heiIm = ctx.heiIm;
    float& autoNR = ctx.autoNR;
    float& autoNRmax = ctx.autoNRmax;
    int& numtiles_W = ctx.numtiles_W;
    int& numtiles_H = ctx.numtiles_H;
    int& tileWskip = ctx.tileWskip;
    int& tileHskip = ctx.tileHskip;
    float*& min_b = ctx.min_b;
    float*& min_r = ctx.min_r;
    float*& lumL = ctx.lumL;
    float*& chromC = ctx.chromC;
    float*& ry = ctx.ry;
    float*& sk = ctx.sk;
    float*& pcsk = ctx.pcsk;
    int*& centerTile_X = ctx.centerTile_X;
    int*& centerTile_Y = ctx.centerTile_Y;

    if ((todo & (M_INIT | M_LINDENOISE | M_HDR)) == 0) {
        return;
    }
    if ((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "PRE") || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "PREV")) {
        PreviewProps pp(trafx, trafy, trafw * skip, trafh * skip, skip);
        parent->imgsrc->getImage(parent->currWB, tr, origCrop, pp, params.toneCurve, params.raw);

        if ((!isDetailWindow) && parent->adnListener && skip == 1 && params.dirpyrDenoise.enabled) {
            float lowdenoise = 1.f;
            int levaut = settings->leveldnaut;

            if (levaut == 1) { //Standard
                lowdenoise = 0.7f;
            }

            int CenterPreview_X = trafx + (trafw * skip) / 2;
            int CenterPreview_Y = trafy + (trafh * skip) / 2;
            int minimuX = 20000;
            int minimuY = 20000;
            int poscenterX = 0;
            int poscenterY = 0;

            for (int cc = 0; cc < numtiles_W; cc++) {
                if (abs(centerTile_X[cc] - CenterPreview_X) < minimuX) {
                    minimuX = abs(centerTile_X[cc] - CenterPreview_X);
                    poscenterX = cc;
                }
            }

            for (int cc = 0; cc < numtiles_H; cc++) {
                if (abs(centerTile_Y[cc] - CenterPreview_Y) < minimuY) {
                    minimuY = abs(centerTile_Y[cc] - CenterPreview_Y);
                    poscenterY = cc;
                }
            }

            //  printf("TileCX=%d  TileCY=%d  prevX=%d  prevY=%d \n",centerTile_X[poscenterX],centerTile_Y[poscenterY],CenterPreview_X,CenterPreview_Y);
            int crW;

            if (settings->leveldnv == 0) {
                crW = 100;
            }

            if (settings->leveldnv == 1) {
                crW = 250;
            }

            //  if (settings->leveldnv ==2) {crW=int(tileWskip/2);crH=int((tileWskip/2));}//adapted to scale of preview
            if (settings->leveldnv == 2) {
                crW = int (tileWskip / 2);
            }

            if (settings->leveldnv == 3) {
                crW = tileWskip - 10;
            }

            float adjustr = 1.f;

            if (params.icm.workingProfile == "ProPhoto")   {
                adjustr = 1.f;
            } else if (params.icm.workingProfile == "Adobe RGB")  {
                adjustr = 1.f / 1.3f;
            } else if (params.icm.workingProfile == "sRGB")       {
                adjustr = 1.f / 1.3f;
            } else if (params.icm.workingProfile == "WideGamut")  {
                adjustr = 1.f / 1.1f;
            } else if (params.icm.workingProfile == "Beta RGB")   {
                adjustr = 1.f / 1.2f;
            } else if (params.icm.workingProfile == "BestRGB")    {
                adjustr = 1.f / 1.2f;
            } else if (params.icm.workingProfile == "BruceRGB")   {
                adjustr = 1.f / 1.2f;
            }

            if (parent->adnListener) {
                parent->adnListener->noiseTilePrev(centerTile_X[poscenterX], centerTile_Y[poscenterY], CenterPreview_X, CenterPreview_Y, crW, trafw * skip);
            }

            // I have tried "blind" some solutions..to move review ...but GUI is not my truc !
            //  int W,H;
            //  cropgl->cropMoved (centerTile_X[poscenterX],centerTile_Y[poscenterY] , W, H);
            //   cropImageListener->setPosition (int x, int y, bool update=true);
            //   bool update;
            //   cropImageListener->setPosition (centerTile_X[poscenterX],centerTile_Y[poscenterY] , true);
            //setCropSizes (centerTile_X[poscenterX], centerTile_Y[poscenterY], trafw*skip,trafh*skip , skip, true);

            // we only need image reduced to 1/4 here
            int W = origCrop->getWidth();
            int H = origCrop->getHeight();
            Imagefloat *provicalc = new Imagefloat((W + 1) / 2, (H + 1) / 2);  //for denoise curves

            for (int ii = 0; ii < H; ii += 2) {
                for (int jj = 0; jj < W; jj += 2) {
                    provicalc->r(ii >> 1, jj >> 1) = origCrop->r(ii, jj);
                    provicalc->g(ii >> 1, jj >> 1) = origCrop->g(ii, jj);
                    provicalc->b(ii >> 1, jj >> 1) = origCrop->b(ii, jj);
                }
            }

            parent->imgsrc->convertColorSpace(provicalc, params.icm, parent->currWB);  //for denoise luminance curve

            float maxr = 0.f;
            float maxb = 0.f;
            float chaut, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc;
            int Nb;

            chaut = 0.f;
            redaut = 0.f;
            blueaut = 0.f;
            maxredaut = 0.f;
            maxblueaut = 0.f;
            minredaut = 0.f;
            minblueaut = 0.f;
            LUTf gamcurve(65536, 0);
            float gam, gamthresh, gamslope;
            parent->ipf.RGB_denoise_infoGamCurve(params.dirpyrDenoise, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope);
            parent->ipf.RGB_denoise_info(origCrop, provicalc, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope, params.dirpyrDenoise, parent->imgsrc->getDirPyrDenoiseExpComp(), chaut, Nb, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc, true);
//                  printf("redy=%f skin=%f pcskin=%f\n",redyel, skinc,nsknc);
//                  printf("DCROP skip=%d cha=%4.0f Nb=%d red=%4.0f bl=%4.0f redM=%4.0f bluM=%4.0f  L=%4.0f sigL=%4.0f Ch=%4.0f Si=%4.0f\n",skip, chaut,Nb, redaut,blueaut, maxredaut, maxblueaut, lumema, sigma_L, chromina, sigma);
            float multip = 1.f;

            if (!parent->imgsrc->isRAW()) {
                multip = 2.f;    //take into account gamma for TIF / JPG approximate value...not good for gamma=1
            }

            float maxmax = max(maxredaut, maxblueaut);
            float delta;
            int mode = 0;
            //  float redyel, skinc, nsknc;
            int lissage = settings->leveldnliss;
            parent->ipf.calcautodn_info(chaut, delta, Nb, levaut, maxmax, lumema, chromina, mode, lissage, redyel, skinc, nsknc);


            if (maxredaut > maxblueaut) {
                //  maxr=(maxredaut-chaut)/((autoNRmax*multip*adjustr)/2.f);
                maxr = (delta) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);

                if (minblueaut <= minredaut  && minblueaut < chaut) {
                    maxb = (-chaut + minblueaut) / (autoNRmax * multip * adjustr * lowdenoise);
                }
            } else {
                //  maxb=(maxblueaut-chaut)/((autoNRmax*multip*adjustr)/2.f);
                maxb = (delta) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);

                if (minredaut <= minblueaut  && minredaut < chaut) {
                    maxr = (-chaut + minredaut) / (autoNRmax * multip * adjustr * lowdenoise);
                }
            }//maxb mxr - empirical evaluation red / blue


            params.dirpyrDenoise.chroma = chaut / (autoNR * multip * adjustr * lowdenoise);
            params.dirpyrDenoise.redchro = maxr;
            params.dirpyrDenoise.bluechro = maxb;
            parent->adnListener->chromaChanged(params.dirpyrDenoise.chroma, params.dirpyrDenoise.redchro, params.dirpyrDenoise.bluechro);

            delete provicalc;
        }
    }

    if (skip == 1 && params.dirpyrDenoise.enabled && !parent->denoiseInfoStore.valid && ((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "AUT")  || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "AUTO"))) {
        MyTime t1aue, t2aue;
        t1aue.set();

        int crW = 100; // settings->leveldnv == 0
        int crH = 100; // settings->leveldnv == 0

        if (settings->leveldnv == 1) {
            crW = 250;
            crH = 250;
        }

        //  if (settings->leveldnv ==2) {crW=int(tileWskip/2);crH=int((tileWskip/2));}//adapted to scale of preview
        if (settings->leveldnv == 2) {
            crW = int (tileWskip / 2);
            crH = int (tileHskip / 2);
        }

        if (settings->leveldnv == 3) {
            crW = tileWskip - 10;
            crH = tileHskip - 10;
        }

        float lowdenoise = 1.f;
        int levaut = settings->leveldnaut;

        if (levaut == 1) { //Standard
            lowdenoise = 0.7f;
        }

        LUTf gamcurve(65536, 0);
        float gam, gamthresh, gamslope;
        parent->ipf.RGB_denoise_infoGamCurve(params.dirpyrDenoise, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope);
        int Nb[9];
#ifdef _OPENMP
        #pragma omp parallel
#endif
        {
            Imagefloat *origCropPart = new Imagefloat(crW, crH); //allocate memory
            Imagefloat *provicalc = new Imagefloat((crW + 1) / 2, (crH + 1) / 2);  //for denoise curves

            int  coordW[3];//coordinate of part of image to measure noise
            int  coordH[3];
            int begW = 50;
            int begH = 50;
            coordW[0] = begW;
            coordW[1] = widIm / 2 - crW / 2;
            coordW[2] = widIm - crW - begW;
            coordH[0] = begH;
            coordH[1] = heiIm / 2 - crH / 2;
            coordH[2] = heiIm - crH - begH;
#ifdef _OPENMP
            #pragma omp for schedule(dynamic) collapse(2) nowait
#endif

            for (int wcr = 0; wcr <= 2; wcr++) {
                for (int hcr = 0; hcr <= 2; hcr++) {
                    PreviewProps ppP(coordW[wcr], coordH[hcr], crW, crH, 1);
                    parent->imgsrc->getImage(parent->currWB, tr, origCropPart, ppP, params.toneCurve, params.raw);

                    // we only need image reduced to 1/4 here
                    for (int ii = 0; ii < crH; ii += 2) {
                        for (int jj = 0; jj < crW; jj += 2) {
                            provicalc->r(ii >> 1, jj >> 1) = origCropPart->r(ii, jj);
                            provicalc->g(ii >> 1, jj >> 1) = origCropPart->g(ii, jj);
                            provicalc->b(ii >> 1, jj >> 1) = origCropPart->b(ii, jj);
                        }
                    }

                    parent->imgsrc->convertColorSpace(provicalc, params.icm, parent->currWB);  //for denoise luminance curve

                    float pondcorrec = 1.0f;
                    float chaut = 0.f, redaut = 0.f, blueaut = 0.f, maxredaut = 0.f, maxblueaut = 0.f, minredaut = 0.f, minblueaut = 0.f, chromina = 0.f, sigma = 0.f, lumema = 0.f, sigma_L = 0.f, redyel = 0.f, skinc = 0.f, nsknc = 0.f;
                    int nb = 0;
                    parent->ipf.RGB_denoise_info(origCropPart, provicalc, parent->imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope, params.dirpyrDenoise, parent->imgsrc->getDirPyrDenoiseExpComp(), chaut, nb, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc);

                    //printf("DCROP skip=%d cha=%f red=%f bl=%f redM=%f bluM=%f chrom=%f sigm=%f lum=%f\n",skip, chaut,redaut,blueaut, maxredaut, maxblueaut, chromina, sigma, lumema);
                    Nb[hcr * 3 + wcr] = nb;
                    parent->denoiseInfoStore.ch_M[hcr * 3 + wcr] = pondcorrec * chaut;
                    parent->denoiseInfoStore.max_r[hcr * 3 + wcr] = pondcorrec * maxredaut;
                    parent->denoiseInfoStore.max_b[hcr * 3 + wcr] = pondcorrec * maxblueaut;
                    min_r[hcr * 3 + wcr] = pondcorrec * minredaut;
                    min_b[hcr * 3 + wcr] = pondcorrec * minblueaut;
                    lumL[hcr * 3 + wcr] = lumema;
                    chromC[hcr * 3 + wcr] = chromina;
                    ry[hcr * 3 + wcr] = redyel;
                    sk[hcr * 3 + wcr] = skinc;
                    pcsk[hcr * 3 + wcr] = nsknc;

                }
            }

            delete provicalc;
            delete origCropPart;
        }
        float chM = 0.f;
        float MaxR = 0.f;
        float MaxB = 0.f;
        float MinR = 100000000000.f;
        float MinB = 100000000000.f;
        float maxr = 0.f;
        float maxb = 0.f;
        float Max_R[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        float Max_B[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
        float Min_R[9];
        float Min_B[9];
        float MaxRMoy = 0.f;
        float MaxBMoy = 0.f;
        float MinRMoy = 0.f;
        float MinBMoy = 0.f;

        float multip = 1.f;

        if (!parent->imgsrc->isRAW()) {
            multip = 2.f;    //take into account gamma for TIF / JPG approximate value...not good for gamma=1
        }

        float adjustr = 1.f;

        if (params.icm.workingProfile == "ProPhoto")   {
            adjustr = 1.f;   //
        } else if (params.icm.workingProfile == "Adobe RGB")  {
            adjustr = 1.f / 1.3f;
        } else if (params.icm.workingProfile == "sRGB")       {
            adjustr = 1.f / 1.3f;
        } else if (params.icm.workingProfile == "WideGamut")  {
            adjustr = 1.f / 1.1f;
        } else if (params.icm.workingProfile == "Beta RGB")   {
            adjustr = 1.f / 1.2f;
        } else if (params.icm.workingProfile == "BestRGB")    {
            adjustr = 1.f / 1.2f;
        } else if (params.icm.workingProfile == "BruceRGB")   {
            adjustr = 1.f / 1.2f;
        }

        float delta[9];
        int mode = 1;
        int lissage = settings->leveldnliss;

        for (int k = 0; k < 9; k++) {
            float maxmax = max(parent->denoiseInfoStore.max_r[k], parent->denoiseInfoStore.max_b[k]);
            parent->ipf.calcautodn_info(parent->denoiseInfoStore.ch_M[k], delta[k], Nb[k], levaut, maxmax, lumL[k], chromC[k], mode, lissage, ry[k], sk[k], pcsk[k]);
            //  printf("ch_M=%f delta=%f\n",ch_M[k], delta[k]);
        }

        for (int k = 0; k < 9; k++) {
            if (parent->denoiseInfoStore.max_r[k] > parent->denoiseInfoStore.max_b[k]) {
                Max_R[k] = (delta[k]) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                Min_B[k] = - (parent->denoiseInfoStore.ch_M[k] - min_b[k]) / (autoNRmax * multip * adjustr * lowdenoise);
                Max_B[k] = 0.f;
                Min_R[k] = 0.f;
            } else {
                Max_B[k] = (delta[k]) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                Min_R[k] = - (parent->denoiseInfoStore.ch_M[k] - min_r[k])   / (autoNRmax * multip * adjustr * lowdenoise);
                Min_B[k] = 0.f;
                Max_R[k] = 0.f;
            }
        }

        for (int k = 0; k < 9; k++) {
            //  printf("ch_M= %f Max_R=%f Max_B=%f min_r=%f min_b=%f\n",ch_M[k],Max_R[k], Max_B[k],Min_R[k], Min_B[k]);
            chM += parent->denoiseInfoStore.ch_M[k];
            MaxBMoy += Max_B[k];
            MaxRMoy += Max_R[k];
            MinRMoy += Min_R[k];
            MinBMoy += Min_B[k];

            if (Max_R[k] > MaxR) {
                MaxR = Max_R[k];
            }

            if (Max_B[k] > MaxB) {
                MaxB = Max_B[k];
            }

            if (Min_R[k] < MinR) {
                MinR = Min_R[k];
            }

            if (Min_B[k] < MinB) {
                MinB = Min_B[k];
            }
        }

        chM /= 9;
        MaxBMoy /= 9;
        MaxRMoy /= 9;
        MinBMoy /= 9;
        MinRMoy /= 9;

        if (MaxR > MaxB) {
            maxr = MaxRMoy + (MaxR - MaxRMoy) * 0.66f; //#std Dev
            //maxb=MinB;
            maxb = MinBMoy + (MinB - MinBMoy) * 0.66f;
        } else {
            maxb = MaxBMoy + (MaxB - MaxBMoy) * 0.66f;
            maxr = MinRMoy + (MinR - MinRMoy) * 0.66f;
        }

//                  printf("DCROP skip=%d cha=%f red=%f bl=%f \n",skip, chM,maxr,maxb);
        params.dirpyrDenoise.chroma = chM / (autoNR * multip * adjustr);
        params.dirpyrDenoise.redchro = maxr;
        params.dirpyrDenoise.bluechro = maxb;
        parent->denoiseInfoStore.valid = true;
        if (parent->adnListener) {
            parent->adnListener->chromaChanged(params.dirpyrDenoise.chroma, params.dirpyrDenoise.redchro, params.dirpyrDenoise.bluechro);
        }

        if (settings->verbose) {
            t2aue.set();
            printf("Info denoise auto performed in %d usec:\n", t2aue.etime(t1aue));
        }

        //end evaluate noise
    }
}

void Crop::stageRawBase(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    int& tr = ctx.tr;
    NoiseCurve& noiseLCurve = ctx.noiseLCurve;
    NoiseCurve& noiseCCurve = ctx.noiseCCurve;
    Imagefloat*& baseCrop = ctx.baseCrop;
    Imagefloat* calclum = nullptr;
    bool spotsDone = false;

    if (todo & (M_INIT | M_LINDENOISE | M_HDR)) {
        //  if (params.dirpyrDenoise.Cmethod=="AUT" || params.dirpyrDenoise.Cmethod=="PON") {//reinit origCrop after Auto
        if ((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "AUT")  || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "AUTO")) { //reinit origCrop after Auto
            PreviewProps pp(trafx, trafy, trafw * skip, trafh * skip, skip);
            parent->imgsrc->getImage(parent->currWB, tr, origCrop, pp, params.toneCurve, params.raw);
        }

        if ((todo & M_SPOT) && params.spot.enabled && !params.spot.entries.empty()) {
            spotsDone = true;
            PreviewProps pp(trafx, trafy, trafw * skip, trafh * skip, skip);
            //parent->imgsrc->getImage(parent->currWB, tr, origCrop, pp, params.toneCurve, params.raw);
            parent->ipf.removeSpots(origCrop, parent->imgsrc, params.spot.entries, pp, parent->currWB, nullptr, tr);
        }

        DirPyrDenoiseParams denoiseParams = params.dirpyrDenoise;

        if (params.dirpyrDenoise.Lmethod == "CUR") {
            if (noiseLCurve) {
                denoiseParams.luma = 0.5f;    //very small value to init process - select curve or slider
            } else {
                denoiseParams.luma = 0.0f;
            }
        } else if (denoiseParams.Lmethod == "SLI") {
            noiseLCurve.Reset();
        }

        if ((noiseLCurve || noiseCCurve) && skip == 1 && denoiseParams.enabled)   {  //only allocate memory if enabled and skip
            // we only need image reduced to 1/4 here
            int W = origCrop->getWidth();
            int H = origCrop->getHeight();
            calclum = new Imagefloat((W + 1) / 2, (H + 1) / 2);  //for denoise curves

            for (int ii = 0; ii < H; ii += 2) {
                for (int jj = 0; jj < W; jj += 2) {
                    calclum->r(ii >> 1, jj >> 1) = origCrop->r(ii, jj);
                    calclum->g(ii >> 1, jj >> 1) = origCrop->g(ii, jj);
                    calclum->b(ii >> 1, jj >> 1) = origCrop->b(ii, jj);
                }
            }

            parent->imgsrc->convertColorSpace(calclum, params.icm, parent->currWB);  //for denoise luminance curve
        }

        if (skip != 1) if (parent->adnListener) {
                parent->adnListener->noiseChanged(0.f, 0.f);
            }

        if (todo & (M_INIT | M_LINDENOISE | M_HDR)) {

            if (skip == 1 && denoiseParams.enabled) {

                float nresi, highresi;
                parent->ipf.RGB_denoise(0, origCrop, origCrop, calclum, parent->denoiseInfoStore.ch_M, parent->denoiseInfoStore.max_r, parent->denoiseInfoStore.max_b, parent->imgsrc->isRAW(), /*Roffset,*/ denoiseParams, parent->imgsrc->getDirPyrDenoiseExpComp(), noiseLCurve, noiseCCurve, nresi, highresi);

                if (parent->adnListener) {
                    parent->adnListener->noiseChanged(nresi, highresi);
                }

                if (settings->leveldnautsimpl == 1) {
                    if ((denoiseParams.Cmethod == "AUT" || denoiseParams.Cmethod == "PRE") && (parent->adnListener)) { // force display value of sliders
                        parent->adnListener->chromaChanged(denoiseParams.chroma, denoiseParams.redchro, denoiseParams.bluechro);
                    }
                } else {
                    if ((denoiseParams.C2method == "AUTO" || denoiseParams.C2method == "PREV") && (parent->adnListener)) { // force display value of sliders
                        parent->adnListener->chromaChanged(denoiseParams.chroma, denoiseParams.redchro, denoiseParams.bluechro);
                    }
                }

            }
        }

        if (params.filmNegative.enabled && params.filmNegative.colorSpace == FilmNegativeParams::ColorSpace::INPUT) {
            parent->ipf.filmNegativeProcess(baseCrop, baseCrop, params.filmNegative);
        }

        parent->imgsrc->convertColorSpace(origCrop, params.icm, parent->currWB);

        if (params.filmNegative.enabled && params.filmNegative.colorSpace != FilmNegativeParams::ColorSpace::INPUT) {
            parent->ipf.filmNegativeProcess(baseCrop, baseCrop, params.filmNegative);
        }

        if (params.cg.enabled) {//gamut compression
            float mac = 0.f;
            float mac0 = 0.f;
            float mac1 = 0.f;
            float mac2 = 0.f;
            int beginend = 0;
            int nbsegam = 0;
            float powe = 1.f;
            parent->ipf.gamutcompr(baseCrop, baseCrop, beginend, powe, nbsegam, mac, mac0, mac1, mac2);
        }

        ctx.releaseRawScratch();
    }

    // has to be called after setCropSizes! Tools prior to this point can't handle the Edit mechanism, but that shouldn't be a problem.
    createBuffer(cropw, croph);

    // Apply Spot removal
    if ((todo & M_SPOT) && !spotsDone) {
        if (params.spot.enabled && !params.spot.entries.empty()) {
            if(!spotCrop) {
                spotCrop = new Imagefloat (cropw, croph);
            }
            baseCrop->copyData (spotCrop);
            PreviewProps pp (trafx, trafy, trafw * skip, trafh * skip, skip);
            int tr = getCoarseBitMask(params.coarse);
            parent->ipf.removeSpots (spotCrop, parent->imgsrc, params.spot.entries, pp, parent->currWB, &params.icm, tr);
        } else {
            if (spotCrop) {
                delete spotCrop;
                spotCrop = nullptr;
            }
        }
    }

    if (spotCrop) {
        baseCrop = spotCrop;
    }

    std::unique_ptr<Imagefloat> fattalCrop;

    if ((todo & M_HDR) && (params.fattal.enabled || params.dehaze.enabled)) {
        Imagefloat *f = origCrop;
        int fw = skips(parent->fw, skip);
        int fh = skips(parent->fh, skip);
        bool need_cropping = false;
        bool need_fattal = true;

        if (trafx || trafy || trafw != fw || trafh != fh) {
            need_cropping = true;

            // fattal needs to work on the full image. So here we get the full
            // image from imgsrc, and replace the denoised crop in case
            if (!params.dirpyrDenoise.enabled && skip == 1 && parent->fattal_11_dcrop_cache) {
                f = parent->fattal_11_dcrop_cache;
                need_fattal = false;
            } else {
                f = new Imagefloat(fw, fh);
                fattalCrop.reset(f);
                PreviewProps pp(0, 0, parent->fw, parent->fh, skip);
                int tr = getCoarseBitMask(params.coarse);
                parent->imgsrc->getImage(parent->currWB, tr, f, pp, params.toneCurve, params.raw);
                parent->imgsrc->convertColorSpace(f, params.icm, parent->currWB);

                if (params.dirpyrDenoise.enabled || params.filmNegative.enabled || params.spot.enabled) {
                    // copy the denoised crop
                    int oy = trafy / skip;
                    int ox = trafx / skip;
#ifdef _OPENMP
                    #pragma omp parallel for
#endif

                    for (int y = 0; y < baseCrop->getHeight(); ++y) {
                        int dy = oy + y;

                        for (int x = 0; x < baseCrop->getWidth(); ++x) {
                            int dx = ox + x;
                            f->r(dy, dx) = baseCrop->r(y, x);
                            f->g(dy, dx) = baseCrop->g(y, x);
                            f->b(dy, dx) = baseCrop->b(y, x);
                        }
                    }
                } else if (skip == 1) {
                    parent->fattal_11_dcrop_cache = f; // cache this globally
                    fattalCrop.release();
                }
            }
        }

        if (need_fattal) {
            parent->ipf.dehaze(f, params.dehaze);
            parent->ipf.ToneMapFattal02(f, params.fattal, 3, 0, nullptr, 0, 0, 0, false);
        }

        // crop back to the size expected by the rest of the pipeline
        if (need_cropping) {
            Imagefloat *c = origCrop;

            int oy = trafy / skip;
            int ox = trafx / skip;
#ifdef _OPENMP
            #pragma omp parallel for
#endif

            for (int y = 0; y < trafh; ++y) {
                int cy = y + oy;

                for (int x = 0; x < trafw; ++x) {
                    int cx = x + ox;
                    c->r(y, x) = f->r(cy, cx);
                    c->g(y, x) = f->g(cy, cx);
                    c->b(y, x) = f->b(cy, cx);
                }
            }

            baseCrop = c;
        } else {
            baseCrop = f;
        }
    }
}

void Crop::stageTransform(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    Imagefloat*& baseCrop = ctx.baseCrop;

    const bool needstransform  = parent->ipf.needsTransform(skips(parent->fw, skip), skips(parent->fh, skip), parent->imgsrc->getRotateDegree(), parent->imgsrc->getMetaData());
    const bool cam02 = params.colorappearance.modelmethod == "02" && params.colorappearance.enabled;
    ctx.cam02 = cam02;

    // transform
   // if (needstransform || ((todo & (M_TRANSFORM | M_RGBCURVE)) && params.dirpyrequalizer.cbdlMethod == "bef" && params.dirpyrequalizer.enabled && !params.colorappearance.enabled)) {
    if (needstransform || ((todo & (M_TRANSFORM | M_RGBCURVE)) && params.dirpyrequalizer.cbdlMethod == "bef" && params.dirpyrequalizer.enabled && !cam02)) {
        if (!transCrop) {
            transCrop = new Imagefloat(cropw, croph);
        }

        if (needstransform)
            parent->ipf.transform(baseCrop, transCrop, cropx / skip, cropy / skip, trafx / skip, trafy / skip, skips(parent->fw, skip), skips(parent->fh, skip), parent->getFullWidth(), parent->getFullHeight(),
                                  parent->imgsrc->getMetaData(),
                                  parent->imgsrc->getRotateDegree(), false);
        else {
            baseCrop->copyData(transCrop);
        }

        if (transCrop) {
            baseCrop = transCrop;
        }
    } else {
        delete transCrop;
        transCrop = nullptr;
    }

   // if ((todo & (M_TRANSFORM | M_RGBCURVE))  && params.dirpyrequalizer.cbdlMethod == "bef" && params.dirpyrequalizer.enabled && !params.colorappearance.enabled) {
    if ((todo & (M_TRANSFORM | M_RGBCURVE))  && params.dirpyrequalizer.cbdlMethod == "bef" && params.dirpyrequalizer.enabled && !cam02) {

        const int W = baseCrop->getWidth();
        const int H = baseCrop->getHeight();
        LabImage labcbdl(W, H);
        parent->ipf.rgb2lab(*baseCrop, labcbdl, params.icm.workingProfile);
        parent->ipf.dirpyrequalizer(&labcbdl, skip);
        parent->ipf.lab2rgb(labcbdl, *baseCrop, params.icm.workingProfile);

    }
}

void Crop::stageLocalAdjustments(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    Imagefloat*& baseCrop = ctx.baseCrop;

    if ((todo & (M_AUTOEXP | M_RGBCURVE)) && params.locallab.enabled && !params.locallab.spots.empty()) {

        //I made a little change here. Rather than have luminanceCurve (and others) use in/out lab images, we can do more if we copy right here.
        parent->ipf.rgb2lab(*baseCrop, *laboCrop, params.icm.workingProfile);


        labnCrop->CopyFrom(laboCrop);

        const std::unique_ptr<LabImage> reservCrop(new LabImage(*laboCrop, true));
        const std::unique_ptr<LabImage> lastorigCrop(new LabImage(*laboCrop, true));
        std::unique_ptr<LabImage> savenormtmCrop;
        std::unique_ptr<LabImage> savenormretiCrop;
        auto& lllocalcurve2 = parent->lllocalcurve;
        auto& cllocalcurve2 = parent->cllocalcurve;
        auto& lclocalcurve2 = parent->lclocalcurve;
        auto& cclocalcurve2 = parent->cclocalcurve;
        auto& rgblocalcurve2 = parent->rgblocalcurve;
        auto& exlocalcurve2 = parent->exlocalcurve;
        auto& lmasklocalcurve2 = parent->lmasklocalcurve;
        auto& lmaskexplocalcurve2 = parent->lmaskexplocalcurve;
        auto& lmaskSHlocalcurve2 = parent->lmaskSHlocalcurve;
       // auto& ghslocalcurve2 = parent->ghslocalcurve;
        auto& lmaskviblocalcurve2 = parent->lmaskviblocalcurve;
        auto& lmasktmlocalcurve2 = parent->lmasktmlocalcurve;
        auto& lmaskretilocalcurve2 = parent->lmaskretilocalcurve;
        auto& lmaskcblocalcurve2 = parent->lmaskcblocalcurve;
        auto& lmaskbllocalcurve2 = parent->lmaskbllocalcurve;
        auto& lmasklclocalcurve2 = parent->lmasklclocalcurve;
        auto& lmaskloglocalcurve2 = parent->lmaskloglocalcurve;
        auto& lmaskcielocalcurve2 = parent->lmaskcielocalcurve;
        auto& cielocalcurve2 = parent->cielocalcurve;
        auto& cielocalcurve22 = parent->cielocalcurve2;
        auto& jzlocalcurve2 = parent->jzlocalcurve;
        auto& czlocalcurve2 = parent->czlocalcurve;
        auto& czjzlocalcurve2 = parent->czjzlocalcurve;
        auto& redlocalcurve2 = parent->redlocalcurve;
        auto& greenlocalcurve2 = parent->greenlocalcurve;
        auto& bluelocalcurve2 = parent->bluelocalcurve;

        auto& hltonecurveloc2 = parent->hltonecurveloc;
        auto& shtonecurveloc2 = parent->shtonecurveloc;
        auto& tonecurveloc2 = parent->tonecurveloc;
        auto& lightCurveloc2 = parent->lightCurveloc;
        auto& locRETgainCurve = parent->locRETgainCurve;
        auto& locRETtransCurve = parent->locRETtransCurve;
        auto& loclhCurve = parent->loclhCurve;
        auto& lochhCurve = parent->lochhCurve;
        auto& locchCurve = parent->locchCurve;
        auto& lochhCurvejz = parent->lochhCurvejz;
        auto& locchCurvejz = parent->locchCurvejz;
        auto& loclhCurvejz = parent->loclhCurvejz;
        auto& locccmasCurve = parent->locccmasCurve;
        auto& locllmasCurve = parent->locllmasCurve;
        auto& lochhmasCurve = parent->lochhmasCurve;
        auto& lochhhmasCurve = parent->lochhhmasCurve;
        auto& lochhhmascieCurve = parent->lochhhmascieCurve;
        auto& locccmasexpCurve = parent->locccmasexpCurve;
        auto& locllmasexpCurve = parent->locllmasexpCurve;
        auto& lochhmasexpCurve = parent->lochhmasexpCurve;
        auto& locccmasSHCurve = parent->locccmasSHCurve;
        auto& locllmasSHCurve = parent->locllmasSHCurve;
        auto& lochhmasSHCurve = parent->lochhmasSHCurve;
        auto& locccmasvibCurve = parent->locccmasvibCurve;
        auto& locllmasvibCurve = parent->locllmasvibCurve;
        auto& lochhmasvibCurve = parent->lochhmasvibCurve;
        auto& locccmaslcCurve = parent->locccmaslcCurve;
        auto& locllmaslcCurve = parent->locllmaslcCurve;
        auto& lochhmaslcCurve = parent->lochhmaslcCurve;
        auto& locccmascbCurve = parent->locccmascbCurve;
        auto& locllmascbCurve = parent->locllmascbCurve;
        auto& lochhmascbCurve = parent->lochhmascbCurve;
        auto& locccmasretiCurve = parent->locccmasretiCurve;
        auto& locllmasretiCurve = parent->locllmasretiCurve;
        auto& lochhmasretiCurve = parent->lochhmasretiCurve;
        auto& locccmastmCurve = parent->locccmastmCurve;
        auto& locllmastmCurve = parent->locllmastmCurve;
        auto& lochhmastmCurve = parent->lochhmastmCurve;
        auto& locccmasblCurve = parent->locccmasblCurve;
        auto& locllmasblCurve = parent->locllmasblCurve;
        auto& lochhmasblCurve = parent->lochhmasblCurve;
        auto& locccmaslogCurve = parent->locccmaslogCurve;
        auto& locllmaslogCurve = parent->locllmaslogCurve;
        auto& lochhmaslogCurve = parent->lochhmaslogCurve;
        auto& locccmascieCurve = parent->locccmascieCurve;
        auto& locllmascieCurve = parent->locllmascieCurve;
        auto& lochhmascieCurve = parent->lochhmascieCurve;

        auto& locccmas_Curve = parent->locccmas_Curve;
        auto& locllmas_Curve = parent->locllmas_Curve;
        auto& lochhmas_Curve = parent->lochhmas_Curve;
        auto& lochhhmas_Curve = parent->lochhhmas_Curve;
        auto& locwavCurve = parent->locwavCurve;
        auto& locwavCurvejz = parent->locwavCurvejz;
        auto& loclmasCurveblwav = parent->loclmasCurveblwav;
        auto& loclmasCurvecolwav = parent->loclmasCurvecolwav;
        auto& loclmasCurveciewav = parent->loclmasCurveciewav;
        auto& loclevwavCurve = parent->loclevwavCurve;
        auto& locconwavCurve = parent->locconwavCurve;
        auto& loccompwavCurve = parent->loccompwavCurve;
        auto& loccomprewavCurve = parent->loccomprewavCurve;
        auto& locedgwavCurve = parent->locedgwavCurve;
        auto& locwavCurvehue = parent->locwavCurvehue;
        auto& locwavCurvehuecont = parent->locwavCurvehuecont;
        auto& locwavCurveden = parent->locwavCurveden;
        auto& lmasklocal_curve2 = parent->lmasklocal_curve;
        auto& loclmasCurve_wav = parent->loclmasCurve_wav;
        //big bug found 29//11/2024
        std::vector<LocallabListener::locallabDenoiseLC> localldenoiselc;
        std::vector<LocallabListener::locallabDenoiseMAD> localldenoisemadl;
        std::vector<LocallabListener::locallabsharAFT> locallsharaft;
        std::vector<LocallabListener::locallabDenoiseLC2> localldenoiselc2;

        for (int sp = 0; sp < (int)params.locallab.spots.size(); sp++) {
            locRETgainCurve.Set(params.locallab.spots.at(sp).localTgaincurve);
            locRETtransCurve.Set(params.locallab.spots.at(sp).localTtranscurve);
            const bool LHutili = loclhCurve.Set(params.locallab.spots.at(sp).LHcurve);
            const bool HHutili = lochhCurve.Set(params.locallab.spots.at(sp).HHcurve);
            const bool CHutili = locchCurve.Set(params.locallab.spots.at(sp).CHcurve);
            const bool HHutilijz = lochhCurvejz.Set(params.locallab.spots.at(sp).HHcurvejz);
            const bool CHutilijz = locchCurvejz.Set(params.locallab.spots.at(sp).CHcurvejz);
            const bool LHutilijz = loclhCurvejz.Set(params.locallab.spots.at(sp).LHcurvejz);
            const bool lcmasutili = locccmasCurve.Set(params.locallab.spots.at(sp).CCmaskcurve);
            const bool llmasutili = locllmasCurve.Set(params.locallab.spots.at(sp).LLmaskcurve);
            const bool lhmasutili = lochhmasCurve.Set(params.locallab.spots.at(sp).HHmaskcurve);
            const bool lhhmasutili = lochhhmasCurve.Set(params.locallab.spots.at(sp).HHhmaskcurve);
            const bool lhhmascieutili = lochhhmascieCurve.Set(params.locallab.spots.at(sp).HHhmaskciecurve);
            const bool lcmasexputili = locccmasexpCurve.Set(params.locallab.spots.at(sp).CCmaskexpcurve);
            const bool llmasexputili = locllmasexpCurve.Set(params.locallab.spots.at(sp).LLmaskexpcurve);
            const bool lhmasexputili = lochhmasexpCurve.Set(params.locallab.spots.at(sp).HHmaskexpcurve);
            const bool lcmasSHutili = locccmasSHCurve.Set(params.locallab.spots.at(sp).CCmaskSHcurve);
            const bool llmasSHutili = locllmasSHCurve.Set(params.locallab.spots.at(sp).LLmaskSHcurve);
            const bool lhmasSHutili = lochhmasSHCurve.Set(params.locallab.spots.at(sp).HHmaskSHcurve);
            const bool lcmasvibutili = locccmasvibCurve.Set(params.locallab.spots.at(sp).CCmaskvibcurve);
            const bool llmasvibutili = locllmasvibCurve.Set(params.locallab.spots.at(sp).LLmaskvibcurve);
            const bool lhmasvibutili = lochhmasvibCurve.Set(params.locallab.spots.at(sp).HHmaskvibcurve);
            const bool lcmascbutili = locccmascbCurve.Set(params.locallab.spots.at(sp).CCmaskcbcurve);
            const bool llmascbutili = locllmascbCurve.Set(params.locallab.spots.at(sp).LLmaskcbcurve);
            const bool lhmascbutili = lochhmascbCurve.Set(params.locallab.spots.at(sp).HHmaskcbcurve);
            const bool lcmasretiutili = locccmasretiCurve.Set(params.locallab.spots.at(sp).CCmaskreticurve);
            const bool llmasretiutili = locllmasretiCurve.Set(params.locallab.spots.at(sp).LLmaskreticurve);
            const bool lhmasretiutili = lochhmasretiCurve.Set(params.locallab.spots.at(sp).HHmaskreticurve);
            const bool lcmastmutili = locccmastmCurve.Set(params.locallab.spots.at(sp).CCmasktmcurve);
            const bool llmastmutili = locllmastmCurve.Set(params.locallab.spots.at(sp).LLmasktmcurve);
            const bool lhmastmutili = lochhmastmCurve.Set(params.locallab.spots.at(sp).HHmasktmcurve);
            const bool lcmasblutili = locccmasblCurve.Set(params.locallab.spots.at(sp).CCmaskblcurve);
            const bool llmasblutili = locllmasblCurve.Set(params.locallab.spots.at(sp).LLmaskblcurve);
            const bool lhmasblutili = lochhmasblCurve.Set(params.locallab.spots.at(sp).HHmaskblcurve);
            const bool lcmaslogutili = locccmaslogCurve.Set(params.locallab.spots.at(sp).CCmaskcurveL);
            const bool llmaslogutili = locllmaslogCurve.Set(params.locallab.spots.at(sp).LLmaskcurveL);
            const bool lhmaslogutili = lochhmaslogCurve.Set(params.locallab.spots.at(sp).HHmaskcurveL);
            const bool lcmascieutili = locccmascieCurve.Set(params.locallab.spots.at(sp).CCmaskciecurve);
            const bool llmascieutili = locllmascieCurve.Set(params.locallab.spots.at(sp).LLmaskciecurve);
            const bool lhmascieutili = lochhmascieCurve.Set(params.locallab.spots.at(sp).HHmaskciecurve);

            const bool lcmas_utili = locccmas_Curve.Set(params.locallab.spots.at(sp).CCmask_curve);
            const bool llmas_utili = locllmas_Curve.Set(params.locallab.spots.at(sp).LLmask_curve);
            const bool lhmas_utili = lochhmas_Curve.Set(params.locallab.spots.at(sp).HHmask_curve);
            const bool lhhmas_utili = lochhhmas_Curve.Set(params.locallab.spots.at(sp).HHhmask_curve);
            const bool lmasutili_wav = loclmasCurve_wav.Set(params.locallab.spots.at(sp).LLmask_curvewav);
            const bool lmasutiliblwav = loclmasCurveblwav.Set(params.locallab.spots.at(sp).LLmaskblcurvewav);
            const bool lmasutilicolwav = loclmasCurvecolwav.Set(params.locallab.spots.at(sp).LLmaskcolcurvewav);
            const bool lmasutiliciewav = loclmasCurveciewav.Set(params.locallab.spots.at(sp).LLmaskciecurvewav);
            const bool lcmaslcutili = locccmaslcCurve.Set(params.locallab.spots.at(sp).CCmasklccurve);
            const bool llmaslcutili = locllmaslcCurve.Set(params.locallab.spots.at(sp).LLmasklccurve);
            const bool lhmaslcutili = lochhmaslcCurve.Set(params.locallab.spots.at(sp).HHmasklccurve);
            const bool locwavutili = locwavCurve.Set(params.locallab.spots.at(sp).locwavcurve);
            const bool locwavutilijz = locwavCurvejz.Set(params.locallab.spots.at(sp).locwavcurvejz);
            const bool locwavhueutili = locwavCurvehue.Set(params.locallab.spots.at(sp).locwavcurvehue);
            const bool locwavhueutilicont = locwavCurvehuecont.Set(params.locallab.spots.at(sp).locwavcurvehuecont);
            const bool locwavdenutili = locwavCurveden.Set(params.locallab.spots.at(sp).locwavcurveden);
            const bool loclevwavutili = loclevwavCurve.Set(params.locallab.spots.at(sp).loclevwavcurve);
            const bool locconwavutili = locconwavCurve.Set(params.locallab.spots.at(sp).locconwavcurve);
            const bool loccompwavutili = loccompwavCurve.Set(params.locallab.spots.at(sp).loccompwavcurve);
            const bool loccomprewavutili = loccomprewavCurve.Set(params.locallab.spots.at(sp).loccomprewavcurve);
            const bool locedgwavutili = locedgwavCurve.Set(params.locallab.spots.at(sp).locedgwavcurve);
            const bool locallutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).llcurve, lllocalcurve2, skip);
            const bool localclutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).clcurve, cllocalcurve2, skip);
            const bool locallcutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).lccurve, lclocalcurve2, skip);
            const bool localrgbutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).rgbcurve, rgblocalcurve2, skip);
            const bool localcutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).cccurve, cclocalcurve2, skip);
            const bool localexutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).excurve, exlocalcurve2, skip);
            const bool localmaskutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskcurve, lmasklocalcurve2, skip);
            const bool localmaskexputili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskexpcurve, lmaskexplocalcurve2, skip);
            const bool localmaskSHutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).LmaskSHcurve, lmaskSHlocalcurve2, skip);
          //  const bool localghsutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).ghscurve, ghslocalcurve2, skip);
            const bool localmaskvibutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskvibcurve, lmaskviblocalcurve2, skip);
            const bool localmasktmutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmasktmcurve, lmasktmlocalcurve2, skip);
            const bool localmaskretiutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskreticurve, lmaskretilocalcurve2, skip);
            const bool localmaskcbutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskcbcurve, lmaskcblocalcurve2, skip);
            const bool localmasklcutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmasklccurve, lmasklclocalcurve2, skip);
            const bool localmaskblutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskblcurve, lmaskbllocalcurve2, skip);
            const bool localmasklogutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).LmaskcurveL, lmaskloglocalcurve2, skip);
            const bool localmask_utili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmask_curve, lmasklocal_curve2, skip);
            const bool localmaskcieutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).Lmaskciecurve, lmaskcielocalcurve2, skip);
            const bool localcieutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).ciecurve, cielocalcurve2, skip);
            const bool localcieutili2 = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).ciecurve2, cielocalcurve22, skip);
            const bool localjzutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).jzcurve, jzlocalcurve2, skip);
            const bool localczutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).czcurve, czlocalcurve2, skip);
            const bool localczjzutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).czjzcurve, czjzlocalcurve2, skip);
            const bool localredutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).redcurve, redlocalcurve2, skip);
            const bool localgreenutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).greencurve, greenlocalcurve2, skip);
            const bool localblueutili = CurveFactory::diagonalCurve2Lut(params.locallab.spots.at(sp).bluecurve, bluelocalcurve2, skip);

            double ecomp = params.locallab.spots.at(sp).expcomp;
            double black = params.locallab.spots.at(sp).black;
            double hlcompr = params.locallab.spots.at(sp).hlcompr;
            double hlcomprthresh = params.locallab.spots.at(sp).hlcomprthresh;
            double shcompr = params.locallab.spots.at(sp).shcompr;
            double br = params.locallab.spots.at(sp).lightness;
            if (black < 0. && params.locallab.spots.at(sp).expMethod == "pde" ) {
                black *= 1.5;
            }

            double cont = params.locallab.spots.at(sp).contrast;
            double huere, chromare, lumare, huerefblu, chromarefblu, lumarefblu, sobelre;
            huerefblu = parent->huerefblurs[sp];
            chromarefblu = parent->chromarefblurs[sp];
            lumarefblu = parent->lumarefblurs[sp];
            huere = parent->huerefs[sp];
            chromare = parent->chromarefs[sp];
            lumare = parent->lumarefs[sp];
            sobelre = parent->sobelrefs[sp];
            const float avge = parent->avgs[sp];
            float meantme = parent->meantms[sp];
            float stdtme = parent->stdtms[sp];
            float meanretie = parent->meanretis[sp];
            float stdretie = parent->stdretis[sp];

            float fab = 1.f;
            float maxicam = -1000.f;
            float rdx, rdy, grx, gry, blx, bly = 0.f;
            float meanx, meany, meanxe, meanye, maxdat = 0.f;
            int ill = 2;
            int prim = 3;
            float minCD;
            float maxCD;
            float mini;
            float maxi;
            float Tmean;
            float Tsigma;
            float Tmin;
            float Tmax;
            int lastsav;
            float resi[8];

            float contsig = params.locallab.spots.at(sp).contsigqcie;
            float slopeg = 1.f;
            bool linkrgb = true;
            float lightsig = params.locallab.spots.at(sp).lightsigqcie;
            float sharc = 0.f;
            float denocont = 0.f;
            int ghsbpwp[2];
            float ghsbpwpvalue[2];
            float savmadl[21]  = {100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f};
            float ghsbwslider[2];
            float michbwslider[2]= {0.f, 0.f};//added to facilitate a possible modification requested by users, but is not currently in use
            float ghscolor[4];
            float ghssym;
            float ghsmid;
            float ghsmaxrgb;
            float ghs3sig;
            bool ghsautsp;
            float maxdatend2 = 0.f;
            float satdatend2 = 0.f;
            bool gamaut2 = false;
/*            huerefp[sp] = huere;
            chromarefp[sp] = chromare;
            lumarefp[sp] = lumare;
*/
            CurveFactory::complexCurvelocal(ecomp, black / 65535., hlcompr, hlcomprthresh, shcompr, br, cont, lumare,
                                            hltonecurveloc2, shtonecurveloc2, tonecurveloc2, lightCurveloc2, avge,
                                            skip);
            // Locallab mask are only shown for selected spot
            int fh = parent->fh;
            int fw = parent->fw;

            if (sp == params.locallab.selspot) {

                parent->ipf.Lab_Local(1, sp, (float**)shbuffer, labnCrop, labnCrop, reservCrop.get(), savenormtmCrop.get(), savenormretiCrop.get(), lastorigCrop.get(), fw, fh, cropx / skip, cropy / skip, skips(parent->fw, skip), skips(parent->fh, skip), trafx, trafy, trafw, trafh , skip, locRETgainCurve, locRETtransCurve,
                        lllocalcurve2,locallutili,
                        cllocalcurve2, localclutili,
                        lclocalcurve2, locallcutili,
                        loclhCurve, lochhCurve, locchCurve,
                        lochhCurvejz, locchCurvejz, loclhCurvejz,

                        lmasklocalcurve2, localmaskutili,
                        lmaskexplocalcurve2, localmaskexputili,
                        lmaskSHlocalcurve2, localmaskSHutili,
                        lmaskviblocalcurve2, localmaskvibutili,
                        lmasktmlocalcurve2, localmasktmutili,
                        lmaskretilocalcurve2, localmaskretiutili,
                        lmaskcblocalcurve2, localmaskcbutili,
                        lmaskbllocalcurve2, localmaskblutili,
                        lmasklclocalcurve2, localmasklcutili,
                        lmaskloglocalcurve2, localmasklogutili,
                        lmasklocal_curve2, localmask_utili,
                        lmaskcielocalcurve2, localmaskcieutili,
                        cielocalcurve2,localcieutili,
                        cielocalcurve22,localcieutili2,
                        jzlocalcurve2,localjzutili,
                        czlocalcurve2,localczutili,
                        czjzlocalcurve2,localczjzutili,
                        redlocalcurve2,localredutili,
                        greenlocalcurve2,localgreenutili,
                        bluelocalcurve2,localblueutili,

                        locccmasCurve, lcmasutili, locllmasCurve, llmasutili, lochhmasCurve, lhmasutili, lochhhmasCurve, lhhmasutili, lochhhmascieCurve, lhhmascieutili, locccmasexpCurve, lcmasexputili, locllmasexpCurve, llmasexputili, lochhmasexpCurve, lhmasexputili,
                        locccmasSHCurve, lcmasSHutili, locllmasSHCurve, llmasSHutili, lochhmasSHCurve, lhmasSHutili,
                        locccmasvibCurve, lcmasvibutili, locllmasvibCurve, llmasvibutili, lochhmasvibCurve, lhmasvibutili,
                        locccmascbCurve, lcmascbutili, locllmascbCurve, llmascbutili, lochhmascbCurve, lhmascbutili,
                        locccmasretiCurve, lcmasretiutili, locllmasretiCurve, llmasretiutili, lochhmasretiCurve, lhmasretiutili,
                        locccmastmCurve, lcmastmutili, locllmastmCurve, llmastmutili, lochhmastmCurve, lhmastmutili,
                        locccmasblCurve, lcmasblutili, locllmasblCurve, llmasblutili, lochhmasblCurve, lhmasblutili,
                        locccmaslcCurve, lcmaslcutili, locllmaslcCurve, llmaslcutili, lochhmaslcCurve, lhmaslcutili,
                        locccmaslogCurve, lcmaslogutili, locllmaslogCurve, llmaslogutili, lochhmaslogCurve, lhmaslogutili,

                        locccmas_Curve, lcmas_utili, locllmas_Curve, llmas_utili, lochhmas_Curve, lhmas_utili,
                        locccmascieCurve, lcmascieutili, locllmascieCurve, llmasSHutili, lochhmascieCurve, lhmascieutili,

                        lochhhmas_Curve, lhhmas_utili,
                        loclmasCurveblwav,lmasutiliblwav,
                        loclmasCurvecolwav,lmasutilicolwav,
                        loclmasCurveciewav,lmasutiliciewav,
                        locwavCurve, locwavutili,
                        locwavCurvejz, locwavutilijz,
                        loclevwavCurve, loclevwavutili,
                        locconwavCurve, locconwavutili,
                        loccompwavCurve, loccompwavutili,
                        loccomprewavCurve, loccomprewavutili,
                        locwavCurvehue, locwavhueutili,
                        locwavCurvehuecont, locwavhueutilicont,
                        locwavCurveden, locwavdenutili,
                        locedgwavCurve, locedgwavutili,
                        loclmasCurve_wav,lmasutili_wav,
                        LHutili, HHutili, CHutili, HHutilijz, CHutilijz, LHutilijz, cclocalcurve2, localcutili, rgblocalcurve2, localrgbutili, localexutili, exlocalcurve2, hltonecurveloc2, shtonecurveloc2, tonecurveloc2, lightCurveloc2,
                        huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, lastsav,
                        parent->previewDeltaE, parent->locallColorMask, parent->locallColorMaskinv, parent->locallExpMask, parent->locallExpMaskinv, parent->locallSHMask, parent->locallSHMaskinv, parent->locallvibMask,  parent->localllcMask, parent->locallsharMask, parent->locallcbMask, parent->locallretiMask, parent->locallsoftMask, parent->localltmMask, parent->locallblMask,
                        parent->localllogMask, parent->locall_Mask, parent->locallcieMask, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax,
                        meantme, stdtme, meanretie, stdretie, fab, maxicam,rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, prim, ill, contsig, lightsig, slopeg, linkrgb,
                        resi, sharc, denocont, ghsbpwp, ghsbpwpvalue, savmadl, ghsbwslider, ghssym, ghsautsp, ghscolor, ghsmid, ghsmaxrgb, ghs3sig, michbwslider, maxdatend2, satdatend2, gamaut2);

                        if (parent->previewDeltaE || parent->locallColorMask == 5 || parent->locallvibMask == 4 || parent->locallExpMask == 5 || parent->locallSHMask == 4 || parent->localllcMask == 4 || parent->localltmMask == 4 || parent->localllogMask == 4 || parent->locallsoftMask == 6 || parent->localllcMask == 4 || parent->locallcieMask == 4) {
                            params.blackwhite.enabled = false;
                            params.colorToning.enabled = false;
                            params.rgbCurves.enabled = false;
                            params.chmixer.enabled = false;
                            params.hsvequalizer.enabled = false;
                            params.filmSimulation.enabled = false;
                            params.toneCurve.black = 0.f;
                            params.toneCurve.saturation = 0.f;
                            params.toneCurve.brightness= 0.f;
                            params.toneCurve.contrast = 0.f;
                            params.toneCurve.hlcompr = 0.f;
                            //these 3 are "before" LA
                            //params.toneCurve.expcomp = 0;
                            //params.toneCurve.curve = { 0 };
                            //params.toneCurve.curve2 = { 0 };
                            params.colorappearance.enabled = false;
                            params.vibrance.enabled = false;
                            params.labCurve.enabled = false;
                            params.wavelet.enabled = false;
                            params.epd.enabled = false;
                            params.softlight.enabled = false;
                        }
                        /*
                        if (parent->locallListener) {
                            parent->locallListener->refChanged2(huerefp, chromarefp, lumarefp, fabrefp, params.locallab.selspot);

                        }
                        */
            } else {
                parent->ipf.Lab_Local(1, sp, (float**)shbuffer, labnCrop, labnCrop, reservCrop.get(), savenormtmCrop.get(), savenormretiCrop.get(), lastorigCrop.get(), fw, fh, cropx / skip, cropy / skip, skips(parent->fw, skip), skips(parent->fh, skip), trafx, trafy, trafw , trafh, skip, locRETgainCurve, locRETtransCurve,
                        lllocalcurve2,locallutili,
                        cllocalcurve2, localclutili,
                        lclocalcurve2, locallcutili,
                        loclhCurve, lochhCurve, locchCurve,
                        lochhCurvejz, locchCurvejz, loclhCurvejz,
                        lmasklocalcurve2, localmaskutili,
                        lmaskexplocalcurve2, localmaskexputili,
                        lmaskSHlocalcurve2, localmaskSHutili,
                        lmaskviblocalcurve2, localmaskvibutili,
                        lmasktmlocalcurve2, localmasktmutili,
                        lmaskretilocalcurve2, localmaskretiutili,
                        lmaskcblocalcurve2, localmaskcbutili,
                        lmaskbllocalcurve2, localmaskblutili,
                        lmasklclocalcurve2, localmasklcutili,
                        lmaskloglocalcurve2, localmasklogutili,
                        lmasklocal_curve2, localmask_utili,
                        lmaskcielocalcurve2, localmaskcieutili,
                        cielocalcurve2,localcieutili,
                        cielocalcurve22,localcieutili2,
                        jzlocalcurve2,localjzutili,
                        czlocalcurve2,localczutili,
                        czjzlocalcurve2,localczjzutili,
                        redlocalcurve2,localredutili,
                        greenlocalcurve2,localgreenutili,
                        bluelocalcurve2,localblueutili,

                        locccmasCurve, lcmasutili, locllmasCurve, llmasutili, lochhmasCurve, lhmasutili,lochhhmasCurve, lhhmasutili, lochhhmascieCurve, lhhmascieutili, locccmasexpCurve, lcmasexputili, locllmasexpCurve, llmasexputili, lochhmasexpCurve, lhmasexputili,
                        locccmasSHCurve, lcmasSHutili, locllmasSHCurve, llmasSHutili, lochhmasSHCurve, lhmasSHutili,
                        locccmasvibCurve, lcmasvibutili, locllmasvibCurve, llmasvibutili, lochhmasvibCurve, lhmasvibutili,
                        locccmascbCurve, lcmascbutili, locllmascbCurve, llmascbutili, lochhmascbCurve, lhmascbutili,
                        locccmasretiCurve, lcmasretiutili, locllmasretiCurve, llmasretiutili, lochhmasretiCurve, lhmasretiutili,
                        locccmastmCurve, lcmastmutili, locllmastmCurve, llmastmutili, lochhmastmCurve, lhmastmutili,
                        locccmasblCurve, lcmasblutili, locllmasblCurve, llmasblutili, lochhmasblCurve, lhmasblutili,
                        locccmaslcCurve, lcmaslcutili, locllmaslcCurve, llmaslcutili, lochhmaslcCurve, lhmaslcutili,
                        locccmaslogCurve, lcmaslogutili, locllmaslogCurve, llmaslogutili, lochhmaslogCurve, lhmaslogutili,

                        locccmas_Curve, lcmas_utili, locllmas_Curve, llmas_utili, lochhmas_Curve, lhmas_utili,
                        locccmascieCurve, lcmascieutili, locllmascieCurve, llmascieutili, lochhmascieCurve, lhmascieutili,
                        lochhhmas_Curve, lhhmas_utili,

                        loclmasCurveblwav,lmasutiliblwav,
                        loclmasCurvecolwav,lmasutilicolwav,
                        loclmasCurveciewav,lmasutiliciewav,
                        locwavCurve, locwavutili,
                        locwavCurvejz, locwavutilijz,
                        loclevwavCurve, loclevwavutili,
                        locconwavCurve, locconwavutili,
                        loccompwavCurve, loccompwavutili,
                        loccomprewavCurve, loccomprewavutili,
                        locwavCurvehue, locwavhueutili,
                        locwavCurvehuecont, locwavhueutilicont,
                        locwavCurveden, locwavdenutili,
                        locedgwavCurve, locedgwavutili,
                        loclmasCurve_wav,lmasutili_wav,
                        LHutili, HHutili, CHutili, HHutilijz, CHutilijz, LHutilijz, cclocalcurve2, localcutili, rgblocalcurve2, localrgbutili, localexutili, exlocalcurve2, hltonecurveloc2, shtonecurveloc2, tonecurveloc2, lightCurveloc2,
                        huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, lastsav, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax,
                        meantme, stdtme, meanretie, stdretie, fab, maxicam, rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, prim, ill, contsig, lightsig, slopeg, linkrgb,
                        resi, sharc, denocont, ghsbpwp, ghsbpwpvalue, savmadl, ghsbwslider, ghssym, ghsautsp, ghscolor, ghsmid, ghsmaxrgb, ghs3sig, michbwslider, maxdatend2, satdatend2, gamaut2);
            }

                       // for (int l = 0; l < 21; l++) {
                        //    params.locallab.spots.at(sp).madlsav[l] = savmadl[l];
                       // }

                        LocallabListener::locallabDenoiseLC2 denoiselc2;
                        denoiselc2.denocontrastaft = denocont;
                        localldenoiselc2.push_back(denoiselc2);

                        LocallabListener::locallabDenoiseLC denoiselc;
                        denoiselc.highres = resi[0];
                        denoiselc.nres = resi[1];
                        denoiselc.highres46 = resi[2];
                        denoiselc.nres46 = resi[3];
                        denoiselc.Lhighres = resi[5];
                        denoiselc.Lnres = resi[4];
                        denoiselc.Lhighres46 = resi[6];
                        denoiselc.Lnres46 = resi[7];
                        localldenoiselc.push_back(denoiselc);

                        LocallabListener::locallabDenoiseMAD madllc;
                        madllc.mad0 = savmadl[0];
                        madllc.mad1 = savmadl[1];
                        madllc.mad2 = savmadl[2];
                        madllc.mad3 = savmadl[3];
                        madllc.mad4 = savmadl[4];
                        madllc.mad5 = savmadl[5];
                        madllc.mad6 = savmadl[6];
                        madllc.mad7 = savmadl[7];
                        madllc.mad8 = savmadl[8];
                        madllc.mad9 = savmadl[9];
                        madllc.mad10 = savmadl[10];
                        madllc.mad11 = savmadl[11];
                        madllc.mad12 = savmadl[12];
                        madllc.mad13 = savmadl[13];
                        madllc.mad14 = savmadl[14];
                        madllc.mad15 = savmadl[15];
                        madllc.mad16 = savmadl[16];
                        madllc.mad17 = savmadl[17];
                        madllc.mad18 = savmadl[18];
                        madllc.mad19 = savmadl[19];
                        madllc.mad20 = savmadl[20];
                        madllc.madlock = params.locallab.spots.at(sp).madllock;
                        localldenoisemadl.push_back(madllc);

                        LocallabListener::locallabsharAFT locsharaft;
                        locsharaft.autocontrastaft = params.locallab.spots.at(sp).deconvAutoshar;
                        locsharaft.sharcontrastaft = sharc;
                        locallsharaft.push_back(locsharaft);
                        /*
                        if(locsharaft.autocontrastaft) {
                            printf("Autoshar\n");
                        } else {
                            printf("Pa sautoshar\n");
                        }
                        */
                        if (parent->locallListener) {
                            if(params.locallab.spots.at(sp).expblur){//if not enable disable locallListener to avoid bad GUI behavior
                                parent->locallListener->denChanged(localldenoiselc, params.locallab.selspot);
                                if (params.locallab.spots.at(sp).lockmadl) {
                                    parent->locallListener->madChanged(localldenoisemadl, params.locallab.selspot);
                                }
                                parent->locallListener->den2Changed(localldenoiselc2, params.locallab.selspot);
                            }
                            parent->locallListener->sharaftChanged(locallsharaft,params.locallab.selspot);
                        }

            if (sp + 1u < params.locallab.spots.size()) {
                // do not copy for last spot as it is not needed anymore
                lastorigCrop->CopyFrom(labnCrop);
            }

            if (skip <= 2) {
                Glib::usleep(settings->cropsleep);    //wait to avoid crash when crop 100% and move window
            }
        }
        /*
                delete [] huerefp;
                delete [] chromarefp;
                delete [] lumarefp;
                delete [] fabrefp;
        */

        parent->ipf.lab2rgb(*labnCrop, *baseCrop, params.icm.workingProfile);
    }
}

void Crop::stageRgbProc(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    Imagefloat*& baseCrop = ctx.baseCrop;

    if (todo & M_RGBCURVE) {
        double rrm, ggm, bbm;
        DCPProfileApplyState as;
        DCPProfile *dcpProf = parent->imgsrc->getDCP(params.icm, as);

        LUTu histToneCurve;
        parent->ipf.rgbProc (baseCrop, laboCrop, this, parent->hltonecurve, parent->shtonecurve, parent->tonecurve,
                            params.toneCurve.saturation, parent->rCurve, parent->gCurve, parent->bCurve, parent->colourToningSatLimit, parent->colourToningSatLimitOpacity, parent->ctColorCurve, parent->ctOpacityCurve, parent->opautili, parent->clToningcurve, parent->cl2Toningcurve,
                            parent->customToneCurve1, parent->customToneCurve2, parent->beforeToneCurveBW, parent->afterToneCurveBW, rrm, ggm, bbm,
                            parent->bwAutoR, parent->bwAutoG, parent->bwAutoB, dcpProf, as, histToneCurve);
    }
}

void Crop::stageLabTools(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    const bool cam02 = ctx.cam02;
    LUTu& dummy = ctx.dummy;

    if ((todo & (M_LUMINANCE + M_COLOR)) == 0) {
        return;
    }
    labnCrop->CopyFrom(laboCrop);

    bool utili = parent->utili;
    bool autili = parent->autili;
    bool butili = parent->butili;
    bool ccutili = parent->ccutili;
    bool clcutili = parent->clcutili;
    bool cclutili = parent->cclutili;

    if (params.colorToning.enabled && params.colorToning.method == "LabGrid") {
        parent->ipf.colorToningLabGrid(labnCrop, 0,labnCrop->W , 0, labnCrop->H, false);
    }

    parent->ipf.shadowsHighlights(labnCrop, params.sh.enabled, params.sh.lab,params.sh.highlights ,params.sh.shadows, params.sh.radius, skip, params.sh.htonalwidth, params.sh.stonalwidth);

    if (params.localContrast.enabled) {
    // Alberto's local contrast
        parent->ipf.localContrast(labnCrop, labnCrop->L, params.localContrast, false, skip);
    }
    parent->ipf.chromiLuminanceCurve(this, 1, labnCrop, labnCrop, parent->chroma_acurve, parent->chroma_bcurve, parent->satcurve, parent->lhskcurve,  parent->clcurve, parent->lumacurve, utili, autili, butili, ccutili, cclutili, clcutili, dummy, dummy);
    parent->ipf.vibrance(labnCrop, params.vibrance, params.toneCurve.hrenabled, params.icm.workingProfile);
    parent->ipf.labColorCorrectionRegions(labnCrop);

   // if ((params.colorappearance.enabled && !params.colorappearance.tonecie) || (!params.colorappearance.enabled)) {
    if ((params.colorappearance.enabled && !params.colorappearance.tonecie) || (!cam02)) {
        parent->ipf.EPDToneMap(labnCrop, 0, skip);
    }
}

void Crop::stageLabSharpening(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    const bool cam02 = ctx.cam02;

    if ((todo & (M_LUMINANCE + M_COLOR)) == 0) {
        return;
    }
    //parent->ipf.EPDToneMap(labnCrop, 5, 1);    //Go with much fewer than normal iterates for fast redisplay.
    // for all treatments Defringe, Sharpening, Contrast detail , Microcontrast they are activated if "CIECAM" function are disabled
    if (skip == 1) {
      //  if ((params.colorappearance.enabled && !settings->autocielab)  || (!params.colorappearance.enabled)) {
        if ((params.colorappearance.enabled && !settings->autocielab)  || (!cam02)) {
            parent->ipf.impulsedenoise(labnCrop);
            parent->ipf.defringe(labnCrop);
        }

        parent->ipf.MLsharpen(labnCrop);

       // if ((params.colorappearance.enabled && !settings->autocielab)  || (!params.colorappearance.enabled)) {
        if ((params.colorappearance.enabled && !settings->autocielab)  || (!cam02)) {
            parent->ipf.MLmicrocontrast(labnCrop);
            parent->ipf.sharpening(labnCrop, params.sharpening, parent->sharpMask);
        }
    }
}

void Crop::stageLabWavelet(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    const bool cam02 = ctx.cam02;

    if ((todo & (M_LUMINANCE + M_COLOR)) == 0) {
        return;
    }
    //   if (skip==1) {

    if (params.dirpyrequalizer.cbdlMethod == "aft") {
       // if (((params.colorappearance.enabled && !settings->autocielab)  || (!params.colorappearance.enabled))) {
        if (((params.colorappearance.enabled && !settings->autocielab)  || (!cam02))) {
            parent->ipf.dirpyrequalizer(labnCrop, skip);
            //  parent->ipf.Lanczoslab (labnCrop,labnCrop , 1.f/skip);
        }
    }

    if ((params.wavelet.enabled)) {
        WaveletParams WaveParams = params.wavelet;
        int kall = 0;
        int minwin = min(labnCrop->W, labnCrop->H);
        int maxlevelcrop = 10;

        //  if (cp.mul[9]!=0)maxlevelcrop=10;
        // adap maximum level wavelet to size of crop
        if (minwin * skip < 1024) {
            maxlevelcrop = 9;    //sampling wavelet 512
        }

        if (minwin * skip < 512) {
            maxlevelcrop = 8;    //sampling wavelet 256
        }

        if (minwin * skip < 256) {
            maxlevelcrop = 7;    //sampling 128
        }

        if (minwin * skip < 128) {
            maxlevelcrop = 6;
        }

        if (minwin < 64) {
            maxlevelcrop = 5;
        }

        int realtile;

        if (params.wavelet.Tilesmethod == "big") {
            realtile = 22;
        } else /*if (params.wavelet.Tilesmethod == "lit")*/ {
            realtile = 12;
        }

        int tilesize = 128 * realtile;
        int overlap = (int) tilesize * 0.125f;

        int numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip;

        parent->ipf.Tile_calc(tilesize, overlap, kall, labnCrop->W, labnCrop->H, numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip);
        //now we have tile dimensions, overlaps
        //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        int minsizetile = min(tilewidth, tileheight);
        int maxlev2 = 10;

        if (minsizetile < 1024 && maxlevelcrop == 10) {
            maxlev2 = 9;
        }

        if (minsizetile < 512) {
            maxlev2 = 8;
        }

        if (minsizetile < 256) {
            maxlev2 = 7;
        }

        if (minsizetile < 128) {
            maxlev2 = 6;
        }

        int maxL = min(maxlev2, maxlevelcrop);

        if (parent->awavListener) {
            parent->awavListener->wavChanged(float (maxL));
        }

        WavCurve wavCLVCurve;
        WavCurve wavdenoise;
        WavCurve wavdenoiseh;
        Wavblcurve wavblcurve;
        WavOpacityCurveRG waOpacityCurveRG;
        WavOpacityCurveSH waOpacityCurveSH;
        WavOpacityCurveBY waOpacityCurveBY;
        WavOpacityCurveW waOpacityCurveW;
        WavOpacityCurveWL waOpacityCurveWL;
        LUTf wavclCurve;

        params.wavelet.getCurves(wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL);
        LabImage *unshar = nullptr;
        Glib::ustring provis;
        LabImage *provradius = nullptr;
        bool procont = WaveParams.expcontrast;
        bool prochro = WaveParams.expchroma;
        bool proedge = WaveParams.expedge;
        bool profin = WaveParams.expfinal;
        bool proton = WaveParams.exptoning;
        bool pronois = WaveParams.expnoise;

        if (WaveParams.showmask) {
       //     WaveParams.showmask = false;
       //     WaveParams.expclari = true;
        }


        if (WaveParams.softrad > 0.f) {
            provradius = new LabImage(*labnCrop, true);
        }



        if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {

            provis = params.wavelet.CLmethod;
            params.wavelet.CLmethod = "all";
            parent->ipf.ip_wavelet(labnCrop, labnCrop, kall, WaveParams, wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, skip);
            unshar = new LabImage(*labnCrop, true);

            params.wavelet.CLmethod = provis;

            WaveParams.expcontrast = false;
            WaveParams.expchroma = false;
            WaveParams.expedge = false;
            WaveParams.expfinal = false;
            WaveParams.exptoning = false;
            WaveParams.expnoise = false;
        }



//        parent->ipf.ip_wavelet(labnCrop, labnCrop, kall, WaveParams, wavCLVCurve, wavblcurve, waOpacityCurveRG, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, skip);

    parent->ipf.ip_wavelet(labnCrop, labnCrop, kall, WaveParams, wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, parent->wavclCurve, skip);

        if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {
            WaveParams.expcontrast = procont;
            WaveParams.expchroma = prochro;
            WaveParams.expedge = proedge;
            WaveParams.expfinal = profin;
            WaveParams.exptoning = proton;
            WaveParams.expnoise = pronois;
            if (WaveParams.softrad > 0.f) {
                array2D<float> ble(labnCrop->W, labnCrop->H);
                array2D<float> guid(labnCrop->W, labnCrop->H);
                Imagefloat *tmpImage = nullptr;
                tmpImage = new Imagefloat(labnCrop->W, labnCrop->H);

#ifdef _OPENMP
                #pragma omp parallel for
#endif

                for (int ir = 0; ir < labnCrop->H ; ir++)
                    for (int jr = 0; jr < labnCrop->W; jr++) {
                        float X, Y, Z;
                        float L = provradius->L[ir][jr];
                        float a = provradius->a[ir][jr];
                        float b = provradius->b[ir][jr];
                        Color::Lab2XYZ(L, a, b, X, Y, Z);

                        guid[ir][jr] = Y / 32768.f;
                        float La = labnCrop->L[ir][jr];
                        float aa = labnCrop->a[ir][jr];
                        float ba = labnCrop->b[ir][jr];
                        Color::Lab2XYZ(La, aa, ba, X, Y, Z);
                        tmpImage->r(ir, jr) = X;
                        tmpImage->g(ir, jr) = Y;
                        tmpImage->b(ir, jr) = Z;
                        ble[ir][jr] = Y / 32768.f;
                    }
                double epsilmax = 0.0001;
                double epsilmin = 0.00001;
                double aepsil = (epsilmax - epsilmin) / 100.f;
                double bepsil = epsilmin; //epsilmax - 100.f * aepsil;
                double epsil = aepsil * WaveParams.softrad + bepsil;

                float blur = 10.f / skip * (0.5f + 0.8f * WaveParams.softrad);
                rtengine::guidedFilter(guid, ble, ble, blur, epsil, false);



#ifdef _OPENMP
                #pragma omp parallel for
#endif

                for (int ir = 0; ir < labnCrop->H; ir++)
                    for (int jr = 0; jr < labnCrop->W; jr++) {
                        float X = tmpImage->r(ir, jr);
                        float Y = 32768.f * ble[ir][jr];
                        float Z = tmpImage->b(ir, jr);
                        float L, a, b;
                        Color::XYZ2Lab(X, Y, Z, L, a, b);
                        labnCrop->L[ir][jr] = L;
                    }
                delete tmpImage;
            }

        }


        if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari")  && WaveParams.expclari && WaveParams.CLmethod != "all") {

            float mL = (float)(WaveParams.mergeL / 100.f);
            float mC = (float)(WaveParams.mergeC / 100.f);
            float mL0;
            float mC0;
            float background = 0.f;
            int show = 0;

            if ((WaveParams.CLmethod == "one" || WaveParams.CLmethod == "inf")  && WaveParams.Backmethod == "black") {
                mL0 = mC0 = 0.f;
                mL = -1.5f * mL;
                mC = -mC;
                background = 12000.f;
                show = 0;
            } else if (WaveParams.CLmethod == "sup" && WaveParams.Backmethod == "resid") {
                mL0 = mL;
                mC0 = mC;
                background = 0.f;
                show = 0;
            } else {
                mL0 = mL = mC0 = mC = 0.f;
                background = 0.f;
                show = 0;
            }

            float indic = 1.f;
            if (WaveParams.showmask){
                mL0 = mC0 = -1.f;
                indic = -1.f;
                mL = fabs(mL);
                mC = fabs(mC);
                show = 1;
            }

#ifdef _OPENMP
            #pragma omp parallel for
#endif

            for (int x = 0; x < labnCrop->H; x++)
                for (int y = 0; y < labnCrop->W; y++) {
                    labnCrop->L[x][y] = LIM((1.f + mL0) * (unshar->L[x][y]) + show * background - mL * indic * labnCrop->L[x][y], 0.f, 32768.f);
                    labnCrop->a[x][y] = (1.f + mC0) * (unshar->a[x][y]) - mC * indic * labnCrop->a[x][y];
                    labnCrop->b[x][y] = (1.f + mC0) * (unshar->b[x][y]) - mC * indic * labnCrop->b[x][y];
                }

            delete unshar;
            unshar    = NULL;
            if (WaveParams.softrad > 0.f) {
                delete provradius;
                provradius    = NULL;
            }

        }

    }
}

void Crop::stageWorkingTrc(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    if ((todo & (M_LUMINANCE + M_COLOR)) == 0) {
        return;
    }
    parent->ipf.softLight(labnCrop, params.softlight);

    if (params.icm.workingTRC != ColorManagementParams::WorkingTrc::NONE && params.icm.trcExp) {
        const int GW = labnCrop->W;
        const int GH = labnCrop->H;
        if(params.icm.trcExp) {//local contrast
            int level_hr = 7;
            int maxlevpo = 9;
            bool wavcurvecont = false;
            WaveletParams WaveParams = params.wavelet;
            ColorManagementParams Colparams = params.icm;
            WavOpacityCurveWL icmOpacityCurveWL;
            Colparams.getCurves(icmOpacityCurveWL);
            parent->ipf.complete_local_contrast(labnCrop, labnCrop, WaveParams, Colparams, icmOpacityCurveWL, skip, level_hr, maxlevpo, wavcurvecont);
            bool enall = false;
            enall = wavcurvecont && Colparams.wavExp;//enable message only if curve enable and Expander on
            if (parent->primListener) {
                parent->primListener->wavlocChanged(float (maxlevpo), float (level_hr), enall);
            }

        }

        std::unique_ptr<LabImage> provis;
        const float pres = 0.01f * params.icm.preser;
        if (pres > 0.f && params.icm.wprim != ColorManagementParams::Primaries::DEFAULT) {
            provis.reset(new LabImage(GW, GH));
            provis->CopyFrom(labnCrop);
        }

        const std::unique_ptr<Imagefloat> tmpImage1(new Imagefloat(GW, GH));
        const std::unique_ptr<Imagefloat> tmpImage2(new Imagefloat(GW, GH));

        parent->ipf.lab2rgb(*labnCrop, *tmpImage1, params.icm.workingProfile);
        tmpImage1.get()->copyData(tmpImage2.get());

        const float gamtone = parent->params->icm.wGamma;
        const float slotone = parent->params->icm.wSlope;

        int illum = rtengine::toUnderlying(params.icm.will);
        const int prim = rtengine::toUnderlying(params.icm.wprim);

        Glib::ustring prof = params.icm.workingProfile;

        cmsHTRANSFORM cmsDummy = nullptr;
        int ill = 0;
        int locprim = 0;
        bool gamutcontrol = params.icm.gamut;
        int catc = rtengine::toUnderlying(params.icm.wcat);

        const int midton = params.icm.wmidtcie;

        if(midton != 0) {
            ToneEqualizerParams params;
            params.enabled = true;
            params.regularization = 0.f;
            params.pivot = 0.f;
            params.bands[0] = 0;
            params.bands[2] = midton;
            params.bands[4] = 0;
            params.bands[5] = 0;
            int mid = abs(midton);
            int threshmid = 50;
            if(mid > threshmid) {
                params.bands[1] = sign(midton) * (mid - threshmid);
                params.bands[3] = sign(midton) * (mid - threshmid);
            }
            parent->ipf.toneEqualizer(tmpImage1.get(), params, prof, skip, false);
        }

        float rdx, rdy, grx, gry, blx, bly = 0.f;
        float meanx, meany, meanxe, meanye, maxdat = 0.f;
        double p[6] = {0., 0., 0., 0., 0., 0.};

        parent->ipf.workingtrc(0, tmpImage1.get(), tmpImage1.get(), GW, GH, -5, prof, 2.4, 12.92310, 0, ill, 0, 0, rdx, rdy, grx, gry, blx, bly,meanx, meany, meanxe, meanye, maxdat, p, cmsDummy, true, false, false, false);
        parent->ipf.workingtrc(0, tmpImage1.get(), tmpImage1.get(), GW, GH, 5, prof, gamtone, slotone, catc,  illum, prim, locprim, rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, p, cmsDummy, false, true, true, gamutcontrol);
        float satu = params.icm.wapsat;
        if(satu > 0.f) {
            parent->ipf.apsatur(0, tmpImage1.get(), tmpImage2.get(), GW, GH, satu) ;
        }


        const float smoothisli = params.icm.wsmoothciesli;
        if(smoothisli > 0.f) {
            ToneEqualizerParams params;
            params.enabled = true;
            params.regularization = 0.f;
            params.pivot = 0.f;
            params.bands[0] = 0;
            params.bands[1] = 0;
            params.bands[2] = 0;
            params.bands[3] = 0;
            params.bands[4] = -40;//arbitrary value to adapt with WhiteEvjz - here White Ev # 10
            params.bands[5] = -80;//8 Ev and above
            bool Evsix = true;
            if(Evsix) {//EV = 6 majority of images
                params.bands[4] = -30 * smoothisli;
                float smmothsli5 = std::min(smoothisli, 1.f);
                params.bands[5] = -80 * smmothsli5;
            }

            parent->ipf.toneEqualizer(tmpImage1.get(), params, prof, skip, false);
        }

        parent->ipf.rgb2lab(*tmpImage1, *labnCrop, params.icm.workingProfile);
        //labnCrop and provis
        if (provis) {
            parent->ipf.preserv(labnCrop, provis.get(), GW, GH);
        }
        if (params.icm.fbw) {
#ifdef _OPENMP
        #pragma omp parallel for
#endif
        for (int x = 0; x < GH; x++)
            for (int y = 0; y < GW; y++) {
                labnCrop->a[x][y] = 0.f;
                labnCrop->b[x][y] = 0.f;
            }
        }
    }
}

void Crop::stageCieCam(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    LUTu& dummy = ctx.dummy;

    if ((todo & (M_LUMINANCE + M_COLOR)) == 0) {
        return;
    }
    if (params.colorappearance.enabled) {
        float fnum = parent->imgsrc->getMetaData()->getFNumber();          // F number
        float fiso = parent->imgsrc->getMetaData()->getISOSpeed() ;        // ISO
        float fspeed = parent->imgsrc->getMetaData()->getShutterSpeed() ;  // Speed
        double fcomp = parent->imgsrc->getMetaData()->getExpComp();        // Compensation +/-
        double adap; // Scene's luminosity adaptation factor

        if (fnum < 0.3f || fiso < 5.f || fspeed < 0.00001f) { //if no exif data or wrong
            adap = 2000.;
        } else {
            double E_V = fcomp + log2(double ((fnum * fnum) / fspeed / (fiso / 100.f)));
            double kexp = 0.;
            E_V += kexp * params.toneCurve.expcomp;// exposure compensation in tonecurve ==> direct EV
            E_V += 0.5 * log2(params.raw.expos);  // exposure raw white point ; log2 ==> linear to EV
            adap = pow(2., E_V - 3.);  // cd / m2
            // end calculation adaptation scene luminosity
        }

        bool execsharp = false;

        if (skip == 1) {
            execsharp = true;
        }

        if (!cieCrop) {
            cieCrop = new CieImage(cropw, croph);
        }

        float d, dj, yb; // not used after this block
        parent->ipf.ciecam_02float(cieCrop, float (adap), 1, 2, labnCrop, &params, parent->customColCurve1, parent->customColCurvered, parent->customColCurvegreen,  parent->customColCurveblue, parent->customColCurve2, parent->customColCurve3,
                                   dummy, dummy, parent->CAMBrightCurveJ, parent->CAMBrightCurveQ, parent->CAMMean, 0, skip, execsharp, d, dj, yb, 1, parent->sharpMask);
    } else {
        // CIECAM is disabled, we free up its image buffer to save some space
        if (cieCrop) {
            delete cieCrop;
        }

        cieCrop = nullptr;
    }
}

void Crop::stageGamut(CropStageContext& ctx)
{
    ProcParams& params = ctx.params;
    const int todo = ctx.todo;
    if ((todo & (M_LUMINANCE + M_COLOR)) == 0) {
        return;
    }
    bool exec = params.icm.wgamut != ColorManagementParams::Wwgamut::NONE  || params.icm.wgamgain != 0.f;

    if (params.icm.workingTRC != ColorManagementParams::WorkingTrc::NONE && params.icm.trcExp  && exec) {

        //compression gamut and gain at the end of process
        const int GW = labnCrop->W;
        const int GH = labnCrop->H;
        TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(params.icm.workingProfile);
            const double wp[3][3] = {
                {wprof[0][0], wprof[0][1], wprof[0][2]},
                {wprof[1][0], wprof[1][1], wprof[1][2]},
                {wprof[2][0], wprof[2][1], wprof[2][2]}
            };
        TMatrix wiprof = ICCStore::getInstance()->workingSpaceInverseMatrix(params.icm.workingProfile);
            const double wip[3][3] = {//improve precision with double
                {wiprof[0][0], wiprof[0][1], wiprof[0][2]},
                {wiprof[1][0], wiprof[1][1], wiprof[1][2]},
                {wiprof[2][0], wiprof[2][1], wiprof[2][2]}
            };

        std::unique_ptr<Imagefloat> provcomp(new Imagefloat(GW, GH));

#ifdef _OPENMP
    #   pragma omp parallel for
#endif
        for (int i = 0; i < GH; ++i){
            for (int j = 0; j < GW; ++j) {
                float X, Y, Z = 0.f;
                Color::Lab2XYZ(labnCrop->L[i][j], labnCrop->a[i][j], labnCrop->b[i][j] , X, Y, Z);
                Color::xyz2rgb(X, Y, Z, provcomp->r(i, j), provcomp->g(i, j), provcomp->b(i, j), wip);
            }
        }

        const float gainev = pow_F(2.f, (float) params.icm.wgamgain);
        if (params.icm.wgamgain != 0.f) {//Final gain in Ev

#ifdef _OPENMP
    #   pragma omp parallel for
#endif
            for (int i = 0; i < GH; ++i){
                for (int j = 0; j < GW; ++j) {
                    provcomp->r(i, j) *= gainev;
                    provcomp->g(i, j) *= gainev;
                    provcomp->b(i, j) *= gainev;
                }
            }
        }

        float mac = 0.f;
        float mac0 = 0.f;
        float mac1 = 0.f;
        float mac2 = 0.f;
        int beginend = 1;
        int nbsegam = 0;
        float powe = 1.f;
        if (params.icm.wgamut != ColorManagementParams::Wwgamut::NONE) {
            parent->ipf.gamutcompr(provcomp.get(), provcomp.get(), beginend, powe, nbsegam, mac, mac0, mac1, mac2);
        }

#ifdef _OPENMP
    #   pragma omp parallel for
#endif
        for (int i = 0; i < GH; ++i){
            for (int j = 0; j < GW; ++j) {
                float x, y, z = 0.f;
                Color::rgbxyz (provcomp->r(i, j), provcomp->g(i, j), provcomp->b(i, j), x, y, z, wp);
                Color::XYZ2Lab(x, y, z, labnCrop->L[i][j], labnCrop->a[i][j], labnCrop->b[i][j]);
        }
        }
    }
}

void Crop::stageViewportOutput(CropStageContext& ctx)
{
    // This block only publishes the Lab buffers it was
    // given; it reads no processing parameter.
    (void)ctx;

    // all pipette buffer processing should be finished now
    PipetteBuffer::setReady();



    // Computing the preview image, i.e. converting from lab->Monitor color space (soft-proofing disabled) or lab->Output profile->Monitor color space (soft-proofing enabled)
    parent->ipf.lab2monitorRgb(labnCrop, cropImg);

    // Opt-in parity hook: the dump carries the whole crop buffer including
    // the processing border, plus the clamped visible rectangle in its
    // descriptor. The requested crop can be larger than the allocated buffer
    // when the image edge falls inside the request.
    if (preview_dump::enabled() && cropImg != nullptr) {
        const int visibleW =
            std::min(rqcropw, cropImg->getWidth() - leftBorder);
        const int visibleH =
            std::min(rqcroph, cropImg->getHeight() - upperBorder);
        preview_dump::dump_crop(cropImg,
                                isDetailWindow ? "detail" : "main",
                                leftBorder, upperBorder,
                                std::max(0, visibleW), std::max(0, visibleH),
                                skip);
    }
}

void Crop::freeAll()
{

    if (cropAllocated) {
        if (origCrop) {
            delete    origCrop;
            origCrop = nullptr;
        }

        if (transCrop) {
            delete    transCrop;
            transCrop = nullptr;
        }

        if (laboCrop) {
            delete    laboCrop;
            laboCrop = nullptr;
        }


        if (labnCrop) {
            delete    labnCrop;
            labnCrop = nullptr;
        }

        if (cropImg) {
            delete    cropImg;
            cropImg = nullptr;
        }

        if (cieCrop) {
            delete    cieCrop;
            cieCrop = nullptr;
        }

        if (shbuffer) {
            delete [] shbuffer;
            shbuffer = nullptr;
        }

        if (shbuf_real) {
            delete [] shbuf_real;
            shbuf_real = nullptr;
        }

        PipetteBuffer::flush();
    }

    cropAllocated = false;
}


namespace
{

bool check_need_larger_crop_for_lcp_distortion(int fw, int fh, int x, int y, int w, int h, const procparams::ProcParams &params)
{
    if (x == 0 && y == 0 && w == fw && h == fh) {
        return false;
    }

    return (params.lensProf.useDist && (params.lensProf.useLensfun() || params.lensProf.useLcp() || params.lensProf.useMetadata()));
}

} // namespace

/** @brief Handles crop's image buffer reallocation and trigger sizeChanged of SizeListener[s]
 * If the scale changes, this method will free all buffers and reallocate ones of the new size.
 * It will then tell to the SizeListener that size has changed (sizeChanged)
 */
bool Crop::setCropSizes(const int cropX, const int cropY, const int cropW, const int cropH, const int skip, const bool internal)
{

    if (!internal) {
        cropMutex.lock();
    }

    bool changed = false;

    rqcropx = cropX;
    rqcropy = cropY;
    rqcropw = cropW;
    rqcroph = cropH;

    // store and set requested crop size
    int rqx1 = LIM(rqcropx, 0, parent->fullw - 1);
    int rqy1 = LIM(rqcropy, 0, parent->fullh - 1);
    int rqx2 = rqx1 + rqcropw - 1;
    int rqy2 = rqy1 + rqcroph - 1;
    rqx2 = LIM(rqx2, 0, parent->fullw - 1);
    rqy2 = LIM(rqy2, 0, parent->fullh - 1);

    this->skip = skip;

    // add border, if possible
    int bx1 = rqx1 - skip * borderRequested;
    int by1 = rqy1 - skip * borderRequested;
    int bx2 = rqx2 + skip * borderRequested;
    int by2 = rqy2 + skip * borderRequested;
    // clip it to fit into image area
    bx1 = LIM(bx1, 0, parent->fullw - 1);
    by1 = LIM(by1, 0, parent->fullh - 1);
    bx2 = LIM(bx2, 0, parent->fullw - 1);
    by2 = LIM(by2, 0, parent->fullh - 1);
    int bw = bx2 - bx1 + 1;
    int bh = by2 - by1 + 1;

    // determine which part of the source image is required to compute the crop rectangle
    int orx, ory, orw, orh;
    orx = bx1;
    ory = by1;
    orw = bw;
    orh = bh;

    parent->ipf.transCoord(parent->fw, parent->fh, bx1, by1, bw, bh, orx, ory, orw, orh);

    if (parent->ipf.needsTransform(skips(parent->fw, skip), skips(parent->fh, skip), parent->imgsrc->getRotateDegree(), parent->imgsrc->getMetaData())) {
        if (check_need_larger_crop_for_lcp_distortion(parent->fw, parent->fh, orx, ory, orw, orh, *parent->params)) {
            // TODO - this is an estimate of the max distortion relative to the image size. ATM it is hardcoded to be 15%, which seems enough. If not, need to revise
            int dW = int (double (parent->fw) * 0.15 / (2 * skip));
            int dH = int (double (parent->fh) * 0.15 / (2 * skip));
            int x1 = orx - dW;
            int x2 = orx + orw + dW;
            int y1 = ory - dH;
            int y2 = ory + orh + dH;

            if (x1 < 0) {
                x2 += -x1;
                x1 = 0;
            }

            if (x2 > parent->fw) {
                x1 -= x2 - parent->fw;
                x2 = parent->fw;
            }

            if (y1 < 0) {
                y2 += -y1;
                y1 = 0;
            }

            if (y2 > parent->fh) {
                y1 -= y2 - parent->fh;
                y2 = parent->fh;
            }

            orx = max(x1, 0);
            ory = max(y1, 0);
            orw = min(x2 - x1, parent->fw - orx);
            orh = min(y2 - y1, parent->fh - ory);
        }
    }
    leftBorder  = skips(rqx1 - bx1, skip);
    upperBorder = skips(rqy1 - by1, skip);

    PreviewProps cp(orx, ory, orw, orh, skip);
    int orW, orH;
    parent->imgsrc->getSize(cp, orW, orH);

    if (trafx != orx || trafy != ory) {
        trafx = orx;
        trafy = ory;
        changed = true;
    }

    int cw = skips(bw, skip);
    int ch = skips(bh, skip);

    EditType editType = ET_PIPETTE;

    if (const auto editProvider = PipetteBuffer::getDataProvider()) {
        if (const auto editSubscriber = editProvider->getCurrSubscriber()) {
            editType = editSubscriber->getEditingType();
        }
    }

    if (cw != cropw || ch != croph || orW != trafw || orH != trafh) {

        cropw = cw;
        croph = ch;
        trafw = orW;
        trafh = orH;

        if (!origCrop) {
            origCrop = new Imagefloat;
        }

        origCrop->allocate(trafw, trafh);  // Resizing the buffer (optimization)

        // if transCrop doesn't exist yet, it'll be created where necessary
        if (transCrop) {
            transCrop->allocate(cropw, croph);
        }

        if (laboCrop) {
            delete laboCrop;    // laboCrop can't be resized
        }

        laboCrop = new LabImage(cropw, croph);

        //     if (translabCrop) translabCrop->reallocLab();

        if (labnCrop) {
            delete labnCrop;    // labnCrop can't be resized
        }

        labnCrop = new LabImage(cropw, croph);

        if (!cropImg) {
            cropImg = new Image8;
        }

        cropImg->allocate(cropw, croph);  // Resizing the buffer (optimization)

        //cieCrop is only used in Crop::update, it is destroyed now but will be allocated on first use
        if (cieCrop) {
            delete cieCrop;
            cieCrop = nullptr;
        }

        if (shbuffer) {
            delete [] shbuffer;
        }

        if (shbuf_real) {
            delete [] shbuf_real;
        }

        shbuffer = new float*[croph];
        shbuf_real = new float[(croph + 2)*cropw];

        for (int i = 0; i < croph; i++) {
            shbuffer[i] = shbuf_real + cropw * i + cropw;
        }

        if (editType == ET_PIPETTE) {
            PipetteBuffer::resize(cropw, croph);
        } else if (PipetteBuffer::bufferCreated()) {
            PipetteBuffer::flush();
        }

        cropAllocated = true;

        changed = true;
    }

    cropx = bx1;
    cropy = by1;

    if (!internal) {
        cropMutex.unlock();
    }

    return changed;
}

/** @brief Look out if a new thread has to be started to process the update
  *
  * @return If true, a new updating thread has to be created. If false, the current updating thread will be used
  */
bool Crop::tryUpdate()
{
    bool needsNewThread = true;

    if (updating) {
        // tells to the updater thread that a new update is pending
        newUpdatePending = true;
        // no need for a new thread, the current one will do the job
        needsNewThread = false;
    } else
        // the crop is now being updated ...well, when fullUpdate will be called
    {
        updating = true;
    }

    return needsNewThread;
}

/* @brief Handles Crop updating in its own thread
 *
 * This method will cycle updates as long as Crop::newUpdatePending will be true. During the processing,
 * intermediary update will be automatically flushed by Crop::tryUpdate.
 *
 * This method is called when the visible part of the crop has changed (resize, zoom, etc..), so it needs a full update
 */
void Crop::fullUpdate()
{

    parent->updaterThreadStart.lock();

    if (parent->updaterRunning && parent->thread) {
        // Do NOT reset changes here, since in a long chain of events it will lead to chroma_scale not being updated,
        // causing Color::lab2rgb to return a black image on some opens
        //parent->changeSinceLast = 0;
        parent->thread->join();
    }

    if (parent->plistener) {
        parent->plistener->setProgressState(true);
    }

    // If there are more update request, the following WHILE will collect it
    newUpdatePending = true;

    while (newUpdatePending) {
        newUpdatePending = false;
        update(ALL);
    }

    updating = false;  // end of crop update

    if (parent->plistener) {
        parent->plistener->setProgressState(false);
    }

    parent->updaterThreadStart.unlock();
}

int Crop::get_skip()
{
    MyMutex::MyLock lock(cropMutex);
    return skip;
}

int Crop::getLeftBorder()
{
    MyMutex::MyLock lock(cropMutex);
    return leftBorder;
}

int Crop::getUpperBorder()
{
    MyMutex::MyLock lock(cropMutex);
    return upperBorder;
}

}
