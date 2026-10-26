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
 * Stage parameter map.
 *
 * Every canonical stage descriptor from the export, preview and crop
 * specifications is mapped here to the ProcParams members its block reads, and
 * the mapping is turned into a stable fingerprint with ParamHasher. The block
 * boundaries and the starting field lists come from
 * devnotes/perf/STAGE_PIPELINE.md; the extra members were added from
 * rtengine/simpleprocess.cc, rtengine/improccoordinator.cc, rtengine/dcrop.cc
 * and the tool implementations that those paths call.
 *
 * A descriptor that two paths share has one definition, so a shared block
 * hashes the same members everywhere. The export table keeps twenty entries,
 * preview selects fourteen and crop fifteen.
 *
 * Whole ProcParams sub-structs are hashed field by field. The field lists below
 * were generated once from the declarations in procparams.h, the params
 * headers and coord.h, reviewed, and committed as ordinary code: when a member
 * is added to one of those sub-structs it must be added here too.
 * Missing a field would let a stale cache entry survive a parameter change, so
 * the ordering and completeness of each list matter.
 */

#include "stage_param_map.h"

#include "coord.h"
#include "params/advanced.h"
#include "params/locallab.h"
#include "params/raw.h"
#include "params/threshold.h"
#include "procparams.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

namespace rtengine::stage
{

namespace
{

using procparams::ProcParams;

/*
 * One typed field. The overload set below is the whole type vocabulary of
 * ProcParams; the generated sub-struct hashers only ever call addField(), so a
 * field whose type is not supported here fails to compile instead of silently
 * dropping out of the fingerprint.
 */

void addField(ParamHasher& hasher, bool value)
{
    hasher.add(value);
}

void addField(ParamHasher& hasher, int value)
{
    hasher.add(static_cast<int32_t>(value));
}

void addField(ParamHasher& hasher, float value)
{
    hasher.add(value);
}

void addField(ParamHasher& hasher, double value)
{
    hasher.add(value);
}

void addField(ParamHasher& hasher, const Glib::ustring& value)
{
    hasher.addString(value);
}

void addField(ParamHasher& hasher, const std::string& value)
{
    hasher.addString(value);
}

/*
 * Every enum is hashed as its underlying integer value. The enums in
 * ProcParams are small and never negative, so int32_t is exact.
 */
template <typename Enum,
          typename std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
void addField(ParamHasher& hasher, Enum value)
{
    hasher.add(static_cast<int32_t>(value));
}

/* Forward declarations of the sub-struct hashers, so the generic
 * container templates can find them by ordinary lookup. */
void addField(ParamHasher& hasher, const procparams::RAWParams::BayerSensor& value);
void addField(ParamHasher& hasher, const procparams::RAWParams::XTransSensor& value);
void addField(ParamHasher& hasher, const procparams::RAWParams::PreprocessWB& value);
void addField(ParamHasher& hasher, const procparams::RAWParams& value);
void addField(ParamHasher& hasher, const procparams::CaptureSharpeningParams& value);
void addField(ParamHasher& hasher, const procparams::LensProfParams& value);
void addField(ParamHasher& hasher, const procparams::CoarseTransformParams& value);
void addField(ParamHasher& hasher, const procparams::DirPyrDenoiseParams& value);
void addField(ParamHasher& hasher, const procparams::WBParams& value);
void addField(ParamHasher& hasher, const procparams::ToneCurveParams& value);
void addField(ParamHasher& hasher, const procparams::SpotEntry& value);
void addField(ParamHasher& hasher, const procparams::SpotParams& value);
void addField(ParamHasher& hasher, const procparams::FilmNegativeParams::RGB& value);
void addField(ParamHasher& hasher, const procparams::FilmNegativeParams& value);
void addField(ParamHasher& hasher, const procparams::RetinexParams& value);
void addField(ParamHasher& hasher, const procparams::DehazeParams& value);
void addField(ParamHasher& hasher, const procparams::FattalToneMappingParams& value);
void addField(ParamHasher& hasher, const procparams::CGParams& value);
void addField(ParamHasher& hasher, const procparams::ColorManagementParams& value);
void addField(ParamHasher& hasher, const procparams::CommonTransformParams& value);
void addField(ParamHasher& hasher, const procparams::RotateParams& value);
void addField(ParamHasher& hasher, const procparams::DistortionParams& value);
void addField(ParamHasher& hasher, const procparams::PerspectiveParams& value);
void addField(ParamHasher& hasher, const procparams::CACorrParams& value);
void addField(ParamHasher& hasher, const procparams::VignettingParams& value);
void addField(ParamHasher& hasher, const procparams::GradientParams& value);
void addField(ParamHasher& hasher, const procparams::PCVignetteParams& value);
void addField(ParamHasher& hasher, const procparams::LocallabParams::LocallabSpot& value);
void addField(ParamHasher& hasher, const procparams::LocallabParams& value);
void addField(ParamHasher& hasher, const procparams::RGBCurvesParams& value);
void addField(ParamHasher& hasher, const procparams::ColorToningParams::LabCorrectionRegion& value);
void addField(ParamHasher& hasher, const procparams::ColorToningParams& value);
void addField(ParamHasher& hasher, const procparams::BlackWhiteParams& value);
void addField(ParamHasher& hasher, const procparams::ChannelMixerParams& value);
void addField(ParamHasher& hasher, const procparams::HSVEqualizerParams& value);
void addField(ParamHasher& hasher, const procparams::FilmSimulationParams& value);
void addField(ParamHasher& hasher, const procparams::SHParams& value);
void addField(ParamHasher& hasher, const procparams::LCurveParams& value);
void addField(ParamHasher& hasher, const procparams::LocalContrastParams& value);
void addField(ParamHasher& hasher, const procparams::VibranceParams& value);
void addField(ParamHasher& hasher, const procparams::EPDParams& value);
void addField(ParamHasher& hasher, const procparams::SoftLightParams& value);
void addField(ParamHasher& hasher, const procparams::SharpeningParams& value);
void addField(ParamHasher& hasher, const procparams::SharpenEdgeParams& value);
void addField(ParamHasher& hasher, const procparams::SharpenMicroParams& value);
void addField(ParamHasher& hasher, const procparams::ImpulseDenoiseParams& value);
void addField(ParamHasher& hasher, const procparams::DefringeParams& value);
void addField(ParamHasher& hasher, const procparams::WaveletParams& value);
void addField(ParamHasher& hasher, const procparams::DirPyrEqualizerParams& value);
void addField(ParamHasher& hasher, const procparams::ToneEqualizerParams& value);
void addField(ParamHasher& hasher, const procparams::ColorAppearanceParams& value);
void addField(ParamHasher& hasher, const procparams::CropParams& value);
void addField(ParamHasher& hasher, const procparams::ResizeParams& value);
void addField(ParamHasher& hasher, const procparams::FramingParams& value);
void addField(ParamHasher& hasher, const procparams::MetaDataParams& value);

/*
 * Containers are length-framed so that {1} and {1, 0} cannot collide, and the
 * empty container differs from an absent field because the length is still
 * added.
 */
template <typename T>
void addField(ParamHasher& hasher, const std::vector<T>& value)
{
    hasher.add(static_cast<uint64_t>(value.size()));
    for (const T& element : value) {
        addField(hasher, element);
    }
}

template <typename T, size_t N>
void addField(ParamHasher& hasher, const std::array<T, N>& value)
{
    hasher.add(static_cast<uint64_t>(N));
    for (const T& element : value) {
        addField(hasher, element);
    }
}

/* C arrays: int[3] in the channel mixer and int[9] in the wavelet tools. */
template <typename T, size_t N>
void addField(ParamHasher& hasher, const T (&value)[N])
{
    hasher.add(static_cast<uint64_t>(N));
    for (const T& element : value) {
        addField(hasher, element);
    }
}

/*
 * Threshold has no public iterable form, so hash the four corner values and
 * the two-point/four-point flag. The unused corners are part of the object and
 * therefore part of the value, even when the flag says they are ignored.
 */
template <typename T>
void addField(ParamHasher& hasher, const procparams::Threshold<T>& value)
{
    addField(hasher, value.getBottomLeft());
    addField(hasher, value.getTopLeft());
    addField(hasher, value.getBottomRight());
    addField(hasher, value.getTopRight());
    hasher.add(value.isDouble());
}

void addField(ParamHasher& hasher, const Coord& value)
{
    addField(hasher, value.x);
    addField(hasher, value.y);
}

/*
 * Metadata maps iterate in key order because std::map is ordered, so the
 * fingerprint is independent of insertion order.
 */
void addField(ParamHasher& hasher, const procparams::ExifPairs& value)
{
    for (auto it = value.begin(); it != value.end(); ++it) {
        addField(hasher, it->first);
        addField(hasher, it->second);
    }
}

void addField(ParamHasher& hasher, const procparams::IPTCPairs& value)
{
    for (auto it = value.begin(); it != value.end(); ++it) {
        addField(hasher, it->first);
        addField(hasher, it->second);
    }
}

/*
 * Sub-struct hashers. Each one adds every data member of its type in
 * declaration order. Static constants, enums, methods and constructors are not
 * data members and are not hashed.
 */


/* Definitions, in declaration order of their dependencies. */
void addField(ParamHasher& hasher, const procparams::RAWParams::BayerSensor& value)
{
    addField(hasher, value.method);
    addField(hasher, value.border);
    addField(hasher, value.imageNum);
    addField(hasher, value.ccSteps);
    addField(hasher, value.black0);
    addField(hasher, value.black1);
    addField(hasher, value.black2);
    addField(hasher, value.black3);
    addField(hasher, value.twogreen);
    addField(hasher, value.Dehablack);
    addField(hasher, value.linenoise);
    addField(hasher, value.linenoiseDirection);
    addField(hasher, value.greenthresh);
    addField(hasher, value.dcb_iterations);
    addField(hasher, value.lmmse_iterations);
    addField(hasher, value.dualDemosaicAutoContrast);
    addField(hasher, value.dualDemosaicContrast);
    addField(hasher, value.pixelShiftMotionCorrectionMethod);
    addField(hasher, value.pixelShiftEperIso);
    addField(hasher, value.pixelShiftSigma);
    addField(hasher, value.pixelShiftShowMotion);
    addField(hasher, value.pixelShiftShowMotionMaskOnly);
    addField(hasher, value.pixelShiftHoleFill);
    addField(hasher, value.pixelShiftMedian);
    addField(hasher, value.pixelShiftAverage);
    addField(hasher, value.pixelShiftGreen);
    addField(hasher, value.pixelShiftBlur);
    addField(hasher, value.pixelShiftSmoothFactor);
    addField(hasher, value.pixelShiftEqualBright);
    addField(hasher, value.pixelShiftEqualBrightChannel);
    addField(hasher, value.pixelShiftNonGreenCross);
    addField(hasher, value.pixelShiftDemosaicMethod);
    addField(hasher, value.dcb_enhance);
    addField(hasher, value.pdafLinesFilter);
}

void addField(ParamHasher& hasher, const procparams::RAWParams::XTransSensor& value)
{
    addField(hasher, value.method);
    addField(hasher, value.dualDemosaicAutoContrast);
    addField(hasher, value.dualDemosaicContrast);
    addField(hasher, value.border);
    addField(hasher, value.ccSteps);
    addField(hasher, value.blackred);
    addField(hasher, value.blackgreen);
    addField(hasher, value.blackblue);
    addField(hasher, value.Dehablackx);
}

void addField(ParamHasher& hasher, const procparams::RAWParams::PreprocessWB& value)
{
    addField(hasher, value.mode);
}

void addField(ParamHasher& hasher, const procparams::RAWParams& value)
{
    addField(hasher, value.bayersensor);
    addField(hasher, value.xtranssensor);
    addField(hasher, value.dark_frame);
    addField(hasher, value.df_autoselect);
    addField(hasher, value.ff_file);
    addField(hasher, value.ff_AutoSelect);
    addField(hasher, value.ff_FromMetaData);
    addField(hasher, value.ff_BlurRadius);
    addField(hasher, value.ff_BlurType);
    addField(hasher, value.ff_AutoClipControl);
    addField(hasher, value.ff_clipControl);
    addField(hasher, value.ca_autocorrect);
    addField(hasher, value.ca_avoidcolourshift);
    addField(hasher, value.caautoiterations);
    addField(hasher, value.cared);
    addField(hasher, value.cablue);
    addField(hasher, value.expos);
    addField(hasher, value.preprocessWB);
    addField(hasher, value.hotPixelFilter);
    addField(hasher, value.deadPixelFilter);
    addField(hasher, value.hotdeadpix_thresh);
}

void addField(ParamHasher& hasher, const procparams::CaptureSharpeningParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.autoContrast);
    addField(hasher, value.autoRadius);
    addField(hasher, value.contrast);
    addField(hasher, value.noisecap);
    addField(hasher, value.noisecapafter);
    addField(hasher, value.deconvradius);
    addField(hasher, value.deconvradiusOffset);
    addField(hasher, value.deconviter);
    addField(hasher, value.deconvitercheck);
    addField(hasher, value.showcap);
    addField(hasher, value.noisecaptype);
}

void addField(ParamHasher& hasher, const procparams::LensProfParams& value)
{
    addField(hasher, value.lcMode);
    addField(hasher, value.lcpFile);
    addField(hasher, value.useDist);
    addField(hasher, value.useVign);
    addField(hasher, value.useCA);
    addField(hasher, value.lfCameraMake);
    addField(hasher, value.lfCameraModel);
    addField(hasher, value.lfLens);
}

void addField(ParamHasher& hasher, const procparams::CoarseTransformParams& value)
{
    addField(hasher, value.rotate);
    addField(hasher, value.hflip);
    addField(hasher, value.vflip);
}

void addField(ParamHasher& hasher, const procparams::DirPyrDenoiseParams& value)
{
    addField(hasher, value.lcurve);
    addField(hasher, value.cccurve);
    addField(hasher, value.enabled);
    addField(hasher, value.enhance);
    addField(hasher, value.median);
    addField(hasher, value.perform);
    addField(hasher, value.luma);
    addField(hasher, value.Ldetail);
    addField(hasher, value.chroma);
    addField(hasher, value.redchro);
    addField(hasher, value.bluechro);
    addField(hasher, value.autoGain);
    addField(hasher, value.gamma);
    addField(hasher, value.dmethod);
    addField(hasher, value.Lmethod);
    addField(hasher, value.Cmethod);
    addField(hasher, value.C2method);
    addField(hasher, value.smethod);
    addField(hasher, value.medmethod);
    addField(hasher, value.methodmed);
    addField(hasher, value.rgbmethod);
    addField(hasher, value.passes);
}

void addField(ParamHasher& hasher, const procparams::WBParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.method);
    addField(hasher, value.temperature);
    addField(hasher, value.green);
    addField(hasher, value.equal);
    addField(hasher, value.tempBias);
    addField(hasher, value.observer);
    addField(hasher, value.itcwb_green);
    addField(hasher, value.itcwb_rgreen);
    addField(hasher, value.itcwb_nopurple);
    addField(hasher, value.itcwb_alg);
    addField(hasher, value.itcwb_prim);
    addField(hasher, value.itcwb_sampling);
    addField(hasher, value.compat_version);
}

