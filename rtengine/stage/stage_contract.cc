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
 * Canonical stage contract.
 *
 * The block table is the single definition of the processor vocabulary. The
 * parameter fields are the members the matching hasher in stage_param_map.cc
 * reads, so the published map and the fingerprint cannot drift apart; the
 * fold, resident, coupled-group and work-item columns are the ones the
 * dirty-block derivation, the work order and the executors consume.
 *
 * The path table is the single definition of the five pipelines. The block
 * table order is a review order; a path's node list is its execution order.
 */

#include "stage_contract.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace rtengine::stage
{

namespace
{

/* The canonical block catalogue. Every id is defined exactly once. */
const std::vector<StageBlockContract>& blockTable()
{
    static const std::vector<StageBlockContract> table = {
        // id                    display name              reuse                  ownership                     effects                                                          working set                                                       params
        {"source.select",        "Select source",          RT_STAGE_RECOMPUTE,
         StageOwnership::Produces, StageEffect::SourceFrame, StageWorkingSet::None,
         {"raw.bayersensor.imageNum"}, {WorkItem::sourceSelect}, {}, {}, true},
        {"source.embedded",      "Select embedded JPEG",   RT_STAGE_RECOMPUTE,
         StageOwnership::Produces, StageEffect::SourceFrame, StageWorkingSet::None,
         {}, {}, {}, {}},
        {"raw.preprocess",       "Raw preprocess",         RT_STAGE_REUSE_OUTPUT,
         StageOwnership::InPlace,
         StageEffect::SourceFrame | StageEffect::RawPlanes,
         StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"raw.bayersensor.black0", "raw.bayersensor.black1",
          "raw.bayersensor.black2", "raw.bayersensor.black3",
          "raw.bayersensor.twogreen", "raw.bayersensor.Dehablack",
          "raw.bayersensor.greenthresh", "raw.bayersensor.linenoise",
          "raw.bayersensor.linenoiseDirection",
          "raw.bayersensor.pdafLinesFilter", "raw.bayersensor.border",
          "raw.xtranssensor.blackred", "raw.xtranssensor.blackgreen",
          "raw.xtranssensor.blackblue", "raw.xtranssensor.Dehablackx",
          "raw.dark_frame", "raw.df_autoselect", "raw.ff_file",
          "raw.ff_AutoSelect", "raw.ff_FromMetaData",
          "raw.ff_BlurRadius", "raw.ff_BlurType",
          "raw.ff_AutoClipControl", "raw.ff_clipControl",
          "raw.ca_autocorrect", "raw.ca_avoidcolourshift",
          "raw.caautoiterations", "raw.cared", "raw.cablue", "raw.expos",
          "raw.preprocessWB", "raw.hotPixelFilter", "raw.deadPixelFilter",
          "raw.hotdeadpix_thresh", "lensProf.lcMode", "lensProf.lcpFile",
          "lensProf.useVign", "lensProf.lfCameraMake",
          "lensProf.lfCameraModel", "lensProf.lfLens"},
         {WorkItem::rawPreprocess},
         {{StagePath::Crop, "source.select"}}, {}, true},
        {"raw.demosaic",         "Demosaic",               RT_STAGE_REUSE_OUTPUT,
         StageOwnership::InPlace,
         StageEffect::SourceFrame | StageEffect::RawPlanes,
         StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"raw.bayersensor.method", "raw.bayersensor.border",
          "raw.bayersensor.dcb_iterations", "raw.bayersensor.dcb_enhance",
          "raw.bayersensor.lmmse_iterations",
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
          "raw.xtranssensor.method", "raw.xtranssensor.border",
          "raw.xtranssensor.dualDemosaicAutoContrast",
          "raw.xtranssensor.dualDemosaicContrast", "raw.expos",
          "pdsharpening"},
         {WorkItem::rawDemosaic},
         {{StagePath::Crop, "source.select"}}, {}, true},
        {"raw.base",             "Base image finish",      RT_STAGE_REUSE_OUTPUT,
         StageOwnership::InPlace,
         StageEffect::RawPlanes | StageEffect::BaseImage | StageEffect::SpotBuffer | StageEffect::Analysis,
         StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"raw", "wb", "toneCurve", "spot", "dirpyrDenoise", "filmNegative",
          "retinex", "dehaze", "fattal", "cg", "icm"},
         {WorkItem::rawBaseRebuild, WorkItem::retinexPrepare,
          WorkItem::spotApply, WorkItem::spotRestore, WorkItem::analysisRun,
          WorkItem::denoiseRun},
         {}, {StagePath::Crop}},
        {"analysis",             "Analysis",               RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::Analysis, StageWorkingSet::None,
         {"icm", "toneCurve", "dirpyrDenoise"},
         {WorkItem::analysisRun},
         {{StagePath::Preview, "raw.base"}}, {}},
        {"transform",            "Geometry transform",     RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::TransformMap,
         {"coarse", "commonTrans", "rotate", "distortion", "perspective",
          "cacorrection", "vignetting", "gradient", "pcvignette", "lensProf",
          "crop"},
         {}, {}, {StagePath::Crop}},
        {"local.adjustments",    "Local adjustments",      RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes | StageWorkingSet::DoubleState,
         {"locallab", "icm", "toneCurve", "raw", "wb", "dirpyrequalizer",
          "dehaze", "epd", "rgbCurves", "hsvequalizer", "chmixer",
          "colorToning"},
         {}, {}, {}},
        {"rgb.proc",             "RGB processing",         RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"toneCurve", "rgbCurves", "colorToning", "blackwhite", "chmixer",
          "hsvequalizer", "filmSimulation", "icm"},
         {}, {}, {}},
        {"lab.tools",            "Lab tools",              RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::DoubleState,
         {"sh", "labCurve", "localContrast", "vibrance", "epd", "softlight",
          "icm", "toneCurve"},
         {}, {}, {}},
        {"lab.sharpening",       "Sharpening",             RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"sharpening", "sharpenEdge", "sharpenMicro", "impulseDenoise",
          "defringe", "icm"},
         {}, {}, {}},
        {"lab.wavelet",          "Wavelet",                RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"wavelet", "dirpyrequalizer", "icm", "toneCurve"},
         {}, {}, {}},
        {"working.trc",          "Working TRC",            RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"toneEqualizer", "icm", "wavelet"},
         {}, {}, {}},
        {"ciecam",               "CIE CAM",                RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"colorappearance", "icm", "toneCurve", "raw", "epd"},
         {}, {}, {}},
        {"gamut",                "Gamut compression",      RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"cg", "icm"},
         {}, {}, {}},
        {"output.convert",       "Output conversion",      RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::None, StageWorkingSet::Image,
         {"icm"}, {}, {}, {}},
        {"crop",                 "Crop",                   RT_STAGE_REUSE_OUTPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"crop"}, {}, {}, {}},
        {"resize",               "Resize",                 RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::TapTables,
         {"resize", "crop", "framing"},
         {}, {}, {}},
        {"output.sharpen",       "Output sharpening",      RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"prsharpening"}, {}, {}, {}},
        {"framing",              "Framing",                RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::None, StageWorkingSet::Image,
         {"framing", "crop", "resize"},
         {}, {}, {}},
        {"output.icc",           "Output ICC",             RT_STAGE_RECOMPUTE,
         StageOwnership::Consumes, StageEffect::Metadata, StageWorkingSet::None,
         {"icm", "metadata"}, {}, {}, {}},
        {"monitor.output",       "Monitor output",         RT_STAGE_RECOMPUTE,
         StageOwnership::Consumes, StageEffect::Display, StageWorkingSet::Image,
         {"icm"}, {}, {}, {StagePath::Preview}},
        {"viewport.output",      "Viewport output",        RT_STAGE_RECOMPUTE,
         StageOwnership::Consumes, StageEffect::Display, StageWorkingSet::Image,
         {"icm"}, {}, {}, {StagePath::Crop}},
        {"thumbnail.decode",     "Decode embedded JPEG",   RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::None, StageWorkingSet::Image,
         {"raw"}, {}, {}, {}},
        {"thumbnail.scale",      "Thumbnail scale",        RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::None, StageWorkingSet::Image,
         {"filmNegative"}, {}, {}, {}},
        {"thumbnail.transform",  "Thumbnail coarse transform", RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"coarse"}, {}, {}, {}},
        {"thumbnail.whitebalance", "Thumbnail white balance", RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"wb", "raw", "toneCurve"}, {}, {}, {}},
        {"thumbnail.rgb",        "Thumbnail RGB processing", RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"toneCurve", "rgbCurves", "colorToning", "blackwhite", "chmixer",
          "hsvequalizer", "filmSimulation", "icm"},
         {}, {}, {}},
        {"thumbnail.lab",        "Thumbnail Lab processing", RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::None, StageWorkingSet::Image,
         {"labCurve", "sh", "localContrast", "vibrance", "epd", "softlight",
          "toneEqualizer", "colorappearance", "cg", "icm"},
         {}, {}, {}},
        {"thumbnail.output",     "Thumbnail output",       RT_STAGE_RECOMPUTE,
         StageOwnership::Consumes, StageEffect::Display, StageWorkingSet::Image,
         {"icm"}, {}, {}, {}},
        // Split of raw.base in the canonical export. The export names the
        // three real sub-blocks; the fast export reuses raw.base.source and
        // raw.color and runs denoise after its early resize.
        {"raw.base.source",      "Base image source",      RT_STAGE_REUSE_OUTPUT,
         StageOwnership::InPlace,
         StageEffect::BaseImage | StageEffect::SpotBuffer | StageEffect::Analysis,
         StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"raw", "wb", "toneCurve", "spot", "dirpyrDenoise", "retinex",
          "dehaze", "fattal", "icm"},
         {WorkItem::rawBaseRebuild, WorkItem::retinexPrepare,
          WorkItem::spotApply, WorkItem::spotRestore},
         {}, {}},
        {"raw.denoise",          "Denoise",                RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::BaseImage, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"dirpyrDenoise", "icm", "raw"},
         {WorkItem::denoiseRun}, {}, {}},
        {"raw.color",            "Source colour conversion", RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::BaseImage, StageWorkingSet::Image,
         {"filmNegative", "cg", "icm", "raw"},
         {}, {}, {}},
        {"early.resize",         "Early resize",           RT_STAGE_REUSE_OUTPUT,
         StageOwnership::Produces, StageEffect::None, StageWorkingSet::Image | StageWorkingSet::TapTables,
         {"resize", "crop", "icm", "prsharpening", "sharpening", "wavelet",
          "dirpyrDenoise", "epd", "dirpyrequalizer", "sh", "localContrast",
          "raw"},
         {}, {}, {}},
        {"denoise",              "Denoise",                RT_STAGE_REUSE_INPUT,
         StageOwnership::InPlace, StageEffect::BaseImage, StageWorkingSet::Image | StageWorkingSet::ScratchPlanes,
         {"dirpyrDenoise", "icm", "raw"},
         {WorkItem::denoiseRun}, {}, {}},
        {"finish",               "Finish output",          RT_STAGE_RECOMPUTE,
         StageOwnership::Consumes, StageEffect::None, StageWorkingSet::Image,
         {"locallab", "icm", "metadata"},
         {}, {}, {}}
    };
    return table;
}

