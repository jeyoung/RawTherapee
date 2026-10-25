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
 * Specification test for the mask-to-block equivalence map.
 *
 * The test proves four things about rtengine/stage/stage_mask_map.cc:
 *
 * 1. the table covers every catalogued `M_*` bit exactly once and every id it
 *    names is a canonical descriptor of the path;
 * 2. every execution bit names a block, and every block of every path is
 *    reachable from a bit or from a documented non-mask input;
 * 3. rows agree on blocks shared by two paths, except the two rows that are
 *    path specific by design;
 * 4. representative composite masks from refreshmap.h produce the expected
 *    block sets.
 *
 * The expected vectors below are the contract. They are in canonical pipeline
 * order. Update them only with a deliberate change to the map, and record the
 * over-approximation in devnotes/perf/STAGE_PIPELINE.md.
 */

#include "stage_mask_map.h"

#include "stage_pipeline.h"

#include "refreshmap.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
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

std::string join(const std::vector<std::string>& values)
{
    std::string text;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += values[i];
    }
    return text;
}

void checkBlocks(int mask, StagePath path,
                 const std::vector<std::string>& expected,
                 const std::string& label)
{
    const std::vector<std::string> actual = blocks_for_mask(mask, path);
    if (actual == expected) {
        return;
    }
    check(false, label + " on " + stagePathName(path) + ": expected ["
                     + join(expected) + "], got [" + join(actual) + "]");
}

std::vector<std::string> nodeIds(StagePath path)
{
    std::vector<std::string> ids;
    for (const auto& node : buildPipelineSpec(path).nodes) {
        ids.push_back(node.id);
    }
    return ids;
}

std::set<std::string> nodeIdSet(StagePath path)
{
    const std::vector<std::string> ids = nodeIds(path);
    return std::set<std::string>(ids.begin(), ids.end());
}

const std::vector<StagePath>& paths()
{
    static const std::vector<StagePath> list = {
        StagePath::Export, StagePath::Preview, StagePath::Crop
    };
    return list;
}

struct ExpectedRow {
    int bit;
    const char* name;
    MaskBitKind kind;
};

const std::vector<ExpectedRow>& expectedRows()
{
    static const std::vector<ExpectedRow> rows = {
        {M_PREPROC, "M_PREPROC", MaskBitKind::Execution},
        {M_RAW, "M_RAW", MaskBitKind::Execution},
        {M_CSHARP, "M_CSHARP", MaskBitKind::Execution},
        {M_WB, "M_WB", MaskBitKind::Execution},
        {M_INIT, "M_INIT", MaskBitKind::Execution},
        {M_SPOT, "M_SPOT", MaskBitKind::Execution},
        {M_LINDENOISE, "M_LINDENOISE", MaskBitKind::Execution},
        {M_RETINEX, "M_RETINEX", MaskBitKind::Execution},
        {M_HDR, "M_HDR", MaskBitKind::Execution},
        {M_TRANSFORM, "M_TRANSFORM", MaskBitKind::Execution},
        {M_BLURMAP, "M_BLURMAP", MaskBitKind::Execution},
        {M_AUTOEXP, "M_AUTOEXP", MaskBitKind::Execution},
        {M_RGBCURVE, "M_RGBCURVE", MaskBitKind::Execution},
        {M_LUMACURVE, "M_LUMACURVE", MaskBitKind::Execution},
        {M_LUMINANCE, "M_LUMINANCE", MaskBitKind::Execution},
        {M_COLOR, "M_COLOR", MaskBitKind::Execution},
        {M_CROP, "M_CROP", MaskBitKind::OutputRequest},
        {M_MONITOR, "M_MONITOR", MaskBitKind::OutputRequest},
        {M_VOID, "M_VOID", MaskBitKind::RequestKind},
        {M_MINUPDATE, "M_MINUPDATE", MaskBitKind::RequestKind},
        {M_HIGHQUAL, "M_HIGHQUAL", MaskBitKind::RequestKind}
    };
    return rows;
}

const StageMaskMapEntry* findRow(int bit)
{
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        if (row.bit == bit) {
            return &row;
        }
    }
    return nullptr;
}