void addField(ParamHasher& hasher, const procparams::ToneCurveParams& value)
{
    addField(hasher, value.autoexp);
    addField(hasher, value.clip);
    addField(hasher, value.hrenabled);
    addField(hasher, value.method);
    addField(hasher, value.expcomp);
    addField(hasher, value.curve);
    addField(hasher, value.curve2);
    addField(hasher, value.curveMode);
    addField(hasher, value.curveMode2);
    addField(hasher, value.brightness);
    addField(hasher, value.black);
    addField(hasher, value.contrast);
    addField(hasher, value.saturation);
    addField(hasher, value.shcompr);
    addField(hasher, value.hlcompr);
    addField(hasher, value.hlbl);
    addField(hasher, value.hlth);
    addField(hasher, value.hlcomprthresh);
    addField(hasher, value.histmatching);
    addField(hasher, value.fromHistMatching);
    addField(hasher, value.clampOOG);
}

void addField(ParamHasher& hasher, const procparams::SpotEntry& value)
{
    addField(hasher, value.sourcePos);
    addField(hasher, value.targetPos);
    addField(hasher, value.radius);
    addField(hasher, value.feather);
    addField(hasher, value.opacity);
}

void addField(ParamHasher& hasher, const procparams::SpotParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.entries);
}

void addField(ParamHasher& hasher, const procparams::FilmNegativeParams::RGB& value)
{
    addField(hasher, value.r);
    addField(hasher, value.g);
    addField(hasher, value.b);
}

void addField(ParamHasher& hasher, const procparams::FilmNegativeParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.redRatio);
    addField(hasher, value.greenExp);
    addField(hasher, value.blueRatio);
    addField(hasher, value.refInput);
    addField(hasher, value.refOutput);
    addField(hasher, value.colorSpace);
    addField(hasher, value.backCompat);
}

void addField(ParamHasher& hasher, const procparams::RetinexParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.cdcurve);
    addField(hasher, value.cdHcurve);
    addField(hasher, value.lhcurve);
    addField(hasher, value.transmissionCurve);
    addField(hasher, value.gaintransmissionCurve);
    addField(hasher, value.mapcurve);
    addField(hasher, value.str);
    addField(hasher, value.scal);
    addField(hasher, value.iter);
    addField(hasher, value.grad);
    addField(hasher, value.grads);
    addField(hasher, value.gam);
    addField(hasher, value.slope);
    addField(hasher, value.neigh);
    addField(hasher, value.offs);
    addField(hasher, value.highlights);
    addField(hasher, value.htonalwidth);
    addField(hasher, value.shadows);
    addField(hasher, value.stonalwidth);
    addField(hasher, value.radius);
    addField(hasher, value.complexmethod);
    addField(hasher, value.retinexMethod);
    addField(hasher, value.retinexcolorspace);
    addField(hasher, value.gammaretinex);
    addField(hasher, value.mapMethod);
    addField(hasher, value.viewMethod);
    addField(hasher, value.vart);
    addField(hasher, value.limd);
    addField(hasher, value.highl);
    addField(hasher, value.skal);
    addField(hasher, value.medianmap);
}

void addField(ParamHasher& hasher, const procparams::DehazeParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.strength);
    addField(hasher, value.saturation);
    addField(hasher, value.showDepthMap);
    addField(hasher, value.depth);
}

void addField(ParamHasher& hasher, const procparams::FattalToneMappingParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.threshold);
    addField(hasher, value.amount);
    addField(hasher, value.anchor);
}

void addField(ParamHasher& hasher, const procparams::CGParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.th_c);
    addField(hasher, value.th_m);
    addField(hasher, value.th_y);
    addField(hasher, value.d_c);
    addField(hasher, value.autodc);
    addField(hasher, value.d_m);
    addField(hasher, value.autodm);
    addField(hasher, value.d_y);
    addField(hasher, value.autody);
    addField(hasher, value.pwr);
    addField(hasher, value.colorspace);
    addField(hasher, value.rolloff);
}

void addField(ParamHasher& hasher, const procparams::ColorManagementParams& value)
{
    addField(hasher, value.inputProfile);
    addField(hasher, value.toneCurve);
    addField(hasher, value.applyLookTable);
    addField(hasher, value.applyBaselineExposureOffset);
    addField(hasher, value.applyHueSatMap);
    addField(hasher, value.dcpIlluminant);
    addField(hasher, value.workingProfile);
    addField(hasher, value.workingTRC);
    addField(hasher, value.wgamut);
    addField(hasher, value.will);
    addField(hasher, value.wprim);
    addField(hasher, value.wcat);
    addField(hasher, value.wGamma);
    addField(hasher, value.wSlope);
    addField(hasher, value.wapsat);
    addField(hasher, value.wmidtcie);
    addField(hasher, value.sigmatrc);
    addField(hasher, value.offstrc);
    addField(hasher, value.residtrc);
    addField(hasher, value.wgampower);
    addField(hasher, value.wgamgain);
    addField(hasher, value.pyrwavtrc);
    addField(hasher, value.opacityCurveWLI);
    addField(hasher, value.wsmoothcie);
    addField(hasher, value.wsmoothciesli);
    addField(hasher, value.redx);
    addField(hasher, value.redy);
    addField(hasher, value.grex);
    addField(hasher, value.grey);
    addField(hasher, value.blux);
    addField(hasher, value.bluy);
    addField(hasher, value.redrot);
    addField(hasher, value.redsat);
    addField(hasher, value.grerot);
    addField(hasher, value.gresat);
    addField(hasher, value.blurot);
    addField(hasher, value.blusat);
    addField(hasher, value.refi);
    addField(hasher, value.shiftx);
    addField(hasher, value.shifty);
    addField(hasher, value.preser);
    addField(hasher, value.fbw);
    addField(hasher, value.trcExp);
    addField(hasher, value.wavExp);
    addField(hasher, value.gamut);
    addField(hasher, value.labgridcieALow);
    addField(hasher, value.labgridcieBLow);
    addField(hasher, value.labgridcieAHigh);
    addField(hasher, value.labgridcieBHigh);
    addField(hasher, value.labgridcieGx);
    addField(hasher, value.labgridcieGy);
    addField(hasher, value.labgridcieWx);
    addField(hasher, value.labgridcieWy);
    addField(hasher, value.labgridcieMx);
    addField(hasher, value.labgridcieMy);
    addField(hasher, value.aRendIntent);
    addField(hasher, value.outputProfile);
    addField(hasher, value.outputIntent);
    addField(hasher, value.outputBPC);
}

void addField(ParamHasher& hasher, const procparams::CommonTransformParams& value)
{
    addField(hasher, value.method);
    addField(hasher, value.autofill);
    addField(hasher, value.scale);
    addField(hasher, value.scale_horizontally);
    addField(hasher, value.scale_vertically);
}

void addField(ParamHasher& hasher, const procparams::RotateParams& value)
{
    addField(hasher, value.degree);
}

void addField(ParamHasher& hasher, const procparams::DistortionParams& value)
{
    addField(hasher, value.amount);
    addField(hasher, value.defish);
    addField(hasher, value.focal_length);
}

void addField(ParamHasher& hasher, const procparams::PerspectiveParams& value)
{
    addField(hasher, value.method);
    addField(hasher, value.render);
    addField(hasher, value.horizontal);
    addField(hasher, value.vertical);
    addField(hasher, value.camera_crop_factor);
    addField(hasher, value.camera_focal_length);
    addField(hasher, value.camera_pitch);
    addField(hasher, value.camera_roll);
    addField(hasher, value.camera_shift_horiz);
    addField(hasher, value.camera_shift_vert);
    addField(hasher, value.camera_yaw);
    addField(hasher, value.projection_pitch);
    addField(hasher, value.projection_rotate);
    addField(hasher, value.projection_shift_horiz);
    addField(hasher, value.projection_shift_vert);
    addField(hasher, value.projection_yaw);
    addField(hasher, value.control_line_values);
    addField(hasher, value.control_line_types);
}