using BlockPair = std::pair<const char*, const char*>;

std::vector<BlockPair> chain(const std::vector<const char*>& nodes)
{
    std::vector<BlockPair> edges;
    for (size_t i = 1; i < nodes.size(); ++i) {
        edges.emplace_back(nodes[i - 1], nodes[i]);
    }
    return edges;
}

const StagePathContract& pathContract(StagePath path)
{
    static const StagePathContract exportPath = [] {
        const std::vector<const char*> nodes = {
            "source.select", "raw.preprocess", "raw.demosaic", "raw.base.source",
            "raw.denoise", "raw.color", "analysis", "transform",
            "local.adjustments", "rgb.proc", "lab.tools", "lab.sharpening",
            "lab.wavelet", "working.trc", "ciecam", "gamut", "crop", "resize",
            "output.sharpen", "output.convert", "framing", "output.icc"
        };
        return StagePathContract{StagePath::Export, nodes, chain(nodes),
                                 {"source.select"}, {"output.icc"}, {}};
    }();

    static const StagePathContract fastExportPath = [] {
        const std::vector<const char*> nodes = {
            "source.select", "raw.preprocess", "raw.demosaic", "raw.base.source",
            "raw.color", "analysis", "transform", "early.resize", "denoise",
            "finish"
        };
        return StagePathContract{StagePath::FastExport, nodes, chain(nodes),
                                 {"source.select"}, {"finish"}, {}};
    }();

    static const StagePathContract previewPath = [] {
        const std::vector<const char*> nodes = {
            "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
            "analysis", "transform", "local.adjustments", "rgb.proc",
            "lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut",
            "monitor.output"
        };
        return StagePathContract{
            StagePath::Preview, nodes, chain(nodes), {"source.select"},
            {"monitor.output"},
            {{"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"}}};
    }();

    static const StagePathContract cropPath = [] {
        const std::vector<const char*> nodes = {
            "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
            "analysis", "transform", "local.adjustments", "rgb.proc",
            "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc",
            "ciecam", "gamut", "viewport.output"
        };
        return StagePathContract{
            StagePath::Crop, nodes, chain(nodes), {"source.select"},
            {"viewport.output"},
            {{"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc",
              "ciecam", "gamut"}}};
    }();

    static const StagePathContract thumbnailPath = [] {
        const std::vector<const char*> nodes = {
            "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
            "source.embedded", "thumbnail.decode", "thumbnail.scale",
            "thumbnail.transform", "thumbnail.whitebalance", "thumbnail.rgb",
            "thumbnail.lab", "thumbnail.output"
        };
        return StagePathContract{
            StagePath::Thumbnail, nodes,
            {{"source.select", "raw.preprocess"},
             {"raw.preprocess", "raw.demosaic"},
             {"raw.demosaic", "raw.base"},
             {"raw.base", "thumbnail.scale"},
             {"source.embedded", "thumbnail.decode"},
             {"thumbnail.decode", "thumbnail.scale"},
             {"thumbnail.scale", "thumbnail.transform"},
             {"thumbnail.transform", "thumbnail.whitebalance"},
             {"thumbnail.whitebalance", "thumbnail.rgb"},
             {"thumbnail.rgb", "thumbnail.lab"},
             {"thumbnail.lab", "thumbnail.output"}},
            {"source.select", "source.embedded"}, {"thumbnail.output"}, {}};
    }();

    switch (path) {
    case StagePath::Export:
        return exportPath;
    case StagePath::FastExport:
        return fastExportPath;
    case StagePath::Preview:
        return previewPath;
    case StagePath::Crop:
        return cropPath;
    case StagePath::Thumbnail:
        return thumbnailPath;
    }
    assert(false && "unknown stage path");
    return exportPath;
}

} // namespace

