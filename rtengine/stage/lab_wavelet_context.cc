#include "stage/lab_wavelet_context.h"

#include "color.h"
#include "curves.h"
#include "dirpyr_equalizer.h"
#include "guidedfilter.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "labimage.h"
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

bool labWaveletSupported(const procparams::ProcParams& params)
{
    const procparams::DirPyrEqualizerParams& equalizer =
        params.dirpyrequalizer;

    if (!equalizer.enabled || equalizer.cbdlMethod != "aft") {
        return false;
    }
    /* The wavelet pass is the next node increment; until it is ported an
     * enabled wavelet tool keeps the whole block on the host. */
    if (params.wavelet.enabled) {
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

    for (int level = 0; level < static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS);
         ++level) {
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

    /* A block whose multipliers are all neutral does not touch the image; it
     * is a residual so the host records it instead of an empty device pass. */
    return labWaveletLastLevel(equalizer.mult) > 0;
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
    return context;
}

int LabWaveletContext::build(RtLabWaveletResult& out)
{
    if (built_) {
        out.luts = luts_.data();
        out.hue = hue_.empty() ? nullptr : hue_.data();
        out.chrom = chrom_.empty() ? nullptr : chrom_.data();
        out.lut_entries = RT_LAB_WAVELET_LUT_ENTRIES;
        out.levels = static_cast<uint32_t>(levels_);
        out.skinprotect = skinprotect_ != 0.f ? 1u : 0u;
        return 0;
    }

    levels_ = labWaveletLastLevel(params_.dirpyrequalizer.mult);
    if (levels_ <= 0 ||
        levels_ > static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS)) {
        return 1;
    }
    if (width_ < 8 || height_ < 8) {
        return 2;
    }

    for (int level = 0; level < static_cast<int>(RT_LAB_WAVELET_MAX_LEVELS);
         ++level) {
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
        for (std::uint32_t index = 0; index < RT_LAB_WAVELET_LUT_ENTRIES;
             ++index) {
            target[index] = table[static_cast<int>(index)];
        }
    }

    if (skinprotect_ != 0.f) {
        const std::size_t pixels =
            static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        hue_.assign(pixels, 0.f);
        chrom_.assign(pixels, 0.f);
    }

    built_ = true;
    out.luts = luts_.data();
    out.hue = hue_.empty() ? nullptr : hue_.data();
    out.chrom = chrom_.empty() ? nullptr : chrom_.data();
    out.lut_entries = RT_LAB_WAVELET_LUT_ENTRIES;
    out.levels = static_cast<uint32_t>(levels_);
    out.skinprotect = skinprotect_ != 0.f ? 1u : 0u;
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