void addField(ParamHasher& hasher, const procparams::CACorrParams& value)
{
    addField(hasher, value.red);
    addField(hasher, value.blue);
}

void addField(ParamHasher& hasher, const procparams::VignettingParams& value)
{
    addField(hasher, value.amount);
    addField(hasher, value.radius);
    addField(hasher, value.strength);
    addField(hasher, value.centerX);
    addField(hasher, value.centerY);
}

void addField(ParamHasher& hasher, const procparams::GradientParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.degree);
    addField(hasher, value.feather);
    addField(hasher, value.strength);
    addField(hasher, value.centerX);
    addField(hasher, value.centerY);
}

void addField(ParamHasher& hasher, const procparams::PCVignetteParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.strength);
    addField(hasher, value.feather);
    addField(hasher, value.roundness);
}

void addField(ParamHasher& hasher, const procparams::LocallabParams::LocallabSpot& value)
{
    addField(hasher, value.name);
    addField(hasher, value.isvisible);
    addField(hasher, value.prevMethod);
    addField(hasher, value.shape);
    addField(hasher, value.spotMethod);
    addField(hasher, value.wavMethod);
    addField(hasher, value.sensiexclu);
    addField(hasher, value.structexclu);
    addField(hasher, value.struc);
    addField(hasher, value.shapeMethod);
    addField(hasher, value.avoidgamutMethod);
    addField(hasher, value.loc);
    addField(hasher, value.centerX);
    addField(hasher, value.centerY);
    addField(hasher, value.circrad);
    addField(hasher, value.qualityMethod);
    addField(hasher, value.complexMethod);
    addField(hasher, value.transit);
    addField(hasher, value.feather);
    addField(hasher, value.thresh);
    addField(hasher, value.iter);
    addField(hasher, value.balan);
    addField(hasher, value.balanh);
    addField(hasher, value.colorde);
    addField(hasher, value.colorscope);
    addField(hasher, value.avoidrad);
    addField(hasher, value.transitweak);
    addField(hasher, value.transitgrad);
    addField(hasher, value.hishow);
    addField(hasher, value.activ);
    addField(hasher, value.avoidneg);
    addField(hasher, value.blwh);
    addField(hasher, value.recurs);
    addField(hasher, value.laplac);
    addField(hasher, value.deltae);
    addField(hasher, value.shortc);
    addField(hasher, value.savrest);
    addField(hasher, value.scopemask);
    addField(hasher, value.denoichmask);
    addField(hasher, value.lumask);
    addField(hasher, value.visicolor);
    addField(hasher, value.expcolor);
    addField(hasher, value.complexcolor);
    addField(hasher, value.curvactiv);
    addField(hasher, value.lightness);
    addField(hasher, value.reparcol);
    addField(hasher, value.gamc);
    addField(hasher, value.contrast);
    addField(hasher, value.chroma);
    addField(hasher, value.labgridALow);
    addField(hasher, value.labgridBLow);
    addField(hasher, value.labgridAHigh);
    addField(hasher, value.labgridBHigh);
    addField(hasher, value.labgridALowmerg);
    addField(hasher, value.labgridBLowmerg);
    addField(hasher, value.labgridAHighmerg);
    addField(hasher, value.labgridBHighmerg);
    addField(hasher, value.strengthgrid);
    addField(hasher, value.sensi);
    addField(hasher, value.structcol);
    addField(hasher, value.strcol);
    addField(hasher, value.strcolab);
    addField(hasher, value.strcolh);
    addField(hasher, value.angcol);
    addField(hasher, value.feathercol);
    addField(hasher, value.blurcolde);
    addField(hasher, value.blurcol);
    addField(hasher, value.contcol);
    addField(hasher, value.blendmaskcol);
    addField(hasher, value.radmaskcol);
    addField(hasher, value.chromaskcol);
    addField(hasher, value.gammaskcol);
    addField(hasher, value.slomaskcol);
    addField(hasher, value.shadmaskcol);
    addField(hasher, value.strumaskcol);
    addField(hasher, value.lapmaskcol);
    addField(hasher, value.qualitycurveMethod);
    addField(hasher, value.gridMethod);
    addField(hasher, value.merMethod);
    addField(hasher, value.toneMethod);
    addField(hasher, value.mergecolMethod);
    addField(hasher, value.llcurve);
    addField(hasher, value.lccurve);
    addField(hasher, value.cccurve);
    addField(hasher, value.clcurve);
    addField(hasher, value.rgbcurve);
    addField(hasher, value.LHcurve);
    addField(hasher, value.HHcurve);
    addField(hasher, value.CHcurve);
    addField(hasher, value.invers);
    addField(hasher, value.special);
    addField(hasher, value.toolcol);
    addField(hasher, value.enaColorMask);
    addField(hasher, value.fftColorMask);
    addField(hasher, value.CCmaskcurve);
    addField(hasher, value.LLmaskcurve);
    addField(hasher, value.HHmaskcurve);
    addField(hasher, value.HHhmaskcurve);
    addField(hasher, value.softradiuscol);
    addField(hasher, value.opacol);
    addField(hasher, value.mercol);
    addField(hasher, value.merlucol);
    addField(hasher, value.conthrcol);
    addField(hasher, value.Lmaskcurve);
    addField(hasher, value.LLmaskcolcurvewav);
    addField(hasher, value.csthresholdcol);
    addField(hasher, value.recothresc);
    addField(hasher, value.lowthresc);
    addField(hasher, value.higthresc);
    addField(hasher, value.decayc);
    addField(hasher, value.visiexpose);
    addField(hasher, value.expexpose);
    addField(hasher, value.complexexpose);
    addField(hasher, value.expcomp);
    addField(hasher, value.hlcompr);
    addField(hasher, value.hlcomprthresh);
    addField(hasher, value.black);
    addField(hasher, value.shadex);
    addField(hasher, value.shcompr);
    addField(hasher, value.expchroma);
    addField(hasher, value.sensiex);
    addField(hasher, value.structexp);
    addField(hasher, value.blurexpde);
    addField(hasher, value.gamex);
    addField(hasher, value.strexp);
    addField(hasher, value.angexp);
    addField(hasher, value.featherexp);
    addField(hasher, value.excurve);
    addField(hasher, value.norm);
    addField(hasher, value.inversex);
    addField(hasher, value.enaExpMask);
    addField(hasher, value.enaExpMaskaft);
    addField(hasher, value.CCmaskexpcurve);
    addField(hasher, value.LLmaskexpcurve);
    addField(hasher, value.HHmaskexpcurve);
    addField(hasher, value.blendmaskexp);
    addField(hasher, value.radmaskexp);
    addField(hasher, value.chromaskexp);
    addField(hasher, value.gammaskexp);
    addField(hasher, value.slomaskexp);
    addField(hasher, value.lapmaskexp);
    addField(hasher, value.strmaskexp);
    addField(hasher, value.angmaskexp);
    addField(hasher, value.softradiusexp);
    addField(hasher, value.Lmaskexpcurve);
    addField(hasher, value.expMethod);
    addField(hasher, value.exnoiseMethod);
    addField(hasher, value.laplacexp);
    addField(hasher, value.reparexp);
    addField(hasher, value.balanexp);
    addField(hasher, value.linear);
    addField(hasher, value.gamm);
    addField(hasher, value.fatamount);
    addField(hasher, value.fatdetail);
    addField(hasher, value.fatsatur);
    addField(hasher, value.fatanchor);
    addField(hasher, value.fatlevel);
    addField(hasher, value.recothrese);
    addField(hasher, value.lowthrese);
    addField(hasher, value.higthrese);
    addField(hasher, value.decaye);
    addField(hasher, value.visishadhigh);
    addField(hasher, value.expshadhigh);
    addField(hasher, value.complexshadhigh);
    addField(hasher, value.shMethod);
    addField(hasher, value.ghsMethod);
    addField(hasher, value.ghsMatmet);
    addField(hasher, value.ghsMode);
    addField(hasher, value.ghs_D);
    addField(hasher, value.ghs_slope);
    addField(hasher, value.ghs_chro);
    addField(hasher, value.ghs_B);
    addField(hasher, value.ghs_SP);
    addField(hasher, value.SPAutoRadius);
    addField(hasher, value.ghs_LP);
    addField(hasher, value.ghs_HP);
    addField(hasher, value.ghs_LC);
    addField(hasher, value.ghs_MID);
    addField(hasher, value.ghs_BLP);
    addField(hasher, value.ghs_HLP);
    addField(hasher, value.ghs_autobw);
    addField(hasher, value.ghs_agx);
    addField(hasher, value.ghs_smooth);
    addField(hasher, value.ghs_inv);
    addField(hasher, value.mich_exp);
    addField(hasher, value.mich_spar);
    addField(hasher, value.mich_kpar);
    addField(hasher, value.mich_sat);
    addField(hasher, value.mich_out);
    addField(hasher, value.mich_black);
    addField(hasher, value.mich_white);
    addField(hasher, value.mich_high);
    addField(hasher, value.mich_jdx);
    addField(hasher, value.multsh);
    addField(hasher, value.highlights);
    addField(hasher, value.h_tonalwidth);
    addField(hasher, value.shadows);
    addField(hasher, value.s_tonalwidth);
    addField(hasher, value.sh_radius);
    addField(hasher, value.sensihs);
    addField(hasher, value.enaSHMask);
    addField(hasher, value.CCmaskSHcurve);
    addField(hasher, value.LLmaskSHcurve);
    addField(hasher, value.HHmaskSHcurve);
    addField(hasher, value.blendmaskSH);
    addField(hasher, value.radmaskSH);
    addField(hasher, value.blurSHde);
    addField(hasher, value.strSH);
    addField(hasher, value.angSH);
    addField(hasher, value.featherSH);
    addField(hasher, value.inverssh);
    addField(hasher, value.chromaskSH);
    addField(hasher, value.gammaskSH);
    addField(hasher, value.slomaskSH);
    addField(hasher, value.lapmaskSH);
    addField(hasher, value.detailSH);
    addField(hasher, value.tePivot);
    addField(hasher, value.reparsh);
    addField(hasher, value.LmaskSHcurve);
    addField(hasher, value.fatamountSH);
    addField(hasher, value.fatanchorSH);
    addField(hasher, value.gamSH);
    addField(hasher, value.sloSH);
    addField(hasher, value.recothress);
    addField(hasher, value.lowthress);
    addField(hasher, value.higthress);
    addField(hasher, value.decays);
    addField(hasher, value.visivibrance);
    addField(hasher, value.expvibrance);
    addField(hasher, value.complexvibrance);
    addField(hasher, value.saturated);
    addField(hasher, value.pastels);
    addField(hasher, value.vibgam);
    addField(hasher, value.warm);
    addField(hasher, value.psthreshold);
    addField(hasher, value.protectskins);
    addField(hasher, value.avoidcolorshift);
    addField(hasher, value.pastsattog);
    addField(hasher, value.sensiv);
    addField(hasher, value.skintonescurve);
    addField(hasher, value.CCmaskvibcurve);
    addField(hasher, value.LLmaskvibcurve);
    addField(hasher, value.HHmaskvibcurve);
    addField(hasher, value.enavibMask);
    addField(hasher, value.blendmaskvib);
    addField(hasher, value.radmaskvib);
    addField(hasher, value.chromaskvib);
    addField(hasher, value.gammaskvib);
    addField(hasher, value.slomaskvib);
    addField(hasher, value.lapmaskvib);
    addField(hasher, value.strvib);
    addField(hasher, value.strvibab);
    addField(hasher, value.strvibh);
    addField(hasher, value.angvib);
    addField(hasher, value.feathervib);
    addField(hasher, value.Lmaskvibcurve);
    addField(hasher, value.recothresv);
    addField(hasher, value.lowthresv);
    addField(hasher, value.higthresv);
    addField(hasher, value.decayv);
    addField(hasher, value.visisoft);
    addField(hasher, value.expsoft);
    addField(hasher, value.complexsoft);
    addField(hasher, value.streng);
    addField(hasher, value.sensisf);
    addField(hasher, value.laplace);
    addField(hasher, value.softMethod);
    addField(hasher, value.visiblur);
    addField(hasher, value.expblur);
    addField(hasher, value.complexblur);
    addField(hasher, value.radius);
    addField(hasher, value.strength);
    addField(hasher, value.sensibn);
    addField(hasher, value.itera);
    addField(hasher, value.guidbl);
    addField(hasher, value.strbl);
    addField(hasher, value.recothres);
    addField(hasher, value.lowthres);
    addField(hasher, value.higthres);
    addField(hasher, value.recothresd);
    addField(hasher, value.lowthresd);
    addField(hasher, value.midthresd);
    addField(hasher, value.midthresdch);
    addField(hasher, value.higthresd);
    addField(hasher, value.decayd);
    addField(hasher, value.isogr);
    addField(hasher, value.strengr);
    addField(hasher, value.scalegr);
    addField(hasher, value.divgr);
    addField(hasher, value.epsbl);
    addField(hasher, value.blMethod);
    addField(hasher, value.chroMethod);
    addField(hasher, value.quamethod);
    addField(hasher, value.blurMethod);
    addField(hasher, value.medMethod);
    addField(hasher, value.usemask);
    addField(hasher, value.invmaskd);
    addField(hasher, value.invmask);
    addField(hasher, value.levelthr);
    addField(hasher, value.lnoiselow);
    addField(hasher, value.levelthrlow);
    addField(hasher, value.activlum);
    addField(hasher, value.madlsav);
    addField(hasher, value.noiselumf);
    addField(hasher, value.noiselumf0);
    addField(hasher, value.noiselumf2);
    addField(hasher, value.noiselumc);
    addField(hasher, value.noiselumdetail);
    addField(hasher, value.noiselequal);
    addField(hasher, value.noisegam);
    addField(hasher, value.noisechrof);
    addField(hasher, value.noisechroc);
    addField(hasher, value.noisechrodetail);
    addField(hasher, value.adjblur);
    addField(hasher, value.bilateral);
    addField(hasher, value.nlstr);
    addField(hasher, value.nldet);
    addField(hasher, value.nlpat);
    addField(hasher, value.nlrad);
    addField(hasher, value.nlgam);
    addField(hasher, value.nliter);
    addField(hasher, value.sensiden);
    addField(hasher, value.reparden);
    addField(hasher, value.detailthr);
    addField(hasher, value.locwavcurveden);
    addField(hasher, value.locwavcurvehue);
    addField(hasher, value.locwavcurvehuecont);
    addField(hasher, value.showmaskblMethodtyp);
    addField(hasher, value.CCmaskblcurve);
    addField(hasher, value.LLmaskblcurve);
    addField(hasher, value.HHmaskblcurve);
    addField(hasher, value.enablMask);
    addField(hasher, value.fftwbl);
    addField(hasher, value.invbl);
    addField(hasher, value.toolbl);
    addField(hasher, value.blendmaskbl);
    addField(hasher, value.radmaskbl);
    addField(hasher, value.chromaskbl);
    addField(hasher, value.gammaskbl);
    addField(hasher, value.slomaskbl);
    addField(hasher, value.lapmaskbl);
    addField(hasher, value.shadmaskbl);
    addField(hasher, value.shadmaskblsha);
    addField(hasher, value.strumaskbl);
    addField(hasher, value.Lmaskblcurve);
    addField(hasher, value.LLmaskblcurvewav);
    addField(hasher, value.csthresholdblur);
    addField(hasher, value.denocontrast);
    addField(hasher, value.denoAutocontrast);
    addField(hasher, value.contrshow);
    addField(hasher, value.lockmadl);
    addField(hasher, value.madllock);
    addField(hasher, value.enacontrast);
    addField(hasher, value.denoratio);
    addField(hasher, value.denomask);
    addField(hasher, value.visitonemap);
    addField(hasher, value.exptonemap);
    addField(hasher, value.complextonemap);
    addField(hasher, value.stren);
    addField(hasher, value.gamma);
    addField(hasher, value.estop);
    addField(hasher, value.scaltm);
    addField(hasher, value.repartm);
    addField(hasher, value.rewei);
    addField(hasher, value.satur);
    addField(hasher, value.sensitm);
    addField(hasher, value.softradiustm);
    addField(hasher, value.amount);
    addField(hasher, value.equiltm);
    addField(hasher, value.CCmasktmcurve);
    addField(hasher, value.LLmasktmcurve);
    addField(hasher, value.HHmasktmcurve);
    addField(hasher, value.enatmMask);
    addField(hasher, value.enatmMaskaft);
    addField(hasher, value.blendmasktm);
    addField(hasher, value.radmasktm);
    addField(hasher, value.chromasktm);
    addField(hasher, value.gammasktm);
    addField(hasher, value.slomasktm);
    addField(hasher, value.lapmasktm);
    addField(hasher, value.Lmasktmcurve);
    addField(hasher, value.recothrest);
    addField(hasher, value.lowthrest);
    addField(hasher, value.higthrest);
    addField(hasher, value.decayt);
    addField(hasher, value.visireti);
    addField(hasher, value.expreti);
    addField(hasher, value.complexreti);
    addField(hasher, value.retinexMethod);
    addField(hasher, value.str);
    addField(hasher, value.chrrt);
    addField(hasher, value.neigh);
    addField(hasher, value.vart);
    addField(hasher, value.offs);
    addField(hasher, value.dehaz);
    addField(hasher, value.depth);
    addField(hasher, value.sensih);
    addField(hasher, value.localTgaincurve);
    addField(hasher, value.localTtranscurve);
    addField(hasher, value.inversret);
    addField(hasher, value.equilret);
    addField(hasher, value.loglin);
    addField(hasher, value.dehazeSaturation);
    addField(hasher, value.dehazeblack);
    addField(hasher, value.softradiusret);
    addField(hasher, value.CCmaskreticurve);
    addField(hasher, value.LLmaskreticurve);
    addField(hasher, value.HHmaskreticurve);
    addField(hasher, value.enaretiMask);
    addField(hasher, value.enaretiMasktmap);
    addField(hasher, value.blendmaskreti);
    addField(hasher, value.radmaskreti);
    addField(hasher, value.chromaskreti);
    addField(hasher, value.gammaskreti);
    addField(hasher, value.slomaskreti);
    addField(hasher, value.lapmaskreti);
    addField(hasher, value.scalereti);
    addField(hasher, value.darkness);
    addField(hasher, value.lightnessreti);
    addField(hasher, value.limd);
    addField(hasher, value.cliptm);
    addField(hasher, value.fftwreti);
    addField(hasher, value.Lmaskreticurve);
    addField(hasher, value.recothresr);
    addField(hasher, value.lowthresr);
    addField(hasher, value.higthresr);
    addField(hasher, value.decayr);
    addField(hasher, value.visisharp);
    addField(hasher, value.expsharp);
    addField(hasher, value.complexsharp);
    addField(hasher, value.sharcontrast);
    addField(hasher, value.deconvAutoshar);
    addField(hasher, value.sharradius);
    addField(hasher, value.sharamount);
    addField(hasher, value.shardamping);
    addField(hasher, value.shariter);
    addField(hasher, value.sharblur);
    addField(hasher, value.shargam);
    addField(hasher, value.sensisha);
    addField(hasher, value.inverssha);
    addField(hasher, value.sharshow);
    addField(hasher, value.itercheck);
    addField(hasher, value.methodcap);
    addField(hasher, value.capradius);
    addField(hasher, value.deconvAutoRadius);
    addField(hasher, value.deconvCoBoost);
    addField(hasher, value.deconvCoProt);
    addField(hasher, value.deconvCoLat);
    addField(hasher, value.deconvCogam);
    addField(hasher, value.reparsha);
    addField(hasher, value.visicontrast);
    addField(hasher, value.expcontrast);
    addField(hasher, value.complexcontrast);
    addField(hasher, value.lcradius);
    addField(hasher, value.lcamount);
    addField(hasher, value.lcdarkness);
    addField(hasher, value.lclightness);
    addField(hasher, value.sigmalc);
    addField(hasher, value.offslc);
    addField(hasher, value.gradlc);
    addField(hasher, value.levelwav);
    addField(hasher, value.residcont);
    addField(hasher, value.residsha);
    addField(hasher, value.residshathr);
    addField(hasher, value.residhi);
    addField(hasher, value.residhithr);
    addField(hasher, value.gamlc);
    addField(hasher, value.residgam);
    addField(hasher, value.residslop);
    addField(hasher, value.residblur);
    addField(hasher, value.levelblur);
    addField(hasher, value.sigmabl);
    addField(hasher, value.residchro);
    addField(hasher, value.residcomp);
    addField(hasher, value.sigma);
    addField(hasher, value.offset);
    addField(hasher, value.sigmadr);
    addField(hasher, value.threswav);
    addField(hasher, value.chromalev);
    addField(hasher, value.chromablu);
    addField(hasher, value.sigmadc);
    addField(hasher, value.deltad);
    addField(hasher, value.fatres);
    addField(hasher, value.clarilres);
    addField(hasher, value.claricres);
    addField(hasher, value.clarisoft);
    addField(hasher, value.sigmalc2);
    addField(hasher, value.strwav);
    addField(hasher, value.angwav);
    addField(hasher, value.featherwav);
    addField(hasher, value.strengthw);
    addField(hasher, value.sigmaed);
    addField(hasher, value.radiusw);
    addField(hasher, value.detailw);
    addField(hasher, value.gradw);
    addField(hasher, value.tloww);
    addField(hasher, value.thigw);
    addField(hasher, value.edgw);
    addField(hasher, value.basew);
    addField(hasher, value.sensilc);
    addField(hasher, value.reparw);
    addField(hasher, value.fftwlc);
    addField(hasher, value.blurlc);
    addField(hasher, value.wavblur);
    addField(hasher, value.wavedg);
    addField(hasher, value.waveshow);
    addField(hasher, value.wavcont);
    addField(hasher, value.wavcomp);
    addField(hasher, value.wavgradl);
    addField(hasher, value.wavcompre);
    addField(hasher, value.origlc);
    addField(hasher, value.processwav);
    addField(hasher, value.limitwav);
    addField(hasher, value.localcontMethod);
    addField(hasher, value.localedgMethod);
    addField(hasher, value.localneiMethod);
    addField(hasher, value.locwavcurve);
    addField(hasher, value.csthreshold);
    addField(hasher, value.loclevwavcurve);
    addField(hasher, value.locconwavcurve);
    addField(hasher, value.loccompwavcurve);
    addField(hasher, value.loccomprewavcurve);
    addField(hasher, value.locedgwavcurve);
    addField(hasher, value.CCmasklccurve);
    addField(hasher, value.LLmasklccurve);
    addField(hasher, value.HHmasklccurve);
    addField(hasher, value.enalcMask);
    addField(hasher, value.blendmasklc);
    addField(hasher, value.radmasklc);
    addField(hasher, value.chromasklc);
    addField(hasher, value.Lmasklccurve);
    addField(hasher, value.recothresw);
    addField(hasher, value.lowthresw);
    addField(hasher, value.higthresw);
    addField(hasher, value.decayw);
    addField(hasher, value.visicbdl);
    addField(hasher, value.expcbdl);
    addField(hasher, value.complexcbdl);
    addField(hasher, value.mult);
    addField(hasher, value.chromacbdl);
    addField(hasher, value.threshold);
    addField(hasher, value.sensicb);
    addField(hasher, value.clarityml);
    addField(hasher, value.contresid);
    addField(hasher, value.softradiuscb);
    addField(hasher, value.enacbMask);
    addField(hasher, value.CCmaskcbcurve);
    addField(hasher, value.LLmaskcbcurve);
    addField(hasher, value.HHmaskcbcurve);
    addField(hasher, value.blendmaskcb);
    addField(hasher, value.radmaskcb);
    addField(hasher, value.chromaskcb);
    addField(hasher, value.gammaskcb);
    addField(hasher, value.slomaskcb);
    addField(hasher, value.lapmaskcb);
    addField(hasher, value.Lmaskcbcurve);
    addField(hasher, value.recothrescb);
    addField(hasher, value.lowthrescb);
    addField(hasher, value.higthrescb);
    addField(hasher, value.decaycb);
    addField(hasher, value.visilog);
    addField(hasher, value.explog);
    addField(hasher, value.complexlog);
    addField(hasher, value.autocompute);
    addField(hasher, value.sourceGray);
    addField(hasher, value.sourceabs);
    addField(hasher, value.targabs);
    addField(hasher, value.targetGray);
    addField(hasher, value.catad);
    addField(hasher, value.saturl);
    addField(hasher, value.chroml);
    addField(hasher, value.lightl);
    addField(hasher, value.lightq);
    addField(hasher, value.contl);
    addField(hasher, value.contthres);
    addField(hasher, value.contq);
    addField(hasher, value.colorfl);
    addField(hasher, value.LcurveL);
    addField(hasher, value.Autogray);
    addField(hasher, value.fullimage);
    addField(hasher, value.repar);
    addField(hasher, value.ciecam);
    addField(hasher, value.satlog);
    addField(hasher, value.blackEv);
    addField(hasher, value.whiteEv);
    addField(hasher, value.whiteslog);
    addField(hasher, value.blackslog);
    addField(hasher, value.comprlog);
    addField(hasher, value.strelog);
    addField(hasher, value.detail);
    addField(hasher, value.sensilog);
    addField(hasher, value.sursour);
    addField(hasher, value.surround);
    addField(hasher, value.baselog);
    addField(hasher, value.strlog);
    addField(hasher, value.anglog);
    addField(hasher, value.featherlog);
    addField(hasher, value.CCmaskcurveL);
    addField(hasher, value.LLmaskcurveL);
    addField(hasher, value.HHmaskcurveL);
    addField(hasher, value.enaLMask);
    addField(hasher, value.blendmaskL);
    addField(hasher, value.radmaskL);
    addField(hasher, value.chromaskL);
    addField(hasher, value.LmaskcurveL);
    addField(hasher, value.recothresl);
    addField(hasher, value.lowthresl);
    addField(hasher, value.higthresl);
    addField(hasher, value.decayl);
    addField(hasher, value.visimask);
    addField(hasher, value.complexmask);
    addField(hasher, value.expmask);
    addField(hasher, value.sensimask);
    addField(hasher, value.blendmask);
    addField(hasher, value.blendmaskab);
    addField(hasher, value.softradiusmask);
    addField(hasher, value.enamask);
    addField(hasher, value.fftmask);
    addField(hasher, value.blurmask);
    addField(hasher, value.contmask);
    addField(hasher, value.CCmask_curve);
    addField(hasher, value.LLmask_curve);
    addField(hasher, value.HHmask_curve);
    addField(hasher, value.strumaskmask);
    addField(hasher, value.toolmask);
    addField(hasher, value.radmask);
    addField(hasher, value.lapmask);
    addField(hasher, value.chromask);
    addField(hasher, value.gammask);
    addField(hasher, value.slopmask);
    addField(hasher, value.shadmask);
    addField(hasher, value.str_mask);
    addField(hasher, value.ang_mask);
    addField(hasher, value.feather_mask);
    addField(hasher, value.HHhmask_curve);
    addField(hasher, value.Lmask_curve);
    addField(hasher, value.LLmask_curvewav);
    addField(hasher, value.csthresholdmask);
    addField(hasher, value.visicie);
    addField(hasher, value.expcie);
    addField(hasher, value.expprecam);
    addField(hasher, value.complexcie);
    addField(hasher, value.reparcie);
    addField(hasher, value.sensicie);
    addField(hasher, value.blurciede);
    addField(hasher, value.Autograycie);
    addField(hasher, value.sigybjz12);
    addField(hasher, value.qtoj);
    addField(hasher, value.jabcie);
    addField(hasher, value.comprcieauto);
    addField(hasher, value.normcie12);
    addField(hasher, value.normcie);
    addField(hasher, value.gamutcie);
    addField(hasher, value.bwcie);
    addField(hasher, value.sigcie);
    addField(hasher, value.logcie);
    addField(hasher, value.satcie);
    addField(hasher, value.logcieq);
    addField(hasher, value.smoothcie);
    addField(hasher, value.smoothcietrc);
    addField(hasher, value.smoothcietrcrel);
    addField(hasher, value.smoothcieyb);
    addField(hasher, value.smoothcielum);
    addField(hasher, value.smoothciehigh);
    addField(hasher, value.smoothcielnk);
    addField(hasher, value.smoothcieinv);
    addField(hasher, value.logjz);
    addField(hasher, value.sigjz12);
    addField(hasher, value.sigjz);
    addField(hasher, value.forcebw);
    addField(hasher, value.sigq12);
    addField(hasher, value.sigq);
    addField(hasher, value.chjzcie);
    addField(hasher, value.sourceGraycie);
    addField(hasher, value.sourceabscie);
    addField(hasher, value.sursourcie);
    addField(hasher, value.modecie);
    addField(hasher, value.modecam);
    addField(hasher, value.modeQJ);
    addField(hasher, value.bwevMethod12);
    addField(hasher, value.bwevMethod);
    addField(hasher, value.saturlcie);
    addField(hasher, value.rstprotectcie);
    addField(hasher, value.chromlcie);
    addField(hasher, value.huecie);
    addField(hasher, value.colorhred);
    addField(hasher, value.schromared);
    addField(hasher, value.redcurve);
    addField(hasher, value.colorhgreen);
    addField(hasher, value.schromagreen);
    addField(hasher, value.greencurve);
    addField(hasher, value.colorhblue);
    addField(hasher, value.schromablue);
    addField(hasher, value.bluecurve);
    addField(hasher, value.brighthres);
    addField(hasher, value.toneMethodcie);
    addField(hasher, value.ciecurve);
    addField(hasher, value.toneMethodcie2);
    addField(hasher, value.ciecurve2);
    addField(hasher, value.chromjzcie);
    addField(hasher, value.saturjzcie);
    addField(hasher, value.huejzcie);
    addField(hasher, value.softjzcie);
    addField(hasher, value.strsoftjzcie);
    addField(hasher, value.thrhjzcie);
    addField(hasher, value.jzcurve);
    addField(hasher, value.czcurve);
    addField(hasher, value.czjzcurve);
    addField(hasher, value.HHcurvejz);
    addField(hasher, value.CHcurvejz);
    addField(hasher, value.LHcurvejz);
    addField(hasher, value.lightlcie);
    addField(hasher, value.lightjzcie);
    addField(hasher, value.lightqcie);
    addField(hasher, value.lightsigqcie);
    addField(hasher, value.contlcie);
    addField(hasher, value.contjzcie);
    addField(hasher, value.detailciejz);
    addField(hasher, value.adapjzcie);
    addField(hasher, value.jz100);
    addField(hasher, value.pqremap);
    addField(hasher, value.pqremapcam16);
    addField(hasher, value.hljzcie);
    addField(hasher, value.hlthjzcie);
    addField(hasher, value.shjzcie);
    addField(hasher, value.shthjzcie);
    addField(hasher, value.radjzcie);
    addField(hasher, value.sigmalcjz);
    addField(hasher, value.clarilresjz);
    addField(hasher, value.claricresjz);
    addField(hasher, value.clarisoftjz);
    addField(hasher, value.locwavcurvejz);
    addField(hasher, value.csthresholdjz);
    addField(hasher, value.contthrescie);
    addField(hasher, value.blackEvjz);
    addField(hasher, value.whiteEvjz);
    addField(hasher, value.targetjz);
    addField(hasher, value.sigmoidldacie12);
    addField(hasher, value.sigmoidthcie12);
    addField(hasher, value.sigmoidblcie12);
    addField(hasher, value.sigmoidldacie);
    addField(hasher, value.sigmoidthcie);
    addField(hasher, value.sigmoidsenscie);
    addField(hasher, value.sigmoidblcie);
    addField(hasher, value.comprcie);
    addField(hasher, value.strcielog);
    addField(hasher, value.comprcieth);
    addField(hasher, value.gamjcie);
    addField(hasher, value.smoothcieth);
    addField(hasher, value.smoothciethtrc);
    addField(hasher, value.slopjcie);
    addField(hasher, value.satjcie);
    addField(hasher, value.smoothjcie);
    addField(hasher, value.contsig);
    addField(hasher, value.skewsig);
    addField(hasher, value.whitsig);
    addField(hasher, value.slopesmo);
    addField(hasher, value.slopesmoq);
    addField(hasher, value.slopesmor);
    addField(hasher, value.slopesmog);
    addField(hasher, value.slopesmob);
    addField(hasher, value.kslopesmor);
    addField(hasher, value.kslopesmog);
    addField(hasher, value.kslopesmob);
    addField(hasher, value.invcurve);
    addField(hasher, value.midtciemet);
    addField(hasher, value.midtcie);
    addField(hasher, value.grexl);
    addField(hasher, value.greyl);
    addField(hasher, value.bluxl);
    addField(hasher, value.bluyl);
    addField(hasher, value.redxl);
    addField(hasher, value.redyl);
    addField(hasher, value.refi);
    addField(hasher, value.shiftxl);
    addField(hasher, value.shiftyl);
    addField(hasher, value.labgridcieALow);
    addField(hasher, value.labgridcieBLow);
    addField(hasher, value.labgridcieAHigh);
    addField(hasher, value.labgridcieBHigh);
    addField(hasher, value.labgridcieGx);
    addField(hasher, value.labgridcieGy);
    addField(hasher, value.labgridcieWx);
    addField(hasher, value.labgridcieWy);
    addField(hasher, value.labgridcieMx);
    addField(hasher, value.labgridcieMy);
    addField(hasher, value.whitescie);
    addField(hasher, value.blackscie);
    addField(hasher, value.illMethod);
    addField(hasher, value.smoothciemet);
    addField(hasher, value.primMethod);
    addField(hasher, value.catMethod);
    addField(hasher, value.sigmoidldajzcie12);
    addField(hasher, value.sigmoidthjzcie12);
    addField(hasher, value.sigmoidbljzcie12);
    addField(hasher, value.sigmoidldajzcie);
    addField(hasher, value.sigmoidthjzcie);
    addField(hasher, value.sigmoidbljzcie);
    addField(hasher, value.contqcie);
    addField(hasher, value.contsigqcie);
    addField(hasher, value.colorflcie);
    addField(hasher, value.targabscie);
    addField(hasher, value.targetGraycie);
    addField(hasher, value.catadcie);
    addField(hasher, value.detailcie);
    addField(hasher, value.surroundcie);
    addField(hasher, value.gamgain);
    addField(hasher, value.gampower);
    addField(hasher, value.gamutw);
    addField(hasher, value.strgradcie);
    addField(hasher, value.anggradcie);
    addField(hasher, value.feathercie);
    addField(hasher, value.enacieMask);
    addField(hasher, value.enacieMaskall);
    addField(hasher, value.CCmaskciecurve);
    addField(hasher, value.LLmaskciecurve);
    addField(hasher, value.HHmaskciecurve);
    addField(hasher, value.HHhmaskciecurve);
    addField(hasher, value.blendmaskcie);
    addField(hasher, value.radmaskcie);
    addField(hasher, value.chromaskcie);
    addField(hasher, value.lapmaskcie);
    addField(hasher, value.gammaskcie);
    addField(hasher, value.slomaskcie);
    addField(hasher, value.Lmaskciecurve);
    addField(hasher, value.recothrescie);
    addField(hasher, value.lowthrescie);
    addField(hasher, value.higthrescie);
    addField(hasher, value.decaycie);
    addField(hasher, value.strumaskcie);
    addField(hasher, value.toolcie);
    addField(hasher, value.fftcieMask);
    addField(hasher, value.contcie);
    addField(hasher, value.blurcie);
    addField(hasher, value.highmaskcie);
    addField(hasher, value.shadmaskcie);
    addField(hasher, value.LLmaskciecurvewav);
    addField(hasher, value.csthresholdcie);
}