const std::vector<std::string>& rowBlocks(const StageMaskMapEntry& row,
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
    return row.exportBlocks;
}

void checkTable()
{
    check(stageMaskMap().size() == expectedRows().size(),
          "table has one row per catalogued bit");

    std::set<int> seenBits;
    for (const ExpectedRow& expected : expectedRows()) {
        const StageMaskMapEntry* row = findRow(expected.bit);
        check(row != nullptr,
              std::string("bit is catalogued: ") + expected.name);
        if (row == nullptr) {
            continue;
        }
        check(row->name == std::string(expected.name),
              std::string("bit name matches: ") + expected.name);
        check(row->kind == expected.kind,
              std::string("bit kind matches: ") + expected.name);
        check((expected.bit & (expected.bit - 1)) == 0,
              std::string("bit is a single bit: ") + expected.name);
        check(seenBits.insert(expected.bit).second,
              std::string("bit appears once: ") + expected.name);
        check(isKnownMaskBit(expected.bit),
              std::string("isKnownMaskBit agrees: ") + expected.name);
        check(maskBitIsPathSpecific(expected.bit)
                  == (expected.bit == M_CROP || expected.bit == M_MONITOR),
              std::string("path-specific classification: ") + expected.name);
    }

    check(!isKnownMaskBit(1 << 22), "unknown bit is rejected");
}

void checkIds()
{
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        for (StagePath path : paths()) {
            const std::vector<std::string>& blocks = rowBlocks(row, path);
            const std::vector<std::string> canonical = nodeIds(path);
            std::set<std::string> seen;

            size_t previous = 0;
            for (const std::string& id : blocks) {
                const auto position =
                    std::find(canonical.begin(), canonical.end(), id);
                check(position != canonical.end(),
                      std::string("id is canonical for the path: ") + id
                          + " in " + row.name + " on "
                          + stagePathName(path));
                if (position == canonical.end()) {
                    continue;
                }
                const size_t index =
                    static_cast<size_t>(position - canonical.begin());
                check(index >= previous,
                      std::string("ids are in pipeline order: ") + row.name
                          + " on " + stagePathName(path));
                previous = index;

                check(seen.insert(id).second,
                      std::string("id is not duplicated: ") + id + " in "
                          + row.name + " on " + stagePathName(path));
            }
        }
    }
}

void checkReachability()
{
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        if (row.kind != MaskBitKind::Execution) {
            continue;
        }
        check(!row.exportBlocks.empty(),
              std::string("export has a block: ") + row.name);
        check(!row.cropBlocks.empty(),
              std::string("crop has a block: ") + row.name);
        /*
         * Preview carries no lab.sharpening block, and M_BLURMAP is the only
         * execution bit that names sharpening alone. It is the single
         * documented empty preview row; the crop viewport owns the work.
         */
        if (row.bit == M_BLURMAP) {
            check(row.previewBlocks.empty(),
                  "M_BLURMAP has no preview block");
        } else {
            check(!row.previewBlocks.empty(),
                  std::string("preview has a block: ") + row.name);
        }
    }

    for (StagePath path : paths()) {
        std::set<std::string> reachable;
        for (const StageMaskMapEntry& row : stageMaskMap()) {
            for (const std::string& id : rowBlocks(row, path)) {
                reachable.insert(id);
            }
        }
        for (const StageInputEntry& input : nonMaskInputs(path)) {
            check(nodeIdSet(path).count(input.block) != 0,
                  std::string("non-mask input is canonical: ") + input.block
                      + " on " + stagePathName(path));
            check(input.reason != nullptr && input.reason[0] != '\0',
                  std::string("non-mask input has a reason: ") + input.block);
            reachable.insert(input.block);
        }
        for (const std::string& id : nodeIds(path)) {
            check(reachable.count(id) != 0,
                  std::string("block is reachable: ") + id + " on "
                      + stagePathName(path));
        }
    }

    check(nonMaskInputs(StagePath::Thumbnail).empty(),
          "thumbnail has no mask equivalence yet");
}

