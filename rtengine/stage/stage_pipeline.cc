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
#include <initializer_list>
#include <sstream>

namespace rtengine::stage
{

namespace
{

/*
 * Canonical block catalogue. Every pipeline is composed from this list, so a
 * block id, its display name, its reuse contract and its parameter provenance
 * are defined exactly once. Domains and consumers vary by path and are applied
 * by domainFor() and consumerFor().
 *
 * The ids follow the processing order of the engine and group the fine-grained
 * ImProcFunctions calls into reviewable units. procparams.h is the source of
 * the paramFields names.
 */
enum class StageBlock {
    SourceSelect,
    SourceEmbedded,
    RawPreprocess,
    RawDemosaic,
    RawBase,
    Analysis,
    Transform,
    LocalAdjustments,
    RgbProc,
    LabTools,
    LabSharpening,
    LabWavelet,
    WorkingTrc,
    CieCam,
    Gamut,
    OutputConvert,
    Crop,
    Resize,
    OutputSharpen,
    Framing,
    OutputIcc,
    MonitorOutput,
    ViewportOutput,
    ThumbnailDecode,
    ThumbnailScale,
    ThumbnailTransform,
    ThumbnailWhiteBalance,
    ThumbnailRgb,
    ThumbnailLab,
    ThumbnailOutput,
    Count
};

struct BlockMeta {
    const char* id;
    const char* name;
    RtReuseMode reuse;
    std::vector<const char*> params;
};

const std::vector<BlockMeta>& blockTable()
{
    static const std::vector<BlockMeta> table = {
        // id                      display name                 reuse                  paramFields
        {"source.select",          "Select source",              RT_STAGE_RECOMPUTE,    {"raw.bayersensor.imageNum"}},
        {"source.embedded",        "Select embedded JPEG",       RT_STAGE_RECOMPUTE,    {}},
        {"raw.preprocess",         "Raw preprocess",             RT_STAGE_REUSE_OUTPUT,
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
          "lensProf.lfCameraModel", "lensProf.lfLens"}},
        {"raw.demosaic",           "Demosaic",                   RT_STAGE_REUSE_OUTPUT,
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
          "pdsharpening"}},
        {"raw.base",               "Base image finish",          RT_STAGE_REUSE_OUTPUT, {"raw", "wb", "toneCurve", "spot", "dirpyrDenoise", "filmNegative", "retinex", "cg", "icm"}},
        {"analysis",               "Analysis",                   RT_STAGE_REUSE_OUTPUT, {"icm", "toneCurve", "dirpyrDenoise"}},
        {"transform",              "Geometry transform",         RT_STAGE_REUSE_INPUT,  {"coarse", "commonTrans", "rotate", "distortion", "perspective", "cacorrection", "vignetting", "gradient", "pcvignette", "lensProf", "dehaze", "fattal"}},
        {"local.adjustments",      "Local adjustments",          RT_STAGE_REUSE_INPUT,  {"locallab"}},
        {"rgb.proc",               "RGB processing",             RT_STAGE_REUSE_INPUT,  {"toneCurve", "rgbCurves", "colorToning", "blackwhite", "chmixer", "hsvequalizer", "filmSimulation", "icm"}},
        {"lab.tools",              "Lab tools",                  RT_STAGE_REUSE_INPUT,  {"sh", "labCurve", "localContrast", "vibrance", "epd", "colorToning"}},
        {"lab.sharpening",         "Sharpening",                 RT_STAGE_REUSE_INPUT,  {"sharpening", "sharpenEdge", "sharpenMicro", "impulseDenoise", "defringe"}},
        {"lab.wavelet",            "Wavelet",                    RT_STAGE_REUSE_INPUT,  {"wavelet", "dirpyrequalizer"}},
        {"working.trc",            "Working TRC",                RT_STAGE_REUSE_INPUT,  {"toneEqualizer", "icm", "softlight"}},
        {"ciecam",                 "CIE CAM",                    RT_STAGE_REUSE_INPUT,  {"colorappearance", "icm"}},
        {"gamut",                  "Gamut compression",          RT_STAGE_REUSE_INPUT,  {"cg", "icm"}},
        {"output.convert",         "Output conversion",          RT_STAGE_REUSE_OUTPUT, {"icm"}},
        {"crop",                   "Crop",                       RT_STAGE_REUSE_OUTPUT, {"crop", "cropGuide"}},
        {"resize",                 "Resize",                     RT_STAGE_REUSE_OUTPUT, {"resize"}},
        {"output.sharpen",         "Output sharpening",          RT_STAGE_REUSE_INPUT,  {"prsharpening"}},
        {"framing",                "Framing",                    RT_STAGE_REUSE_OUTPUT, {"framing"}},
        {"output.icc",             "Output ICC",                 RT_STAGE_RECOMPUTE,    {"icm", "metadata"}},
        {"monitor.output",         "Monitor output",             RT_STAGE_RECOMPUTE,    {"icm"}},
        {"viewport.output",        "Viewport output",            RT_STAGE_RECOMPUTE,    {"icm"}},
        {"thumbnail.decode",       "Decode embedded JPEG",       RT_STAGE_REUSE_OUTPUT, {"raw"}},
        {"thumbnail.scale",        "Thumbnail scale",            RT_STAGE_REUSE_OUTPUT, {"filmNegative"}},
        {"thumbnail.transform",    "Thumbnail coarse transform", RT_STAGE_REUSE_INPUT,  {"coarse"}},
        {"thumbnail.whitebalance", "Thumbnail white balance",    RT_STAGE_REUSE_INPUT,  {"wb", "raw", "toneCurve"}},
        {"thumbnail.rgb",          "Thumbnail RGB processing",   RT_STAGE_REUSE_INPUT,  {"toneCurve", "rgbCurves", "colorToning", "blackwhite", "chmixer", "hsvequalizer", "filmSimulation", "icm"}},
        {"thumbnail.lab",          "Thumbnail Lab processing",   RT_STAGE_REUSE_INPUT,  {"labCurve", "sh", "localContrast", "vibrance", "epd", "softlight", "toneEqualizer", "colorappearance", "cg", "icm"}},
        {"thumbnail.output",       "Thumbnail output",           RT_STAGE_RECOMPUTE,    {"icm"}}
    };
    return table;
}