void addField(ParamHasher& hasher, const procparams::LocallabParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.selspot);
    addField(hasher, value.spots);
}

void addField(ParamHasher& hasher, const procparams::RGBCurvesParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.lumamode);
    addField(hasher, value.rcurve);
    addField(hasher, value.gcurve);
    addField(hasher, value.bcurve);
}

void addField(ParamHasher& hasher, const procparams::ColorToningParams::LabCorrectionRegion& value)
{
    addField(hasher, value.a);
    addField(hasher, value.b);
    addField(hasher, value.saturation);
    addField(hasher, value.slope);
    addField(hasher, value.offset);
    addField(hasher, value.power);
    addField(hasher, value.hueMask);
    addField(hasher, value.chromaticityMask);
    addField(hasher, value.lightnessMask);
    addField(hasher, value.maskBlur);
    addField(hasher, value.channel);
}

void addField(ParamHasher& hasher, const procparams::ColorToningParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.autosat);
    addField(hasher, value.opacityCurve);
    addField(hasher, value.colorCurve);
    addField(hasher, value.satProtectionThreshold);
    addField(hasher, value.saturatedOpacity);
    addField(hasher, value.strength);
    addField(hasher, value.balance);
    addField(hasher, value.hlColSat);
    addField(hasher, value.shadowsColSat);
    addField(hasher, value.clcurve);
    addField(hasher, value.cl2curve);
    addField(hasher, value.method);
    addField(hasher, value.twocolor);
    addField(hasher, value.redlow);
    addField(hasher, value.greenlow);
    addField(hasher, value.bluelow);
    addField(hasher, value.redmed);
    addField(hasher, value.greenmed);
    addField(hasher, value.bluemed);
    addField(hasher, value.redhigh);
    addField(hasher, value.greenhigh);
    addField(hasher, value.bluehigh);
    addField(hasher, value.satlow);
    addField(hasher, value.sathigh);
    addField(hasher, value.lumamode);
    addField(hasher, value.labgridALow);
    addField(hasher, value.labgridBLow);
    addField(hasher, value.labgridAHigh);
    addField(hasher, value.labgridBHigh);
    addField(hasher, value.labregions);
    addField(hasher, value.labregionsShowMask);
}

