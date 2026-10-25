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
 * Specification test for the export stage parameter map.
 *
 * The test proves that every canonical export descriptor has a fingerprint,
 * that the fingerprint is a pure function of the parameters, and that each
 * descriptor responds to the members it claims to consume. It does not run any
 * image processing.
 *
 * The mutation table is the core evidence: one representative change per
 * parameter family, checked against all twenty descriptors. `exclusive` means
 * the changed member is listed by exactly one descriptor, so every other
 * fingerprint must stay put. A non-exclusive field may legitimately move more
 * than one fingerprint; the test then only requires the named descriptor to
 * change.
 */

#include "stage_param_map.h"

#include "stage_pipeline.h"

#include "rtengine/procparams.h"

#include <cstdio>
#include <map>
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
 * ProcParams members named by the table. A field name outside this list has
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

std::vector<std::string> exportIds()
{
    std::vector<std::string> ids;
    for (const auto& node : buildPipelineSpec(StagePath::Export).nodes) {
        ids.push_back(node.id);
    }
    return ids;
}

using FingerprintMap = std::map<std::string, ParamFingerprint>;

FingerprintMap fingerprints(const procparams::ProcParams& params)
{
    FingerprintMap result;
    for (const auto& entry : exportParamMap()) {
        result.emplace(entry.id, export_fingerprint(entry.id, params));
    }
    return result;
}

/* Criterion 1 and 5: table shape, ids and member names. */
void checkTable()
{
    const std::vector<std::string> ids = exportIds();
    const std::vector<ExportParamMapEntry>& table = exportParamMap();

    check(table.size() == ids.size(),
          "table covers every export descriptor: expected " +
              std::to_string(ids.size()) + ", got " +
              std::to_string(table.size()));
    check(table.size() == 20, "the export path has twenty descriptors");

    std::set<std::string> seen;
    for (const auto& entry : table) {
        check(!entry.id.empty(), "descriptor id is not empty");
        check(seen.insert(entry.id).second,
              "descriptor id is unique: " + entry.id);
        check(!entry.fields.empty(),
              "descriptor lists at least one member: " + entry.id);
        for (const std::string& field : entry.fields) {
            check(knownParamFields().count(field) != 0,
                  "descriptor " + entry.id +
                      " names a known ProcParams member: " + field);
        }
    }

    const std::set<std::string> specIds(ids.begin(), ids.end());
    for (const auto& entry : table) {
        check(specIds.count(entry.id) != 0,
              "table id is in the export spec: " + entry.id);
    }
    for (const std::string& id : ids) {
        check(seen.count(id) != 0, "export id has a table row: " + id);
    }

    /* The table reads like the pipeline, so review diffs stay in order. */
    for (size_t i = 0; i < table.size() && i < ids.size(); ++i) {
        check(table[i].id == ids[i],
              "table order matches the export pipeline at index " +
                  std::to_string(i));
    }
}

/* Criterion 1: every export descriptor has a fingerprint. */
void checkFingerprintPresence()
{
    const procparams::ProcParams params;
    for (const auto& node : buildPipelineSpec(StagePath::Export).nodes) {
        check(has_export_fingerprint(node.id),
              "export descriptor has a fingerprint: " + node.id);
    }
    check(!has_export_fingerprint(""),
          "an empty id has no fingerprint");
    check(!has_export_fingerprint("monitor.output"),
          "a preview descriptor has no export fingerprint");
    check(!has_export_fingerprint("viewport.output"),
          "a crop descriptor has no export fingerprint");
    check(!has_export_fingerprint("thumbnail.output"),
          "a thumbnail descriptor has no export fingerprint");
    check(!has_export_fingerprint("nonsense"),
          "an unknown id has no fingerprint");
}