const BlockMeta& meta(StageBlock block)
{
    const std::vector<BlockMeta>& table = blockTable();
    // The catalogue is indexed by StageBlock, so it must cover every entry.
    assert(table.size() == static_cast<size_t>(StageBlock::Count));
    return table[static_cast<size_t>(block)];
}

StageDomain domainFor(StagePath path, StageBlock block)
{
    switch (path) {
    case StagePath::Export:
    case StagePath::Preview:
        return StageDomain::Full;
    case StagePath::Crop:
        // Raw loading is always full-frame; everything from getImage onwards
        // runs on the crop region.
        switch (block) {
        case StageBlock::SourceSelect:
        case StageBlock::RawPreprocess:
        case StageBlock::RawDemosaic:
            return StageDomain::Full;
        default:
            return StageDomain::Roi;
        }
    case StagePath::Thumbnail:
        switch (block) {
        case StageBlock::SourceSelect:
        case StageBlock::RawPreprocess:
        case StageBlock::RawDemosaic:
            return StageDomain::Full;
        default:
            return StageDomain::Reduced;
        }
    }
    return StageDomain::Full;
}

StageConsumer consumerFor(StagePath path)
{
    // Only the export pipeline has to be bit-exact; the others feed a viewer
    // or a thumbnail cache and may reuse approximate intermediate results.
    return path == StagePath::Export ? StageConsumer::Exact : StageConsumer::Interactive;
}

using BlockPair = std::pair<StageBlock, StageBlock>;

struct PathDefinition {
    StagePath path;
    std::vector<StageBlock> nodes;
    std::vector<BlockPair> edges;
    std::vector<StageBlock> roots;
    std::vector<StageBlock> sinks;
};

std::vector<BlockPair> chain(const std::vector<StageBlock>& nodes)
{
    std::vector<BlockPair> edges;
    for (size_t i = 1; i < nodes.size(); ++i) {
        edges.emplace_back(nodes[i - 1], nodes[i]);
    }
    return edges;
}

// Every full-frame path starts with the same raw core.
std::vector<StageBlock> coreBlocks()
{
    return {
        StageBlock::SourceSelect,
        StageBlock::RawPreprocess,
        StageBlock::RawDemosaic,
        StageBlock::RawBase,
        StageBlock::Analysis,
        StageBlock::Transform
    };
}

std::vector<StageBlock> withTail(std::vector<StageBlock> blocks,
                                 std::initializer_list<StageBlock> tail)
{
    blocks.insert(blocks.end(), tail.begin(), tail.end());
    return blocks;
}

