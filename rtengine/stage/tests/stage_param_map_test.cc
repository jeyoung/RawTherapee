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
 * Specification test for the stage parameter map.
 *
 * The test proves that every canonical export, preview and crop descriptor has
 * a fingerprint, that the fingerprint is a pure function of the parameters,
 * and that each descriptor responds to the members it claims to consume. It
 * does not run any image processing.
 *
 * The mutation table is the core evidence: one representative change per
 * parameter family, checked against every path that carries the descriptor.
 * `exclusive` means the changed member is listed by exactly one descriptor on
 * that path, so every other fingerprint must stay put. A non-exclusive field
 * may legitimately move more than one fingerprint; the test then only requires
 * the named descriptor to change.
 *
 * The export regression table pins the neutral-parameter fingerprints from
 * before the preview and crop tables were added. Update it only with a
 * deliberate fingerprint format change.
 */

#include "stage_param_map.h"

#include "stage_pipeline.h"

#include "rtengine/procparams.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace
{

using namespace rtengine;
using namespace rtengine::stage;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

/*
 * ProcParams members named by the tables. A field name outside this list has
 * been invented and must not pass review. The list mirrors the whitelist in
 * stage_pipeline_test.cc, which owns the canonical inventory.
 */
const std::set<std::string>& knownParamFields()
{
    static const std::set<std::string> fields = {
        "toneCurve", "labCurve", "retinex", "localContrast", "rgbCurves",
        "colorToning", "sharpening", "prsharpening", "pdsharpening",
        "sharpenEdge", "sharpenMicro", "vibrance", "wb", "colorappearance",
        "defringe", "impulseDenoise", "dirpyrDenoise", "epd", "fattal", "sh",
        "cg", "toneEqualizer", "crop", "cropGuide", "coarse", "commonTrans",
        "rotate", "distortion", "lensProf", "perspective", "gradient",
        "locallab", "pcvignette", "cacorrection", "vignetting", "chmixer",
        "blackwhite", "resize", "framing", "spot", "icm", "raw", "wavelet",
        "dirpyrequalizer", "hsvequalizer", "filmSimulation", "softlight",
        "dehaze", "filmNegative", "metadata"
    };
    return fields;
}

/*
 * Sub-fields of the ProcParams members whose consumers read disjoint parts.
 * `raw` and `lensProf` are split per consumer; the dotted spelling is the same
 * one the mutation table uses for documentation. A name outside this set and
 * outside knownParamFields() has been invented.
 */
const std::set<std::string>& knownParamSubFields()
{
    static const std::set<std::string> fields = {
        "raw.bayersensor.imageNum", "raw.bayersensor.black0",
        "raw.bayersensor.black1", "raw.bayersensor.black2",
        "raw.bayersensor.black3", "raw.bayersensor.twogreen",
        "raw.bayersensor.Dehablack", "raw.bayersensor.greenthresh",
        "raw.bayersensor.linenoise", "raw.bayersensor.linenoiseDirection",
        "raw.bayersensor.pdafLinesFilter", "raw.bayersensor.border",
        "raw.bayersensor.method", "raw.bayersensor.dcb_iterations",
        "raw.bayersensor.dcb_enhance", "raw.bayersensor.lmmse_iterations",
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
        "raw.xtranssensor.blackred", "raw.xtranssensor.blackgreen",
        "raw.xtranssensor.blackblue", "raw.xtranssensor.Dehablackx",
        "raw.xtranssensor.method", "raw.xtranssensor.border",
        "raw.xtranssensor.dualDemosaicAutoContrast",
        "raw.xtranssensor.dualDemosaicContrast", "raw.dark_frame",
        "raw.df_autoselect", "raw.ff_file", "raw.ff_AutoSelect",
        "raw.ff_FromMetaData", "raw.ff_BlurRadius", "raw.ff_BlurType",
        "raw.ff_AutoClipControl", "raw.ff_clipControl",
        "raw.ca_autocorrect", "raw.ca_avoidcolourshift",
        "raw.caautoiterations", "raw.cared", "raw.cablue", "raw.expos",
        "raw.preprocessWB", "raw.hotPixelFilter", "raw.deadPixelFilter",
        "raw.hotdeadpix_thresh", "lensProf.lcMode", "lensProf.lcpFile",
        "lensProf.useVign", "lensProf.lfCameraMake",
        "lensProf.lfCameraModel", "lensProf.lfLens"
    };
    return fields;
}

bool isKnownParamField(const std::string& field)
{
    return knownParamFields().count(field) != 0
        || knownParamSubFields().count(field) != 0;
}

std::vector<std::string> pipelineIds(StagePath path)
{
    std::vector<std::string> ids;
    for (const auto& node : buildPipelineSpec(path).nodes) {
        ids.push_back(node.id);
    }
    return ids;
}

using FingerprintFn =
    ParamFingerprint (*)(const std::string&, const procparams::ProcParams&);

struct PathUnderTest {
    const char* name;
    const std::vector<StageParamMapEntry>* table;
    FingerprintFn fingerprint;
    bool (*has)(const std::string&);
    std::vector<std::string> ids;
};

const std::vector<PathUnderTest>& paths()
{
    static const std::vector<PathUnderTest> list = {
        {"export", &exportParamMap(), export_fingerprint,
         has_export_fingerprint, pipelineIds(StagePath::Export)},
        {"preview", &previewParamMap(), preview_fingerprint,
         has_preview_fingerprint, pipelineIds(StagePath::Preview)},
        {"crop", &cropParamMap(), crop_fingerprint, has_crop_fingerprint,
         pipelineIds(StagePath::Crop)}
    };
    return list;
}

using FingerprintMap = std::map<std::string, ParamFingerprint>;

FingerprintMap fingerprints(const std::vector<StageParamMapEntry>& table,
                            FingerprintFn fingerprint,
                            const procparams::ProcParams& params)
{
    FingerprintMap result;
    for (const StageParamMapEntry& entry : table) {
        result.emplace(entry.id, fingerprint(entry.id, params));
    }
    return result;
}

/* Criterion 1: table shape, ids, order and member names for every path. */
void checkTable(const PathUnderTest& path, size_t expectedCount)
{
    const std::vector<StageParamMapEntry>& table = *path.table;
    const std::string label(path.name);

    check(table.size() == path.ids.size(),
          label + " table covers every descriptor: expected " +
              std::to_string(path.ids.size()) + ", got " +
              std::to_string(table.size()));
    check(table.size() == expectedCount,
          label + " path has " + std::to_string(expectedCount) +
              " descriptors");

    std::set<std::string> seen;
    for (const StageParamMapEntry& entry : table) {
        check(!entry.id.empty(), label + " descriptor id is not empty");
        check(seen.insert(entry.id).second,
              label + " descriptor id is unique: " + entry.id);
        check(!entry.fields.empty(),
              label + " descriptor lists at least one member: " + entry.id);
        for (const std::string& field : entry.fields) {
            check(isKnownParamField(field),
                  label + " descriptor " + entry.id +
                      " names a known ProcParams member: " + field);
        }
    }

    const std::set<std::string> specIds(path.ids.begin(), path.ids.end());
    for (const StageParamMapEntry& entry : table) {
        check(specIds.count(entry.id) != 0,
              label + " table id is in the spec: " + entry.id);
    }
    for (const std::string& id : path.ids) {
        check(seen.count(id) != 0, label + " id has a table row: " + id);
    }

    /* The tables read like the pipeline, so review diffs stay in order. */
    for (size_t i = 0; i < table.size() && i < path.ids.size(); ++i) {
        check(table[i].id == path.ids[i],
              label + " table order matches the pipeline at index " +
                  std::to_string(i));
    }
}

void checkTables()
{
    checkTable(paths()[0], 20);
    checkTable(paths()[1], 14);
    checkTable(paths()[2], 15);
}

/* Criterion 2: every descriptor has a fingerprint only on its own paths. */
void checkFingerprintPresence()
{
    for (const PathUnderTest& path : paths()) {
        for (const std::string& id : path.ids) {
            check(path.has(id),
                  std::string(path.name) +
                      " descriptor has a fingerprint: " + id);
        }
    }

    check(!has_export_fingerprint(""),
          "an empty id has no export fingerprint");
    check(!has_export_fingerprint("monitor.output"),
          "a preview sink has no export fingerprint");
    check(!has_export_fingerprint("viewport.output"),
          "a crop sink has no export fingerprint");
    check(!has_export_fingerprint("thumbnail.output"),
          "a thumbnail descriptor has no export fingerprint");
    check(!has_export_fingerprint("nonsense"),
          "an unknown id has no export fingerprint");

    check(!has_preview_fingerprint("lab.sharpening"),
          "the crop-only sharpening block has no preview fingerprint");
    check(!has_preview_fingerprint("viewport.output"),
          "the crop sink has no preview fingerprint");
    check(!has_preview_fingerprint("output.icc"),
          "an export-only descriptor has no preview fingerprint");
    check(!has_preview_fingerprint("nonsense"),
          "an unknown id has no preview fingerprint");

    check(!has_crop_fingerprint("monitor.output"),
          "the preview sink has no crop fingerprint");
    check(!has_crop_fingerprint("output.icc"),
          "an export-only descriptor has no crop fingerprint");
    check(!has_crop_fingerprint("nonsense"),
          "an unknown id has no crop fingerprint");
}

/* Criterion 3: identical parameters give identical fingerprints. */
void checkDeterminism()
{
    for (const PathUnderTest& path : paths()) {
        const procparams::ProcParams params;
        const procparams::ProcParams copy;
        for (const StageParamMapEntry& entry : *path.table) {
            const ParamFingerprint first =
                path.fingerprint(entry.id, params);
            const ParamFingerprint second =
                path.fingerprint(entry.id, params);
            const ParamFingerprint third = path.fingerprint(entry.id, copy);
            check(first == second &&
                      first.hexString() == second.hexString() &&
                      first.hash() == second.hash(),
                  std::string(path.name) +
                      " repeated fingerprint is identical: " + entry.id);
            check(first == third && first.hexString() == third.hexString(),
                  std::string(path.name) +
                      " equal parameters give equal fingerprints: " +
                      entry.id);
        }
    }
}

/*
 * Criterion 3, storage independence: a neutral ProcParams must hash the same
 * regardless of the bytes that occupied its storage before construction. Two
 * placement-new objects over differently poisoned buffers catch a member that
 * reads the object's own prior bytes. A value copied from an uninitialised
 * temporary is invisible here; the known sharpening case is covered by
 * checkSharpeningExclusions() and the regression constants.
 */
void checkNeutralStability()
{
    alignas(procparams::ProcParams)
        unsigned char storage_a[sizeof(procparams::ProcParams)];
    alignas(procparams::ProcParams)
        unsigned char storage_b[sizeof(procparams::ProcParams)];

    std::memset(storage_a, 0xA5, sizeof(storage_a));
    std::memset(storage_b, 0x5A, sizeof(storage_b));

    auto* first = new (storage_a) procparams::ProcParams();
    auto* second = new (storage_b) procparams::ProcParams();

    for (const PathUnderTest& path : paths()) {
        const FingerprintMap a =
            fingerprints(*path.table, path.fingerprint, *first);
        const FingerprintMap b =
            fingerprints(*path.table, path.fingerprint, *second);
        for (const StageParamMapEntry& entry : *path.table) {
            check(a.at(entry.id) == b.at(entry.id),
                  std::string(path.name) +
                      " neutral fingerprint is independent of prior storage: " +
                      entry.id);
        }
    }

    first->~ProcParams();
    second->~ProcParams();
}

/*
 * Neutral-parameter export fingerprints. Eighteen values were captured from
 * the mapping before the preview and crop tables were added and must not
 * change. The two sharpening descriptors differ by design: the map now
 * excludes five dead SharpeningParams members that its constructor leaves
 * uninitialised, so their old values depended on stack contents and were not
 * reproducible. Their new values are pinned below, and
 * checkSharpeningExclusions() proves the dead members stay out of the hash.
 * The analysis value changed when the denoise parameters joined its field
 * list: the automatic denoise measurement reads them, so the deliberate
 * format change is recorded here rather than reverted. The three raw consumer
 * values changed when `raw` was split into its per-consumer sub-fields; see
 * checkRawConsumerSplit() and the field lists in stage_param_map.cc.
 */
const std::map<std::string, std::string>& neutralExportFingerprints()
{
    static const std::map<std::string, std::string> expected = {
        {"source.select", "a28f2a11dca1007b39a8e9fff24e578c"},
        {"raw.preprocess", "88942882c721eb8ec614f59d30600242"},
        {"raw.demosaic", "78ac13127d9722d02402b459d1989afa"},
        {"raw.base", "8786a9e2404ada652d318a74b1005f4e"},
        {"analysis", "653441262d4cd6d9713a0afe66ae9d3f"},
        {"transform", "a6a394fbf372e1052ebe4fb397d7ef00"},
        {"local.adjustments", "69a6d4f54577a0f76ee897f84b8df245"},
        {"rgb.proc", "3c401612bc6c48edeb1ab96aa95e9c30"},
        {"lab.tools", "2c2a5e5f0bea257cc0cd2b28203feaa0"},
        {"lab.sharpening", "e5f4657d9a33ec8bfba04c3a1ba684b7"},
        {"lab.wavelet", "ecccf5776f2c1ad804b3ce3910570b9e"},
        {"working.trc", "ef4352f9d7ea183c2ad7da4e714e1911"},
        {"ciecam", "096306aeec4f6b7b84baeb3545af6e38"},
        {"gamut", "d12209a1e74a77810ceedf0c6277655a"},
        {"crop", "b90f448526a1bb3a46ba63abb42ef16f"},
        {"resize", "0c752b6efd4a185bfb532c60d2804c36"},
        {"output.sharpen", "f1771ba1d829c0d6b3ea818129ca054e"},
        {"output.convert", "7bce452d266f1a113b15c374a359c3d2"},
        {"framing", "8cb0a8abab706d8e36293b5928c03fea"},
        {"output.icc", "f63843ed81ce8fd1384be3a8e51ae9d7"}
    };
    return expected;
}

void checkExportRegression()
{
    const procparams::ProcParams params;
    const std::map<std::string, std::string>& expected =
        neutralExportFingerprints();

    check(expected.size() == exportParamMap().size(),
          "the export regression table covers every export descriptor");

    for (const StageParamMapEntry& entry : exportParamMap()) {
        const auto found = expected.find(entry.id);
        check(found != expected.end(),
              "export regression has a value for " + entry.id);
        if (found != expected.end()) {
            const std::string actual =
                export_fingerprint(entry.id, params).hexString();
            check(actual == found->second,
                  "neutral export fingerprint is unchanged: " + entry.id +
                      " expected " + found->second + ", got " + actual);
        }
    }
}

struct Mutation {
    const char* descriptor;
    const char* field;
    void (*apply)(procparams::ProcParams&);
    bool exclusive;
};

/*
 * One representative change per parameter family. The field names are
 * documentation for the report; the lambda is the mutation. A mutation runs
 * only on paths that carry `descriptor`.
 */
const std::vector<Mutation>& mutations()
{
    static const std::vector<Mutation> table = {
        /* WB */
        {"raw.base", "wb.temperature",
         [](procparams::ProcParams& p) { p.wb.temperature = 7000; }, false},
        /* Demosaic */
        {"raw.demosaic", "raw.bayersensor.method",
         [](procparams::ProcParams& p) { p.raw.bayersensor.method = "RCD"; },
         false},
        /* Transform */
        {"transform", "rotate.degree",
         [](procparams::ProcParams& p) { p.rotate.degree = 1.5; }, true},
        /* Local adjustments */
        {"local.adjustments", "locallab.selspot",
         [](procparams::ProcParams& p) { p.locallab.selspot = 3; }, true},
        /* RGB curves */
        {"rgb.proc", "rgbCurves.rcurve",
         [](procparams::ProcParams& p) {
             p.rgbCurves.enabled = true;
             p.rgbCurves.rcurve = {0.0, 0.25, 0.5, 0.75, 1.0};
         },
         false},
        /* Tone curve */
        {"rgb.proc", "toneCurve.curve",
         [](procparams::ProcParams& p) {
             p.toneCurve.curve = {0.0, 0.4, 1.0};
         },
         false},
        /* RGB-only member for the exclusivity assertion */
        {"rgb.proc", "filmSimulation.strength",
         [](procparams::ProcParams& p) { p.filmSimulation.strength = 44; },
         true},
        /* Wavelet */
        {"lab.wavelet", "wavelet.strength",
         [](procparams::ProcParams& p) { p.wavelet.strength = 7; }, false},
        /* CIE CAM */
        {"ciecam", "colorappearance.degree",
         [](procparams::ProcParams& p) { p.colorappearance.degree = 85; },
         true},
        /* Gamut compression */
        {"gamut", "cg.pwr",
         [](procparams::ProcParams& p) { p.cg.pwr = 1.5; }, false},
        /* Output profile / output conversion */
        {"output.icc", "icm.outputProfile",
         [](procparams::ProcParams& p) { p.icm.outputProfile = "Adobe RGB"; },
         false},
        {"output.convert", "icm.outputIntent",
         [](procparams::ProcParams& p) { p.icm.outputIntent = RI_ABSOLUTE; },
         false},
        /* Crop */
        {"crop", "crop.x",
         [](procparams::ProcParams& p) {
             p.crop.enabled = true;
             p.crop.x = 10;
         },
         false},
        /* Crop ROI: the rectangle is an input of the ROI transform. */
        {"transform", "crop.w",
         [](procparams::ProcParams& p) {
             p.crop.enabled = true;
             p.crop.w = 120;
         },
         false},
        /* Monitor output: the display sink of the preview path. */
        {"monitor.output", "icm.outputProfile",
         [](procparams::ProcParams& p) { p.icm.outputProfile = "Adobe RGB"; },
         false},
        /* Viewport output: the display sink of the crop path. */
        {"viewport.output", "icm.outputIntent",
         [](procparams::ProcParams& p) { p.icm.outputIntent = RI_ABSOLUTE; },
         false},
        /* Resize */
        {"resize", "resize.scale",
         [](procparams::ProcParams& p) {
             p.resize.enabled = true;
             p.resize.scale = 0.5;
         },
         false},
        /* Framing: resize hashes the whole framing member, so borderRed is
         * not exclusive to the framing descriptor. */
        {"framing", "framing.borderRed",
         [](procparams::ProcParams& p) {
             p.framing.enabled = true;
             p.framing.borderRed = 200;
         },
         false},
        /* Raw preprocess: a preprocess-only raw field. */
        {"raw.preprocess", "raw.hotPixelFilter",
         [](procparams::ProcParams& p) { p.raw.hotPixelFilter = true; }, false},
        /* Raw base, exclusive member */
        {"raw.base", "spot.enabled",
         [](procparams::ProcParams& p) { p.spot.enabled = true; }, true},
        /* Output sharpening */
        {"output.sharpen", "prsharpening.contrast",
         [](procparams::ProcParams& p) { p.prsharpening.contrast = 33.0; },
         true},
        /* Lab sharpening */
        {"lab.sharpening", "sharpening.contrast",
         [](procparams::ProcParams& p) { p.sharpening.contrast = 42.0; },
         true},
        /* Lab tools */
        {"lab.tools", "vibrance.pastels",
         [](procparams::ProcParams& p) { p.vibrance.pastels = 17; }, true},
        /* Working TRC */
        {"working.trc", "toneEqualizer.bands[0]",
         [](procparams::ProcParams& p) { p.toneEqualizer.bands[0] = 5; },
         true},
        /* Output metadata, exclusive member */
        {"output.icc", "metadata.mode",
         [](procparams::ProcParams& p) {
             p.metadata.mode = procparams::MetaDataParams::STRIP;
         },
         true},
        /* Analysis */
        {"analysis", "toneCurve.autoexp",
         [](procparams::ProcParams& p) { p.toneCurve.autoexp = true; }, false},
        /*
         * The automatic denoise measurement reads the denoise parameters, so
         * an analysis measured for another denoise configuration must not be
         * reused.
         */
        {"analysis", "dirpyrDenoise.enabled",
         [](procparams::ProcParams& p) { p.dirpyrDenoise.enabled = true; },
         false},
        /* Source select */
        {"source.select", "raw.bayersensor.imageNum",
         [](procparams::ProcParams& p) { p.raw.bayersensor.imageNum = 1; },
         false}
    };
    return table;
}

/* Criteria 4 to 6: mutation behaviour of every family on every path. */
void checkMutations()
{
    const procparams::ProcParams baseline;
    std::map<std::string, FingerprintMap> base;
    std::map<std::string, std::set<std::string>> covered;

    for (const PathUnderTest& path : paths()) {
        base.emplace(path.name,
                     fingerprints(*path.table, path.fingerprint, baseline));
    }

    for (const Mutation& mutation : mutations()) {
        procparams::ProcParams mutated = baseline;
        mutation.apply(mutated);

        for (const PathUnderTest& path : paths()) {
            const FingerprintMap& before = base.at(path.name);
            if (before.count(mutation.descriptor) == 0) {
                /* The descriptor is not on this path; skip the whole path. */
                continue;
            }

            const FingerprintMap after = fingerprints(
                *path.table, path.fingerprint, mutated);
            covered[path.name].insert(mutation.descriptor);

            for (const StageParamMapEntry& entry : *path.table) {
                const bool changed =
                    before.at(entry.id) != after.at(entry.id);
                if (entry.id == mutation.descriptor) {
                    check(changed,
                          std::string(path.name) +
                              " mutation changes its own descriptor: " +
                              mutation.descriptor + " <- " + mutation.field);
                } else if (mutation.exclusive) {
                    check(!changed,
                          std::string(path.name) +
                              " exclusive mutation leaves " + entry.id +
                              " unchanged: " + mutation.descriptor + " <- " +
                              mutation.field);
                }
            }
        }
    }

    for (const PathUnderTest& path : paths()) {
        for (const std::string& id : path.ids) {
            check(covered[path.name].count(id) != 0,
                  std::string(path.name) +
                      " mutation table covers descriptor: " + id);
        }
    }

    const std::set<std::string>& exportCovered = covered["export"];
    const std::set<std::string>& previewCovered = covered["preview"];
    const std::set<std::string>& cropCovered = covered["crop"];

    check(exportCovered.count("raw.base") != 0 &&
              exportCovered.count("raw.demosaic") != 0 &&
              exportCovered.count("transform") != 0 &&
              exportCovered.count("local.adjustments") != 0 &&
              exportCovered.count("rgb.proc") != 0 &&
              exportCovered.count("lab.wavelet") != 0 &&
              exportCovered.count("ciecam") != 0 &&
              exportCovered.count("gamut") != 0 &&
              exportCovered.count("output.icc") != 0 &&
              exportCovered.count("crop") != 0 &&
              exportCovered.count("resize") != 0 &&
              exportCovered.count("framing") != 0,
          "export mutations cover the required parameter families");

    check(previewCovered.count("raw.base") != 0 &&
              previewCovered.count("raw.demosaic") != 0 &&
              previewCovered.count("transform") != 0 &&
              previewCovered.count("local.adjustments") != 0 &&
              previewCovered.count("rgb.proc") != 0 &&
              previewCovered.count("lab.tools") != 0 &&
              previewCovered.count("lab.wavelet") != 0 &&
              previewCovered.count("monitor.output") != 0,
          "preview mutations cover the required parameter families");

    check(cropCovered.count("raw.base") != 0 &&
              cropCovered.count("raw.demosaic") != 0 &&
              cropCovered.count("transform") != 0 &&
              cropCovered.count("local.adjustments") != 0 &&
              cropCovered.count("rgb.proc") != 0 &&
              cropCovered.count("lab.tools") != 0 &&
              cropCovered.count("lab.sharpening") != 0 &&
              cropCovered.count("lab.wavelet") != 0 &&
              cropCovered.count("viewport.output") != 0,
          "crop mutations cover the required parameter families");
}

/*
 * The three raw consumer fingerprints must not be coarser than the legacy
 * raw gates. Each edit below is one event family; it may move its own block
 * (and raw.base, which reads the whole raw member), but must leave the other
 * raw consumers alone. Before the split, every raw edit moved all three.
 */
void checkRawConsumerSplit()
{
    const procparams::ProcParams baseline;
    const FingerprintMap base =
        fingerprints(exportParamMap(), export_fingerprint, baseline);

    const auto after = [&baseline](auto mutate) {
        procparams::ProcParams changed = baseline;
        mutate(changed);
        return fingerprints(exportParamMap(), export_fingerprint, changed);
    };

    /* A demosaic-method edit is M_RAW, not M_PREPROC or a frame selection. */
    {
        const FingerprintMap changed = after([](procparams::ProcParams& p) {
            p.raw.bayersensor.method = "RCD";
        });
        check(base.at("raw.demosaic") != changed.at("raw.demosaic"),
              "demosaic method moves raw.demosaic");
        check(base.at("raw.preprocess") == changed.at("raw.preprocess"),
              "demosaic method leaves raw.preprocess unchanged");
        check(base.at("source.select") == changed.at("source.select"),
              "demosaic method leaves source.select unchanged");
    }

    /* The denoise family is M_LINDENOISE, not a raw preprocessing family. */
    {
        const FingerprintMap changed = after([](procparams::ProcParams& p) {
            p.dirpyrDenoise.luma += 5;
        });
        check(base.at("raw.base") != changed.at("raw.base"),
              "denoise moves raw.base");
        check(base.at("analysis") != changed.at("analysis"),
              "denoise moves analysis");
        check(base.at("raw.preprocess") == changed.at("raw.preprocess"),
              "denoise leaves raw.preprocess unchanged");
        check(base.at("raw.demosaic") == changed.at("raw.demosaic"),
              "denoise leaves raw.demosaic unchanged");
        check(base.at("source.select") == changed.at("source.select"),
              "denoise leaves source.select unchanged");
    }

    /* A coarse rotate is applied after the raw stage: M_TRANSFORM only. */
    {
        const FingerprintMap changed = after([](procparams::ProcParams& p) {
            p.coarse.rotate = 90;
        });
        check(base.at("transform") != changed.at("transform"),
              "coarse rotate moves transform");
        check(base.at("raw.preprocess") == changed.at("raw.preprocess"),
              "coarse rotate leaves raw.preprocess unchanged");
        check(base.at("raw.demosaic") == changed.at("raw.demosaic"),
              "coarse rotate leaves raw.demosaic unchanged");
    }

    /* Selecting another frame is the source-select family. */
    {
        const FingerprintMap changed = after([](procparams::ProcParams& p) {
            p.raw.bayersensor.imageNum = 1;
        });
        check(base.at("source.select") != changed.at("source.select"),
              "frame selection moves source.select");
        check(base.at("raw.preprocess") == changed.at("raw.preprocess"),
              "frame selection leaves raw.preprocess unchanged");
        check(base.at("raw.demosaic") == changed.at("raw.demosaic"),
              "frame selection leaves raw.demosaic unchanged");
    }

    /* Capture sharpening is a demosaic-body input. */
    {
        const FingerprintMap changed = after([](procparams::ProcParams& p) {
            p.pdsharpening.contrast += 0.1;
        });
        check(base.at("raw.demosaic") != changed.at("raw.demosaic"),
              "capture sharpening moves raw.demosaic");
        check(base.at("raw.preprocess") == changed.at("raw.preprocess"),
              "capture sharpening leaves raw.preprocess unchanged");
    }
}

/* Criterion 7: a member outside a descriptor's list must not move it. */
void checkUnrelated()
{
    const procparams::ProcParams baseline;
    const FingerprintMap base =
        fingerprints(exportParamMap(), export_fingerprint, baseline);

    {
        procparams::ProcParams changed = baseline;
        changed.vibrance.pastels = 23;
        const FingerprintMap after =
            fingerprints(exportParamMap(), export_fingerprint, changed);
        for (const char* id : {"transform", "crop", "resize", "framing",
                               "raw.demosaic", "output.sharpen"}) {
            check(base.at(id) == after.at(id),
                  std::string("vibrance does not affect ") + id);
        }
    }

    {
        procparams::ProcParams changed = baseline;
        changed.metadata.mode = procparams::MetaDataParams::STRIP;
        const FingerprintMap after =
            fingerprints(exportParamMap(), export_fingerprint, changed);
        for (const char* id : {"crop", "resize", "framing", "lab.tools",
                               "gamut"}) {
            check(base.at(id) == after.at(id),
                  std::string("metadata does not affect ") + id);
        }
    }

    {
        procparams::ProcParams changed = baseline;
        changed.rotate.degree = 2.0;
        const FingerprintMap after =
            fingerprints(exportParamMap(), export_fingerprint, changed);
        for (const char* id : {"crop", "raw.base", "lab.tools", "ciecam"}) {
            check(base.at(id) == after.at(id),
                  std::string("rotation does not affect ") + id);
        }
    }

    /*
     * Export-only finishing state must not move a preview or crop
     * fingerprint. Crop and viewport sinks are covered too: they consume
     * `icm`, not the finishing members.
     */
    {
        procparams::ProcParams changed = baseline;
        changed.metadata.mode = procparams::MetaDataParams::STRIP;
        changed.resize.enabled = true;
        changed.resize.scale = 0.5;
        changed.framing.enabled = true;
        changed.framing.borderRed = 200;
        changed.prsharpening.contrast = 33.0;

        const FingerprintMap previewBase = fingerprints(
            previewParamMap(), preview_fingerprint, baseline);
        const FingerprintMap previewAfter = fingerprints(
            previewParamMap(), preview_fingerprint, changed);
        for (const StageParamMapEntry& entry : previewParamMap()) {
            check(previewBase.at(entry.id) == previewAfter.at(entry.id),
                  "export-only finishing state does not affect preview " +
                      entry.id);
        }

        const FingerprintMap cropBase =
            fingerprints(cropParamMap(), crop_fingerprint, baseline);
        const FingerprintMap cropAfter =
            fingerprints(cropParamMap(), crop_fingerprint, changed);
        for (const StageParamMapEntry& entry : cropParamMap()) {
            check(cropBase.at(entry.id) == cropAfter.at(entry.id),
                  "export-only finishing state does not affect crop " +
                      entry.id);
        }
    }

    /*
     * The crop rectangle enters the transform stage because post-crop
     * vignetting reads it; it must not move the display sinks.
     */
    {
        procparams::ProcParams changed = baseline;
        changed.crop.enabled = true;
        changed.crop.w = 50;

        const FingerprintMap previewBase = fingerprints(
            previewParamMap(), preview_fingerprint, baseline);
        const FingerprintMap previewAfter = fingerprints(
            previewParamMap(), preview_fingerprint, changed);
        check(previewBase.at("monitor.output") ==
                  previewAfter.at("monitor.output"),
              "crop rectangle does not affect monitor.output");
        check(previewBase.at("transform") != previewAfter.at("transform"),
              "crop rectangle affects the preview transform");

        const FingerprintMap cropBase =
            fingerprints(cropParamMap(), crop_fingerprint, baseline);
        const FingerprintMap cropAfter =
            fingerprints(cropParamMap(), crop_fingerprint, changed);
        check(cropBase.at("viewport.output") ==
                  cropAfter.at("viewport.output"),
              "crop rectangle does not affect viewport.output");
        check(cropBase.at("transform") != cropAfter.at("transform"),
              "crop rectangle affects the ROI transform");
    }
}

/*
 * The five dead SharpeningParams members are excluded because the constructor
 * leaves them uninitialised. Writing them must not move a fingerprint; if this
 * fails, the exclusion was removed without initialising them first.
 */
void checkSharpeningExclusions()
{
    const procparams::ProcParams baseline;
    const FingerprintMap base =
        fingerprints(exportParamMap(), export_fingerprint, baseline);

    procparams::ProcParams changed = baseline;
    changed.sharpening.deconvAutoRadius = true;
    changed.sharpening.deconvCornerBoost = 1.25;
    changed.sharpening.deconvCornerLatitude = 3;
    changed.sharpening.psf_kernel = "gaussian";
    changed.sharpening.psf_iterations = 2.5;
    changed.prsharpening.deconvAutoRadius = true;
    changed.prsharpening.deconvCornerBoost = 1.25;
    changed.prsharpening.deconvCornerLatitude = 3;
    changed.prsharpening.psf_kernel = "gaussian";
    changed.prsharpening.psf_iterations = 2.5;

    const FingerprintMap after =
        fingerprints(exportParamMap(), export_fingerprint, changed);
    for (const char* id : {"lab.sharpening", "output.sharpen"}) {
        check(base.at(id) == after.at(id),
              std::string("dead SharpeningParams members are excluded: ") + id);
    }
}

} // namespace

int main()
{
    checkTables();
    checkFingerprintPresence();
    checkDeterminism();
    checkNeutralStability();
    checkExportRegression();
    checkMutations();
    checkRawConsumerSplit();
    checkUnrelated();
    checkSharpeningExclusions();

    if (failures == 0) {
        std::puts("stage-param-map-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
