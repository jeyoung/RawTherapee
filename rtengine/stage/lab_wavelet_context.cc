#include "stage/lab_wavelet_context.h"

#include "color.h"
#include "curves.h"
#include "dirpyr_equalizer.h"
#include "guidedfilter.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "labimage.h"
#include "cplx_wavelet_filter_coeffs.h"
#include "procparams.h"
#include "rt_math.h"
#include "settings.h"
#include "stage/stage_buffer.h"
#include "stage/stage_profile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rtengine::stage
{

namespace
{

int build_thunk(void* self, RtLabWaveletResult* out)
{
    return static_cast<LabWaveletContext*>(self)->build(*out);
}

int prepare_thunk(void* self, RtStageBuffer* lab)
{
    return static_cast<LabWaveletContext*>(self)->prepare(*lab);
}

int run_host_thunk(void* self, RtStageBuffer* lab)
{
    return static_cast<LabWaveletContext*>(self)->runHost(*lab);
}

int compute_mea_thunk(void* self, const float* stats, uint32_t levels)
{
    return static_cast<LabWaveletContext*>(self)->computeMea(stats, levels);
}

/*
 * Copy one pitched three-plane F32 buffer into a LabImage, and back. The
 * buffer's planes may carry padding, so every row is copied by width.
 */
void copy_to_host(const RtStageBuffer& src, LabImage& dst)
{
    const auto* base = static_cast<const unsigned char*>(src.data);
    for (int y = 0; y < src.height; ++y) {
        for (std::uint32_t plane = 0; plane < 3; ++plane) {
            const auto* row = reinterpret_cast<const float*>(
                base + plane * src.plane_pitch +
                static_cast<std::size_t>(y) * src.row_pitch);
            float* target = plane == 0 ? dst.L[y]
                            : plane == 1 ? dst.a[y]
                                         : dst.b[y];
            std::memcpy(target, row,
                        static_cast<std::size_t>(src.width) * sizeof(float));
        }
    }
}

void copy_from_host(const LabImage& src, RtStageBuffer& dst)
{
    auto* base = static_cast<unsigned char*>(dst.data);
    for (int y = 0; y < dst.height; ++y) {
        for (std::uint32_t plane = 0; plane < 3; ++plane) {
            auto* row = reinterpret_cast<float*>(
                base + plane * dst.plane_pitch +
                static_cast<std::size_t>(y) * dst.row_pitch);
            const float* source = plane == 0 ? src.L[y]
                                 : plane == 1 ? src.a[y]
                                              : src.b[y];
            std::memcpy(row, source,
                        static_cast<std::size_t>(dst.width) * sizeof(float));
        }
    }
}

bool hueInRange(int value)
{
    const double normalised = static_cast<double>(value) / 100.0;
    return normalised >= LAB_WAVELET_MIN_HUE &&
           normalised <= LAB_WAVELET_MAX_HUE;
}

} // namespace

int labWaveletLastLevel(const double* mult)
{
    int last = static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS);
    while (last > 0 && std::fabs(mult[last - 1] - 1.0) < 0.001) {
        --last;
    }
    return last;
}