void addField(ParamHasher& hasher, const procparams::BlackWhiteParams& value)
{
    addField(hasher, value.beforeCurve);
    addField(hasher, value.beforeCurveMode);
    addField(hasher, value.afterCurve);
    addField(hasher, value.afterCurveMode);
    addField(hasher, value.algo);
    addField(hasher, value.luminanceCurve);
    addField(hasher, value.autoc);
    addField(hasher, value.enabledcc);
    addField(hasher, value.enabled);
    addField(hasher, value.filter);
    addField(hasher, value.setting);
    addField(hasher, value.method);
    addField(hasher, value.mixerRed);
    addField(hasher, value.mixerOrange);
    addField(hasher, value.mixerYellow);
    addField(hasher, value.mixerGreen);
    addField(hasher, value.mixerCyan);
    addField(hasher, value.mixerBlue);
    addField(hasher, value.mixerMagenta);
    addField(hasher, value.mixerPurple);
    addField(hasher, value.gammaRed);
    addField(hasher, value.gammaGreen);
    addField(hasher, value.gammaBlue);
}

void addField(ParamHasher& hasher, const procparams::ChannelMixerParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.red);
    addField(hasher, value.green);
    addField(hasher, value.blue);
}

void addField(ParamHasher& hasher, const procparams::HSVEqualizerParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.hcurve);
    addField(hasher, value.scurve);
    addField(hasher, value.vcurve);
}

