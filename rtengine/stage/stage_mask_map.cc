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
 * Mask-to-block equivalence map.
 *
 * Phase F of the stage runtime removes the legacy `M_*` bitset. Before that is
 * safe, the bitset's behaviour has to be expressed in stage terms, including
 * the bits that do not name a processing block at all. This unit holds that
 * description. It reads refreshmap.h for the bit values and stage_pipeline.h
 * for the canonical descriptor ids; it does not read the pipelines or execute
 * anything.
 *
 * The mapping was derived from the gates in the legacy paths:
 *
 * - rtengine/improccoordinator.cc: preprocess 1656, demosaic 1705, capture
 *   sharpening 1746, base 1782/1810, spot 2173, Fattal/dehaze 2286,
 *   directional-pyramid equaliser 2328, analysis 2337, local adjustments 2583,
 *   RGB curves 3234, Lab curves 3324, Lab tools 3366, monitor output 4110.
 * - rtengine/dcrop.cc: base and denoise 189/665, spot 625, Fattal/dehaze 749,
 *   equaliser 834/856, RGB curves 1386, luminance/colour tools 1399.
 * - rtengine/simpleprocess.cc pins the export order recorded in
 *   devnotes/perf/STAGE_PIPELINE.md.
 *
 * Blocks below are the direct consumers of a parameter family. Where the
 * legacy gate is broader than the family (for example `M_AUTOEXP` also forces
 * a Lab pass), the map records the family and lists the difference as an
 * over-approximation in devnotes/perf/STAGE_PIPELINE.md, so a reader can tell
 * a deliberate choice from a missed bit.
 */

#include "stage_mask_map.h"

#include "refreshmap.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