bool labWaveletSupported(const procparams::ProcParams& params, int width,
                         int height)
{
    const procparams::DirPyrEqualizerParams& equalizer =
        params.dirpyrequalizer;

    const bool equalizerOn = equalizer.enabled;
    const bool waveletOn = params.wavelet.enabled;
    if (!equalizerOn && !waveletOn) {
        return false;
    }

    if (equalizerOn) {
        if (equalizer.cbdlMethod != "aft") {
            return false;
        }
        if (settings == nullptr) {
            return false;
        }

        /* stage_lab_wavelet()'s gate around the equaliser call. */
        const bool cam02 = params.colorappearance.enabled &&
                           params.colorappearance.modelmethod == "02";
        if (!((params.colorappearance.enabled && !settings->autocielab) ||
              !cam02)) {
            return false;
        }

        for (int level = 0;
             level < static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS); ++level) {
            const double value = equalizer.mult[level];
            if (!std::isfinite(value) || value < LAB_WAVELET_MIN_MULT ||
                value > LAB_WAVELET_MAX_MULT) {
                return false;
            }
        }
        if (!std::isfinite(equalizer.threshold) ||
            equalizer.threshold < LAB_WAVELET_MIN_THRESHOLD ||
            equalizer.threshold > LAB_WAVELET_MAX_THRESHOLD) {
            return false;
        }
        if (!std::isfinite(equalizer.skinprotect) ||
            equalizer.skinprotect < LAB_WAVELET_MIN_SKIN ||
            equalizer.skinprotect > LAB_WAVELET_MAX_SKIN) {
            return false;
        }
        if (!hueInRange(equalizer.hueskin.getBottomLeft()) ||
            !hueInRange(equalizer.hueskin.getTopLeft()) ||
            !hueInRange(equalizer.hueskin.getBottomRight()) ||
            !hueInRange(equalizer.hueskin.getTopRight())) {
            return false;
        }

        /* A block whose multipliers are all neutral does not touch the image;
         * it is a residual so the host records it instead of an empty device
         * pass. */
        if (labWaveletLastLevel(equalizer.mult) <= 0) {
            return false;
        }
    }

    return !waveletOn || labWaveletWaveletSupported(params, width, height);
}