PathDefinition definitionFor(StagePath path)
{
    switch (path) {
    case StagePath::Export: {
        // The export implementation crops, resizes and post-sharpens the Lab
        // image before converting to the output space, because lab2rgbOut()
        // consumes the crop rectangle directly. The output blocks follow that
        // order; the catalogue ids, edges and reuse modes are unchanged.
        const std::vector<StageBlock> nodes = withTail(coreBlocks(), {
            StageBlock::LocalAdjustments,
            StageBlock::RgbProc,
            StageBlock::LabTools,
            StageBlock::LabSharpening,
            StageBlock::LabWavelet,
            StageBlock::WorkingTrc,
            StageBlock::CieCam,
            StageBlock::Gamut,
            StageBlock::Crop,
            StageBlock::Resize,
            StageBlock::OutputSharpen,
            StageBlock::OutputConvert,
            StageBlock::Framing,
            StageBlock::OutputIcc
        });
        return {path, nodes, chain(nodes), {StageBlock::SourceSelect},
                {StageBlock::OutputIcc}};
    }
    case StagePath::Preview: {
        // The preview full-frame path applies neither sharpening nor the
        // export-only blocks; the crop viewport owns sharpening.
        const std::vector<StageBlock> nodes = withTail(coreBlocks(), {
            StageBlock::LocalAdjustments,
            StageBlock::RgbProc,
            StageBlock::LabTools,
            StageBlock::LabWavelet,
            StageBlock::WorkingTrc,
            StageBlock::CieCam,
            StageBlock::Gamut,
            StageBlock::MonitorOutput
        });
        return {path, nodes, chain(nodes), {StageBlock::SourceSelect},
                {StageBlock::MonitorOutput}};
    }
    case StagePath::Crop: {
        const std::vector<StageBlock> nodes = withTail(coreBlocks(), {
            StageBlock::LocalAdjustments,
            StageBlock::RgbProc,
            StageBlock::LabTools,
            StageBlock::LabSharpening,
            StageBlock::LabWavelet,
            StageBlock::WorkingTrc,
            StageBlock::CieCam,
            StageBlock::Gamut,
            StageBlock::ViewportOutput
        });
        return {path, nodes, chain(nodes), {StageBlock::SourceSelect},
                {StageBlock::ViewportOutput}};
    }
    case StagePath::Thumbnail: {
        const std::vector<StageBlock> nodes = {
            StageBlock::SourceSelect,
            StageBlock::RawPreprocess,
            StageBlock::RawDemosaic,
            StageBlock::RawBase,
            StageBlock::SourceEmbedded,
            StageBlock::ThumbnailDecode,
            StageBlock::ThumbnailScale,
            StageBlock::ThumbnailTransform,
            StageBlock::ThumbnailWhiteBalance,
            StageBlock::ThumbnailRgb,
            StageBlock::ThumbnailLab,
            StageBlock::ThumbnailOutput
        };
        // Two roots: the embedded JPEG branch and the raw fallback. They join
        // at thumbnail.scale, exactly as Thumbnail::quickProcessImage() and
        // Thumbnail::processImage() do on the shared thumbImg.
        const std::vector<BlockPair> edges = {
            {StageBlock::SourceSelect, StageBlock::RawPreprocess},
            {StageBlock::RawPreprocess, StageBlock::RawDemosaic},
            {StageBlock::RawDemosaic, StageBlock::RawBase},
            {StageBlock::RawBase, StageBlock::ThumbnailScale},
            {StageBlock::SourceEmbedded, StageBlock::ThumbnailDecode},
            {StageBlock::ThumbnailDecode, StageBlock::ThumbnailScale},
            {StageBlock::ThumbnailScale, StageBlock::ThumbnailTransform},
            {StageBlock::ThumbnailTransform, StageBlock::ThumbnailWhiteBalance},
            {StageBlock::ThumbnailWhiteBalance, StageBlock::ThumbnailRgb},
            {StageBlock::ThumbnailRgb, StageBlock::ThumbnailLab},
            {StageBlock::ThumbnailLab, StageBlock::ThumbnailOutput}
        };
        return {path, nodes, edges,
                {StageBlock::SourceSelect, StageBlock::SourceEmbedded},
                {StageBlock::ThumbnailOutput}};
    }
    }
    return {path, {}, {}, {}, {}};
}

StageDescriptorSpec makeDescriptor(StageBlock block, StagePath path)
{
    const BlockMeta& m = meta(block);

    StageDescriptorSpec descriptor;
    descriptor.id = m.id;
    descriptor.name = m.name;
    descriptor.domain = domainFor(path, block);
    descriptor.consumer = consumerFor(path);
    descriptor.reuse = m.reuse;
    descriptor.outputs.push_back(descriptor.id);
    for (const char* param : m.params) {
        descriptor.paramFields.emplace_back(param);
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

} // namespace

const char* stagePathName(StagePath path)
{
    switch (path) {
    case StagePath::Export:
        return "export";
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

StagePipelineSpec buildPipelineSpec(StagePath path)
{
    const PathDefinition definition = definitionFor(path);

    StagePipelineSpec spec;
    spec.path = path;

    spec.nodes.reserve(definition.nodes.size());
    for (StageBlock block : definition.nodes) {
        spec.nodes.push_back(makeDescriptor(block, path));
    }

    spec.edges.reserve(definition.edges.size());
    for (const auto& edge : definition.edges) {
        spec.edges.emplace_back(meta(edge.first).id, meta(edge.second).id);
    }

    for (StageBlock block : definition.roots) {
        spec.roots.emplace_back(meta(block).id);
    }
    for (StageBlock block : definition.sinks) {
        spec.sinks.emplace_back(meta(block).id);
    }

    populateInputs(spec);
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
            << " reuse=" << stageReuseName(node.reuse) << " params=";
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