void checkSharedBlocks()
{
    const std::vector<std::pair<StagePath, StagePath>> pairs = {
        {StagePath::Export, StagePath::Preview},
        {StagePath::Export, StagePath::Crop},
        {StagePath::Preview, StagePath::Crop}
    };

    for (const auto& pair : pairs) {
        const std::set<std::string> first = nodeIdSet(pair.first);
        const std::set<std::string> second = nodeIdSet(pair.second);
        for (const StageMaskMapEntry& row : stageMaskMap()) {
            if (maskBitIsPathSpecific(row.bit)) {
                continue;
            }
            const std::set<std::string> firstBlocks =
                [&row, &pair] {
                    const std::vector<std::string>& blocks =
                        rowBlocks(row, pair.first);
                    return std::set<std::string>(blocks.begin(), blocks.end());
                }();
            const std::set<std::string> secondBlocks =
                [&row, &pair] {
                    const std::vector<std::string>& blocks =
                        rowBlocks(row, pair.second);
                    return std::set<std::string>(blocks.begin(), blocks.end());
                }();

            for (const std::string& id : first) {
                if (second.count(id) == 0) {
                    continue;
                }
                check((firstBlocks.count(id) != 0)
                          == (secondBlocks.count(id) != 0),
                      std::string("shared block agrees: ") + row.name + " / "
                          + id + " on " + stagePathName(pair.first) + " and "
                          + stagePathName(pair.second));
            }
        }
    }
}

void checkRequestKinds()
{
    const std::vector<int> requestBits = {M_VOID, M_MINUPDATE, M_HIGHQUAL};

    for (int bit : requestBits) {
        const StageMaskMapEntry* row = findRow(bit);
        check(row != nullptr, "request bit is in the table");
        if (row == nullptr) {
            continue;
        }
        check(row->kind == MaskBitKind::RequestKind,
              std::string("row is a request kind: ") + row->name);
        check(row->exportBlocks.empty(),
              std::string("request kind has no export block: ") + row->name);
        check(row->previewBlocks.empty(),
              std::string("request kind has no preview block: ") + row->name);
        check(row->cropBlocks.empty(),
              std::string("request kind has no crop block: ") + row->name);
        for (StagePath path : paths()) {
            check(blocks_for_mask(bit, path).empty(),
                  std::string("request kind dirties nothing: ") + row->name
                      + " on " + stagePathName(path));
        }
    }

    check(mask_for_blocks({}) == 0, "empty block set inverts to zero");

    for (StagePath path : paths()) {
        const int inverse = mask_for_blocks(nodeIds(path));
        for (int bit : requestBits) {
            check((inverse & bit) == 0,
                  std::string("request kind is never inferred: bit ")
                      + std::to_string(bit) + " on " + stagePathName(path));
        }
    }
}

struct MaskExpectation {
    const char* label;
    int mask;
    std::vector<std::string> exportBlocks;
    std::vector<std::string> previewBlocks;
    std::vector<std::string> cropBlocks;
};

