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

int compute_mea_thunk(void* self, const float* stats, uint32_t levels,
                      uint32_t plane)
{
    return static_cast<LabWaveletContext*>(self)->computeMea(stats, levels,
                                                            plane);
}

int compute_final_thunk(void* self, const float* stats, uint32_t levels)
{
    return static_cast<LabWaveletContext*>(self)->computeFinalStats(stats,
                                                                   levels);
}

int compute_mad_thunk(void* self, const uint32_t* histogram, uint32_t levels,
                      uint32_t plane)
{
    return static_cast<LabWaveletContext*>(self)->computeMad(histogram, levels,
                                                             plane);
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

/*
 * ip_wavelet()'s mulopaRG/mulopaBY: nine samples of the 501-point opacity
 * curve at the fixed indices, scaled to a percentage. An unallocated curve
 * leaves every multiplier at zero, which is the block's own no-op.
 */
void toningMultipliers(const procparams::WaveletParams& wavelet,
                       float rg[RT_LAB_WAVELET_TONING_MULS],
                       float by[RT_LAB_WAVELET_TONING_MULS], bool& any)
{
    static constexpr int indices[RT_LAB_WAVELET_TONING_MULS] = {
        0, 62, 125, 187, 250, 312, 375, 438, 500};
    WavOpacityCurveRG curve_rg;
    WavOpacityCurveBY curve_by;
    curve_rg.Set(wavelet.opacityCurveRG);
    curve_by.Set(wavelet.opacityCurveBY);
    const bool rg_active = static_cast<bool>(curve_rg);
    const bool by_active = static_cast<bool>(curve_by);
    any = false;
    for (std::uint32_t level = 0; level < RT_LAB_WAVELET_TONING_MULS;
         ++level) {
        rg[level] = rg_active
                        ? 200.f * (curve_rg[indices[level]] - 0.5f)
                        : 0.f;
        by[level] = by_active
                        ? 200.f * (curve_by[indices[level]] - 0.5f)
                        : 0.f;
        any = any || rg[level] != 0.f || by[level] != 0.f;
    }
}

/*
 * The standard-L denoise panel's arithmetic plus the chroma denoise arm the
 * block forces when Expnoise selects six levels. Only the plain slider set is
 * ported: the denoise curves, the "cur" slim method, the expert arm and the
 * hue modulation all stay host residuals, and a user chroma denoise or noise
 * balance would change the forced 0.01 chroma strength.
 */
bool denoiseAdmitted(const procparams::WaveletParams& wavelet, int width,
                     int height)
{
    if (!wavelet.enabled || !wavelet.expnoise) {
        return false;
    }
    if (width <= 0 || height <= 0 ||
        std::min(width, height) <= LAB_WAVELET_MIN_DENOISE_WINDOW) {
        return false;
    }
    /* The standard WaveletDenoiseAllL/AB arms, not the BiShrink pair. */
    if (wavelet.quamethod != "cons") {
        return false;
    }
    /* The chroma strength is the block's own forced 0.01, so no user chroma
     * denoise, no balance and no colour-by-noise may alter it. */
    if (wavelet.chromfi != 0.0 || wavelet.chromco != 0.0 ||
        wavelet.balchrom != 0.0 || wavelet.cbenab) {
        return false;
    }
    /* The post-denoise slider refinement (slimethod "cur" and Thrden) is a
     * separate host multiply; the expert arm changes its level pairing. */
    if (wavelet.slimethod != "sli" || wavelet.thrden != 0.0 ||
        wavelet.complexmethod != "normal") {
        return false;
    }
    /* A non-identity hue curve modulates noisevarlum per pixel. */
    {
        const FlatCurve hue_curve(wavelet.wavhuecurve);
        if (!hue_curve.isIdentity()) {
            return false;
        }
    }
    const double levels[5] = {
        wavelet.level0noise.getTop(), wavelet.level1noise.getTop(),
        wavelet.level2noise.getTop(), wavelet.level3noise.getTop(),
        wavelet.leveldenoise.getTop()};
    bool active = false;
    for (double value : levels) {
        if (!std::isfinite(value) ||
            value < LAB_WAVELET_MIN_WAVELET_NOISE ||
            value > LAB_WAVELET_MAX_WAVELET_NOISE) {
            return false;
        }
        active = active || value > LAB_WAVELET_DENOISE_ACTIVE;
    }
    if (!active) {
        return false;
    }
    if (!std::isfinite(wavelet.ballum) ||
        wavelet.ballum < LAB_WAVELET_MIN_WAVELET_BALLUM ||
        wavelet.ballum > LAB_WAVELET_MAX_WAVELET_BALLUM) {
        return false;
    }
    return true;
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

float labWaveletMadFromHistogram(const std::uint32_t* histogram,
                                 std::uint32_t samples)
{
    if (histogram == nullptr || samples <= 1) {
        return 0.f;
    }
    const std::uint32_t half = samples / 2;
    std::uint32_t count = 0;
    int median = 0;
    while (count < half) {
        count += histogram[median];
        ++median;
    }
    const std::uint32_t previous = count - histogram[median - 1];
    return ((static_cast<float>(median - 1) +
             static_cast<float>(half - previous) /
                 static_cast<float>(count - previous)) /
            0.6745f);
}

/*
 * The node supports the equaliser alone, the wavelet contrast tool alone, and
 * both in the block's order. Each half is validated only when it is enabled,
 * so a wavelet-only profile does not need dirpyrequalizer.enabled.
 */
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
    /*
     * Start from ip_wavelet()'s base decomposition level: the MaxLev slider,
     * forced to six by Expnoise. The chroma panel, and the chroma denoise
     * Expnoise forces, raise only the L decomposition (`levwavL`) to seven,
     * and this function returns that L count. `levwava` and `levwavb` start
     * from the unraised base and are then clamped with `min(levwav, ...)`,
     * so the a and b planes keep the base count and do not follow L to
     * seven; the launcher derives their count from the same base. Everything
     * below the raise therefore computes L's level count.
     */
    const bool chroma_user = wavelet.chromfi > 0.0 || wavelet.chromco > 0.0;
    if (chroma_user && levwav < 7) {
        levwav = 7;
    } else if (!chroma_user && wavelet.expnoise && minwin > 128 &&
               levwav == 6) {
        levwav = 7;
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

    /*
     * The trailing-zero trim in ip_wavelet(): a level whose multiplier is
     * exactly zero is dead when nothing else in the block needs it. Thrend is
     * read unconditionally afterwards and, when positive, restores the
     * untrimmed count so the residual tone mapping keeps its levels; delta and
     * the highlight/shadow kH then use that larger maxlvl.
     */
    const int capped = levwav;
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
    const float thrend = 0.01f * static_cast<float>(wavelet.thrend);
    if (thrend > 0.f) {
        levwav = capped;
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

    /*
     * The contrast panel, the chroma panel's "link" arm, the edge panel's
     * plain arm and the final panel's curve arm are the admitted set. Every
     * other export panel is a residual.
     */
    const bool contrast = wavelet.expcontrast;
    const bool chroma = wavelet.expchroma;
    const bool edge = wavelet.expedge;
    const bool edge_active = edge && wavelet.edgval > 0;
    const bool final_curves = wavelet.expfinal;
    const bool toning = wavelet.exptoning;
    const bool denoise = wavelet.expnoise;
    if ((!contrast && !chroma && !edge_active && !final_curves && !toning &&
         !denoise) ||
        wavelet.expbl || wavelet.expresid || wavelet.expclari) {
        return false;
    }
    /*
     * The standard denoise arm. ip_wavelet() forces six levels and, at six
     * levels, forces the chroma denoise strength to 0.01 when the profile's
     * own strength is zero, so the host always denoises the a and b bands as
     * well. The admitted set is the plain slider arm only.
     */
    if (denoise && !denoiseAdmitted(wavelet, width, height)) {
        return false;
    }
    /*
     * Toning's protection range is not ported; the zero-protection arm
     * (protab == 0) is the coefficient multiply. The opacity curves must
     * actually produce a multiplier, otherwise the panel is a no-op dressed
     * up as a device pass. sigmaton must be inside its slider range because
     * calceffect() divides by it.
     */
    if (toning) {
        if (wavelet.protab != 0.0 ||
            !std::isfinite(wavelet.sigmaton) ||
            wavelet.sigmaton < LAB_WAVELET_MIN_WAVELET_SIGMATON ||
            wavelet.sigmaton > LAB_WAVELET_MAX_WAVELET_SIGMATON) {
            return false;
        }
        float rg[RT_LAB_WAVELET_TONING_MULS] = {};
        float by[RT_LAB_WAVELET_TONING_MULS] = {};
        bool any = false;
        toningMultipliers(wavelet, rg, by, any);
        if (!any) {
            return false;
        }
    }
    /*
     * An enabled edge panel with a zero strength is a no-op, but the panel is
     * advertised as running, so refuse it rather than record a device pass
     * that did nothing; the whole block then stays on the host.
     */
    if (edge && !edge_active) {
        return false;
    }
    /*
     * The edge panel runs only its plain arm: no median level detection
     * (calckoe()'s Lipschitz coefficients), no np-method neighbour smoothing
     * and no lipst Lipschitz influence. `EDmethod` is displayed by the GUI
     * but the engine hard-codes EDmet == 2, so the threshold adjuster
     * (edgcont) is dead code; the SL selection is refused rather than
     * silently ignoring its visible sliders.
     */
    if (edge_active) {
        if (settings == nullptr) {
            return false;
        }
        if (wavelet.medianlev || wavelet.NPmethod != "none" ||
            wavelet.lipst || wavelet.EDmethod != "CU") {
            return false;
        }
        if (wavelet.edgval < LAB_WAVELET_MIN_WAVELET_EDGVAL ||
            wavelet.edgval > LAB_WAVELET_MAX_WAVELET_EDGVAL ||
            wavelet.edgrad < LAB_WAVELET_MIN_WAVELET_EDGRAD ||
            wavelet.edgrad > LAB_WAVELET_MAX_WAVELET_EDGRAD ||
            wavelet.edgthresh < LAB_WAVELET_MIN_WAVELET_EDGTHRESH ||
            wavelet.edgthresh > LAB_WAVELET_MAX_WAVELET_EDGTHRESH ||
            !std::isfinite(wavelet.edgeffect) ||
            wavelet.edgeffect < LAB_WAVELET_MIN_WAVELET_EDGEFFECT ||
            wavelet.edgeffect > LAB_WAVELET_MAX_WAVELET_EDGEFFECT) {
            return false;
        }
        /* Every Medgreinf mode is arithmetic, but an unknown value leaves the
         * block's `reinforce` uninitialised, so refuse it. */
        if (wavelet.Medgreinf != "more" && wavelet.Medgreinf != "none" &&
            wavelet.Medgreinf != "less") {
            return false;
        }
    }
    /*
     * The final panel's curve arm needs only the two curve controls. Its
     * balance-chroma method and residual tone mapping stay host residuals,
     * and sigmafin must be inside the GUI slider range because the block
     * divides by it.
     */
    if (final_curves &&
        (!std::isfinite(wavelet.sigmafin) ||
         wavelet.sigmafin < LAB_WAVELET_MIN_WAVELET_SIGMAFIN ||
         wavelet.sigmafin > LAB_WAVELET_MAX_WAVELET_SIGMAFIN)) {
        return false;
    }
    /*
     * ContAllAB()'s BA arm (balance chroma) runs when BAmethod is not "none"
     * and TMr is set, independently of the panels above. It is not ported, so
     * a profile that can reach it stays on the host.
     */
    if (wavelet.tmr || wavelet.BAmethod != "none") {
        return false;
    }
    if (wavelet.CLmethod != "all" || wavelet.Tilesmethod != "full") {
        return false;
    }
    if (wavelet.softrad != 0.0 || wavelet.softradend != 0.0) {
        return false;
    }
    /*
     * The contrast path's own skin protection is not ported. The chroma arm
     * is different: it always uses the wavelet block's skin mask, and when
     * the contrast panel is off no L coefficient is touched, so the two do
     * not mix.
     */
    if (wavelet.skinprotect != 0.0 && !(chroma && !contrast)) {
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
    /*
     * A non-identity chroma-by-hue curve scales the L contrast per pixel, and
     * a non-identity H curve switches the a/b pass to its joint arm. Neither
     * is ported.
     */
    {
        const FlatCurve chroma_curve(wavelet.Chcurve);
        const FlatCurve hue_curve(wavelet.hhcurve);
        if (!chroma_curve.isIdentity() || !hue_curve.isIdentity()) {
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
    if (!std::isfinite(wavelet.thrend) ||
        wavelet.thrend < LAB_WAVELET_MIN_WAVELET_THREND ||
        wavelet.thrend > LAB_WAVELET_MAX_WAVELET_THREND) {
        return false;
    }
    for (int level = 0; level < 9; ++level) {
        if (wavelet.c[level] < LAB_WAVELET_MIN_WAVELET_C ||
            wavelet.c[level] > LAB_WAVELET_MAX_WAVELET_C) {
            return false;
        }
    }
    if (chroma) {
        /* CHmethod "link" is the only arm with a simple coefficient: it
         * multiplies the contrast c[] by chro through the betaab ramp. */
        if (wavelet.CHmethod != "link") {
            return false;
        }
        if (wavelet.chro < LAB_WAVELET_MIN_WAVELET_CHRO ||
            wavelet.chro > LAB_WAVELET_MAX_WAVELET_CHRO) {
            return false;
        }
        if (!std::isfinite(wavelet.sigmacol) ||
            wavelet.sigmacol < LAB_WAVELET_MIN_WAVELET_SIGMACOL ||
            wavelet.sigmacol > LAB_WAVELET_MAX_WAVELET_SIGMACOL) {
            return false;
        }
        if (!std::isfinite(wavelet.skinprotect) ||
            wavelet.skinprotect < LAB_WAVELET_MIN_SKIN ||
            wavelet.skinprotect > LAB_WAVELET_MAX_SKIN) {
            return false;
        }
        if (!hueInRange(wavelet.hueskin.getBottomLeft()) ||
            !hueInRange(wavelet.hueskin.getTopLeft()) ||
            !hueInRange(wavelet.hueskin.getBottomRight()) ||
            !hueInRange(wavelet.hueskin.getTopRight())) {
            return false;
        }
        /* A chroma-only profile needs both a strength and a contrast
         * multiplier to do any work; otherwise it is a no-op dressed up as a
         * device pass. */
        if (!contrast) {
            bool active = wavelet.chro != 0;
            if (active) {
                active = false;
                for (int level = 0; level < 9; ++level) {
                    if (level < wavelet.thres && wavelet.c[level] != 0) {
                        active = true;
                    }
                }
            }
            if (!active) {
                return false;
            }
        }
    }
    /* The scoped contrast set leaves the finest level active; an all-zero
     * pyramid would be a no-op dressed up as a device pass. */
    if (contrast && wavelet.c[0] == 0) {
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

    const procparams::WaveletParams& wavelet = params.wavelet;
    wavelet_skinprotect_ = static_cast<float>(wavelet.skinprotect);
    wavelet_b_l_ =
        static_cast<float>(wavelet.hueskin.getBottomLeft()) / 100.f;
    wavelet_t_l_ = static_cast<float>(wavelet.hueskin.getTopLeft()) / 100.f;
    wavelet_t_r_ = static_cast<float>(wavelet.hueskin.getTopRight()) / 100.f;
    wavelet_b_r_ =
        static_cast<float>(wavelet.hueskin.getBottomRight()) / 100.f;
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
        levels_ > 0 && gamutlab_ && skinprotect_ != 0.f ? 1u : 0u;
    context.levels = static_cast<uint32_t>(levels_);
    context.skinprotect =
        (skinprotect_ != 0.f ||
         (wavelet_chroma_ && wavelet_skinprotect_ != 0.f))
            ? 1u
            : 0u;
    context.wavelet.active = wavelet_active_ ? 1u : 0u;
    context.wavelet.levels = static_cast<uint32_t>(wavelet_levels_);
    context.wavelet.taps = 8u;
    context.wavelet.planes = wavelet_ab_ ? 3u : 1u;
    context.wavelet.final = wavelet_final_ ? 1u : 0u;
    context.wavelet.toning = wavelet_toning_ ? 1u : 0u;
    context.wavelet.denoise = wavelet_denoise_ ? 1u : 0u;
    context.wavelet.final_refresh = wavelet_final_refresh_ ? 1u : 0u;
    context.wavelet.compute_mad = &compute_mad_thunk;
    context.wavelet.entries = static_cast<uint32_t>(
        wavelet_params_.size());
    context.wavelet.params =
        wavelet_params_.empty() ? nullptr : wavelet_params_.data();
    context.wavelet.compute_mea = &compute_mea_thunk;
    context.wavelet.compute_final = &compute_final_thunk;
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
        out.skinprotect =
            (skinprotect_ != 0.f ||
             (wavelet_chroma_ && wavelet_skinprotect_ != 0.f))
                ? 1u
                : 0u;
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
    wavelet_chroma_ = params_.wavelet.enabled &&
                      params_.wavelet.expchroma &&
                      params_.wavelet.CHmethod == "link";
    /*
     * The plain edge arm. `edgval == 0` leaves the host's edge block
     * untouched, so the device pass is only armed when a strength is set.
     * The sub-mode guards repeat the admission rules so a forced build can
     * never run a partial edge pass.
     */
    wavelet_edge_ = params_.wavelet.enabled && params_.wavelet.expedge &&
                    params_.wavelet.edgval > 0 &&
                    !params_.wavelet.medianlev &&
                    params_.wavelet.NPmethod == "none" &&
                    !params_.wavelet.lipst &&
                    params_.wavelet.EDmethod == "CU" &&
                    (params_.wavelet.Medgreinf == "more" ||
                     params_.wavelet.Medgreinf == "none" ||
                     params_.wavelet.Medgreinf == "less") &&
                    settings != nullptr;
    /*
     * The final panel's curve arm. The admission rules repeat here so a
     * forced build can never run a partial final pass: the balance-chroma
     * method, the residual tone mapping and the curves outside their range
     * are host residuals.
     */
    wavelet_final_ = params_.wavelet.enabled && params_.wavelet.expfinal &&
                     params_.wavelet.BAmethod == "none" &&
                     !params_.wavelet.tmr &&
                     params_.wavelet.softradend == 0.0 &&
                     params_.wavelet.softrad == 0.0 &&
                     std::isfinite(params_.wavelet.sigmafin) &&
                     params_.wavelet.sigmafin >=
                         LAB_WAVELET_MIN_WAVELET_SIGMAFIN &&
                     params_.wavelet.sigmafin <=
                         LAB_WAVELET_MAX_WAVELET_SIGMAFIN;
    /*
     * The toning panel's zero-protection arm. The admission rules repeat here
     * so a forced build can never run a partial toning pass.
     */
    wavelet_toning_ = false;
    if (params_.wavelet.enabled && params_.wavelet.exptoning &&
        params_.wavelet.protab == 0.0 &&
        std::isfinite(params_.wavelet.sigmaton) &&
        params_.wavelet.sigmaton >= LAB_WAVELET_MIN_WAVELET_SIGMATON &&
        params_.wavelet.sigmaton <= LAB_WAVELET_MAX_WAVELET_SIGMATON) {
        float rg[RT_LAB_WAVELET_TONING_MULS] = {};
        float by[RT_LAB_WAVELET_TONING_MULS] = {};
        bool any = false;
        toningMultipliers(params_.wavelet, rg, by, any);
        wavelet_toning_ = any;
    }
    /*
     * ip_wavelet() keeps the a/b decomposition alive whenever the edge,
     * final or toning panel is enabled: the level-trim around ContAllAB() has
     * a `!cp.edgeena`/`!cp.finena`/`!cp.toningena` condition, so a/b are
     * decomposed and reconstructed even with Expchroma off. The round trip is
     * not an exact identity, so the device must run it to match the block. The
     * denoise arm adds the forced chroma denoise, which needs the a/b planes
     * too.
     */
    wavelet_denoise_ = denoiseAdmitted(params_.wavelet, width_, height_);
    wavelet_ab_ = wavelet_chroma_ || wavelet_edge_ || wavelet_final_ ||
                  wavelet_toning_ || wavelet_denoise_;

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
    }

    /*
     * The wavelet chroma arm reuses the same hue/chroma masks as the
     * equaliser: the equaliser only changes L, so a/b are identical at the
     * start of both passes. Allocate them when either pass needs them.
     */
    if (skinprotect_ != 0.f ||
        (wavelet_chroma_ && wavelet_skinprotect_ != 0.f)) {
        const std::size_t pixels = static_cast<std::size_t>(width_) *
                                   static_cast<std::size_t>(height_);
        hue_.assign(pixels, 0.f);
        chrom_.assign(pixels, 0.f);
    }

    wavelet_levels_ =
        labWaveletTransformLevels(params_, width_, height_);
    wavelet_active_ = params_.wavelet.enabled && wavelet_levels_ >= 1;
    if (params_.wavelet.enabled && !wavelet_active_) {
        return 3;
    }
    {
        /*
         * ip_wavelet() re-evaluates the L statistics after the contrast,
         * edge and final multiplies only when
         * `cp.val > 0 || ref || contr || cp.diagcurv` (ipwavelet.cc:1573).
         * Otherwise finalContAllL() keeps the statistics Evaluate2() built
         * before the denoise and the multiply. cp.val is the edge strength,
         * ref a non-zero noise-curve bottom under the 128-pixel gate, contr
         * any level multiplier below the L count and diagcurv a non-identity
         * WL opacity curve.
         */
        const procparams::WaveletParams& wavelet = params_.wavelet;
        const bool noiseena =
            wavelet.expnoise &&
            std::min(width_, height_) > LAB_WAVELET_MIN_DENOISE_WINDOW;
        const bool ref =
            noiseena && (wavelet.level0noise.getBottom() > 0.0 ||
                         wavelet.level1noise.getBottom() > 0.0 ||
                         wavelet.level2noise.getBottom() > 0.0 ||
                         wavelet.level3noise.getBottom() > 0.0);
        bool contr = false;
        for (int level = 0; level < wavelet_levels_; ++level) {
            /* cp.mul[f] is c[f] below MaxLev and sup at index 9. */
            double value = 0.0;
            if (level == 9) {
                value = wavelet.sup;
            } else if (level < wavelet.thres && level < 9) {
                value = wavelet.c[level];
            }
            if (value != 0.0) {
                contr = true;
            }
        }
        WavOpacityCurveWL opacity_curve;
        opacity_curve.Set(wavelet.opacityCurveWL);
        const bool diagcurv = static_cast<bool>(opacity_curve);
        wavelet_final_refresh_ = wavelet.edgval > 0 || ref || contr ||
                                 diagcurv;
    }
    if (wavelet_active_) {
        if (width_ <= 0 || height_ <= 0 ||
            wavelet_levels_ >
                static_cast<int>(RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS)) {
            return 4;
        }
        const procparams::WaveletParams& wavelet = params_.wavelet;
        const std::size_t planes = wavelet_ab_ ? 3u : 1u;
        wavelet_params_.assign(
            labWaveletParamEntries(static_cast<std::uint32_t>(planes),
                                   static_cast<std::uint32_t>(wavelet_levels_),
                                   wavelet_edge_, wavelet_final_,
                                   wavelet_toning_, wavelet_denoise_),
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
        table[RT_LW_PARAM_CONTENA] = wavelet.expcontrast ? 1.f : 0.f;
        table[RT_LW_PARAM_CHRO] = static_cast<float>(wavelet.chro);
        table[RT_LW_PARAM_CHMET] = wavelet_chroma_ ? 2.f : 0.f;
        table[RT_LW_PARAM_CHROMA] = wavelet_ab_ ? 1.f : 0.f;
        table[RT_LW_PARAM_SIGMACOL] = fold(wavelet.sigmacol);
        table[RT_LW_PARAM_W_SKIN] = wavelet_skinprotect_;
        table[RT_LW_PARAM_W_B_L] = wavelet_b_l_;
        table[RT_LW_PARAM_W_T_L] = wavelet_t_l_;
        table[RT_LW_PARAM_W_T_R] = wavelet_t_r_;
        table[RT_LW_PARAM_W_B_R] = wavelet_b_r_;
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

        table[RT_LW_PARAM_EDGEENA] = wavelet_edge_ ? 1.f : 0.f;
        if (wavelet_edge_) {
            const int reinforce = wavelet.Medgreinf == "more"
                                      ? 1
                                      : (wavelet.Medgreinf == "none" ? 2 : 3);
            table[RT_LW_PARAM_EDG_VAL] =
                static_cast<float>(wavelet.edgval);
            table[RT_LW_PARAM_EDG_RAD] =
                static_cast<float>(wavelet.edgrad);
            table[RT_LW_PARAM_EDG_TIL] =
                static_cast<float>(wavelet.edgthresh);
            table[RT_LW_PARAM_EDG_EFF] = fold(wavelet.edgeffect);
            table[RT_LW_PARAM_EDG_REINFORCE] =
                static_cast<float>(reinforce);
            table[RT_LW_PARAM_EDG_HIG] =
                static_cast<float>(settings->edghi);
            table[RT_LW_PARAM_EDG_LOW] =
                static_cast<float>(settings->edglo);

            const std::size_t edge_offset =
                RT_LAB_WAVELET_WAVELET_PARAMS +
                planes * static_cast<std::size_t>(wavelet_levels_) *
                    RT_LAB_WAVELET_MEA_ENTRIES;
            float* edge_tail = table + edge_offset;
            edge_tail[0] = static_cast<float>(settings->limrad);

            /*
             * The edge curve is the block's own WavCurve over ccwcurve; the
             * device reads the same 501 samples through the LUTf float
             * interpolation, so a curve that builds here is followed exactly.
             */
            WavCurve curve;
            curve.Set(wavelet.ccwcurve);
            const bool curve_active = static_cast<bool>(curve);
            table[RT_LW_PARAM_EDG_CURVE] = curve_active ? 1.f : 0.f;
            if (curve_active) {
                float* sampled =
                    edge_tail + 1 +
                    static_cast<std::size_t>(wavelet_levels_) *
                        RT_LAB_WAVELET_EDGE_ENTRIES;
                for (std::uint32_t index = 0;
                     index < RT_LAB_WAVELET_CURVE_ENTRIES; ++index) {
                    sampled[index] =
                        curve[static_cast<int>(index)];
                }
            }
        }
        if (wavelet_final_) {
            const std::size_t final_offset =
                RT_LAB_WAVELET_WAVELET_PARAMS +
                planes * static_cast<std::size_t>(wavelet_levels_) *
                    RT_LAB_WAVELET_MEA_ENTRIES +
                (wavelet_edge_
                     ? 1 + static_cast<std::size_t>(wavelet_levels_) *
                               RT_LAB_WAVELET_EDGE_ENTRIES +
                           RT_LAB_WAVELET_CURVE_ENTRIES
                     : 0);
            float* final_tail = table + final_offset;
            final_tail[0] = static_cast<float>(wavelet.sigmafin);
            /*
             * Both tables are the block's own LUTs, sampled at every integer
             * index so the device reproduces the host float lookup exactly.
             * The final diagonal LUT is built with no clipping flags, as the
             * host block does, so its lookup extrapolates beyond the last
             * interval rather than clamping.
             */
            WavOpacityCurveWL opacity;
            opacity.Set(wavelet.opacityCurveWL);
            /* An empty curve leaves the host LUT unallocated and the block
             * skips finalContAllL(); refuse the device pass rather than
             * apply a coefficient curve the host did not. The pixel curve
             * would still match, but the host fallback keeps both arms on
             * one path. */
            if (!opacity) {
                return 5;
            }
            float* final_curve =
                final_tail + 1 +
                static_cast<std::size_t>(wavelet_levels_) *
                    RT_LAB_WAVELET_FINAL_STATS_ENTRIES;
            for (std::uint32_t index = 0;
                 index < RT_LAB_WAVELET_CURVE_ENTRIES; ++index) {
                final_curve[index] = opacity[static_cast<int>(index)];
            }
            LUTf wavcl_curve(static_cast<int>(
                                 RT_LAB_WAVELET_FINAL_LUT_ENTRIES),
                             0);
            CurveFactory::diagonalCurve2Lut(wavelet.wavclCurve, wavcl_curve,
                                            1);
            float* final_lut = final_curve + RT_LAB_WAVELET_CURVE_ENTRIES;
            for (std::uint32_t index = 0;
                 index < RT_LAB_WAVELET_FINAL_LUT_ENTRIES; ++index) {
                final_lut[index] = wavcl_curve[static_cast<int>(index)];
            }
        }
        if (wavelet_toning_) {
            const std::size_t toning_offset =
                RT_LAB_WAVELET_WAVELET_PARAMS +
                planes * static_cast<std::size_t>(wavelet_levels_) *
                    RT_LAB_WAVELET_MEA_ENTRIES +
                (wavelet_edge_
                     ? 1 + static_cast<std::size_t>(wavelet_levels_) *
                               RT_LAB_WAVELET_EDGE_ENTRIES +
                           RT_LAB_WAVELET_CURVE_ENTRIES
                     : 0) +
                (wavelet_final_
                     ? 1 + static_cast<std::size_t>(wavelet_levels_) *
                               RT_LAB_WAVELET_FINAL_STATS_ENTRIES +
                           RT_LAB_WAVELET_CURVE_ENTRIES +
                           RT_LAB_WAVELET_FINAL_LUT_ENTRIES
                     : 0);
            float* toning_tail = table + toning_offset;
            toning_tail[0] = static_cast<float>(wavelet.sigmaton);
            float rg[RT_LAB_WAVELET_TONING_MULS] = {};
            float by[RT_LAB_WAVELET_TONING_MULS] = {};
            bool any = false;
            toningMultipliers(wavelet, rg, by, any);
            for (std::uint32_t level = 0;
                 level < RT_LAB_WAVELET_TONING_MULS; ++level) {
                toning_tail[1 + level] = rg[level];
                toning_tail[1 + RT_LAB_WAVELET_TONING_MULS + level] =
                    by[level];
            }
        }
        if (wavelet_denoise_) {
            const std::size_t denoise_offset =
                RT_LAB_WAVELET_WAVELET_PARAMS +
                planes * static_cast<std::size_t>(wavelet_levels_) *
                    RT_LAB_WAVELET_MEA_ENTRIES +
                (wavelet_edge_
                     ? 1 + static_cast<std::size_t>(wavelet_levels_) *
                               RT_LAB_WAVELET_EDGE_ENTRIES +
                           RT_LAB_WAVELET_CURVE_ENTRIES
                     : 0) +
                (wavelet_final_
                     ? 1 + static_cast<std::size_t>(wavelet_levels_) *
                               RT_LAB_WAVELET_FINAL_STATS_ENTRIES +
                           RT_LAB_WAVELET_CURVE_ENTRIES +
                           RT_LAB_WAVELET_FINAL_LUT_ENTRIES
                     : 0) +
                (wavelet_toning_
                     ? RT_LAB_WAVELET_TONING_HEADER +
                           2 * static_cast<std::size_t>(wavelet_levels_) *
                               RT_LAB_WAVELET_TONING_MEA_ENTRIES
                     : 0);
            float* denoise_tail = table + denoise_offset;
            const float lev[5] = {
                static_cast<float>(wavelet.level0noise.getTop()),
                static_cast<float>(wavelet.level1noise.getTop()),
                static_cast<float>(wavelet.level2noise.getTop()),
                static_cast<float>(wavelet.level3noise.getTop()),
                static_cast<float>(wavelet.leveldenoise.getTop())};
            /* ip_wavelet(): vari[0..2], then kr3 and kr4 on vari[3..5]. The
             * multipliers are part of the denoise arithmetic, not a curve. */
            const auto variance = [](float value) {
                return 0.8f *
                       SQR((value / 125.f) * (1.f + value / 25.f));
            };
            float kr3 = 1.f;
            if (lev[3] < 10.f) {
                kr3 = 0.3f;
            } else if (lev[3] < 30.f) {
                kr3 = 0.6f;
            } else if (lev[3] < 70.f) {
                kr3 = 0.8f;
            }
            float kr4 = 1.f;
            if (lev[4] < 10.f) {
                kr4 = 0.6f;
            } else if (lev[4] < 30.f) {
                kr4 = 0.8f;
            } else if (lev[4] < 70.f) {
                kr4 = 0.9f;
            }
            denoise_tail[0] = std::max(0.000001f, variance(lev[0]));
            denoise_tail[1] = std::max(0.000001f, variance(lev[1]));
            denoise_tail[2] = std::max(0.000001f, variance(lev[2]));
            denoise_tail[3] =
                std::max(0.000001f, kr3 * variance(lev[3]));
            denoise_tail[4] =
                std::max(0.000001f, kr4 * variance(lev[4]));
            denoise_tail[5] =
                std::max(0.000001f, kr4 * variance(lev[4]));
            denoise_tail[6] = static_cast<float>(wavelet.ballum);
        }
    }

    built_ = true;
    fill_result();
    return 0;
}

int LabWaveletContext::computeMea(const float* stats, uint32_t levels,
                                  uint32_t plane)
{
    const uint32_t planes = wavelet_ab_ ? 3u : 1u;
    const uint32_t stride = static_cast<uint32_t>(wavelet_levels_);
    if (!wavelet_active_ || stats == nullptr || plane >= planes ||
        levels < 1 || levels > stride ||
        stride > RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) {
        return 1;
    }

    float mean[RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS] = {};
    float sigma[RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS] = {};
    /* ContAllL() uses sigma/offset on L; ContAllAB() uses sigmacol with an
     * offset of 1 on a and b. */
    const float effect =
        plane == 0 ? static_cast<float>(params_.wavelet.sigma)
                   : static_cast<float>(params_.wavelet.sigmacol);
    const float offs =
        plane == 0 ? static_cast<float>(params_.wavelet.offset) : 1.f;
    float* mea = wavelet_params_.data() + RT_LAB_WAVELET_WAVELET_PARAMS +
                 static_cast<std::size_t>(plane) *
                     static_cast<std::size_t>(stride) *
                     RT_LAB_WAVELET_MEA_ENTRIES;
    float* edge_mea = nullptr;
    const std::size_t edge_offset =
        RT_LAB_WAVELET_WAVELET_PARAMS +
        static_cast<std::size_t>(planes) *
            static_cast<std::size_t>(stride) *
            RT_LAB_WAVELET_MEA_ENTRIES;
    if (wavelet_edge_ && plane == 0) {
        edge_mea = wavelet_params_.data() + edge_offset + 1;
    }
    float* toning_mea = nullptr;
    if (wavelet_toning_ && plane >= 1) {
        const std::size_t final_offset =
            edge_offset +
            (wavelet_edge_
                 ? 1 + static_cast<std::size_t>(stride) *
                           RT_LAB_WAVELET_EDGE_ENTRIES +
                       RT_LAB_WAVELET_CURVE_ENTRIES
                 : 0);
        const std::size_t toning_offset =
            final_offset +
            (wavelet_final_
                 ? 1 + static_cast<std::size_t>(stride) *
                           RT_LAB_WAVELET_FINAL_STATS_ENTRIES +
                       RT_LAB_WAVELET_CURVE_ENTRIES +
                       RT_LAB_WAVELET_FINAL_LUT_ENTRIES
                 : 0);
        toning_mea = wavelet_params_.data() + toning_offset +
                     RT_LAB_WAVELET_TONING_HEADER +
                     static_cast<std::size_t>(plane - 1) *
                         static_cast<std::size_t>(stride) *
                         RT_LAB_WAVELET_TONING_MEA_ENTRIES;
    }

    for (uint32_t level = 0; level < levels; ++level) {
        float averagePlus[4] = {};
        float sigmaPlus[4] = {};
        float maxPlus[4] = {};
        for (int dir = 1; dir < 4; ++dir) {
            const std::size_t base =
                (static_cast<std::size_t>(level) * 3 +
                 static_cast<std::size_t>(dir - 1)) *
                RT_LAB_WAVELET_STATS_PER_BAND;
            averagePlus[dir] = stats[base + 0];
            maxPlus[dir] = stats[base + 2];
            sigmaPlus[dir] = stats[base + 4];
        }

        /* Eval2()'s accumulation order and its / 3. */
        float averageL = 0.f;
        float sigL = 0.f;
        float maxL = 0.f;
        for (int dir = 1; dir < 4; ++dir) {
            averageL += averagePlus[dir];
            sigL += sigmaPlus[dir];
            maxL += maxPlus[dir];
        }
        averageL /= 3;
        sigL /= 3;
        maxL /= 3;
        mean[level] = averageL;
        sigma[level] = sigL;

        if (toning_mea != nullptr) {
            ipf_.calceffect(
                static_cast<int>(level), mean, sigma,
                toning_mea + static_cast<std::size_t>(level) *
                                 RT_LAB_WAVELET_TONING_MEA_ENTRIES,
                static_cast<float>(params_.wavelet.sigmaton), 1.f);
        }

        ipf_.calceffect(static_cast<int>(level), mean, sigma,
                        mea + static_cast<std::size_t>(level) *
                                  RT_LAB_WAVELET_MEA_ENTRIES,
                        effect, offs);
        if (edge_mea != nullptr) {
            float* entry = edge_mea +
                           static_cast<std::size_t>(level) *
                               RT_LAB_WAVELET_EDGE_ENTRIES;
            /* ContAllL()'s edge beta uses calceffect() with the edge effect
             * and an offset of 1. */
            ipf_.calceffect(
                static_cast<int>(level), mean, sigma, entry,
                static_cast<float>(params_.wavelet.edgeffect), 1.f);
            entry[10] = maxL;
            entry[11] = averageL;
            entry[12] = sigL;
        }
    }
    return 0;
}

int LabWaveletContext::computeFinalStats(const float* stats, uint32_t levels)
{
    const uint32_t planes = wavelet_ab_ ? 3u : 1u;
    if (!wavelet_active_ || !wavelet_final_ || stats == nullptr ||
        levels != static_cast<uint32_t>(wavelet_levels_) ||
        levels < 1 ||
        levels > RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) {
        return 1;
    }

    const std::size_t edge_offset =
        RT_LAB_WAVELET_WAVELET_PARAMS +
        static_cast<std::size_t>(planes) *
            static_cast<std::size_t>(levels) *
            RT_LAB_WAVELET_MEA_ENTRIES;
    const std::size_t final_offset =
        edge_offset +
        (wavelet_edge_
             ? 1 + static_cast<std::size_t>(levels) *
                       RT_LAB_WAVELET_EDGE_ENTRIES +
                   RT_LAB_WAVELET_CURVE_ENTRIES
             : 0);
    float* final_stats = wavelet_params_.data() + final_offset + 1;
    const float sigmafin = static_cast<float>(params_.wavelet.sigmafin);

    for (uint32_t level = 0; level < levels; ++level) {
        float averagePlus[4] = {};
        float sigmaPlus[4] = {};
        float maxPlus[4] = {};
        for (int dir = 1; dir < 4; ++dir) {
            const std::size_t base =
                (static_cast<std::size_t>(level) * 3 +
                 static_cast<std::size_t>(dir - 1)) *
                RT_LAB_WAVELET_STATS_PER_BAND;
            averagePlus[dir] = stats[base + 0];
            maxPlus[dir] = stats[base + 2];
            sigmaPlus[dir] = stats[base + 4];
        }

        /* Eval2()'s accumulation order and its / 3, over the bands the
         * contrast and edge passes have already multiplied. */
        float averageL = 0.f;
        float sigL = 0.f;
        float maxL = 0.f;
        for (int dir = 1; dir < 4; ++dir) {
            averageL += averagePlus[dir];
            sigL += sigmaPlus[dir];
            maxL += maxPlus[dir];
        }
        averageL /= 3;
        sigL /= 3;
        maxL /= 3;

        /*
         * finalContAllL() divides by log(rapX); an exact rapX of one (the
         * degenerate mean + sigmafin * sigma == MaxP level) is singular on the
         * host too. Fall back before any image write.
         */
        if (maxL > 0.f && averageL + sigmafin * sigL == maxL) {
            return 2;
        }

        float* entry = final_stats +
                       static_cast<std::size_t>(level) *
                           RT_LAB_WAVELET_FINAL_STATS_ENTRIES;
        entry[0] = maxL;
        entry[1] = averageL;
        entry[2] = sigL;
    }
    return 0;
}

int LabWaveletContext::computeMad(const std::uint32_t* histogram,
                                  uint32_t levels, uint32_t plane)
{
    const uint32_t planes = wavelet_ab_ ? 3u : 1u;
    const uint32_t stride = static_cast<uint32_t>(wavelet_levels_);
    if (!wavelet_active_ || !wavelet_denoise_ || histogram == nullptr ||
        plane >= planes || levels < 1 || levels > stride ||
        stride > RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) {
        return 1;
    }

    const std::size_t denoise_offset =
        RT_LAB_WAVELET_WAVELET_PARAMS +
        static_cast<std::size_t>(planes) *
            static_cast<std::size_t>(stride) *
            RT_LAB_WAVELET_MEA_ENTRIES +
        (wavelet_edge_
             ? 1 + static_cast<std::size_t>(stride) *
                       RT_LAB_WAVELET_EDGE_ENTRIES +
                   RT_LAB_WAVELET_CURVE_ENTRIES
             : 0) +
        (wavelet_final_
             ? 1 + static_cast<std::size_t>(stride) *
                       RT_LAB_WAVELET_FINAL_STATS_ENTRIES +
                   RT_LAB_WAVELET_CURVE_ENTRIES +
                   RT_LAB_WAVELET_FINAL_LUT_ENTRIES
             : 0) +
        (wavelet_toning_
             ? RT_LAB_WAVELET_TONING_HEADER +
                   2 * static_cast<std::size_t>(stride) *
                       RT_LAB_WAVELET_TONING_MEA_ENTRIES
             : 0);
    float* tail = wavelet_params_.data() + denoise_offset +
                  RT_LAB_WAVELET_DENOISE_HEADER;
    const std::uint32_t samples =
        static_cast<std::uint32_t>(((width_ + 1) / 2) * ((height_ + 1) / 2));

    for (uint32_t level = 0; level < levels; ++level) {
        for (uint32_t dir = 0; dir < 3; ++dir) {
            const std::uint32_t* band =
                histogram +
                (static_cast<std::size_t>(level) * 3 + dir) * 32768u;
            const float mad = labWaveletMadFromHistogram(band, samples);
            tail[(static_cast<std::size_t>(plane) * stride + level) *
                     RT_LAB_WAVELET_DENOISE_MAD_ENTRIES +
                 dir] = SQR(mad);
        }
    }
    return 0;
}

int LabWaveletContext::prepare(RtStageBuffer& lab)
{
    const bool need_masks =
        skinprotect_ != 0.f ||
        (wavelet_chroma_ && wavelet_skinprotect_ != 0.f);
    if (!need_masks) {
        return 0;
    }
    if (lab.width != width_ || lab.height != height_ ||
        lab.format != RT_STAGE_F32 || lab.planes != 3) {
        return 1;
    }

    LabImage image(width_, height_);
    copy_to_host(lab, image);

    /* The block's gamutlab pre-pass: dirpyrequalizer() runs it only when the
     * equaliser is enabled with both gamutlab and its own skin protection. */
    const bool repaired = levels_ > 0 && gamutlab_ && skinprotect_ != 0.f;
    if (repaired) {
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

    if (repaired) {
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