void addField(ParamHasher& hasher, const procparams::FilmSimulationParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.clutFilename);
    addField(hasher, value.strength);
}

void addField(ParamHasher& hasher, const procparams::SHParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.highlights);
    addField(hasher, value.htonalwidth);
    addField(hasher, value.shadows);
    addField(hasher, value.stonalwidth);
    addField(hasher, value.radius);
    addField(hasher, value.lab);
}

void addField(ParamHasher& hasher, const procparams::LCurveParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.lcurve);
    addField(hasher, value.acurve);
    addField(hasher, value.bcurve);
    addField(hasher, value.cccurve);
    addField(hasher, value.chcurve);
    addField(hasher, value.lhcurve);
    addField(hasher, value.hhcurve);
    addField(hasher, value.lccurve);
    addField(hasher, value.clcurve);
    addField(hasher, value.brightness);
    addField(hasher, value.contrast);
    addField(hasher, value.chromaticity);
    addField(hasher, value.gamutmunselmethod);
    addField(hasher, value.rstprotection);
    addField(hasher, value.lcredsk);
}

void addField(ParamHasher& hasher, const procparams::LocalContrastParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.radius);
    addField(hasher, value.amount);
    addField(hasher, value.darkness);
    addField(hasher, value.lightness);
}

void addField(ParamHasher& hasher, const procparams::VibranceParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.pastels);
    addField(hasher, value.saturated);
    addField(hasher, value.psthreshold);
    addField(hasher, value.protectskins);
    addField(hasher, value.avoidcolorshift);
    addField(hasher, value.pastsattog);
    addField(hasher, value.skintonescurve);
}

void addField(ParamHasher& hasher, const procparams::EPDParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.strength);
    addField(hasher, value.gamma);
    addField(hasher, value.edgeStopping);
    addField(hasher, value.scale);
    addField(hasher, value.reweightingIterates);
}

void addField(ParamHasher& hasher, const procparams::SoftLightParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.strength);
}

/*
 * SharpeningParams leaves five members uninitialised and never reads them:
 * deconvAutoRadius, deconvCornerBoost, deconvCornerLatitude, psf_kernel and
 * psf_iterations. Its constructor does not set them, operator==() ignores
 * them, the parameter file does not save or load them, and no processing path
 * reads them. Hashing them would make the fingerprint depend on stack garbage,
 * so they are deliberately omitted. Add them back only when the constructor
 * initialises them and a processing path consumes them.
 */
void addField(ParamHasher& hasher, const procparams::SharpeningParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.contrast);
    addField(hasher, value.autoContrast);
    addField(hasher, value.blurradius);
    addField(hasher, value.gamma);
    addField(hasher, value.radius);
    addField(hasher, value.amount);
    addField(hasher, value.threshold);
    addField(hasher, value.edgesonly);
    addField(hasher, value.edges_radius);
    addField(hasher, value.edges_tolerance);
    addField(hasher, value.halocontrol);
    addField(hasher, value.halocontrol_amount);
    addField(hasher, value.method);
    addField(hasher, value.deconvamount);
    addField(hasher, value.deconvradius);
    addField(hasher, value.deconviter);
    addField(hasher, value.deconvdamping);
}

void addField(ParamHasher& hasher, const procparams::SharpenEdgeParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.passes);
    addField(hasher, value.amount);
    addField(hasher, value.threechannels);
}

void addField(ParamHasher& hasher, const procparams::SharpenMicroParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.matrix);
    addField(hasher, value.amount);
    addField(hasher, value.contrast);
    addField(hasher, value.uniformity);
}

void addField(ParamHasher& hasher, const procparams::ImpulseDenoiseParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.thresh);
}

void addField(ParamHasher& hasher, const procparams::DefringeParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.radius);
    addField(hasher, value.threshold);
    addField(hasher, value.huecurve);
}