int labWaveletTransformLevels(const procparams::ProcParams& params,
                              int width, int height)
{
    const procparams::WaveletParams& wavelet = params.wavelet;
    if (!wavelet.enabled || width <= 0 || height <= 0) {
        return 0;
    }

    /* ip_wavelet(): the export call site passes skip = 1. */
    constexpr int skip = 1;
    const int minwin = std::min(width, height);
    const bool extra = wavelet.sup != 0;

    int maxlevelcrop = 9;
    if (extra) {
        maxlevelcrop = 10;
    }
    if (minwin * skip < 1024) {
        maxlevelcrop = 9;
    }
    if (minwin * skip < 512) {
        maxlevelcrop = 8;
    }
    if (minwin * skip < 256) {
        maxlevelcrop = 7;
    }
    if (minwin * skip < 128) {
        maxlevelcrop = 6;
    }
    if (minwin * skip < 64) {
        maxlevelcrop = 5;
    }
    if (minwin * skip < 32) {
        maxlevelcrop = 4;
    }

    int levwav = wavelet.thres;
    if (wavelet.expnoise) {
        levwav = 6;
    }
    if (levwav == 9 && extra) {
        levwav = 10;
    }
    levwav = std::min(maxlevelcrop, levwav);
    if (levwav < 1) {
        return 0;
    }

    const int minsizetile = std::min(width, height);
    int maxlev2 = 10;
    if (minsizetile < 1024 && levwav == 10) {
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
    if (minsizetile < 64) {
        maxlev2 = 5;
    }
    if (minsizetile < 32) {
        maxlev2 = 4;
    }
    levwav = std::min(maxlev2, levwav);

    /* The trailing-zero trim in ip_wavelet(): a level whose multiplier is
     * exactly zero is dead when nothing else in the block needs it. */
    const bool noiseena = wavelet.expnoise && minwin > 128;
    const bool ref0 =
        noiseena && (wavelet.level0noise.getBottom() > 0. ||
                     wavelet.level1noise.getBottom() > 0. ||
                     wavelet.level2noise.getBottom() > 0. ||
                     wavelet.level3noise.getBottom() > 0.);
    const bool dead = !wavelet.expbl && wavelet.contrast == 0 &&
                      wavelet.resblur == 0 && !noiseena &&
                      wavelet.tmrs == 0 && !wavelet.expresid &&
                      !wavelet.expchroma && !wavelet.exptoning &&
                      !wavelet.expfinal && !wavelet.expedge &&
                      wavelet.rescon == 0 && wavelet.resconH == 0 &&
                      wavelet.edgval == 0 && !ref0 &&
                      wavelet.CLmethod == "all";
    if (dead) {
        const auto multiplier = [&](int index) -> int {
            if (index < wavelet.thres && index < 9) {
                return wavelet.c[index];
            }
            return index == 9 ? wavelet.sup : 0;
        };
        while (levwav > 0 && multiplier(levwav - 1) == 0) {
            --levwav;
        }
    }
    return levwav > 0 ? levwav : 0;
}

bool labWaveletWaveletSupported(const procparams::ProcParams& params,
                                int width, int height)
{
    const procparams::WaveletParams& wavelet = params.wavelet;
    if (!wavelet.enabled) {
        return true;
    }

    /* Contrast only: every other export panel is a residual. */
    if (!wavelet.expcontrast || wavelet.expchroma || wavelet.expedge ||
        wavelet.expbl || wavelet.expresid || wavelet.expfinal ||
        wavelet.exptoning || wavelet.expnoise || wavelet.expclari) {
        return false;
    }
    if (wavelet.CLmethod != "all" || wavelet.Tilesmethod != "full") {
        return false;
    }
    if (wavelet.softrad != 0.0 || wavelet.softradend != 0.0) {
        return false;
    }
    if (wavelet.skinprotect != 0.0) {
        return false;
    }
    if (wavelet.avoid || wavelet.median) {
        return false;
    }
    if (wavelet.sup != 0) {
        return false;
    }
    /* Only the default 8-tap Daubechies analysis is ported. */
    if (wavelet.daubcoeffmethod != "6_") {
        return false;
    }
    if (wavelet.HSmethod != "with" && wavelet.HSmethod != "without") {
        return false;
    }
    /* An active chroma-by-hue curve would scale the contrast per pixel. */
    {
        const FlatCurve chroma_curve(wavelet.Chcurve);
        if (!chroma_curve.isIdentity()) {
            return false;
        }
    }
    if (!std::isfinite(wavelet.sigma) ||
        wavelet.sigma < LAB_WAVELET_MIN_WAVELET_SIGMA ||
        wavelet.sigma > LAB_WAVELET_MAX_WAVELET_SIGMA ||
        !std::isfinite(wavelet.offset) ||
        wavelet.offset < LAB_WAVELET_MIN_WAVELET_OFFSET ||
        wavelet.offset > LAB_WAVELET_MAX_WAVELET_OFFSET ||
        !std::isfinite(wavelet.lowthr) ||
        wavelet.lowthr < LAB_WAVELET_MIN_WAVELET_LOWTHR ||
        wavelet.lowthr > LAB_WAVELET_MAX_WAVELET_LOWTHR) {
        return false;
    }
    if (wavelet.thres < LAB_WAVELET_MIN_WAVELET_LEVELS ||
        wavelet.thres > LAB_WAVELET_MAX_WAVELET_LEVELS ||
        wavelet.threshold < LAB_WAVELET_MIN_THRESHOLD_LEVEL ||
        wavelet.threshold > LAB_WAVELET_MAX_THRESHOLD_LEVEL ||
        wavelet.threshold2 < LAB_WAVELET_MIN_THRESHOLD2_LEVEL ||
        wavelet.threshold2 > LAB_WAVELET_MAX_THRESHOLD2_LEVEL) {
        return false;
    }
    if (wavelet.strength < LAB_WAVELET_MIN_STRENGTH ||
        wavelet.strength > LAB_WAVELET_MAX_STRENGTH) {
        return false;
    }
    for (int level = 0; level < 9; ++level) {
        if (wavelet.c[level] < LAB_WAVELET_MIN_WAVELET_C ||
            wavelet.c[level] > LAB_WAVELET_MAX_WAVELET_C) {
            return false;
        }
    }
    /* The scoped set leaves the finest level active; an all-zero pyramid
     * would be a no-op dressed up as a device pass. */
    if (wavelet.c[0] == 0) {
        return false;
    }

    const int levels = labWaveletTransformLevels(params, width, height);
    if (levels < 1 ||
        levels > static_cast<int>(RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS)) {
        return false;
    }
    if (width > 0 && height > 0) {
        /* The undecimated Haar levels read skip pixels to the right and
         * below; the block leaves the border band unwritten when the grid is
         * too small, so refuse instead of reproducing an uninitialised read. */
        const int half_w = (width + 1) / 2;
        const int half_h = (height + 1) / 2;
        if (levels >= 2) {
            const int reach = 1 << (levels - 1);
            if (half_w < reach || half_h < reach) {
                return false;
            }
        }
    }
    return true;
}

/*
 * The block as it ran in stage_lab_wavelet(), moved here unchanged. The only
 * substitution is the image extent: the call site's fw/fh are the LabImage's
 * own width and height at this point in the export sequence.
 */
void labWaveletHostBlock(ImProcFunctions& ipf,
                         procparams::ProcParams& params, LabImage& labView)
{
    const int fw = labView.W;
    const int fh = labView.H;

    // directional pyramid wavelet
    if (params.dirpyrequalizer.cbdlMethod == "aft") {
     //   if ((params.colorappearance.enabled && !settings->autocielab)  || !params.colorappearance.enabled) {
        if ((params.colorappearance.enabled && !settings->autocielab)  || !(params.colorappearance.modelmethod == "02" && params.colorappearance.enabled)) {
            ipf.dirpyrequalizer(&labView, 1);     //TODO: this is the luminance tonecurve, not the RGB one
        }
    }

    if ((params.wavelet.enabled)){
        ScopedProfile profile("finish.wavelet");
        LabImage *unshar = nullptr;
        WaveletParams WaveParams = params.wavelet;
        WavCurve wavCLVCurve;
        /* The call site's ImageProcessor owns this table; fillLut() writes
         * every entry, so a local instance of the same size is equivalent. */
        LUTf wavclCurve(65536, 0);
        WavCurve wavdenoise;
        WavCurve wavdenoiseh;
        Wavblcurve wavblcurve;
        WavOpacityCurveRG waOpacityCurveRG;
        WavOpacityCurveSH waOpacityCurveSH;
        WavOpacityCurveBY waOpacityCurveBY;
        WavOpacityCurveW waOpacityCurveW;
        WavOpacityCurveWL waOpacityCurveWL;
        LabImage *provradius = nullptr;
        bool procont = WaveParams.expcontrast;
        bool prochro = WaveParams.expchroma;
        bool proedge = WaveParams.expedge;
        bool profin = WaveParams.expfinal;
        bool proton = WaveParams.exptoning;
        bool pronois = WaveParams.expnoise;

        if (WaveParams.softrad > 0.f) {
            provradius = new LabImage(labView, true);
        }

        params.wavelet.getCurves(wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL);

        CurveFactory::diagonalCurve2Lut(params.wavelet.wavclCurve, wavclCurve, 1);

        if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {
            const Glib::ustring provis = params.wavelet.CLmethod;
            params.wavelet.CLmethod = "all";
            ipf.ip_wavelet(&labView, &labView, 2, WaveParams, wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW,  waOpacityCurveWL, wavclCurve, 1);
            unshar = new LabImage(labView, true);
            params.wavelet.CLmethod = provis;

            WaveParams.expcontrast = false;
            WaveParams.expchroma = false;
            WaveParams.expedge = false;
            WaveParams.expfinal = false;
            WaveParams.exptoning = false;
            WaveParams.expnoise = false;
        }

        ipf.ip_wavelet(&labView, &labView, 2, WaveParams, wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW,  waOpacityCurveWL, wavclCurve, 1);

        if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {
            WaveParams.expcontrast = procont;
            WaveParams.expchroma = prochro;
            WaveParams.expedge = proedge;
            WaveParams.expfinal = profin;
            WaveParams.exptoning = proton;
            WaveParams.expnoise = pronois;

            if (WaveParams.softrad > 0.f) {
                array2D<float> ble(fw, fh);
                array2D<float> guid(fw, fh);
                Imagefloat *tmpImage = nullptr;
                tmpImage = new Imagefloat(fw, fh);
#ifdef _OPENMP
                #pragma omp parallel for
#endif

                for (int ir = 0; ir < fh; ir++)
                    for (int jr = 0; jr < fw; jr++) {
                        float X, Y, Z;
                        float L = provradius->L[ir][jr];
                        float a = provradius->a[ir][jr];
                        float b = provradius->b[ir][jr];
                        Color::Lab2XYZ(L, a, b, X, Y, Z);

                        guid[ir][jr] = Y / 32768.f;
                        float La = labView.L[ir][jr];
                        float aa = labView.a[ir][jr];
                        float ba = labView.b[ir][jr];
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

                float blur = 10.f / 1 * (0.5f + 0.8f * WaveParams.softrad);
                // rtengine::guidedFilter(guid, ble, ble, blur, 0.001, multiTh);
                rtengine::guidedFilter(guid, ble, ble, blur, epsil, false);



#ifdef _OPENMP
                #pragma omp parallel for
#endif

                for (int ir = 0; ir < fh; ir++)
                    for (int jr = 0; jr < fw; jr++) {
                        float X = tmpImage->r(ir, jr);
                        float Y = 32768.f * ble[ir][jr];
                        float Z = tmpImage->b(ir, jr);
                        float L, a, b;
                        Color::XYZ2Lab(X, Y, Z, L, a, b);
                        labView.L[ir][jr] = L;
                    }

                delete tmpImage;
            }

        }

        if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {
            float mL = (float)(WaveParams.mergeL / 100.f);
            float mC = (float)(WaveParams.mergeC / 100.f);
            float mL0;
            float mC0;

            if ((WaveParams.CLmethod == "one" || WaveParams.CLmethod == "inf")  && WaveParams.Backmethod == "black") {
                mL0 = mC0 = 0.f;
                mL = -1.5f * mL;
                mC = -mC;
            } else if (WaveParams.CLmethod == "sup" && WaveParams.Backmethod == "resid") {
                mL0 = mL;
                mC0 = mC;
            } else {
                mL0 = mL = mC0 = mC = 0.f;
            }


#ifdef _OPENMP
            #pragma omp parallel for
#endif

            for (int x = 0; x < fh; x++)
                for (int y = 0; y < fw; y++) {
                    labView.L[x][y] = LIM((1.f + mL0) * (unshar->L[x][y]) - mL * labView.L[x][y], 0.f, 32768.f);
                    labView.a[x][y] = (1.f + mC0) * (unshar->a[x][y]) - mC * labView.a[x][y];
                    labView.b[x][y] = (1.f + mC0) * (unshar->b[x][y]) - mC * labView.b[x][y];
                }

            delete unshar;
            unshar    = NULL;

            if (WaveParams.softrad > 0.f) {
                delete provradius;
                provradius    = NULL;
            }

        }

        wavCLVCurve.Reset();
    }
}

LabWaveletContext::LabWaveletContext(procparams::ProcParams& params,
                                     ImProcFunctions& ipf, int width,
                                     int height)
    : params_(params), ipf_(ipf), width_(width), height_(height)
{
    const procparams::DirPyrEqualizerParams& equalizer =
        params.dirpyrequalizer;
    gamutlab_ = equalizer.gamutlab;
    skinprotect_ = static_cast<float>(equalizer.skinprotect);
    b_l_ = static_cast<float>(equalizer.hueskin.getBottomLeft()) / 100.f;
    t_l_ = static_cast<float>(equalizer.hueskin.getTopLeft()) / 100.f;
    t_r_ = static_cast<float>(equalizer.hueskin.getTopRight()) / 100.f;
}

LabWaveletContext::~LabWaveletContext() = default;

RtLabWaveletContext LabWaveletContext::descriptor(bool supported)
{
    RtLabWaveletContext context{};
    context.size = sizeof(RtLabWaveletContext);
    context.version = RT_LAB_WAVELET_CONTEXT_VERSION;
    context.magic = RT_LAB_WAVELET_CONTEXT_MAGIC;
    context.supported = supported ? 1u : 0u;
    context.self = this;
    context.build = &build_thunk;
    context.prepare = &prepare_thunk;
    context.run_host = &run_host_thunk;
    context.needs_scratch =
        gamutlab_ && skinprotect_ != 0.f ? 1u : 0u;
    context.levels = static_cast<uint32_t>(levels_);
    context.skinprotect = skinprotect_ != 0.f ? 1u : 0u;
    context.wavelet.active = wavelet_active_ ? 1u : 0u;
    context.wavelet.levels = static_cast<uint32_t>(wavelet_levels_);
    context.wavelet.taps = 8u;
    context.wavelet.entries = static_cast<uint32_t>(
        wavelet_params_.size());
    context.wavelet.params =
        wavelet_params_.empty() ? nullptr : wavelet_params_.data();
    context.wavelet.compute_mea = &compute_mea_thunk;
    return context;
}

int LabWaveletContext::build(RtLabWaveletResult& out)
{
    const auto fill_result = [&out, this]() {
        out.luts = luts_.empty() ? nullptr : luts_.data();
        out.hue = hue_.empty() ? nullptr : hue_.data();
        out.chrom = chrom_.empty() ? nullptr : chrom_.data();
        out.lut_entries = RT_LAB_WAVELET_LUT_ENTRIES;
        out.levels = static_cast<uint32_t>(levels_);
        out.skinprotect = skinprotect_ != 0.f ? 1u : 0u;
        out.wavelet = wavelet_active_ ? 1u : 0u;
        out.wavelet_levels = static_cast<uint32_t>(wavelet_levels_);
        out.wavelet_entries =
            static_cast<uint32_t>(wavelet_params_.size());
        out.params =
            wavelet_params_.empty() ? nullptr : wavelet_params_.data();
    };

    if (built_) {
        fill_result();
        return 0;
    }

    levels_ = params_.dirpyrequalizer.enabled
                  ? labWaveletLastLevel(params_.dirpyrequalizer.mult)
                  : 0;
    if (levels_ > static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS)) {
        return 1;
    }

    if (levels_ > 0) {
        if (width_ < 8 || height_ < 8) {
            return 2;
        }

        for (int level = 0;
             level < static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS); ++level) {
            multi_[level] =
                static_cast<float>(params_.dirpyrequalizer.mult[level]);
        }

        /*
         * One host-built range table per active level, through the block's own
         * fillLut(). The device never re-derives the threshold arithmetic.
         */
        luts_.assign(static_cast<std::size_t>(levels_) *
                         RT_LAB_WAVELET_LUT_ENTRIES,
                     0.f);
        for (int level = 0; level < levels_; ++level) {
            LUTf table(static_cast<int>(RT_LAB_WAVELET_LUT_ENTRIES));
            dirpyrFillLut(table, level, params_.dirpyrequalizer.threshold,
                          multi_[level], skinprotect_);
            float* target = luts_.data() +
                            static_cast<std::size_t>(level) *
                                RT_LAB_WAVELET_LUT_ENTRIES;
            for (std::uint32_t index = 0;
                 index < RT_LAB_WAVELET_LUT_ENTRIES; ++index) {
                target[index] = table[static_cast<int>(index)];
            }
        }

        if (skinprotect_ != 0.f) {
            const std::size_t pixels = static_cast<std::size_t>(width_) *
                                       static_cast<std::size_t>(height_);
            hue_.assign(pixels, 0.f);
            chrom_.assign(pixels, 0.f);
        }
    }

    wavelet_levels_ =
        labWaveletTransformLevels(params_, width_, height_);
    wavelet_active_ = params_.wavelet.enabled && wavelet_levels_ >= 1;
    if (params_.wavelet.enabled && !wavelet_active_) {
        return 3;
    }
    if (wavelet_active_) {
        if (width_ <= 0 || height_ <= 0 ||
            wavelet_levels_ >
                static_cast<int>(RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS)) {
            return 4;
        }
        const procparams::WaveletParams& wavelet = params_.wavelet;
        constexpr std::size_t params_count =
            RT_LAB_WAVELET_WAVELET_PARAMS;
        constexpr std::size_t mea_count = RT_LAB_WAVELET_MEA_ENTRIES;
        wavelet_params_.assign(
            params_count +
                static_cast<std::size_t>(wavelet_levels_) * mea_count,
            0.f);

        float* table = wavelet_params_.data();
        const auto fold = [](double value) {
            return static_cast<float>(value);
        };
        table[RT_LW_PARAM_LEVELS] =
            static_cast<float>(wavelet_levels_);
        table[RT_LW_PARAM_HSMET] =
            wavelet.HSmethod == "with" ? 1.f : 0.f;
        table[RT_LW_PARAM_NUMLEVH] =
            static_cast<float>(wavelet.threshold - 1);
        table[RT_LW_PARAM_NUMLEVS] =
            static_cast<float>(wavelet.threshold2);
        for (int level = 0; level < 10; ++level) {
            float value = 0.f;
            if (level < wavelet.thres && level < 9) {
                value = static_cast<float>(wavelet.c[level]);
            } else if (level == 9) {
                value = static_cast<float>(wavelet.sup);
            }
            table[RT_LW_PARAM_C + level] = value;
        }
        table[RT_LW_PARAM_THRESHOLD] =
            static_cast<float>(wavelet.threshold);
        table[RT_LW_PARAM_THRESHOLD2] =
            static_cast<float>(wavelet.threshold2);
        table[RT_LW_PARAM_SIGMA] = fold(wavelet.sigma);
        table[RT_LW_PARAM_OFFSET] = fold(wavelet.offset);
        table[RT_LW_PARAM_LOWTHR] = fold(wavelet.lowthr);
        table[RT_LW_PARAM_STRENGTH] = std::min(
            1.f, std::max(0.f, static_cast<float>(wavelet.strength) / 100.f));
        table[RT_LW_PARAM_B_LHL] =
            static_cast<float>(wavelet.hllev.getBottomLeft());
        table[RT_LW_PARAM_T_LHL] =
            static_cast<float>(wavelet.hllev.getTopLeft());
        table[RT_LW_PARAM_B_RHL] =
            static_cast<float>(wavelet.hllev.getBottomRight());
        table[RT_LW_PARAM_T_RHL] =
            static_cast<float>(wavelet.hllev.getTopRight());
        table[RT_LW_PARAM_B_LSL] =
            static_cast<float>(wavelet.bllev.getBottomLeft());
        table[RT_LW_PARAM_T_LSL] =
            static_cast<float>(wavelet.bllev.getTopLeft());
        table[RT_LW_PARAM_B_RSL] =
            static_cast<float>(wavelet.bllev.getBottomRight());
        table[RT_LW_PARAM_T_RSL] =
            static_cast<float>(wavelet.bllev.getTopRight());
        constexpr int taps = 8;
        table[RT_LW_PARAM_TAPS] = static_cast<float>(taps);
        for (int index = 0; index < taps; ++index) {
            table[RT_LW_PARAM_FILTER_ANAL_LO + index] = Daub4_anal8[0][index];
            table[RT_LW_PARAM_FILTER_ANAL_HI + index] = Daub4_anal8[1][index];
            table[RT_LW_PARAM_FILTER_SYNTH_LO + index] =
                Daub4_anal8[0][taps - 1 - index];
            table[RT_LW_PARAM_FILTER_SYNTH_HI + index] =
                Daub4_anal8[1][taps - 1 - index];
        }
    }

    built_ = true;
    fill_result();
    return 0;
}