const std::vector<MaskExpectation>& representativeMasks()
{
    static const std::vector<MaskExpectation> rows = {
        {"ALL", ALL,
         {"raw.preprocess", "raw.demosaic", "raw.base", "analysis",
          "transform", "local.adjustments", "rgb.proc", "lab.tools",
          "lab.sharpening", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"raw.preprocess", "raw.demosaic", "raw.base", "analysis",
          "transform", "local.adjustments", "rgb.proc", "lab.tools",
          "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"raw.preprocess", "raw.demosaic", "raw.base", "analysis",
          "transform", "local.adjustments", "rgb.proc", "lab.tools",
          "lab.sharpening", "lab.wavelet", "working.trc", "ciecam", "gamut"}},
        {"COMPR", COMPR,
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut", "output.convert", "output.icc"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut",
          "monitor.output"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut", "viewport.output"}},
        {"ALLNORAW", ALLNORAW,
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"DEMOSAIC", DEMOSAIC,
         {"raw.demosaic", "raw.base", "analysis", "transform",
          "local.adjustments", "rgb.proc", "lab.tools", "lab.sharpening",
          "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"raw.demosaic", "raw.base", "analysis", "transform",
          "local.adjustments", "rgb.proc", "lab.tools", "lab.wavelet",
          "working.trc", "ciecam", "gamut"},
         {"raw.demosaic", "raw.base", "analysis", "transform",
          "local.adjustments", "rgb.proc", "lab.tools", "lab.sharpening",
          "lab.wavelet", "working.trc", "ciecam", "gamut"}},
        {"WB", WB,
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"RGBCURVE", RGBCURVE,
         {"local.adjustments", "rgb.proc", "lab.tools", "lab.sharpening",
          "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"local.adjustments", "rgb.proc", "lab.tools", "lab.wavelet",
          "working.trc", "ciecam", "gamut"},
         {"local.adjustments", "rgb.proc", "lab.tools", "lab.sharpening",
          "lab.wavelet", "working.trc", "ciecam", "gamut"}},
        {"LUMINANCECURVE", LUMINANCECURVE,
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"SHARPENING", SHARPENING,
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"DIRPYRDENOISE", DIRPYRDENOISE,
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"DIRPYREQUALIZER", DIRPYREQUALIZER,
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"TRANSFORM", TRANSFORM,
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.wavelet", "working.trc", "ciecam", "gamut"},
         {"raw.base", "analysis", "transform", "local.adjustments", "rgb.proc",
          "lab.tools", "lab.sharpening", "lab.wavelet", "working.trc", "ciecam",
          "gamut"}},
        {"CROP", CROP,
         {"crop"},
         {"analysis", "local.adjustments", "rgb.proc", "lab.tools"},
         {"analysis", "local.adjustments", "rgb.proc", "lab.tools"}},
        {"RESIZE", RESIZE, {}, {}, {}},
        {"MONITORTRANSFORM", MONITORTRANSFORM,
         {"output.convert", "output.icc"},
         {"monitor.output"},
         {"viewport.output"}},
        {"OUTPUTPROFILE", OUTPUTPROFILE,
         {"output.convert", "output.icc"},
         {"monitor.output"},
         {"viewport.output"}}
    };
    return rows;
}

void checkRepresentativeMasks()
{
    for (const MaskExpectation& row : representativeMasks()) {
        checkBlocks(row.mask, StagePath::Export, row.exportBlocks, row.label);
        checkBlocks(row.mask, StagePath::Preview, row.previewBlocks, row.label);
        checkBlocks(row.mask, StagePath::Crop, row.cropBlocks, row.label);
    }
}

void checkInverse()
{
    for (const StageMaskMapEntry& row : stageMaskMap()) {
        if (row.kind == MaskBitKind::RequestKind) {
            continue;
        }
        for (StagePath path : paths()) {
            if (rowBlocks(row, path).empty()) {
                continue;
            }
            const int inverse = mask_for_blocks(blocks_for_mask(row.bit, path));
            check((inverse & row.bit) != 0,
                  std::string("inverse contains the bit: ") + row.name
                      + " on " + stagePathName(path));
        }
    }

    for (const MaskExpectation& expected : representativeMasks()) {
        for (StagePath path : paths()) {
            const std::vector<std::string>& blocks =
                path == StagePath::Export ? expected.exportBlocks
                : path == StagePath::Preview ? expected.previewBlocks
                                             : expected.cropBlocks;
            const int inverse = mask_for_blocks(blocks);
            for (const StageMaskMapEntry& row : stageMaskMap()) {
                if (row.kind == MaskBitKind::RequestKind) {
                    continue;
                }
                if ((expected.mask & row.bit) == 0) {
                    continue;
                }
                if (rowBlocks(row, path).empty()) {
                    continue;
                }
                check((inverse & row.bit) != 0,
                      std::string("representative inverse contains a bit: ")
                          + expected.label + " / " + row.name + " on "
                          + stagePathName(path));
            }
        }
    }
}

} // namespace

int main()
{
    checkTable();
    checkIds();
    checkReachability();
    checkSharedBlocks();
    checkRequestKinds();
    checkRepresentativeMasks();
    checkInverse();

    if (failures == 0) {
        std::puts("stage-mask-map-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