void addField(ParamHasher& hasher, const procparams::WaveletParams& value)
{
    addField(hasher, value.ccwcurve);
    addField(hasher, value.wavdenoise);
    addField(hasher, value.wavdenoiseh);
    addField(hasher, value.blcurve);
    addField(hasher, value.levelshc);
    addField(hasher, value.opacityCurveRG);
    addField(hasher, value.opacityCurveBY);
    addField(hasher, value.opacityCurveW);
    addField(hasher, value.opacityCurveWL);
    addField(hasher, value.hhcurve);
    addField(hasher, value.wavguidcurve);
    addField(hasher, value.wavhuecurve);
    addField(hasher, value.Chcurve);
    addField(hasher, value.wavclCurve);
    addField(hasher, value.enabled);
    addField(hasher, value.median);
    addField(hasher, value.medianlev);
    addField(hasher, value.linkedg);
    addField(hasher, value.cbenab);
    addField(hasher, value.greenlow);
    addField(hasher, value.bluelow);
    addField(hasher, value.greenmed);
    addField(hasher, value.bluemed);
    addField(hasher, value.greenhigh);
    addField(hasher, value.bluehigh);
    addField(hasher, value.ballum);
    addField(hasher, value.sigm);
    addField(hasher, value.levden);
    addField(hasher, value.thrden);
    addField(hasher, value.limden);
    addField(hasher, value.balchrom);
    addField(hasher, value.chromfi);
    addField(hasher, value.chromco);
    addField(hasher, value.mergeL);
    addField(hasher, value.mergeC);
    addField(hasher, value.softrad);
    addField(hasher, value.softradend);
    addField(hasher, value.strend);
    addField(hasher, value.detend);
    addField(hasher, value.thrend);
    addField(hasher, value.lipst);
    addField(hasher, value.avoid);
    addField(hasher, value.showmask);
    addField(hasher, value.oldsh);
    addField(hasher, value.tmr);
    addField(hasher, value.strength);
    addField(hasher, value.balance);
    addField(hasher, value.sigmafin);
    addField(hasher, value.sigmaton);
    addField(hasher, value.sigmacol);
    addField(hasher, value.sigmadir);
    addField(hasher, value.rangeab);
    addField(hasher, value.protab);
    addField(hasher, value.iter);
    addField(hasher, value.expcontrast);
    addField(hasher, value.expchroma);
    addField(hasher, value.c);
    addField(hasher, value.ch);
    addField(hasher, value.expedge);
    addField(hasher, value.expbl);
    addField(hasher, value.expresid);
    addField(hasher, value.expfinal);
    addField(hasher, value.exptoning);
    addField(hasher, value.expnoise);
    addField(hasher, value.expclari);
    addField(hasher, value.labgridALow);
    addField(hasher, value.labgridBLow);
    addField(hasher, value.labgridAHigh);
    addField(hasher, value.labgridBHigh);
    addField(hasher, value.Lmethod);
    addField(hasher, value.CLmethod);
    addField(hasher, value.Backmethod);
    addField(hasher, value.Tilesmethod);
    addField(hasher, value.complexmethod);
    addField(hasher, value.mixmethod);
    addField(hasher, value.slimethod);
    addField(hasher, value.quamethod);
    addField(hasher, value.daubcoeffmethod);
    addField(hasher, value.CHmethod);
    addField(hasher, value.Medgreinf);
    addField(hasher, value.ushamethod);
    addField(hasher, value.CHSLmethod);
    addField(hasher, value.EDmethod);
    addField(hasher, value.NPmethod);
    addField(hasher, value.BAmethod);
    addField(hasher, value.TMmethod);
    addField(hasher, value.Dirmethod);
    addField(hasher, value.HSmethod);
    addField(hasher, value.sigma);
    addField(hasher, value.offset);
    addField(hasher, value.lowthr);
    addField(hasher, value.rescon);
    addField(hasher, value.resconH);
    addField(hasher, value.reschro);
    addField(hasher, value.resblur);
    addField(hasher, value.resblurc);
    addField(hasher, value.tmrs);
    addField(hasher, value.edgs);
    addField(hasher, value.scale);
    addField(hasher, value.gamma);
    addField(hasher, value.sup);
    addField(hasher, value.sky);
    addField(hasher, value.thres);
    addField(hasher, value.chroma);
    addField(hasher, value.chro);
    addField(hasher, value.threshold);
    addField(hasher, value.threshold2);
    addField(hasher, value.edgedetect);
    addField(hasher, value.edgedetectthr);
    addField(hasher, value.edgedetectthr2);
    addField(hasher, value.edgesensi);
    addField(hasher, value.edgeampli);
    addField(hasher, value.contrast);
    addField(hasher, value.edgrad);
    addField(hasher, value.edgeffect);
    addField(hasher, value.edgval);
    addField(hasher, value.edgthresh);
    addField(hasher, value.thr);
    addField(hasher, value.thrH);
    addField(hasher, value.radius);
    addField(hasher, value.skinprotect);
    addField(hasher, value.chrwav);
    addField(hasher, value.bluwav);
    addField(hasher, value.hueskin);
    addField(hasher, value.hueskin2);
    addField(hasher, value.hllev);
    addField(hasher, value.bllev);
    addField(hasher, value.pastlev);
    addField(hasher, value.satlev);
    addField(hasher, value.edgcont);
    addField(hasher, value.level0noise);
    addField(hasher, value.level1noise);
    addField(hasher, value.level2noise);
    addField(hasher, value.level3noise);
    addField(hasher, value.leveldenoise);
    addField(hasher, value.levelsigm);
}

void addField(ParamHasher& hasher, const procparams::DirPyrEqualizerParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.gamutlab);
    addField(hasher, value.mult);
    addField(hasher, value.threshold);
    addField(hasher, value.skinprotect);
    addField(hasher, value.hueskin);
    addField(hasher, value.cbdlMethod);
}

void addField(ParamHasher& hasher, const procparams::ToneEqualizerParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.bands);
    addField(hasher, value.regularization);
    addField(hasher, value.show_colormap);
    addField(hasher, value.pivot);
}

void addField(ParamHasher& hasher, const procparams::ColorAppearanceParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.degree);
    addField(hasher, value.autodegree);
    addField(hasher, value.degreeout);
    addField(hasher, value.autodegreeout);
    addField(hasher, value.curve);
    addField(hasher, value.curvered);
    addField(hasher, value.curvegreen);
    addField(hasher, value.curveblue);
    addField(hasher, value.curve2);
    addField(hasher, value.curve3);
    addField(hasher, value.curveMode);
    addField(hasher, value.curveMode2);
    addField(hasher, value.curveMode3);
    addField(hasher, value.complexmethod);
    addField(hasher, value.modelmethod);
    addField(hasher, value.catmethod);
    addField(hasher, value.surround);
    addField(hasher, value.surrsrc);
    addField(hasher, value.adapscen);
    addField(hasher, value.autoadapscen);
    addField(hasher, value.ybscen);
    addField(hasher, value.autoybscen);
    addField(hasher, value.adaplum);
    addField(hasher, value.badpixsl);
    addField(hasher, value.wbmodel);
    addField(hasher, value.illum);
    addField(hasher, value.algo);
    addField(hasher, value.contrast);
    addField(hasher, value.qcontrast);
    addField(hasher, value.jlight);
    addField(hasher, value.qbright);
    addField(hasher, value.chroma);
    addField(hasher, value.schroma);
    addField(hasher, value.schromared);
    addField(hasher, value.schromagreen);
    addField(hasher, value.schromablue);
    addField(hasher, value.mchroma);
    addField(hasher, value.colorh);
    addField(hasher, value.colorhred);
    addField(hasher, value.colorhgreen);
    addField(hasher, value.colorhblue);
    addField(hasher, value.brighthres);
    addField(hasher, value.rstprotection);
    addField(hasher, value.surrsource);
    addField(hasher, value.gamut);
    addField(hasher, value.datacie);
    addField(hasher, value.tonecie);
    addField(hasher, value.tempout);
    addField(hasher, value.autotempout);
    addField(hasher, value.ybout);
    addField(hasher, value.greenout);
    addField(hasher, value.tempsc);
    addField(hasher, value.greensc);
}

void addField(ParamHasher& hasher, const procparams::CropParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.x);
    addField(hasher, value.y);
    addField(hasher, value.w);
    addField(hasher, value.h);
    addField(hasher, value.fixratio);
    addField(hasher, value.ratio);
    addField(hasher, value.orientation);
}

void addField(ParamHasher& hasher, const procparams::ResizeParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.scale);
    addField(hasher, value.appliesTo);
    addField(hasher, value.method);
    addField(hasher, value.dataspec);
    addField(hasher, value.width);
    addField(hasher, value.height);
    addField(hasher, value.longedge);
    addField(hasher, value.shortedge);
    addField(hasher, value.allowUpscaling);
}

void addField(ParamHasher& hasher, const procparams::FramingParams& value)
{
    addField(hasher, value.enabled);
    addField(hasher, value.framingMethod);
    addField(hasher, value.aspectRatio);
    addField(hasher, value.orientation);
    addField(hasher, value.framedWidth);
    addField(hasher, value.framedHeight);
    addField(hasher, value.allowUpscaling);
    addField(hasher, value.borderSizingMethod);
    addField(hasher, value.basis);
    addField(hasher, value.relativeBorderSize);
    addField(hasher, value.minSizeEnabled);
    addField(hasher, value.minWidth);
    addField(hasher, value.minHeight);
    addField(hasher, value.absWidth);
    addField(hasher, value.absHeight);
    addField(hasher, value.borderRed);
    addField(hasher, value.borderGreen);
    addField(hasher, value.borderBlue);
}

void addField(ParamHasher& hasher, const procparams::MetaDataParams& value)
{
    addField(hasher, value.mode);
    addField(hasher, value.exifKeys);
    addField(hasher, value.exif);
    addField(hasher, value.iptc);
}


/*
 * Descriptor hashers.
 *
 * Each function adds the members listed for that descriptor in
 * exportParamMap(). The caller adds the section label, so the descriptor id is
 * not repeated here.
 *
 * Order inside a descriptor is fixed: the fingerprint format depends on it.
 */

/* Select the frame and its raw decoder state; no image math runs here. */
void hashSourceSelect(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.raw);
}

/*
 * imgsrc->preprocess() takes raw, lensProf, coarse and the denoise enable flag
 * (simpleprocess.cc:393). The whole dirpyrDenoise member is hashed because the
 * flag is read from it.
 */
void hashRawPreprocess(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.raw);
    addField(hasher, params.lensProf);
    addField(hasher, params.coarse);
    addField(hasher, params.dirpyrDenoise);
}

/* Demosaic plus post-demosaic capture sharpening (simpleprocess.cc:399-405). */
void hashRawDemosaic(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.raw);
    addField(hasher, params.pdsharpening);
}

/*
 * getImage() and the operations that finish the raw source: white balance,
 * tone-curve exposure, spot removal, denoise, film negative, Retinex, dehaze,
 * Fattal and source-side gamut compression. icm enters through
 * convertColorSpace() and the Retinex colour-space conversion.
 */
void hashRawBase(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.raw);
    addField(hasher, params.wb);
    addField(hasher, params.toneCurve);
    addField(hasher, params.spot);
    addField(hasher, params.dirpyrDenoise);
    addField(hasher, params.filmNegative);
    addField(hasher, params.retinex);
    addField(hasher, params.dehaze);
    addField(hasher, params.fattal);
    addField(hasher, params.cg);
    addField(hasher, params.icm);
}

/*
 * firstAnalysis() reads icm.workingProfile to build the luminance weights
 * (improcfun.cc:515-521). toneCurve is kept from the inventory because the
 * histogram feeds the auto-exposure parameters consumed by raw.base. The
 * automatic denoise measurement reads the denoise configuration (the automatic
 * methods it selects, and the curve or slider mode), so the whole member is
 * hashed: an analysis measured for one denoise configuration must not be
 * reused for another.
 */
void hashAnalysis(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.icm);
    addField(hasher, params.toneCurve);
    addField(hasher, params.dirpyrDenoise);
}

/*
 * iptransform.cc reads the geometry members and crop: post-crop vignetting
 * needs the crop rectangle to place its centre (iptransform.cc:1030, :1103).
 */
void hashTransform(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.coarse);
    addField(hasher, params.commonTrans);
    addField(hasher, params.rotate);
    addField(hasher, params.distortion);
    addField(hasher, params.perspective);
    addField(hasher, params.cacorrection);
    addField(hasher, params.vignetting);
    addField(hasher, params.gradient);
    addField(hasher, params.pcvignette);
    addField(hasher, params.lensProf);
    addField(hasher, params.crop);
}

/*
 * Lab_Local() owns locallab and reads the global members that local spots
 * borrow: the working profile, CIE adaptation from toneCurve and raw exposure,
 * the white-balance observer, the dirpyrequalizer/dehaze/epd enable flags, and
 * the RGB tool enables that decide negative-value handling
 * (iplocallab.cc:2648, :2788-2789, :3510, :15684-15687, :16688, :23816-23826).
 */
void hashLocalAdjustments(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.locallab);
    addField(hasher, params.icm);
    addField(hasher, params.toneCurve);
    addField(hasher, params.raw);
    addField(hasher, params.wb);
    addField(hasher, params.dirpyrequalizer);
    addField(hasher, params.dehaze);
    addField(hasher, params.epd);
    addField(hasher, params.rgbCurves);
    addField(hasher, params.hsvequalizer);
    addField(hasher, params.chmixer);
    addField(hasher, params.colorToning);
}

/*
 * rgbProc(), the Lab-grid colour-toning pass, B&W conversion and film
 * simulation. icm supplies the working-space matrix and the DCP profile
 * (simpleprocess.cc:1604-1657).
 */
void hashRgbProc(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.toneCurve);
    addField(hasher, params.rgbCurves);
    addField(hasher, params.colorToning);
    addField(hasher, params.blackwhite);
    addField(hasher, params.chmixer);
    addField(hasher, params.hsvequalizer);
    addField(hasher, params.filmSimulation);
    addField(hasher, params.icm);
}