const std::vector<StageBlockContract>& stageBlockContracts()
{
    return blockTable();
}

const StagePathContract& stagePathContract(StagePath path)
{
    return pathContract(path);
}

const StageBlockContract* findStageBlockContract(const std::string& id)
{
    for (const StageBlockContract& block : blockTable()) {
        if (id == block.id) {
            return &block;
        }
    }
    return nullptr;
}

bool isStageBlockId(const std::string& id)
{
    return findStageBlockContract(id) != nullptr;
}

const char* foldHost(StagePath path, const std::string& id)
{
    const StageBlockContract* block = findStageBlockContract(id);
    if (block == nullptr) {
        return nullptr;
    }
    for (const StageFoldRule& fold : block->folds) {
        if (fold.path == path) {
            return fold.host;
        }
    }
    return nullptr;
}

std::vector<std::string> foldedBlockIds(StagePath path)
{
    std::vector<std::string> ids;
    for (const char* id : stagePathContract(path).nodes) {
        if (foldHost(path, id) != nullptr) {
            ids.emplace_back(id);
        }
    }
    return ids;
}

std::vector<std::string> workItemBlocks(StagePath path, WorkItem item)
{
    std::vector<std::string> blocks;
    for (const char* id : stagePathContract(path).nodes) {
        const StageBlockContract* block = findStageBlockContract(id);
        assert(block != nullptr && "a path names an unknown block");
        if (block == nullptr) {
            continue;
        }
        if (std::find(block->workItems.begin(), block->workItems.end(), item)
            != block->workItems.end()) {
            blocks.emplace_back(id);
        }
    }
    return blocks;
}