/* Criterion 2: identical parameters give identical fingerprints. */
void checkDeterminism()
{
    const procparams::ProcParams params;
    const procparams::ProcParams copy;
    for (const auto& entry : exportParamMap()) {
        const ParamFingerprint first = export_fingerprint(entry.id, params);
        const ParamFingerprint second = export_fingerprint(entry.id, params);
        const ParamFingerprint third = export_fingerprint(entry.id, copy);
        check(first == second && first.hexString() == second.hexString() &&
                  first.hash() == second.hash(),
              "repeated fingerprint is identical: " + entry.id);
        check(first == third && first.hexString() == third.hexString(),
              "equal parameters give equal fingerprints: " + entry.id);
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
 * documentation for the report; the lambda is the mutation.
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
        /* Raw preprocess */
        {"raw.preprocess", "lensProf.useCA",
         [](procparams::ProcParams& p) { p.lensProf.useCA = true; }, false},
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
        /* Source select */
        {"source.select", "raw.bayersensor.imageNum",
         [](procparams::ProcParams& p) { p.raw.bayersensor.imageNum = 1; },
         false}
    };
    return table;
}

/* Criteria 3, 4 and 6: mutation behaviour of every family. */
void checkMutations()
{
    const procparams::ProcParams baseline;
    const FingerprintMap base = fingerprints(baseline);

    std::set<std::string> covered;
    for (const Mutation& mutation : mutations()) {
        check(base.count(mutation.descriptor) != 0,
              std::string("mutation names an export descriptor: ") +
                  mutation.descriptor);

        procparams::ProcParams mutated = baseline;
        mutation.apply(mutated);
        const FingerprintMap after = fingerprints(mutated);
        covered.insert(mutation.descriptor);

        for (const auto& entry : exportParamMap()) {
            const bool changed = base.at(entry.id) != after.at(entry.id);
            if (entry.id == mutation.descriptor) {
                check(changed,
                      std::string("mutation changes its own descriptor: ") +
                          mutation.descriptor + " <- " + mutation.field);
            } else if (mutation.exclusive) {
                check(!changed,
                      std::string("exclusive mutation leaves ") + entry.id +
                          " unchanged: " + mutation.descriptor + " <- " +
                          mutation.field);
            }
        }
    }

    /* Every export descriptor is exercised by at least one mutation. */
    for (const std::string& id : exportIds()) {
        check(covered.count(id) != 0,
              "mutation table covers descriptor: " + id);
    }

    check(covered.count("raw.base") != 0 && covered.count("raw.demosaic") != 0 &&
              covered.count("transform") != 0 &&
              covered.count("local.adjustments") != 0 &&
              covered.count("rgb.proc") != 0 &&
              covered.count("lab.wavelet") != 0 &&
              covered.count("ciecam") != 0 && covered.count("gamut") != 0 &&
              covered.count("output.icc") != 0 && covered.count("crop") != 0 &&
              covered.count("resize") != 0 && covered.count("framing") != 0,
          "mutation table covers the required parameter families");
}

/* Criterion 4: a member outside a descriptor's list must not move it. */
void checkUnrelated()
{
    const procparams::ProcParams baseline;
    const FingerprintMap base = fingerprints(baseline);

    {
        procparams::ProcParams changed = baseline;
        changed.vibrance.pastels = 23;
        const FingerprintMap after = fingerprints(changed);
        for (const char* id : {"transform", "crop", "resize", "framing",
                               "raw.demosaic", "output.sharpen"}) {
            check(base.at(id) == after.at(id),
                  std::string("vibrance does not affect ") + id);
        }
    }

    {
        procparams::ProcParams changed = baseline;
        changed.metadata.mode = procparams::MetaDataParams::STRIP;
        const FingerprintMap after = fingerprints(changed);
        for (const char* id : {"crop", "resize", "framing", "lab.tools",
                               "gamut"}) {
            check(base.at(id) == after.at(id),
                  std::string("metadata does not affect ") + id);
        }
    }

    {
        procparams::ProcParams changed = baseline;
        changed.rotate.degree = 2.0;
        const FingerprintMap after = fingerprints(changed);
        for (const char* id : {"crop", "raw.base", "lab.tools", "ciecam"}) {
            check(base.at(id) == after.at(id),
                  std::string("rotation does not affect ") + id);
        }
    }
}

} // namespace

int main()
{
    checkTable();
    checkFingerprintPresence();
    checkDeterminism();
    checkMutations();
    checkUnrelated();

    if (failures == 0) {
        std::puts("stage-param-map-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
