/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 RawTherapee contributors
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
#include "compareparams.h"

#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

namespace rtengine
{
namespace procparams
{

ProcParams makeComparisonParams(const ProcParams& live, const ProcParams& snapshot)
{
    ProcParams result = snapshot;

    result.crop = live.crop;
    result.cropGuide = live.cropGuide;
    result.coarse = live.coarse;
    result.commonTrans = live.commonTrans;
    result.rotate = live.rotate;
    result.distortion = live.distortion;
    result.perspective = live.perspective;
    result.lensProf = live.lensProf;

    return result;
}

bool geometryDiffers(const ProcParams& live, const ProcParams& snapshot)
{
    return live.crop != snapshot.crop
        || live.cropGuide != snapshot.cropGuide
        || live.coarse != snapshot.coarse
        || live.commonTrans != snapshot.commonTrans
        || live.rotate != snapshot.rotate
        || live.distortion != snapshot.distortion
        || live.perspective != snapshot.perspective
        || live.lensProf != snapshot.lensProf;
}

namespace
{

bool effectiveRawPath(const Glib::ustring& path)
{
    if (path.empty()) {
        return false;
    }

    // Relative paths may be resolved against the image or profile directory,
    // which is not known here. Only discount an absolute path that does not
    // exist; a relative path is always treated as meaningful.
    if (!Glib::path_is_absolute(path)) {
        return true;
    }

    return Glib::file_test(path, Glib::FILE_TEST_EXISTS);
}

RAWParams normalizeRawPaths(RAWParams raw)
{
    // Dark-frame and flat-field paths can be written into the parameters from
    // image metadata after the loaded snapshot was created. A path that is
    // not a readable file cannot be loaded by the managers and has no effect
    // on the base image. Automatic selection ignores the stored path anyway.
    if (raw.df_autoselect || !effectiveRawPath(raw.dark_frame)) {
        raw.dark_frame.clear();
    }

    if (raw.ff_AutoSelect || !effectiveRawPath(raw.ff_file)) {
        raw.ff_file.clear();
    }

    return raw;
}

bool sameWhiteBalanceInputs(const WBParams& a, const WBParams& b)
{
    if (a.enabled != b.enabled || a.method != b.method || a.equal != b.equal
            || a.tempBias != b.tempBias || a.observer != b.observer
            || a.itcwb_green != b.itcwb_green
            || a.itcwb_rgreen != b.itcwb_rgreen
            || a.itcwb_nopurple != b.itcwb_nopurple
            || a.itcwb_alg != b.itcwb_alg
            || a.itcwb_prim != b.itcwb_prim
            || a.itcwb_sampling != b.itcwb_sampling
            || a.compat_version != b.compat_version) {
        return false;
    }

    // Camera and automatic methods derive temperature and green from the
    // source. The stored values are outputs and can differ after processing
    // even though the rendered white balance is the same.
    if (a.method == "Camera" || a.method == "autold"
            || a.method == "autitcgreen") {
        return true;
    }

    return a.temperature == b.temperature && a.green == b.green;
}

bool sameCaptureSharpeningInputs(const CaptureSharpeningParams& a,
                                 const CaptureSharpeningParams& b)
{
    if (a.enabled != b.enabled || a.autoContrast != b.autoContrast
            || a.autoRadius != b.autoRadius
            || a.noisecap != b.noisecap
            || a.noisecapafter != b.noisecapafter
            || a.deconvradiusOffset != b.deconvradiusOffset
            || a.deconviter != b.deconviter
            || a.deconvitercheck != b.deconvitercheck
            || a.showcap != b.showcap || a.noisecaptype != b.noisecaptype) {
        return false;
    }

    // Automatic contrast and radius are written back as computed values. They
    // are outputs, not inputs, while their automatic flags remain equal.
    if (!a.autoContrast && a.contrast != b.contrast) {
        return false;
    }

    if (!a.autoRadius && a.deconvradius != b.deconvradius) {
        return false;
    }

    return true;
}

bool sameHighlightRecovery(const ToneCurveParams& a, const ToneCurveParams& b)
{
    return a.hrenabled == b.hrenabled
        && a.method == b.method
        && a.hlbl == b.hlbl
        && a.hlth == b.hlth
        && a.clampOOG == b.clampOOG;
}

} // namespace

bool comparisonRequiresRawStage(const ProcParams& current,
                                const ProcParams& target)
{
    // Preprocess, demosaic, capture sharpening and the raw-plane retinex
    // preparation all need the raw stage. White balance can also force a
    // re-demosaic for colour highlight recovery. Highlight recovery is part of
    // the preview base cache key, so it is included here as well.
    return normalizeRawPaths(current.raw) != normalizeRawPaths(target.raw)
        || !sameWhiteBalanceInputs(current.wb, target.wb)
        || current.lensProf != target.lensProf
        || current.retinex != target.retinex
        || !sameCaptureSharpeningInputs(current.pdsharpening,
                                        target.pdsharpening)
        || !sameHighlightRecovery(current.toneCurve, target.toneCurve);
}

} // namespace procparams
} // namespace rtengine