/*
 * Shadows/highlights, Lab curves, local contrast, vibrance, EPD and soft
 * light. vibrance() also reads toneCurve.hrenabled and icm.workingProfile
 * (simpleprocess.cc:1750).
 */
void hashLabTools(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.sh);
    addField(hasher, params.labCurve);
    addField(hasher, params.localContrast);
    addField(hasher, params.vibrance);
    addField(hasher, params.epd);
    addField(hasher, params.softlight);
    addField(hasher, params.icm);
    addField(hasher, params.toneCurve);
}

/*
 * Impulse denoise, defringe, microcontrast and sharpening. ipsharpen.cc reads
 * icm.workingProfile alongside the sharpening members.
 */
void hashLabSharpening(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.sharpening);
    addField(hasher, params.sharpenEdge);
    addField(hasher, params.sharpenMicro);
    addField(hasher, params.impulseDenoise);
    addField(hasher, params.defringe);
    addField(hasher, params.icm);
}

/*
 * Directional pyramid equaliser and wavelet. ipwavelet.cc also reads the
 * working profile and toneCurve.hrenabled (ipwavelet.cc:223, :2017).
 */
void hashLabWavelet(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.wavelet);
    addField(hasher, params.dirpyrequalizer);
    addField(hasher, params.icm);
    addField(hasher, params.toneCurve);
}

/*
 * Tone equaliser and working-TRC conversion. The TRC completion path reuses
 * the wavelet member as its local-contrast parameter set
 * (simpleprocess.cc:1956-1960).
 */
void hashWorkingTrc(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.toneEqualizer);
    addField(hasher, params.icm);
    addField(hasher, params.wavelet);
}

/*
 * CIE CAM 02. Adapting luminance is derived from toneCurve.expcomp and
 * raw.expos (simpleprocess.cc:2113-2114), and the tone-mapped path runs
 * EPDToneMapCIE with the epd member (improcfun.cc:1869, :5312-5328).
 */
void hashCieCam(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.colorappearance);
    addField(hasher, params.icm);
    addField(hasher, params.toneCurve);
    addField(hasher, params.raw);
    addField(hasher, params.epd);
}

/* Working-space gamut compression and final gain (simpleprocess.cc:2129-2185). */
void hashGamut(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.cg);
    addField(hasher, params.icm);
}

/* Lab to output RGB with the output profile (simpleprocess.cc:2316-2317). */
void hashOutputConvert(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.icm);
}

/*
 * Monitor and viewport output.
 *
 * `lab2monitorRgb()` converts the finished Lab image to the display space.
 * With soft proofing the image passes through the output profile first;
 * without it the monitor profile is the only target. The output profile,
 * intent, black-point compensation and gamut flag are ProcParams members and
 * are hashed here. The monitor profile, monitor intent and the soft-proof and
 * gamut-check switches live in application or GUI state, not in ProcParams, so
 * they cannot enter this fingerprint; they belong to the future RenderStateKey.
 */
void hashDisplayOutput(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.icm);
}

/*
 * Crop rectangle. cropGuide is not included: it is an editor overlay and no
 * export path reads it.
 */
void hashCrop(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.crop);
}

/*
 * Resize. The target size is computed by resizeScale() from resize, crop and
 * framing (ipresize.cc:1034-1077), so all three are read here.
 */
void hashResize(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.resize);
    addField(hasher, params.crop);
    addField(hasher, params.framing);
}

/* Post-resize sharpening (simpleprocess.cc:2290-2297). */
void hashOutputSharpen(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.prsharpening);
}

/*
 * Framing and frame drawing. The framed size is derived from framing, crop
 * and resize by ImProcFunctions::framing(); the colour and border fields are
 * consumed by drawFrame() (simpleprocess.cc:2223-2357).
 */
void hashFraming(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.framing);
    addField(hasher, params.crop);
    addField(hasher, params.resize);
}

/* Output ICC attachment and metadata handling (simpleprocess.cc:2362-2410). */
void hashOutputIcc(ParamHasher& hasher, const ProcParams& params)
{
    addField(hasher, params.icm);
    addField(hasher, params.metadata);
}

using HashFn = void (*)(ParamHasher&, const ProcParams&);

struct DescriptorDefinition {
    const char* id;
    std::vector<const char*> fields;
    HashFn hash;
};

/*
 * One definition per distinct stage descriptor across the canonical paths.
 *
 * A descriptor that two paths share appears once, so preview, crop and export
 * cannot drift apart while their semantics are the same. The two display sinks
 * are separate entries so the paths can diverge later; both currently read only
 * `icm`.
 */
const std::vector<DescriptorDefinition>& definitions()
{
    static const std::vector<DescriptorDefinition> catalogue = {
        // Export pipeline order.
        {"source.select", {"raw"}, hashSourceSelect},
        {"raw.preprocess",
         {"raw", "lensProf", "coarse", "dirpyrDenoise"},
         hashRawPreprocess},
        {"raw.demosaic", {"raw", "pdsharpening"}, hashRawDemosaic},
        {"raw.base",
         {"raw", "wb", "toneCurve", "spot", "dirpyrDenoise", "filmNegative",
          "retinex", "dehaze", "fattal", "cg", "icm"},
         hashRawBase},
        {"analysis", {"icm", "toneCurve", "dirpyrDenoise"}, hashAnalysis},
        {"transform",
         {"coarse", "commonTrans", "rotate", "distortion", "perspective",
          "cacorrection", "vignetting", "gradient", "pcvignette", "lensProf",
          "crop"},
         hashTransform},
        {"local.adjustments",
         {"locallab", "icm", "toneCurve", "raw", "wb", "dirpyrequalizer",
          "dehaze", "epd", "rgbCurves", "hsvequalizer", "chmixer",
          "colorToning"},
         hashLocalAdjustments},
        {"rgb.proc",
         {"toneCurve", "rgbCurves", "colorToning", "blackwhite", "chmixer",
          "hsvequalizer", "filmSimulation", "icm"},
         hashRgbProc},
        {"lab.tools",
         {"sh", "labCurve", "localContrast", "vibrance", "epd", "softlight",
          "icm", "toneCurve"},
         hashLabTools},
        {"lab.sharpening",
         {"sharpening", "sharpenEdge", "sharpenMicro", "impulseDenoise",
          "defringe", "icm"},
         hashLabSharpening},
        {"lab.wavelet",
         {"wavelet", "dirpyrequalizer", "icm", "toneCurve"},
         hashLabWavelet},
        {"working.trc", {"toneEqualizer", "icm", "wavelet"}, hashWorkingTrc},
        {"ciecam", {"colorappearance", "icm", "toneCurve", "raw", "epd"},
         hashCieCam},
        {"gamut", {"cg", "icm"}, hashGamut},
        {"crop", {"crop"}, hashCrop},
        {"resize", {"resize", "crop", "framing"}, hashResize},
        {"output.sharpen", {"prsharpening"}, hashOutputSharpen},
        {"output.convert", {"icm"}, hashOutputConvert},
        {"framing", {"framing", "crop", "resize"}, hashFraming},
        {"output.icc", {"icm", "metadata"}, hashOutputIcc},
        // Display sinks. Preview ends at monitor.output; the crop viewport
        // ends at viewport.output.
        {"monitor.output", {"icm"}, hashDisplayOutput},
        {"viewport.output", {"icm"}, hashDisplayOutput}
    };
    return catalogue;
}

const DescriptorDefinition* findDefinition(const std::string& id)
{
    for (const DescriptorDefinition& definition : definitions()) {
        if (id == definition.id) {
            return &definition;
        }
    }
    return nullptr;
}

using PathTable = std::vector<const DescriptorDefinition*>;

PathTable buildPath(std::initializer_list<const char*> ids)
{
    PathTable path;
    path.reserve(ids.size());
    for (const char* id : ids) {
        const DescriptorDefinition* definition = findDefinition(id);
        assert(definition != nullptr);
        if (definition != nullptr) {
            path.push_back(definition);
        }
    }
    return path;
}

/*
 * Path tables. The order is the execution order of
 * buildPipelineSpec(StagePath::...) and every id must exist in the catalogue.
 */
const PathTable& exportPath()
{
    static const PathTable path = buildPath({
        "source.select",
        "raw.preprocess",
        "raw.demosaic",
        "raw.base",
        "analysis",
        "transform",
        "local.adjustments",
        "rgb.proc",
        "lab.tools",
        "lab.sharpening",
        "lab.wavelet",
        "working.trc",
        "ciecam",
        "gamut",
        "crop",
        "resize",
        "output.sharpen",
        "output.convert",
        "framing",
        "output.icc"
    });
    return path;
}

const PathTable& previewPath()
{
    static const PathTable path = buildPath({
        "source.select",
        "raw.preprocess",
        "raw.demosaic",
        "raw.base",
        "analysis",
        "transform",
        "local.adjustments",
        "rgb.proc",
        "lab.tools",
        "lab.wavelet",
        "working.trc",
        "ciecam",
        "gamut",
        "monitor.output"
    });
    return path;
}

const PathTable& cropPath()
{
    static const PathTable path = buildPath({
        "source.select",
        "raw.preprocess",
        "raw.demosaic",
        "raw.base",
        "analysis",
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
    });
    return path;
}

const DescriptorDefinition* findInPath(const PathTable& path,
                                       const std::string& id)
{
    for (const DescriptorDefinition* definition : path) {
        if (id == definition->id) {
            return definition;
        }
    }
    return nullptr;
}

std::vector<StageParamMapEntry> project(const PathTable& path)
{
    std::vector<StageParamMapEntry> entries;
    entries.reserve(path.size());
    for (const DescriptorDefinition* definition : path) {
        entries.push_back(
            {definition->id,
             std::vector<std::string>(definition->fields.begin(),
                                      definition->fields.end())});
    }
    return entries;
}

ParamFingerprint fingerprint(const PathTable& path, const std::string& id,
                             const ProcParams& params)
{
    const DescriptorDefinition* definition = findInPath(path, id);
    assert(definition != nullptr);

    if (definition == nullptr) {
        return ParamFingerprint{};
    }

    ParamHasher hasher;
    /* The section label is the descriptor id itself, verbatim. */
    hasher.addSectionLabel(definition->id);
    definition->hash(hasher, params);
    return hasher.finish();
}

} // namespace

const std::vector<StageParamMapEntry>& exportParamMap()
{
    static const std::vector<StageParamMapEntry> entries = project(exportPath());
    return entries;
}

const std::vector<StageParamMapEntry>& previewParamMap()
{
    static const std::vector<StageParamMapEntry> entries =
        project(previewPath());
    return entries;
}

const std::vector<StageParamMapEntry>& cropParamMap()
{
    static const std::vector<StageParamMapEntry> entries = project(cropPath());
    return entries;
}

bool has_export_fingerprint(const std::string& id)
{
    return findInPath(exportPath(), id) != nullptr;
}

bool has_preview_fingerprint(const std::string& id)
{
    return findInPath(previewPath(), id) != nullptr;
}

bool has_crop_fingerprint(const std::string& id)
{
    return findInPath(cropPath(), id) != nullptr;
}

ParamFingerprint export_fingerprint(const std::string& id,
                                    const procparams::ProcParams& params)
{
    return fingerprint(exportPath(), id, params);
}

ParamFingerprint preview_fingerprint(const std::string& id,
                                     const procparams::ProcParams& params)
{
    return fingerprint(previewPath(), id, params);
}

ParamFingerprint crop_fingerprint(const std::string& id,
                                  const procparams::ProcParams& params)
{
    return fingerprint(cropPath(), id, params);
}

} // namespace rtengine::stage