bool pathSchedulesWorkItem(StagePath path, WorkItem item)
{
    return !workItemBlocks(path, item).empty();
}

std::vector<WorkItem> pathWorkItems(StagePath path)
{
    std::vector<WorkItem> items;
    for (size_t i = 0; i < workItemCount; ++i) {
        const WorkItem item = static_cast<WorkItem>(i);
        if (pathSchedulesWorkItem(path, item)) {
            items.push_back(item);
        }
    }
    return items;
}

const char* stageOwnershipName(StageOwnership ownership)
{
    switch (ownership) {
    case StageOwnership::Produces:
        return "produces";
    case StageOwnership::InPlace:
        return "in-place";
    case StageOwnership::Consumes:
        return "consumes";
    }
    return "unknown";
}

std::string stageEffectName(StageEffect effect)
{
    if (effect == StageEffect::None) {
        return "none";
    }
    struct Named {
        StageEffect effect;
        const char* name;
    };
    static const Named names[] = {
        {StageEffect::SourceFrame, "source-frame"},
        {StageEffect::RawPlanes, "raw-planes"},
        {StageEffect::BaseImage, "base-image"},
        {StageEffect::SpotBuffer, "spot-buffer"},
        {StageEffect::Analysis, "analysis"},
        {StageEffect::Display, "display"},
        {StageEffect::Metadata, "metadata"}
    };
    std::ostringstream out;
    bool first = true;
    for (const Named& named : names) {
        if (!hasEffect(effect, named.effect)) {
            continue;
        }
        if (!first) {
            out << "+";
        }
        out << named.name;
        first = false;
    }
    return out.str();
}