namespace
{

StageMaskMapEntry entry(int bit, const char* name, MaskBitKind kind,
                        std::vector<std::string> exportBlocks,
                        std::vector<std::string> previewBlocks,
                        std::vector<std::string> cropBlocks)
{
    return {bit, name, kind, std::move(exportBlocks), std::move(previewBlocks),
            std::move(cropBlocks)};
}

/*
 * Execution bits, in pipeline order. A block appears here when the legacy gate
 * for its parameter family selects it. `lab.sharpening` is absent from the
 * preview path because preview carries no sharpening block; the crop viewport
 * owns it.
 *
 * M_WB, M_INIT, M_SPOT, M_LINDENOISE and M_RETINEX all dirty `raw.base`: the
 * legacy base gate is `M_INIT | M_LINDENOISE | M_HDR` plus the spot, Retinex
 * and white-balance work folded into getImage(). `M_HDR` is listed as
 * `transform` because Fattal and dehaze sit in that block, even though the
 * legacy base gate also tests it.
 */
const std::vector<StageMaskMapEntry>& executionEntries()
{
    static const std::vector<StageMaskMapEntry> table = {
        entry(M_PREPROC, "M_PREPROC", MaskBitKind::Execution,
              {"raw.preprocess"}, {"raw.preprocess"}, {"raw.preprocess"}),
        entry(M_RAW, "M_RAW", MaskBitKind::Execution,
              {"raw.demosaic"}, {"raw.demosaic"}, {"raw.demosaic"}),
        entry(M_CSHARP, "M_CSHARP", MaskBitKind::Execution,
              {"raw.demosaic"}, {"raw.demosaic"}, {"raw.demosaic"}),
        entry(M_WB, "M_WB", MaskBitKind::Execution,
              {"raw.base"}, {"raw.base"}, {"raw.base"}),
        entry(M_INIT, "M_INIT", MaskBitKind::Execution,
              {"raw.base"}, {"raw.base"}, {"raw.base"}),
        entry(M_SPOT, "M_SPOT", MaskBitKind::Execution,
              {"raw.base"}, {"raw.base"}, {"raw.base"}),
        entry(M_LINDENOISE, "M_LINDENOISE", MaskBitKind::Execution,
              {"raw.base"}, {"raw.base"}, {"raw.base"}),
        entry(M_RETINEX, "M_RETINEX", MaskBitKind::Execution,
              {"raw.base"}, {"raw.base"}, {"raw.base"}),
        entry(M_HDR, "M_HDR", MaskBitKind::Execution,
              {"transform"}, {"transform"}, {"transform"}),
        entry(M_TRANSFORM, "M_TRANSFORM", MaskBitKind::Execution,
              {"transform"}, {"transform"}, {"transform"}),
        entry(M_BLURMAP, "M_BLURMAP", MaskBitKind::Execution,
              {"lab.sharpening"}, {}, {"lab.sharpening"}),
        entry(M_AUTOEXP, "M_AUTOEXP", MaskBitKind::Execution,
              {"analysis", "local.adjustments"},
              {"analysis", "local.adjustments"},
              {"analysis", "local.adjustments"}),
        entry(M_RGBCURVE, "M_RGBCURVE", MaskBitKind::Execution,
              {"local.adjustments", "rgb.proc", "lab.wavelet"},
              {"local.adjustments", "rgb.proc", "lab.wavelet"},
              {"local.adjustments", "rgb.proc", "lab.wavelet"}),
        entry(M_LUMACURVE, "M_LUMACURVE", MaskBitKind::Execution,
              {"lab.tools", "ciecam"}, {"lab.tools", "ciecam"},
              {"lab.tools", "ciecam"}),
        entry(M_LUMINANCE, "M_LUMINANCE", MaskBitKind::Execution,
              {"lab.tools", "lab.sharpening", "lab.wavelet"},
              {"lab.tools", "lab.wavelet"},
              {"lab.tools", "lab.sharpening", "lab.wavelet"}),
        entry(M_COLOR, "M_COLOR", MaskBitKind::Execution,
              {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc",
               "ciecam", "gamut"},
              {"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
              {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc",
               "ciecam", "gamut"})
    };
    return table;
}

/*
 * Output and view requests.
 *
 * M_CROP is not a parameter family. On export it is the crop itself. On the
 * preview and crop paths it rebuilds the tone-curve LUTs and the Lab-curve
 * LUTs from the cached working histogram, so it depends on `analysis` even
 * though a crop change does not alter the histogram, and it re-runs local
 * adjustments. `analysis` is listed so the implicit histogram input is
 * explicit; recomputing it is the conservative choice and is recorded as an
 * over-approximation.
 *
 * M_MONITOR is shared by two meanings: EvMonitorTransform (the display
 * transform) and EvOProfile/EvOIntent/EvOBPCompens (the output profile and its
 * intent). Each path is given the sinks it owns.
 */
const std::vector<StageMaskMapEntry>& outputEntries()
{
    static const std::vector<StageMaskMapEntry> table = {
        entry(M_CROP, "M_CROP", MaskBitKind::OutputRequest,
              {"crop"},
              {"analysis", "local.adjustments", "rgb.proc", "lab.tools"},
              {"analysis", "local.adjustments", "rgb.proc", "lab.tools"}),
        entry(M_MONITOR, "M_MONITOR", MaskBitKind::OutputRequest,
              {"output.convert", "output.icc"},
              {"monitor.output"},
              {"viewport.output"})
    };
    return table;
}

/*
 * Request kinds. None of these carries block dirtiness. A mask that consists
 * only of these bits asks the coordinator to do nothing to the image, to
 * repaint only, or to force the accurate path.
 */
const std::vector<StageMaskMapEntry>& requestEntries()
{
    static const std::vector<StageMaskMapEntry> table = {
        entry(M_VOID, "M_VOID", MaskBitKind::RequestKind, {}, {}, {}),
        entry(M_MINUPDATE, "M_MINUPDATE", MaskBitKind::RequestKind, {}, {}, {}),
        entry(M_HIGHQUAL, "M_HIGHQUAL", MaskBitKind::RequestKind, {}, {}, {})
    };
    return table;
}

const std::vector<std::string>& blocksForPath(const StageMaskMapEntry& row,
                                              StagePath path)
{
    switch (path) {
    case StagePath::Export:
        return row.exportBlocks;
    case StagePath::Preview:
        return row.previewBlocks;
    case StagePath::Crop:
        return row.cropBlocks;
    case StagePath::Thumbnail:
        break;
    }

    assert(false && "stage mask map covers export, preview and crop only");
    return row.exportBlocks;
}

int knownMaskBits()
{
    int bits = 0;
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        bits |= row.bit;
    }
    return bits;
}

const std::set<std::string>& canonicalIds(StagePath path)
{
    static const std::set<std::string> exportIds = [] {
        std::set<std::string> ids;
        for (const auto& node : buildPipelineSpec(StagePath::Export).nodes) {
            ids.insert(node.id);
        }
        return ids;
    }();
    static const std::set<std::string> previewIds = [] {
        std::set<std::string> ids;
        for (const auto& node : buildPipelineSpec(StagePath::Preview).nodes) {
            ids.insert(node.id);
        }
        return ids;
    }();
    static const std::set<std::string> cropIds = [] {
        std::set<std::string> ids;
        for (const auto& node : buildPipelineSpec(StagePath::Crop).nodes) {
            ids.insert(node.id);
        }
        return ids;
    }();

    switch (path) {
    case StagePath::Export:
        return exportIds;
    case StagePath::Preview:
        return previewIds;
    case StagePath::Crop:
        return cropIds;
    case StagePath::Thumbnail:
        break;
    }

    assert(false && "stage mask map covers export, preview and crop only");
    return exportIds;
}

bool isCanonicalBlock(const std::string& id)
{
    return canonicalIds(StagePath::Export).count(id) != 0
           || canonicalIds(StagePath::Preview).count(id) != 0
           || canonicalIds(StagePath::Crop).count(id) != 0;
}

} // namespace

const char* maskBitKindName(MaskBitKind kind)
{
    switch (kind) {
    case MaskBitKind::Execution:
        return "execution";
    case MaskBitKind::OutputRequest:
        return "output-request";
    case MaskBitKind::RequestKind:
        return "request-kind";
    }

    return "unknown";
}