int LabWaveletContext::computeMea(const float* stats, uint32_t levels)
{
    if (!wavelet_active_ || stats == nullptr ||
        levels != static_cast<uint32_t>(wavelet_levels_) ||
        levels < 1 ||
        levels > RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) {
        return 1;
    }

    float mean[RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS] = {};
    float sigma[RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS] = {};
    const float effect = static_cast<float>(params_.wavelet.sigma);
    const float offs = static_cast<float>(params_.wavelet.offset);
    float* mea = wavelet_params_.data() + RT_LAB_WAVELET_WAVELET_PARAMS;

    for (uint32_t level = 0; level < levels; ++level) {
        float averagePlus[4] = {};
        float sigmaPlus[4] = {};
        for (int dir = 1; dir < 4; ++dir) {
            const std::size_t base =
                (static_cast<std::size_t>(level) * 3 +
                 static_cast<std::size_t>(dir - 1)) *
                RT_LAB_WAVELET_STATS_PER_BAND;
            averagePlus[dir] = stats[base + 0];
            sigmaPlus[dir] = stats[base + 4];
        }

        /* Eval2()'s accumulation order and its / 3. */
        float averageL = 0.f;
        float sigL = 0.f;
        for (int dir = 1; dir < 4; ++dir) {
            averageL += averagePlus[dir];
            sigL += sigmaPlus[dir];
        }
        averageL /= 3;
        sigL /= 3;
        mean[level] = averageL;
        sigma[level] = sigL;

        ipf_.calceffect(static_cast<int>(level), mean, sigma,
                        mea + static_cast<std::size_t>(level) *
                                  RT_LAB_WAVELET_MEA_ENTRIES,
                        effect, offs);
    }
    return 0;
}