std::string stageWorkingSetName(StageWorkingSet workingSet)
{
    if (workingSet == StageWorkingSet::None) {
        return "none";
    }
    struct Named {
        StageWorkingSet workingSet;
        const char* name;
    };
    static const Named names[] = {
        {StageWorkingSet::Image, "image"},
        {StageWorkingSet::ScratchPlanes, "scratch-planes"},
        {StageWorkingSet::TransformMap, "transform-map"},
        {StageWorkingSet::TapTables, "tap-tables"},
        {StageWorkingSet::DoubleState, "double-state"}
    };
    std::ostringstream out;
    bool first = true;
    for (const Named& named : names) {
        if (!hasWorkingSet(workingSet, named.workingSet)) {
            continue;
        }
        if (!first) {
            out << "+";
        }
        out << named.name;
        first = false;
    }
    return out.str();
}

const char* workItemName(WorkItem item)
{
    switch (item) {
    case WorkItem::sourceSelect:
        return "sourceSelect";
    case WorkItem::rawPreprocess:
        return "rawPreprocess";
    case WorkItem::rawDemosaic:
        return "rawDemosaic";
    case WorkItem::rawBaseRebuild:
        return "rawBaseRebuild";
    case WorkItem::retinexPrepare:
        return "retinexPrepare";
    case WorkItem::spotApply:
        return "spotApply";
    case WorkItem::spotRestore:
        return "spotRestore";
    case WorkItem::analysisRun:
        return "analysisRun";
    case WorkItem::denoiseRun:
        return "denoiseRun";
    case WorkItem::Count:
        break;
    }
    return "unknown";
}

const char* stagePathName(StagePath path)
{
    switch (path) {
    case StagePath::Export:
        return "export";
    case StagePath::FastExport:
        return "fast_export";
    case StagePath::Preview:
        return "preview";
    case StagePath::Crop:
        return "crop";
    case StagePath::Thumbnail:
        return "thumbnail";
    }
    return "unknown";
}

const char* stageDomainName(StageDomain domain)
{
    switch (domain) {
    case StageDomain::Full:
        return "full";
    case StageDomain::Roi:
        return "roi";
    case StageDomain::Reduced:
        return "reduced";
    }
    return "unknown";
}

const char* stageConsumerName(StageConsumer consumer)
{
    switch (consumer) {
    case StageConsumer::Exact:
        return "exact";
    case StageConsumer::Interactive:
        return "interactive";
    }
    return "unknown";
}

const char* stageReuseName(RtReuseMode reuse)
{
    switch (reuse) {
    case RT_STAGE_RECOMPUTE:
        return "recompute";
    case RT_STAGE_REUSE_INPUT:
        return "reuse-input";
    case RT_STAGE_REUSE_OUTPUT:
        return "reuse-output";
    }
    return "unknown";
}

} // namespace rtengine::stage