const std::vector<StageMaskMapEntry>& stageMaskMap()
{
    static const std::vector<StageMaskMapEntry> table = [] {
        std::vector<StageMaskMapEntry> rows = executionEntries();
        const std::vector<StageMaskMapEntry>& outputs = outputEntries();
        rows.insert(rows.end(), outputs.begin(), outputs.end());
        const std::vector<StageMaskMapEntry>& requests = requestEntries();
        rows.insert(rows.end(), requests.begin(), requests.end());
        return rows;
    }();
    return table;
}

bool isKnownMaskBit(int bit)
{
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        if (row.bit == bit) {
            return true;
        }
    }
    return false;
}

bool maskBitIsPathSpecific(int bit)
{
    return bit == M_CROP || bit == M_MONITOR;
}

std::vector<std::string> blocks_for_mask(int mask, StagePath path)
{
    assert(mask >= 0);
    assert((path == StagePath::Export || path == StagePath::Preview
            || path == StagePath::Crop)
           && "stage mask map covers export, preview and crop only");
    const int unknownBits = mask & ~knownMaskBits();
    if (unknownBits != 0) {
        assert(false && "unknown bit in a stage mask");
    }

    std::set<std::string> dirty;
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        if ((mask & row.bit) == 0) {
            continue;
        }
        const std::vector<std::string>& blocks = blocksForPath(row, path);
        dirty.insert(blocks.begin(), blocks.end());
    }

    /*
     * Emit in canonical pipeline order and drop anything that is not part of
     * this path, so callers can compare vectors instead of sets.
     */
    std::vector<std::string> ordered;
    for (const auto& node : buildPipelineSpec(path).nodes) {
        if (dirty.find(node.id) != dirty.end()) {
            ordered.push_back(node.id);
        }
    }
    assert(ordered.size() == dirty.size()
           && "stage mask map names a block outside the path");
    return ordered;
}

int mask_for_blocks(const std::vector<std::string>& blocks)
{
    const std::set<std::string> requested(blocks.begin(), blocks.end());

    for (const std::string& id : requested) {
        if (!isCanonicalBlock(id)) {
            assert(false && "unknown id passed to mask_for_blocks");
        }
    }

    int mask = 0;
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        /*
         * A request kind has an empty block set, which would make it a subset
         * of every input. It is never inferred.
         */
        if (row.kind == MaskBitKind::RequestKind) {
            continue;
        }

        /*
         * The row is inferred when any single path's block set is fully
         * covered. Requiring the union of all three would reject M_CROP on an
         * export-only set (`crop` and not the preview LUTs) and M_MONITOR on a
         * preview-only set (`monitor.output` and not the output sinks).
         */
        bool covered = false;
        for (StagePath path : {StagePath::Export, StagePath::Preview,
                               StagePath::Crop}) {
            const std::vector<std::string>& rowBlocks = blocksForPath(row, path);
            if (rowBlocks.empty()) {
                continue;
            }
            if (std::all_of(rowBlocks.begin(), rowBlocks.end(),
                            [&requested](const std::string& id) {
                                return requested.count(id) != 0;
                            })) {
                covered = true;
                break;
            }
        }
        if (covered) {
            mask |= row.bit;
        }
    }
    return mask;
}

const char* stageInputKindName(StageInputKind kind)
{
    switch (kind) {
    case StageInputKind::Source:
        return "source";
    case StageInputKind::Parameters:
        return "parameters";
    case StageInputKind::RenderState:
        return "render-state";
    case StageInputKind::Request:
        return "request";
    }

    return "unknown";
}

const std::vector<StageInputEntry>& nonMaskInputs(StagePath path)
{
    /*
     * Export-only blocks driven by events that map to M_VOID, or by members
     * with no event at all. The bitset deliberately says "parameters changed,
     * do not repaint"; the export path still has to recompute them, which the
     * parameter fingerprints provide.
     */
    static const std::vector<StageInputEntry> exportInputs = {
        {"source.select", StageInputKind::Source,
         "image load and source generation"},
        {"resize", StageInputKind::Parameters,
         "resize events map to M_VOID"},
        {"output.sharpen", StageInputKind::Parameters,
         "post-resize sharpening events map to M_VOID"},
        {"framing", StageInputKind::Parameters,
         "framing parameters have no dedicated mask bit"},
        {"output.icc", StageInputKind::Parameters,
         "metadata events map to M_VOID"}
    };

    static const std::vector<StageInputEntry> previewInputs = {
        {"source.select", StageInputKind::Source,
         "image load and source generation"},
        {"monitor.output", StageInputKind::RenderState,
         "monitor profile, intent, soft proof and gamut check"}
    };

    static const std::vector<StageInputEntry> cropInputs = {
        {"source.select", StageInputKind::Source,
         "image load and source generation"},
        {"viewport.output", StageInputKind::RenderState,
         "monitor configuration and crop or ROI geometry"}
    };

    static const std::vector<StageInputEntry> noInputs;

    switch (path) {
    case StagePath::Export:
        return exportInputs;
    case StagePath::Preview:
        return previewInputs;
    case StagePath::Crop:
        return cropInputs;
    case StagePath::Thumbnail:
        return noInputs;
    }

    return noInputs;
}

} // namespace rtengine::stage