int LabWaveletContext::prepare(RtStageBuffer& lab)
{
    if (skinprotect_ == 0.f) {
        /* The gamutlab pre-pass only runs together with skin protection. */
        return 0;
    }
    if (lab.width != width_ || lab.height != height_ ||
        lab.format != RT_STAGE_F32 || lab.planes != 3) {
        return 1;
    }

    LabImage image(width_, height_);
    copy_to_host(lab, image);

    if (gamutlab_) {
        const double artifact = 4.0;
        const double radius = artifact / RT_LAB_WAVELET_EXPORT_SCALE;
        ipf_.BadpixelsLab(&image, radius, 5, 50.f);
    }

    std::vector<float*> hue_rows(static_cast<std::size_t>(height_));
    std::vector<float*> chrom_rows(static_cast<std::size_t>(height_));
    for (int y = 0; y < height_; ++y) {
        hue_rows[static_cast<std::size_t>(y)] =
            hue_.data() + static_cast<std::size_t>(y) * width_;
        chrom_rows[static_cast<std::size_t>(y)] =
            chrom_.data() + static_cast<std::size_t>(y) * width_;
    }
    dirpyrHueChroma(image.a, image.b, hue_rows.data(), chrom_rows.data(),
                    width_, height_);

    if (gamutlab_) {
        copy_from_host(image, lab);
    }
    return 0;
}

int LabWaveletContext::runHost(RtStageBuffer& lab)
{
    if (lab.data == nullptr || lab.format != RT_STAGE_F32 ||
        lab.planes != 3 || lab.width <= 0 || lab.height <= 0) {
        return 1;
    }

    LabImage image(lab.width, lab.height);
    copy_to_host(lab, image);
    labWaveletHostBlock(ipf_, params_, image);
    copy_from_host(image, lab);
    return 0;
}

} // namespace rtengine::stage
