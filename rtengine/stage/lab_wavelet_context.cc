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

int compute_refine_thunk(void* self, const float* pre_stats,
                         const float* post_stats, uint32_t levels)
{
    return static_cast<LabWaveletContext*>(self)->computeRefine(
        pre_stats, post_stats, levels);
}

int compute_softend_thunk(void* self, const float* stats_ll,
                          const float* stats_dst, uint32_t levels)
{
    return static_cast<LabWaveletContext*>(self)->computeSoftend(
        stats_ll, stats_dst, levels);
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
 * The CLmethod selection and the clarity arm, decoded from the wavelet
 * parameters exactly as ip_wavelet()'s finalContAllL()/ContAllAB() and the
 * export block's clarity sequence decode them. The admission predicate, the
 * host context builder and the descriptor all read this one structure, so a
 * refused value can never reach the launcher.
 */
struct WaveletClarity
{
    bool select = false;
    std::uint32_t cl_method = RT_LAB_WAVELET_CL_ALL;
    std::uint32_t back_method = RT_LAB_WAVELET_BACK_GREY;
    std::uint32_t choice_level = LAB_WAVELET_DEFAULT_CHOICE_LEVEL;
    std::uint32_t choice_dir = RT_LAB_WAVELET_DIR_ALL;
    std::uint32_t dir1 = 2;
    std::uint32_t dir2 = 3;
    std::uint32_t max_ilev = 0;

    bool active = false;
    bool merge = false;
    float merge_l0 = 0.f;
    float merge_l = 0.f;
    float merge_c0 = 0.f;
    float merge_c = 0.f;
};

bool decodeWaveletClarity(const procparams::WaveletParams& wavelet,
                          WaveletClarity& out)
{
    out = WaveletClarity{};

    if (wavelet.CLmethod == "one") {
        out.cl_method = RT_LAB_WAVELET_CL_ONE;
    } else if (wavelet.CLmethod == "inf") {
        out.cl_method = RT_LAB_WAVELET_CL_INF;
    } else if (wavelet.CLmethod == "sup") {
        out.cl_method = RT_LAB_WAVELET_CL_SUP;
    } else if (wavelet.CLmethod == "all") {
        out.cl_method = RT_LAB_WAVELET_CL_ALL;
    } else {
        return false;
    }
    out.select = out.cl_method != RT_LAB_WAVELET_CL_ALL;

    /* The clarity sequence is the block's exact gate. */
    out.active = wavelet.enabled &&
                 (wavelet.ushamethod == "sharp" ||
                  wavelet.ushamethod == "clari") &&
                 wavelet.expclari && out.select;
    if (!out.select) {
        return true;
    }

    if (wavelet.Backmethod == "black") {
        out.back_method = RT_LAB_WAVELET_BACK_BLACK;
    } else if (wavelet.Backmethod == "grey") {
        out.back_method = RT_LAB_WAVELET_BACK_GREY;
    } else if (wavelet.Backmethod == "resid") {
        out.back_method = RT_LAB_WAVELET_BACK_RESID;
    } else {
        return false;
    }

    if (wavelet.Dirmethod == "all") {
        out.choice_dir = RT_LAB_WAVELET_DIR_ALL;
    } else if (wavelet.Dirmethod == "one") {
        out.choice_dir = RT_LAB_WAVELET_DIR_ONE;
    } else if (wavelet.Dirmethod == "two") {
        out.choice_dir = RT_LAB_WAVELET_DIR_TWO;
    } else if (wavelet.Dirmethod == "thr") {
        out.choice_dir = RT_LAB_WAVELET_DIR_THR;
    } else {
        return false;
    }
    out.dir1 = out.choice_dir == RT_LAB_WAVELET_DIR_TWO ? 1 : 2;
    out.dir2 = out.choice_dir == RT_LAB_WAVELET_DIR_THR ? 1 : 3;

    if (wavelet.Lmethod < LAB_WAVELET_MIN_LMETHOD ||
        wavelet.Lmethod > LAB_WAVELET_MAX_LMETHOD) {
        return false;
    }
    out.choice_level =
        wavelet.Lmethod == 0
            ? LAB_WAVELET_DEFAULT_CHOICE_LEVEL
            : static_cast<std::uint32_t>(wavelet.Lmethod - 1);
    out.max_ilev = static_cast<std::uint32_t>(wavelet.thres);

    if (!out.active) {
        return true;
    }

    if (!std::isfinite(wavelet.mergeL) ||
        wavelet.mergeL < LAB_WAVELET_MIN_MERGE ||
        wavelet.mergeL > LAB_WAVELET_MAX_MERGE ||
        !std::isfinite(wavelet.mergeC) ||
        wavelet.mergeC < LAB_WAVELET_MIN_MERGE ||
        wavelet.mergeC > LAB_WAVELET_MAX_MERGE) {
        return false;
    }

    /*
     * The merge's three arms. The block scales the sliders by 100 and then
     * negates the luminance factor by 1.5 for the black background arms; every
     * other combination collapses the merge to the clamped first pass.
     */
    const float mL = static_cast<float>(wavelet.mergeL / 100.f);
    const float mC = static_cast<float>(wavelet.mergeC / 100.f);
    if ((out.cl_method == RT_LAB_WAVELET_CL_ONE ||
         out.cl_method == RT_LAB_WAVELET_CL_INF) &&
        out.back_method == RT_LAB_WAVELET_BACK_BLACK) {
        out.merge = true;
        out.merge_l0 = 0.f;
        out.merge_l = -1.5f * mL;
        out.merge_c0 = 0.f;
        out.merge_c = -mC;
    } else if (out.cl_method == RT_LAB_WAVELET_CL_SUP &&
               out.back_method == RT_LAB_WAVELET_BACK_RESID) {
        out.merge = true;
        out.merge_l0 = mL;
        out.merge_l = mL;
        out.merge_c0 = mC;
        out.merge_c = mC;
    }
    return true;
}

/*
 * The clarity arm's guided soft radius, decoded from the export block's
 * `softrad` sequence. guidedfilter.cc derives its subsampling, radius and
 * scales from the image extent; the block derives epsilon and the blur radius
 * from the slider. Every derived value is computed here with the block's own
 * float arithmetic so the device never re-derives one and cannot drift.
 */
struct WaveletSoftRadius
{
    bool active = false;
    float epsilon = 0.f;
    int subsampling = 1;
    int width = 0;
    int height = 0;
    int box_rad = 0;
    float scale_dx = 1.f;
    float scale_dy = 1.f;
    float scale_ux = 1.f;
    float scale_uy = 1.f;
};

/* guidedfilter.cc's calculate_subsampling(), copied unchanged. */
int calculateGuidedSubsampling(int w, int h, int r)
{
    if (r == 1) {
        return 1;
    }
    if (std::max(w, h) <= 600) {
        return 1;
    }
    for (int s = 5; s > 0; --s) {
        if (r % s == 0) {
            return s;
        }
    }
    return LIM(r / 2, 2, 4);
}

/*
 * The block reads `softrad` only inside the clarity sequence. When the clarity
 * arm does not run the slider has no effect, so the admission predicate treats
 * a non-zero value as a residual there rather than silently ignoring it.
 */
bool decodeWaveletSoftRadius(const procparams::WaveletParams& wavelet,
                             int width, int height,
                             WaveletSoftRadius& out)
{
    out = WaveletSoftRadius{};
    if (wavelet.softrad == 0.0) {
        return true;
    }
    if (!std::isfinite(wavelet.softrad) ||
        wavelet.softrad < LAB_WAVELET_MIN_SOFT_RAD ||
        wavelet.softrad > LAB_WAVELET_MAX_SOFT_RAD) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    /* The block's blur, kept in the block's own types: the slider is a
     * double, so the literal products round as doubles before the float
     * assignment and the int truncation. */
    const float blur = 10.f / 1 * (0.5f + 0.8f * wavelet.softrad);
    const int radius = static_cast<int>(blur);
    const int subsampling = calculateGuidedSubsampling(width, height, radius);
    const int w = width / subsampling;
    const int h = height / subsampling;
    if (w < 1 || h < 1) {
        return false;
    }

    /*
     * f_mean()'s radius: the block truncates float(r) / subsampling to int,
     * clamps with LIM(rad, 0, (min(w, h) - 1) / 2 - 1) and boxblur() then
     * clamps with min(radius, W - 1, H - 1).
     */
    const float r1 = static_cast<float>(radius) / subsampling;
    int rad = static_cast<int>(r1);
    rad = std::max(0, std::min(rad, (std::min(w, h) - 1) / 2 - 1));
    rad = std::min(std::min(rad, w - 1), h - 1);

    /* epsil = aepsil * softrad + bepsil, in double as the block computes it. */
    const double epsilmax = 0.0001;
    const double epsilmin = 0.00001;
    const double aepsil = (epsilmax - epsilmin) / 100.f;
    const double epsil = aepsil * wavelet.softrad + epsilmin;

    out.active = true;
    out.epsilon = static_cast<float>(epsil);
    out.subsampling = subsampling;
    out.width = w;
    out.height = h;
    out.box_rad = rad;
    /* rescaleBilinear()'s float(Ws) / float(Wd), both directions. */
    out.scale_dx = static_cast<float>(width) / static_cast<float>(w);
    out.scale_dy = static_cast<float>(height) / static_cast<float>(h);
    out.scale_ux = static_cast<float>(w) / static_cast<float>(width);
    out.scale_uy = static_cast<float>(h) / static_cast<float>(height);
    return true;
}


/*
 * The final panel's guided soft radius, decoded from the export block's
 * `softradend` sequence. The whole block is ported: the guided filter, the
 * coefficient-domain second pass (`Thrend > 0`) and the hue modulation (a
 * non-identity `wavguidcurve`). The block derives the radius from the slider
 * and the image extent; guidedfilter.cc derives the subsampling, the box
 * radius and the bilinear scales from that radius. Every derived value is
 * computed here with the block's own arithmetic.
 */
struct WaveletSoftEnd
{
    bool active = false;
    float blend = 0.f;
    float epsilon = 0.f;
    int subsampling = 1;
    int width = 0;
    int height = 0;
    int box_rad = 0;
    float scale_dx = 1.f;
    float scale_dy = 1.f;
    float scale_ux = 1.f;
    float scale_uy = 1.f;
};

bool decodeWaveletSoftEnd(const procparams::WaveletParams& wavelet,
                          int width, int height, WaveletSoftEnd& out)
{
    out = WaveletSoftEnd{};
    if (wavelet.softradend == 0.0) {
        return true;
    }
    if (!std::isfinite(wavelet.softradend) ||
        wavelet.softradend < LAB_WAVELET_MIN_SOFT_END ||
        wavelet.softradend > LAB_WAVELET_MAX_SOFT_END) {
        return false;
    }
    /* The block gates the whole sequence on `cp.finena`. */
    if (!wavelet.expfinal) {
        return false;
    }
    if (!std::isfinite(wavelet.thrend) ||
        wavelet.thrend < LAB_WAVELET_MIN_WAVELET_THREND ||
        wavelet.thrend > LAB_WAVELET_MAX_WAVELET_THREND) {
        return false;
    }
    if (!std::isfinite(wavelet.strend) ||
        wavelet.strend < LAB_WAVELET_MIN_STREND ||
        wavelet.strend > LAB_WAVELET_MAX_STREND ||
        wavelet.detend < LAB_WAVELET_MIN_DETEND ||
        wavelet.detend > LAB_WAVELET_MAX_DETEND) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    /*
     * ip_wavelet(): `r = max(int(softradend / skip), 1)`. The node is
     * full-tile only, so the call site's skip is one; assert rather than
     * silently deriving a different radius.
     */
    constexpr int skip = 1;
    static_assert(skip == 1, "the lab.wavelet node is full-tile only");
    const float guid = static_cast<float>(wavelet.softradend);
    const int radius = std::max(static_cast<int>(guid / skip), 1);

    const int subsampling =
        calculateGuidedSubsampling(width, height, radius);
    const int w = width / subsampling;
    const int h = height / subsampling;
    if (w < 1 || h < 1) {
        return false;
    }

    /* f_mean()'s radius: the block truncates float(r) / subsampling to int,
     * clamps with LIM(rad, 0, (min(w, h) - 1) / 2 - 1) and boxblur() then
     * clamps with min(radius, W - 1, H - 1). */
    const float r1 = static_cast<float>(radius) / subsampling;
    int rad = static_cast<int>(r1);
    rad = std::max(0, std::min(rad, (std::min(w, h) - 1) / 2 - 1));
    rad = std::min(std::min(rad, w - 1), h - 1);

    /* blend = LIM01(float(strend) / 100.f), epsilon = 0.001f * 2^-detend. */
    const float blend = std::min(
        1.f, std::max(0.f, static_cast<float>(wavelet.strend) / 100.f));
    const float epsilon = static_cast<float>(
        0.001f * std::pow(2.0, -static_cast<double>(wavelet.detend)));

    out.active = true;
    out.blend = blend;
    out.epsilon = epsilon;
    out.subsampling = subsampling;
    out.width = w;
    out.height = h;
    out.box_rad = rad;
    out.scale_dx = static_cast<float>(width) / static_cast<float>(w);
    out.scale_dy = static_cast<float>(height) / static_cast<float>(h);
    out.scale_ux = static_cast<float>(w) / static_cast<float>(width);
    out.scale_uy = static_cast<float>(h) / static_cast<float>(height);
    return true;
}

/*
 * A FlatCurve's faceted polyline in the tail encoding the refinement uses:
 * the knot count, then the knot abscissae, the ordinates and the slopes, each
 * array `count` long except the slopes, which stop one short. The device walks
 * the same binary search FlatCurve::getVal() walks. Returns false when the
 * polyline is empty or longer than the tail's bound.
 */
bool encodeFlatCurvePolyline(const FlatCurve& curve,
                             std::vector<float>& out)
{
    const std::vector<double>& px = curve.polylineX();
    const std::vector<double>& py = curve.polylineY();
    const std::vector<double>& ps = curve.polylineSlope();
    out.clear();
    if (px.empty() || px.size() > RT_LAB_WAVELET_REFINE_POLY_MAX ||
        py.size() != px.size() || ps.size() + 1 != px.size()) {
        return false;
    }
    const std::size_t count = px.size();
    /* count, count x, count y and count-1 slopes. */
    out.resize(3 * count);
    out[0] = static_cast<float>(count);
    for (std::size_t index = 0; index < count; ++index) {
        out[1 + index] = static_cast<float>(px[index]);
        out[1 + count + index] = static_cast<float>(py[index]);
        if (index + 1 < count) {
            out[1 + 2 * count + index] =
                static_cast<float>(ps[index]);
        }
    }
    return true;
}


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
 * block forces when Expnoise selects six levels. The plain slider set, the
 * "sli" Thrden refinement, the "cur" denoise-curve refinement, the expert
 * level pairing and the BiShrink pair (`quaMethod == "agre"`) are ported; the
 * hue modulation stays a host residual, and a user chroma denoise or noise
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
    /*
     * "cons" runs WaveletDenoiseAllL()/WaveletDenoiseAllAB() once per plane;
     * "agre" runs WaveletDenoiseAll_BiShrinkL()/AB() first and then those same
     * passes, so the pair reuses `vari`, `ballum` and the squared MAD tail.
     * An unknown method leaves cp.quamet uninitialised in the block.
     */
    if (wavelet.quamethod != "cons" && wavelet.quamethod != "agre") {
        return false;
    }
    /*
     * The user chroma strength is inert for this arm: the block's `variC` is
     * dead at the forced `local == 1`, and only `cp.chromco`'s `> 30` test
     * changes the chroma-magnitude ramp. Balance-chroma would move `variC`
     * and colour-by-noise changes the source, so both stay refused.
     */
    if (!std::isfinite(wavelet.chromfi) ||
        wavelet.chromfi < LAB_WAVELET_MIN_CHROMA_STRENGTH ||
        wavelet.chromfi > LAB_WAVELET_MAX_CHROMA_STRENGTH ||
        !std::isfinite(wavelet.chromco) ||
        wavelet.chromco < LAB_WAVELET_MIN_CHROMA_STRENGTH ||
        wavelet.chromco > LAB_WAVELET_MAX_CHROMA_STRENGTH ||
        wavelet.balchrom != 0.0 || wavelet.cbenab) {
        return false;
    }
    /*
     * "sli" is the slider arm, refined by the Thrden wavlow curve when the
     * slider is non-zero; "cur" adds the wavdenoise refinement. Both feed the
     * standard pairing (levref four) or the expert pairing (levref six); an
     * unknown method is refused because the block leaves `cp.complex`
     * uninitialised.
     */
    const bool expert = wavelet.complexmethod == "expert";
    if ((wavelet.slimethod != "sli" && wavelet.slimethod != "cur") ||
        (wavelet.complexmethod != "normal" && !expert)) {
        return false;
    }
    if (!std::isfinite(wavelet.thrden) ||
        wavelet.thrden < LAB_WAVELET_MIN_THRDEN ||
        wavelet.thrden > LAB_WAVELET_MAX_THRDEN) {
        return false;
    }
    /*
     * The refinement reads cp.mixmet and cp.sigmm whichever curve feeds it.
     * An unknown method leaves the block's mixmet uninitialised, and sigma
     * divides the curve abscissa, so both must be inside their ranges.
     */
    if (wavelet.mixmethod != "nois" && wavelet.mixmethod != "mix" &&
        wavelet.mixmethod != "mix7" && wavelet.mixmethod != "den") {
        return false;
    }
    if (!std::isfinite(wavelet.sigm) ||
        wavelet.sigm < LAB_WAVELET_MIN_WAVELET_SIGM ||
        wavelet.sigm > LAB_WAVELET_MAX_WAVELET_SIGM) {
        return false;
    }
    /*
     * The expert arm's three extra inputs. `Levelsigm` feeds siglh per level
     * through the same divisions as Sigm, `Limden` moves the wavmed curve's
     * breaking point and `Leveldenoise`'s lower value is the wavhigh
     * curve's threshold. All three must be inside their widget ranges.
     */
    if (expert) {
        const double sigmm14 = wavelet.levelsigm.getTop();
        const double sigmm56 = wavelet.levelsigm.getBottom();
        if (!std::isfinite(sigmm14) || !std::isfinite(sigmm56) ||
            sigmm14 < LAB_WAVELET_MIN_LEVELSIGM ||
            sigmm14 > LAB_WAVELET_MAX_LEVELSIGM ||
            sigmm56 < LAB_WAVELET_MIN_LEVELSIGM ||
            sigmm56 > LAB_WAVELET_MAX_LEVELSIGM) {
            return false;
        }
        if (!std::isfinite(wavelet.limden) ||
            wavelet.limden < LAB_WAVELET_MIN_LIMDEN ||
            wavelet.limden > LAB_WAVELET_MAX_LIMDEN) {
            return false;
        }
        if (!std::isfinite(wavelet.leveldenoise.getBottom()) ||
            wavelet.leveldenoise.getBottom() < 0.0 ||
            wavelet.leveldenoise.getBottom() > 100.0) {
            return false;
        }
        /*
         * cp.denoicurvh is the only thing `wavdenoiseh` feeds: it re-arms the
         * block's Evaluate2 gate at ipwavelet.cc:1219 (the levwavL restore at
         * 1155-1160 is commented out), which this arm's curve or Thrden
         * already satisfies. The block sets denoicurvh when any of the first
         * 500 WavCurve entries differs from one; the device does not model
         * the curve, so refuse that state rather than rely on the
         * coincidence.
         */
        WavCurve high_curve;
        high_curve.Set(wavelet.wavdenoiseh);
        if (static_cast<bool>(high_curve)) {
            for (int index = 0; index < 500; ++index) {
                if (high_curve[index] != 1.0f) {
                    return false;
                }
            }
        }
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

namespace
{

int labWaveletLevels(const procparams::ProcParams& params,
                     int width, int height, bool force_all_method,
                     bool keep_untrimmed)
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
     * ip_wavelet() keeps the unraised count in `levwav` and writes it back
     * into `levwavL` when Thrend is positive, so the restore target is this
     * base (after the two caps) and not L's raised count.
     */
    int base = levwav;
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
        base = 10;
    }
    levwav = std::min(maxlevelcrop, levwav);
    base = std::min(maxlevelcrop, base);
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
    base = std::min(maxlev2, base);

    /*
     * The trailing-zero trim in ip_wavelet(): a level whose multiplier is
     * exactly zero is dead when nothing else in the block needs it. Thrend is
     * read unconditionally afterwards and, when positive, restores the
     * unraised, untrimmed count so the residual tone mapping keeps its levels;
     * delta and the highlight/shadow kH then use that larger maxlvl.
     */
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
                      (force_all_method || wavelet.CLmethod == "all");
    if (dead && !keep_untrimmed) {
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
        levwav = base;
    }
    return levwav > 0 ? levwav : 0;
}

} // namespace

int labWaveletTransformLevels(const procparams::ProcParams& params,
                              int width, int height, bool force_all_method)
{
    return labWaveletLevels(params, width, height, force_all_method, false);
}

int labWaveletUntrimmedLevels(const procparams::ProcParams& params,
                              int width, int height)
{
    return labWaveletLevels(params, width, height, false, true);
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
        wavelet.expbl || wavelet.expresid) {
        return false;
    }
    /*
     * The standard denoise arm. ip_wavelet() forces six levels and, at six
     * levels, forces the chroma denoise strength to 0.01 when the profile's
     * own strength is zero, so the host always denoises the a and b bands as
     * well. The admitted set is the plain slider arm, its "cur" and
     * "sli"+Thrden refinements, and the expert level pairing for both.
     */
    if (denoise && !denoiseAdmitted(wavelet, width, height)) {
        return false;
    }
    /*
     * The expert pairing's refinement reads levels 0..5; a decomposition
     * shorter than that would index past the last band. The block's own
     * Expnoise raise makes L seven, so this only fires on a degenerate grid.
     */
    if (denoise && wavelet.complexmethod == "expert" &&
        (wavelet.slimethod == "cur" ||
         (wavelet.slimethod == "sli" && wavelet.thrden > 0.0)) &&
        labWaveletTransformLevels(params, width, height) <
            static_cast<int>(RT_LAB_WAVELET_REFINE_LEVELS_EXPERT)) {
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
     * The edge panel runs the plain arm and, with Medianlev, the Lipschitz
     * control: calckoe() per level and direction, the neighbour smoothing
     * that NPMethod selects with Lipst, and the direction-combination
     * conversion in the block's order. `EDmethod` is displayed by the GUI but
     * the engine hard-codes EDmet == 2 (ipwavelet.cc:419), so its "SL"
     * threshold-adjuster branch is unreachable and both selections produce
     * the same EDmet == 2 block; unknown selections stay refused.
     */
    if (edge_active) {
        if (settings == nullptr) {
            return false;
        }
        if (wavelet.NPmethod != "none" && wavelet.NPmethod != "low" &&
            wavelet.NPmethod != "high") {
            return false;
        }
        if (wavelet.EDmethod != "CU" && wavelet.EDmethod != "SL") {
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
        /* The Lipschitz control's sliders, read only by the Medianlev arm. */
        if (wavelet.medianlev &&
            (wavelet.edgedetect < LAB_WAVELET_MIN_WAVELET_EDGEDETECT ||
             wavelet.edgedetect > LAB_WAVELET_MAX_WAVELET_EDGEDETECT ||
             wavelet.edgedetectthr <
                 LAB_WAVELET_MIN_WAVELET_EDGEDETECTTHR ||
             wavelet.edgedetectthr >
                 LAB_WAVELET_MAX_WAVELET_EDGEDETECTTHR ||
             wavelet.edgedetectthr2 <
                 LAB_WAVELET_MIN_WAVELET_EDGEDETECTTHR2 ||
             wavelet.edgedetectthr2 >
                 LAB_WAVELET_MAX_WAVELET_EDGEDETECTTHR2)) {
            return false;
        }
        /* Lipst selects the influence sliders, so their range matters only
         * when the control reads them. */
        if (wavelet.medianlev && wavelet.lipst &&
            (wavelet.edgesensi < LAB_WAVELET_MIN_WAVELET_EDGESENSI ||
             wavelet.edgesensi > LAB_WAVELET_MAX_WAVELET_EDGESENSI ||
             wavelet.edgeampli < LAB_WAVELET_MIN_WAVELET_EDGEAMPLI ||
             wavelet.edgeampli > LAB_WAVELET_MAX_WAVELET_EDGEAMPLI)) {
            return false;
        }
    }
    /*
     * The final panel's curve arm needs only the two curve controls. Its
     * sigmafin must be inside the GUI slider range because the block
     * divides by it. The balance-chroma method is admitted separately below
     * and shares the same `expfinal` gate.
     */
    if (final_curves &&
        (!std::isfinite(wavelet.sigmafin) ||
         wavelet.sigmafin < LAB_WAVELET_MIN_WAVELET_SIGMAFIN ||
         wavelet.sigmafin > LAB_WAVELET_MAX_WAVELET_SIGMAFIN)) {
        return false;
    }
    /*
     * ContAllL()/ContAllAB()'s balance-chroma arm. The L arm runs with
     * `expfinal` (cp.finena) and the a/b arm with `TMr` (cp.diag); both need
     * a known method (1 for "sli", 2 for "cur"). "cur" additionally reads a
     * non-identity WavOpacityCurveW (cp.opaW), otherwise the block skips it.
     * The arm multiplies the bands by a per-pixel factor derived from the
     * original (L) or reconstructed (a/b) luminance, so the node carries one
     * level count only when both planes keep the same count; the block
     * disables the a/b trailing-zero trim while TMr is set, so a profile that
     * would trim L alone stays on the host.
     */
    if (wavelet.BAmethod != "none") {
        if (wavelet.BAmethod != "sli" && wavelet.BAmethod != "cur") {
            return false;
        }
        if (!wavelet.expfinal && !wavelet.tmr) {
            return false;
        }
        if (wavelet.balance < LAB_WAVELET_MIN_WAVELET_BALANCE ||
            wavelet.balance > LAB_WAVELET_MAX_WAVELET_BALANCE ||
            wavelet.iter < LAB_WAVELET_MIN_WAVELET_ITER ||
            wavelet.iter > LAB_WAVELET_MAX_WAVELET_ITER ||
            !std::isfinite(wavelet.sigmadir) ||
            wavelet.sigmadir < LAB_WAVELET_MIN_WAVELET_SIGMADIR ||
            wavelet.sigmadir > LAB_WAVELET_MAX_WAVELET_SIGMADIR) {
            return false;
        }
        if (wavelet.BAmethod == "cur") {
            WavOpacityCurveW opacity;
            opacity.Set(wavelet.opacityCurveW);
            if (!static_cast<bool>(opacity)) {
                return false;
            }
        }
        if (labWaveletTransformLevels(params, width, height) !=
            labWaveletUntrimmedLevels(params, width, height)) {
            return false;
        }
    } else if (wavelet.tmr) {
        /* With no method the block's BA arm never runs, but cp.diag still
         * disables the a/b trim; that is a residual. */
        return false;
    }
    if (wavelet.Tilesmethod != "full") {
        return false;
    }
    /*
     * CLmethod's level selection runs on the device for every admitted panel
     * set; the clarity arm's two-pass sequence additionally needs its own
     * decoded merge. Unknown CLmethod, Backmethod or Dirmethod values and an
     * out-of-range Lmethod or merge slider are residuals.
     */
    WaveletClarity clarity;
    if (!decodeWaveletClarity(wavelet, clarity)) {
        return false;
    }
    /*
     * The clarity arm's first pass runs with CLmethod forced to "all", so
     * ip_wavelet()'s a/b level trim runs. With no a/b-modifying panel the
     * device skips the a/b planes, which matches the block only when the trim
     * actually reaches zero levels. These are the values that would stop it
     * short of zero; a profile that sets one stays on the host.
     */
    if (clarity.active &&
        !(chroma || edge_active || final_curves || toning || denoise)) {
        bool ch_zero = true;
        for (int level = 0; level < 9; ++level) {
            ch_zero = ch_zero && wavelet.ch[level] == 0;
        }
        WavOpacityCurveRG opacity_rg;
        opacity_rg.Set(wavelet.opacityCurveRG);
        bool opacity_zero = true;
        if (static_cast<bool>(opacity_rg)) {
            static constexpr int samples[9] = {
                0, 62, 125, 187, 250, 312, 375, 438, 500};
            for (int level = 0; level < 9; ++level) {
                opacity_zero =
                    opacity_zero &&
                    200.f * (opacity_rg[samples[level]] - 0.5f) == 0.f;
            }
        }
        if (wavelet.cbenab || !ch_zero || !opacity_zero ||
            (wavelet.CHmethod == "link" && wavelet.chro != 0) ||
            wavelet.contrast != 0 || wavelet.rescon != 0 ||
            wavelet.resconH != 0 || wavelet.reschro != 0 ||
            wavelet.resblur != 0 || wavelet.resblurc != 0 ||
            wavelet.tmrs != 0.0) {
            return false;
        }
    }
    /*
     * The clarity arm's guided soft radius. The clarity arm reads `softrad`
     * only inside its two-pass sequence, so a value set while the arm is off
     * is refused rather than silently ignored, and an admitted value must be
     * inside the 0..100 slider range and leave a non-empty subsampled
     * guided-filter grid.
     */
    /*
     * The final panel's guided soft radius (`softradend`). It runs only with
     * the final panel enabled; the block's coefficient-domain second pass
     * (Thrend > 0) and its hue modulation (non-identity wavguidcurve) are both
     * ported and admitted alongside it. The clarity arm and the clarity soft
     * radius cannot run alongside it, and a value outside the 0..1000 slider
     * range or a derived grid with no pixels is refused.
     */
    if (wavelet.softradend != 0.0) {
        WaveletSoftEnd softend;
        if (!decodeWaveletSoftEnd(wavelet, width, height, softend) ||
            !softend.active || !final_curves || clarity.active ||
            wavelet.softrad != 0.0) {
            return false;
        }
    }
    if (wavelet.softrad != 0.0) {
        if (!clarity.active) {
            return false;
        }
        WaveletSoftRadius soft;
        if (!decodeWaveletSoftRadius(wavelet, width, height, soft) ||
            !soft.active) {
            return false;
        }
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
    context.wavelet.bishrink = wavelet_bishrink_ ? 1u : 0u;
    context.wavelet.softend = softend_active_ ? 1u : 0u;
    context.wavelet.softend_thrend = softend_thrend_ ? 1u : 0u;
    context.wavelet.softend_thrend_poly = static_cast<uint32_t>(
        softend_thrend_ ? softend_thrend_curve_.size() / 3 : 0);
    context.wavelet.softend_hue_poly = static_cast<uint32_t>(
        softend_hue_ ? softend_hue_curve_.size() / 3 : 0);
    context.wavelet.compute_softend = &compute_softend_thunk;
    context.wavelet.ba = wavelet_ba_ ? 1u : 0u;
    context.wavelet.ba_l = wavelet_ba_l_ ? 1u : 0u;
    context.wavelet.ba_ab = wavelet_ba_ab_ ? 1u : 0u;
    context.wavelet.refine = wavelet_refine_ ? 1u : 0u;
    context.wavelet.refine_poly = static_cast<uint32_t>(
        wavelet_refine_poly_x_.size());
    context.wavelet.refine_levels =
        wavelet_refine_ ? wavelet_refine_levels_ : 0u;
    context.wavelet.refine_expert =
        (wavelet_refine_ && wavelet_refine_expert_) ? 1u : 0u;
    context.wavelet.refine_med_poly = static_cast<uint32_t>(
        wavelet_refine_med_x_.size());
    context.wavelet.refine_high_poly = static_cast<uint32_t>(
        wavelet_refine_high_x_.size());
    context.wavelet.final_refresh = wavelet_final_refresh_ ? 1u : 0u;
    context.wavelet.compute_mad = &compute_mad_thunk;
    context.wavelet.compute_refine = &compute_refine_thunk;
    context.wavelet.entries = static_cast<uint32_t>(
        wavelet_params_.size());
    context.wavelet.params =
        wavelet_params_.empty() ? nullptr : wavelet_params_.data();
    context.wavelet.compute_mea = &compute_mea_thunk;
    context.wavelet.compute_final = &compute_final_thunk;
    context.selection.active = selection_active_ ? 1u : 0u;
    context.selection.method = cl_method_;
    context.selection.back_method = back_method_;
    context.selection.choice_level = choice_level_;
    context.selection.choice_dir = choice_dir_;
    context.selection.dir1 = dir1_;
    context.selection.dir2 = dir2_;
    context.selection.max_ilev = max_ilev_;
    context.clarity.active = clarity_active_ ? 1u : 0u;
    context.clarity.merge = clarity_merge_ ? 1u : 0u;
    context.clarity.levels_first =
        static_cast<uint32_t>(clarity_levels_first_);
    context.clarity.merge_l0 = clarity_l0_;
    context.clarity.merge_l = clarity_l_;
    context.clarity.merge_c0 = clarity_c0_;
    context.clarity.merge_c = clarity_c_;
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

    /*
     * Decode CLmethod's selection and the clarity arm once. A value the block
     * would leave to an uninitialised cp field is refused here, before the
     * launcher sees the descriptor.
     */
    WaveletClarity clarity;
    if (!decodeWaveletClarity(params_.wavelet, clarity)) {
        return 5;
    }
    selection_active_ = clarity.select;
    cl_method_ = clarity.cl_method;
    back_method_ = clarity.back_method;
    choice_level_ = clarity.choice_level;
    choice_dir_ = clarity.choice_dir;
    dir1_ = clarity.dir1;
    dir2_ = clarity.dir2;
    max_ilev_ = clarity.max_ilev;
    clarity_active_ = clarity.active;
    clarity_merge_ = clarity.merge;
    clarity_l0_ = clarity.merge_l0;
    clarity_l_ = clarity.merge_l;
    clarity_c0_ = clarity.merge_c0;
    clarity_c_ = clarity.merge_c;

    /*
     * The guided soft radius. It only runs inside the clarity sequence and
     * only affects the merged result when the merge reads the second pass, so
     * it is armed exactly when both hold. `decodeWaveletSoftRadius()` repeats
     * the admission rules so a forced build can never run a partial guided
     * filter.
     */
    {
        WaveletSoftRadius soft;
        if (!decodeWaveletSoftRadius(params_.wavelet, width_, height_, soft) ||
            (soft.active && !clarity.active)) {
            return 5;
        }
        wavelet_soft_ = soft.active && clarity.active && clarity.merge;
        soft_epsilon_ = soft.epsilon;
        soft_w_ = soft.width;
        soft_h_ = soft.height;
        soft_box_rad_ = soft.box_rad;
        soft_scale_dx_ = soft.scale_dx;
        soft_scale_dy_ = soft.scale_dy;
        soft_scale_ux_ = soft.scale_ux;
        soft_scale_uy_ = soft.scale_uy;
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
     * The edge arm: the plain sliders plus, with Medianlev, the Lipschitz
     * control. `edgval == 0` leaves the host's edge block untouched, so the
     * device pass is only armed when a strength is set. The sub-mode guards
     * repeat the admission rules so a forced build can never run a partial
     * edge pass.
     */
    wavelet_edge_ = params_.wavelet.enabled && params_.wavelet.expedge &&
                    params_.wavelet.edgval > 0 &&
                    (params_.wavelet.NPmethod == "none" ||
                     params_.wavelet.NPmethod == "low" ||
                     params_.wavelet.NPmethod == "high") &&
                    (params_.wavelet.EDmethod == "CU" ||
                     params_.wavelet.EDmethod == "SL") &&
                    (params_.wavelet.Medgreinf == "more" ||
                     params_.wavelet.Medgreinf == "none" ||
                     params_.wavelet.Medgreinf == "less") &&
                    settings != nullptr;
    /*
     * The final panel's curve arm. The admission rules repeat here so a
     * forced build can never run a partial final pass: the balance-chroma
     * method (when present) must be known, the residual tone mapping stays a
     * host residual and the curves outside their range are refused. The
     * balance-chroma arm runs inside this panel's coefficient pass, so the
     * two share the same `expfinal` gate.
     */
    wavelet_final_ = params_.wavelet.enabled && params_.wavelet.expfinal &&
                     (params_.wavelet.BAmethod == "none" ||
                      params_.wavelet.BAmethod == "sli" ||
                      params_.wavelet.BAmethod == "cur") &&
                     params_.wavelet.softrad == 0.0 &&
                     std::isfinite(params_.wavelet.sigmafin) &&
                     params_.wavelet.sigmafin >=
                         LAB_WAVELET_MIN_WAVELET_SIGMAFIN &&
                     params_.wavelet.sigmafin <=
                         LAB_WAVELET_MAX_WAVELET_SIGMAFIN;
    /*
     * The final panel's guided soft radius. It runs after the final pixel
     * curve, so it needs the final arm; the clarity arm and the clarity soft
     * radius are refused alongside it. `decodeWaveletSoftEnd()` repeats the
     * admission rules so a forced build can never run a partial pass.
     */
    {
        WaveletSoftEnd softend;
        if (!decodeWaveletSoftEnd(params_.wavelet, width_, height_, softend) ||
            (softend.active &&
             (!wavelet_final_ || clarity_active_ ||
              params_.wavelet.softrad != 0.0))) {
            return 5;
        }
        softend_active_ = softend.active;
        softend_blend_ = softend.blend;
        softend_epsilon_ = softend.epsilon;
        softend_w_ = softend.width;
        softend_h_ = softend.height;
        softend_box_rad_ = softend.box_rad;
        softend_scale_dx_ = softend.scale_dx;
        softend_scale_dy_ = softend.scale_dy;
        softend_scale_ux_ = softend.scale_ux;
        softend_scale_uy_ = softend.scale_uy;
        softend_thrend_ = false;
        softend_hue_ = false;
        softend_thrend_curve_.clear();
        softend_hue_curve_.clear();
        if (softend.active) {
            /*
             * The block's coefficient-domain second pass. Its wavguid curve is
             * a four-point FlatCurve whose breaking point and plateau follow
             * thrend, built with the block's own float arithmetic.
             */
            const float thrend =
                static_cast<float>(0.01f * params_.wavelet.thrend);
            if (thrend > 0.f) {
                float thr = 0.f;
                if (thrend < 0.02f) {
                    thr = 0.5f;
                } else if (thrend < 0.1f) {
                    thr = 0.2f;
                } else {
                    thr = 0.f;
                }
                const std::vector<double> points = {
                    static_cast<double>(FCT_MinMaxCPoints), 0.0, 1.0, 0.35,
                    0.35, static_cast<double>(thrend), 1.0, 0.35, 0.35,
                    static_cast<double>(thrend + 0.01f),
                    static_cast<double>(thr), 0.35, 0.35, 1.0,
                    static_cast<double>(thr), 0.35, 0.35};
                FlatCurve wavguid(points);
                if (!encodeFlatCurvePolyline(wavguid,
                                             softend_thrend_curve_)) {
                    return 5;
                }
                softend_thrend_ = true;
                softend_thrend_value_ = thrend;
            }
            /* The hue modulation: the block's own wavguidCurve. */
            const FlatCurve guid_curve(params_.wavelet.wavguidcurve);
            if (!guid_curve.isIdentity()) {
                if (!encodeFlatCurvePolyline(guid_curve,
                                             softend_hue_curve_)) {
                    return 5;
                }
                softend_hue_ = true;
            }
        }
    }
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
     * too. A CLmethod selection zeroes and repaints the a/b coefficients as
     * well, so it needs the planes even when no panel modifies them.
     */
    wavelet_denoise_ = denoiseAdmitted(params_.wavelet, width_, height_);
    wavelet_bishrink_ =
        wavelet_denoise_ && params_.wavelet.quamethod == "agre";
    {
        const procparams::WaveletParams& wavelet = params_.wavelet;
        /* The "cur" arm samples the block's WavCurve over wavdenoise when
         * `cp.denoicurv` is set (any of the first 500 entries differs from
         * one). The "sli" arm instead samples the Thrden wavlow curve the
         * block builds from the slider. */
        const bool cur = wavelet.slimethod == "cur";
        const bool sli_thrden =
            wavelet.slimethod == "sli" && wavelet.thrden > 0.0;
        wavelet_refine_ = wavelet_denoise_ && (cur || sli_thrden);
        wavelet_refine_active_ = false;
        wavelet_refine_expert_ = wavelet.complexmethod == "expert";
        wavelet_refine_levels_ =
            wavelet_refine_expert_
                ? RT_LAB_WAVELET_REFINE_LEVELS_EXPERT
                : RT_LAB_WAVELET_REFINE_LEVELS_STANDARD;
        wavelet_refine_mode_ = 0;
        wavelet_refine_curve_.clear();
        wavelet_refine_poly_x_.clear();
        wavelet_refine_poly_y_.clear();
        wavelet_refine_poly_slope_.clear();
        wavelet_refine_med_x_.clear();
        wavelet_refine_med_y_.clear();
        wavelet_refine_med_slope_.clear();
        wavelet_refine_high_x_.clear();
        wavelet_refine_high_y_.clear();
        wavelet_refine_high_slope_.clear();
        /* Encode one FlatCurve into the triple the tail stores. */
        const auto encode = [](const std::vector<double>& points,
                               std::vector<float>& px_out,
                               std::vector<float>& py_out,
                               std::vector<float>& ps_out) -> bool {
            const FlatCurve curve(points);
            const std::vector<double>& px = curve.polylineX();
            const std::vector<double>& py = curve.polylineY();
            const std::vector<double>& ps = curve.polylineSlope();
            if (px.empty() || px.size() > RT_LAB_WAVELET_REFINE_POLY_MAX ||
                py.size() != px.size() || ps.size() + 1 != px.size()) {
                return false;
            }
            px_out.resize(px.size());
            py_out.resize(px.size());
            ps_out.resize(ps.size());
            for (std::size_t index = 0; index < px.size(); ++index) {
                px_out[index] = static_cast<float>(px[index]);
                py_out[index] = static_cast<float>(py[index]);
                if (index < ps.size()) {
                    ps_out[index] = static_cast<float>(ps[index]);
                }
            }
            return true;
        };
        if (wavelet_refine_ && cur) {
            WavCurve denoise_curve;
            denoise_curve.Set(wavelet.wavdenoise);
            if (static_cast<bool>(denoise_curve)) {
                for (int index = 0; index < 500; ++index) {
                    if (denoise_curve[index] != 1.0f) {
                        wavelet_refine_active_ = true;
                        break;
                    }
                }
            }
            if (wavelet_refine_active_) {
                wavelet_refine_curve_.resize(RT_LAB_WAVELET_REFINE_LUT);
                for (std::uint32_t index = 0;
                     index < RT_LAB_WAVELET_REFINE_LUT; ++index) {
                    wavelet_refine_curve_[index] =
                        denoise_curve[static_cast<int>(index)];
                }
            }
        } else if (wavelet_refine_) {
            /* ip_wavelet()'s wavlow: a four-point flat curve whose breaking
             * point and plateau follow `cp.thrden`. */
            const float thrend = static_cast<float>(0.01f * wavelet.thrden);
            float thr = 0.f;
            if (thrend < 0.01f) {
                thr = 0.95f;
            } else if (thrend < 0.02f) {
                thr = 0.9f;
            } else if (thrend < 0.04f) {
                thr = 0.8f;
            } else if (thrend < 0.06f) {
                thr = 0.7f;
            } else if (thrend < 0.08f) {
                thr = 0.6f;
            } else if (thrend < 0.1f) {
                thr = 0.5f;
            } else if (thrend < 0.2f) {
                thr = 0.2f;
            }
            const std::vector<double> points = {
                static_cast<double>(FCT_MinMaxCPoints), 0.0, 1.0, 0.35, 0.35,
                static_cast<double>(thrend), 1.0, 0.35, 0.35,
                static_cast<double>(thrend + 0.01f),
                static_cast<double>(thr), 0.35, 0.35, 1.0,
                static_cast<double>(thr), 0.35, 0.35};
            if (!encode(points, wavelet_refine_poly_x_,
                        wavelet_refine_poly_y_,
                        wavelet_refine_poly_slope_)) {
                return 3;
            }
            wavelet_refine_mode_ = 1;
            wavelet_refine_active_ = true;
        }
        /*
         * The expert pairing's two extra curves, both built with the block's
         * own float arithmetic. wavmed follows `1 - Limden` and is only
         * consulted when Limden is positive; wavhigh always runs and takes
         * its breaking point from Leveldenoise's lower value.
         */
        if (wavelet_refine_ && wavelet_refine_expert_) {
            if (wavelet.limden > 0.0) {
                const float threndmed =
                    1.f - static_cast<float>(wavelet.limden);
                float thrmed = 0.f;
                if (threndmed < 0.02f) {
                    thrmed = 0.5f;
                } else if (threndmed < 0.05f) {
                    thrmed = 0.2f;
                }
                const std::vector<double> points = {
                    static_cast<double>(FCT_MinMaxCPoints), 0.0, 1.0, 0.35,
                    0.35, static_cast<double>(threndmed), 1.0, 0.35, 0.35,
                    static_cast<double>(threndmed + 0.01f),
                    static_cast<double>(thrmed), 0.35, 0.35, 1.0,
                    static_cast<double>(thrmed), 0.35, 0.35};
                if (!encode(points, wavelet_refine_med_x_,
                            wavelet_refine_med_y_,
                            wavelet_refine_med_slope_)) {
                    return 3;
                }
            }
            const float threndhigh =
                0.01f * static_cast<float>(wavelet.leveldenoise.getBottom());
            float thrhigh = 0.f;
            if (threndhigh < 0.01f) {
                thrhigh = 0.95f;
            } else if (threndhigh < 0.02f) {
                thrhigh = 0.9f;
            } else if (threndhigh < 0.04f) {
                thrhigh = 0.8f;
            } else if (threndhigh < 0.06f) {
                thrhigh = 0.7f;
            } else if (threndhigh < 0.08f) {
                thrhigh = 0.6f;
            } else if (threndhigh < 0.1f) {
                thrhigh = 0.5f;
            }
            const std::vector<double> points = {
                static_cast<double>(FCT_MinMaxCPoints), 0.0, 1.0, 0.35, 0.35,
                static_cast<double>(threndhigh), 1.0, 0.35, 0.35,
                static_cast<double>(threndhigh + 0.01f),
                static_cast<double>(thrhigh), 0.35, 0.35, 1.0,
                static_cast<double>(thrhigh), 0.35, 0.35};
            if (!encode(points, wavelet_refine_high_x_,
                        wavelet_refine_high_y_,
                        wavelet_refine_high_slope_)) {
                return 3;
            }
        }
    }
    /*
     * The balance-chroma arm. The admission rules repeat here so a forced
     * build can never run a partial arm: the method must be known, at least
     * one of the two gates must be set, the sliders must be in range, "cur"
     * needs a non-identity opacity curve and the L and a/b level counts must
     * agree.
     */
    wavelet_ba_ = false;
    wavelet_ba_l_ = false;
    wavelet_ba_ab_ = false;
    if (params_.wavelet.BAmethod != "none") {
        const procparams::WaveletParams& wavelet = params_.wavelet;
        if ((wavelet.BAmethod != "sli" && wavelet.BAmethod != "cur") ||
            (!wavelet.expfinal && !wavelet.tmr) ||
            wavelet.balance < LAB_WAVELET_MIN_WAVELET_BALANCE ||
            wavelet.balance > LAB_WAVELET_MAX_WAVELET_BALANCE ||
            wavelet.iter < LAB_WAVELET_MIN_WAVELET_ITER ||
            wavelet.iter > LAB_WAVELET_MAX_WAVELET_ITER ||
            !std::isfinite(wavelet.sigmadir) ||
            wavelet.sigmadir < LAB_WAVELET_MIN_WAVELET_SIGMADIR ||
            wavelet.sigmadir > LAB_WAVELET_MAX_WAVELET_SIGMADIR ||
            labWaveletTransformLevels(params_, width_, height_) !=
                labWaveletUntrimmedLevels(params_, width_, height_)) {
            return 5;
        }
        if (wavelet.BAmethod == "cur") {
            WavOpacityCurveW opacity;
            opacity.Set(wavelet.opacityCurveW);
            if (!static_cast<bool>(opacity)) {
                return 5;
            }
        }
        wavelet_ba_ = true;
        wavelet_ba_l_ = wavelet.expfinal;
        wavelet_ba_ab_ = wavelet.tmr;
    } else if (params_.wavelet.tmr) {
        return 5;
    }
    wavelet_ab_ = wavelet_chroma_ || wavelet_edge_ || wavelet_final_ ||
                  wavelet_toning_ || wavelet_denoise_ || wavelet_ba_ ||
                  (params_.wavelet.enabled && selection_active_);

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
    /*
     * The clarity arm's first pass forces CLmethod to "all", so its L
     * decomposition can be trimmed below the profile's untrimmed count. The
     * trim is invisible without highlight/shadow, where maxlvl only feeds the
     * unused kH table, but the contrast kernel's kH depends on it.
     */
    clarity_levels_first_ =
        clarity_active_
            ? labWaveletTransformLevels(params_, width_, height_, true)
            : wavelet_levels_;
    wavelet_active_ = params_.wavelet.enabled && wavelet_levels_ >= 1;
    if (params_.wavelet.enabled && !wavelet_active_) {
        return 3;
    }
    /*
     * The refinement indexes the pairing's own levref bands on both sides, so
     * a decomposition shorter than that would read past the last band.
     */
    if (wavelet_refine_ &&
        wavelet_levels_ < static_cast<int>(wavelet_refine_levels_)) {
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
        /* ip_wavelet() forces cp.mul[0] to 0.01f when cp.BAmet != 0, so the
         * block's `contr` gate is always true for the balance-chroma arm. */
        if (wavelet_ba_) {
            contr = true;
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
                                   wavelet_toning_, wavelet_denoise_,
                                   wavelet_refine_,
                                   static_cast<std::uint32_t>(
                                       wavelet_refine_poly_x_.size()),
                                   static_cast<std::uint32_t>(
                                       wavelet_refine_med_x_.size()),
                                   static_cast<std::uint32_t>(
                                       wavelet_refine_high_x_.size()),
                                   wavelet_soft_, softend_active_,
                                   softend_thrend_,
                                   static_cast<std::uint32_t>(
                                       softend_thrend_curve_.size() / 3),
                                   static_cast<std::uint32_t>(
                                       softend_hue_ ? softend_hue_curve_.size() / 3
                                                    : 0),
                                   wavelet_ba_),
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
        /*
         * ip_wavelet() forces cp.mul[0] to 0.01f when cp.BAmet != 0 so the L
         * pass always runs; the contrast and chroma multiplies read that
         * value, so the table must carry it.
         */
        if (wavelet_ba_ && table[RT_LW_PARAM_C] == 0.f) {
            table[RT_LW_PARAM_C] = 0.01f;
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

        /* The clarity arm's guided soft radius, computed by the host. */
        table[RT_LW_PARAM_SOFT_ACTIVE] = wavelet_soft_ ? 1.f : 0.f;
        if (wavelet_soft_) {
            table[RT_LW_PARAM_SOFT_EPSILON] = soft_epsilon_;
            table[RT_LW_PARAM_SOFT_W] = static_cast<float>(soft_w_);
            table[RT_LW_PARAM_SOFT_H] = static_cast<float>(soft_h_);
            table[RT_LW_PARAM_SOFT_BOXRAD] =
                static_cast<float>(soft_box_rad_);
            table[RT_LW_PARAM_SOFT_SCALE_DX] = soft_scale_dx_;
            table[RT_LW_PARAM_SOFT_SCALE_DY] = soft_scale_dy_;
            table[RT_LW_PARAM_SOFT_SCALE_UX] = soft_scale_ux_;
            table[RT_LW_PARAM_SOFT_SCALE_UY] = soft_scale_uy_;
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
             * The Lipschitz control's scalars, derived with the block's own
             * float arithmetic (ipwavelet.cc:284, 2912). `detectedge` gates
             * calckoe(); the smoothing mode is lip3 && lipp, with `neigh`
             * selecting the 28/40 or 38/50 tap pair. EDmethod is not carried:
             * the engine forces EDmet == 2 for both selections.
             */
            const bool detectedge = wavelet.medianlev;
            float edgsens = 60.f;
            float edgampl = 10.f;
            if (wavelet.lipst) {
                edgsens = static_cast<float>(wavelet.edgesensi);
                edgampl = static_cast<float>(wavelet.edgeampli);
            }
            table[RT_LW_PARAM_EDG_DETECT] = detectedge ? 1.f : 0.f;
            table[RT_LW_PARAM_EDG_GRADW] =
                static_cast<float>(wavelet.edgedetect);
            table[RT_LW_PARAM_EDG_TLOWW] =
                static_cast<float>(wavelet.edgedetectthr);
            table[RT_LW_PARAM_EDG_AAMP] =
                1.f + static_cast<float>(wavelet.edgedetectthr2) / 100.f;
            table[RT_LW_PARAM_EDG_LIPINFL] = 0.005f * edgsens + 0.4f;
            table[RT_LW_PARAM_EDG_LIPAMPL] = 1.f + edgampl / 50.f;
            int smoothing = 0;
            if (detectedge && wavelet.lipst &&
                wavelet.NPmethod != "none") {
                smoothing = wavelet.NPmethod == "high" ? 2 : 1;
            }
            table[RT_LW_PARAM_EDG_SMOOTH] =
                static_cast<float>(smoothing);

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

            if (wavelet_refine_) {
                const std::size_t refine_offset =
                    denoise_offset + RT_LAB_WAVELET_DENOISE_HEADER +
                    static_cast<std::size_t>(planes) *
                        static_cast<std::size_t>(wavelet_levels_) *
                        RT_LAB_WAVELET_DENOISE_MAD_ENTRIES;
                float* refine_tail = table + refine_offset;
                refine_tail[0] = 0.f;
                if (wavelet_refine_active_) {
                    float* lut = refine_tail +
                                 RT_LAB_WAVELET_REFINE_HEADER +
                                 static_cast<std::size_t>(
                                     RT_LAB_WAVELET_REFINE_LEVELS) *
                                     RT_LAB_WAVELET_REFINE_SCALARS;
                    for (std::uint32_t index = 0;
                         index < RT_LAB_WAVELET_REFINE_LUT; ++index) {
                        lut[index] = wavelet_refine_curve_.empty()
                                         ? 1.f
                                         : wavelet_refine_curve_[index];
                    }
                    float* source =
                        lut + RT_LAB_WAVELET_REFINE_LUT;
                    const std::uint32_t count =
                        static_cast<std::uint32_t>(
                            wavelet_refine_poly_x_.size());
                    source[0] = static_cast<float>(wavelet_refine_mode_);
                    source[1] = static_cast<float>(count);
                    for (std::uint32_t index = 0; index < count; ++index) {
                        source[2 + index] =
                            wavelet_refine_poly_x_[index];
                        source[2 + count + index] =
                            wavelet_refine_poly_y_[index];
                    }
                    for (std::size_t index = 0;
                         index < wavelet_refine_poly_slope_.size();
                         ++index) {
                        source[2 + 2 * count + index] =
                            wavelet_refine_poly_slope_[index];
                    }
                    /*
                     * The expert pairing's wavmed and wavhigh curves follow in
                     * the same encoding. The mode is zero when the curve is
                     * absent: wavmed only runs with a positive Limden.
                     */
                    const auto write_curve = [](float* block, float mode,
                                                const std::vector<float>& px,
                                                const std::vector<float>& py,
                                                const std::vector<float>& ps) {
                        const std::uint32_t knots =
                            static_cast<std::uint32_t>(px.size());
                        block[0] = mode;
                        block[1] = static_cast<float>(knots);
                        for (std::uint32_t index = 0; index < knots; ++index) {
                            block[2 + index] = px[index];
                            block[2 + knots + index] = py[index];
                        }
                        for (std::size_t index = 0; index < ps.size();
                             ++index) {
                            block[2 + 2 * knots + index] = ps[index];
                        }
                    };
                    float* med = source +
                                 RT_LAB_WAVELET_REFINE_CURVE_HEADER +
                                 3 * static_cast<std::size_t>(count);
                    write_curve(med,
                                wavelet_refine_med_x_.empty() ? 0.f : 1.f,
                                wavelet_refine_med_x_,
                                wavelet_refine_med_y_,
                                wavelet_refine_med_slope_);
                    const std::size_t med_count =
                        wavelet_refine_med_x_.size();
                    float* high = med + RT_LAB_WAVELET_REFINE_CURVE_HEADER +
                                  3 * med_count;
                    write_curve(high,
                                wavelet_refine_high_x_.empty() ? 0.f : 1.f,
                                wavelet_refine_high_x_,
                                wavelet_refine_high_y_,
                                wavelet_refine_high_slope_);
                }
            }
        }
        if (wavelet_soft_) {
            /*
             * XYZ2Lab()'s L output is a float lookup through the block's own
             * Color::cachefy table. Uploading the table reproduces the host
             * rounding; std::cbrt() on the device would not. The tail is the
             * last one, so its offset is the table length without it.
             */
            if (!Color::cachefy) {
                return 5;
            }
            const std::size_t soft_offset = labWaveletParamEntries(
                static_cast<std::uint32_t>(planes),
                static_cast<std::uint32_t>(wavelet_levels_), wavelet_edge_,
                wavelet_final_, wavelet_toning_, wavelet_denoise_,
                wavelet_refine_,
                static_cast<std::uint32_t>(wavelet_refine_poly_x_.size()),
                static_cast<std::uint32_t>(wavelet_refine_med_x_.size()),
                static_cast<std::uint32_t>(wavelet_refine_high_x_.size()),
                false, false, false, 0, 0);
            if (soft_offset + RT_LAB_WAVELET_SOFT_LUT_ENTRIES >
                wavelet_params_.size()) {
                return 5;
            }
            float* soft_lut = table + soft_offset;
            for (std::uint32_t index = 0;
                 index < RT_LAB_WAVELET_SOFT_LUT_ENTRIES; ++index) {
                soft_lut[index] = Color::cachefy[static_cast<int>(index)];
            }
        }
        if (softend_active_) {
            const std::size_t softend_offset = labWaveletParamEntries(
                static_cast<std::uint32_t>(planes),
                static_cast<std::uint32_t>(wavelet_levels_), wavelet_edge_,
                wavelet_final_, wavelet_toning_, wavelet_denoise_,
                wavelet_refine_,
                static_cast<std::uint32_t>(wavelet_refine_poly_x_.size()),
                static_cast<std::uint32_t>(wavelet_refine_med_x_.size()),
                static_cast<std::uint32_t>(wavelet_refine_high_x_.size()),
                wavelet_soft_, false, false, 0, 0);
            if (softend_offset + RT_LAB_WAVELET_SOFTEND_ENTRIES >
                wavelet_params_.size()) {
                return 5;
            }
            float* softend_tail = table + softend_offset;
            softend_tail[RT_LAB_WAVELET_SOFTEND_BLEND] = softend_blend_;
            softend_tail[RT_LAB_WAVELET_SOFTEND_EPSILON] = softend_epsilon_;
            softend_tail[RT_LAB_WAVELET_SOFTEND_W] =
                static_cast<float>(softend_w_);
            softend_tail[RT_LAB_WAVELET_SOFTEND_H] =
                static_cast<float>(softend_h_);
            softend_tail[RT_LAB_WAVELET_SOFTEND_BOXRAD] =
                static_cast<float>(softend_box_rad_);
            softend_tail[RT_LAB_WAVELET_SOFTEND_SCALE_DX] =
                softend_scale_dx_;
            softend_tail[RT_LAB_WAVELET_SOFTEND_SCALE_DY] =
                softend_scale_dy_;
            softend_tail[RT_LAB_WAVELET_SOFTEND_SCALE_UX] =
                softend_scale_ux_;
            softend_tail[RT_LAB_WAVELET_SOFTEND_SCALE_UY] =
                softend_scale_uy_;
            float* cursor =
                softend_tail + RT_LAB_WAVELET_SOFTEND_ENTRIES;
            softend_thrend_offset_ = 0;
            if (softend_thrend_) {
                softend_thrend_offset_ =
                    static_cast<std::size_t>(cursor - table);
                /* The level count, then the per-level scalars that
                 * computeSoftend() fills once both decompositions have run. */
                cursor[0] = static_cast<float>(wavelet_levels_);
                cursor += RT_LAB_WAVELET_SOFTEND_THREND_HEADER +
                          (wavelet_levels_ > 1
                               ? static_cast<std::size_t>(
                                     wavelet_levels_ - 1) *
                                     RT_LAB_WAVELET_SOFTEND_LEVEL_SCALARS
                               : 0);
                for (float value : softend_thrend_curve_) {
                    *cursor++ = value;
                }
            }
            if (softend_hue_) {
                for (float value : softend_hue_curve_) {
                    *cursor++ = value;
                }
            }
            if (cursor > table + wavelet_params_.size()) {
                return 5;
            }
        }
        /*
         * The balance-chroma arm. It is the table's last tail; the mea block
         * is filled by computeMea() once the device statistics are read back,
         * so only the header and the opacity curve are written here.
         */
        if (wavelet_ba_) {
            const std::size_t ba_entries =
                RT_LAB_WAVELET_BA_HEADER +
                static_cast<std::size_t>(wavelet_levels_) *
                    RT_LAB_WAVELET_BA_MEA_ENTRIES +
                RT_LAB_WAVELET_CURVE_ENTRIES;
            if (ba_entries > wavelet_params_.size()) {
                return 5;
            }
            wavelet_ba_offset_ = wavelet_params_.size() - ba_entries;
            float* ba_tail = table + wavelet_ba_offset_;
            ba_tail[0] = 1.f;
            ba_tail[1] = wavelet.BAmethod == "cur" ? 2.f : 1.f;
            ba_tail[2] = static_cast<float>(wavelet.balance);
            ba_tail[3] = static_cast<float>(wavelet.iter);
            if (wavelet.BAmethod == "cur") {
                WavOpacityCurveW opacity;
                opacity.Set(wavelet.opacityCurveW);
                if (!static_cast<bool>(opacity)) {
                    return 5;
                }
                float* curve =
                    ba_tail + RT_LAB_WAVELET_BA_HEADER +
                    static_cast<std::size_t>(wavelet_levels_) *
                        RT_LAB_WAVELET_BA_MEA_ENTRIES;
                for (std::uint32_t index = 0;
                     index < RT_LAB_WAVELET_CURVE_ENTRIES; ++index) {
                    curve[index] = opacity[static_cast<int>(index)];
                }
            }
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
    /*
     * The balance-chroma L arm rebuilds ContAllL()'s beta ramp with sigmadir.
     * ContAllAB()'s arm reads no ramp, so only plane 0 needs a block.
     */
    float* ba_mea = nullptr;
    if (wavelet_ba_l_ && plane == 0) {
        ba_mea = wavelet_params_.data() + wavelet_ba_offset_ +
                 RT_LAB_WAVELET_BA_HEADER;
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
        if (ba_mea != nullptr) {
            ipf_.calceffect(
                static_cast<int>(level), mean, sigma,
                ba_mea + static_cast<std::size_t>(level) *
                             RT_LAB_WAVELET_BA_MEA_ENTRIES,
                static_cast<float>(params_.wavelet.sigmadir), 1.f);
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

int LabWaveletContext::computeRefine(const float* pre_stats,
                                     const float* post_stats,
                                     uint32_t levels)
{
    const uint32_t planes = wavelet_ab_ ? 3u : 1u;
    const uint32_t stride = static_cast<uint32_t>(wavelet_levels_);
    if (!wavelet_active_ || !wavelet_refine_ || pre_stats == nullptr ||
        post_stats == nullptr || levels < 1 || levels > stride ||
        stride > RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) {
        return 1;
    }

    const auto aggregate = [](const float* stats, uint32_t level,
                              float& mean, float& sigma, float& maxp) {
        const auto value = [&](int direction, int entry) {
            return stats[(static_cast<std::size_t>(level) * 3 +
                          static_cast<std::size_t>(direction)) *
                             RT_LAB_WAVELET_STATS_PER_BAND +
                         static_cast<std::size_t>(entry)];
        };
        /* Eval2()'s accumulation order and its / 3. */
        mean = (value(0, 0) + value(1, 0) + value(2, 0)) / 3.f;
        sigma = (value(0, 4) + value(1, 4) + value(2, 4)) / 3.f;
        maxp = (value(0, 2) + value(1, 2) + value(2, 2)) / 3.f;
    };

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
    const std::size_t refine_offset =
        denoise_offset + RT_LAB_WAVELET_DENOISE_HEADER +
        static_cast<std::size_t>(planes) *
            static_cast<std::size_t>(stride) *
            RT_LAB_WAVELET_DENOISE_MAD_ENTRIES;
    float* tail = wavelet_params_.data() + refine_offset;
    tail[0] = 0.f;

    /*
     * ip_wavelet()'s early-out: any level whose pre- or post-denoise mean or
     * sigma is below 0.1 makes the block skip the whole refinement. It is a
     * per-pass decision over the pass's own level count.
     */
    bool exitifzero = true;
    for (uint32_t level = 0; level < levels; ++level) {
        float mean = 0.f;
        float sigma = 0.f;
        float maxp = 0.f;
        float meand = 0.f;
        float sigmad = 0.f;
        float maxpd = 0.f;
        aggregate(pre_stats, level, mean, sigma, maxp);
        aggregate(post_stats, level, meand, sigmad, maxpd);
        if (mean < 0.1f || meand < 0.1f || sigma < 0.1f ||
            sigmad < 0.1f) {
            exitifzero = false;
        }
    }

    if (!wavelet_refine_active_ || !exitifzero ||
        levels < wavelet_refine_levels_) {
        return 0;
    }

    int mixmet = 3;
    if (params_.wavelet.mixmethod == "nois") {
        mixmet = 0;
    } else if (params_.wavelet.mixmethod == "mix") {
        mixmet = 1;
    } else if (params_.wavelet.mixmethod == "mix7") {
        mixmet = 2;
    }
    constexpr float weights[4] = {0.f, 0.5f, 0.7f, 1.f};
    const float weight = weights[mixmet];
    const float sigm = static_cast<float>(params_.wavelet.sigm);
    /* The expert pairing's two ends, levels 0..3 and 4..5. */
    const float sigmm14 =
        static_cast<float>(params_.wavelet.levelsigm.getTop());
    const float sigmm56 =
        static_cast<float>(params_.wavelet.levelsigm.getBottom());
    /* ip_wavelet.cc calls the double `log()` and stores into a float. */
    const float inx = static_cast<float>(std::log(static_cast<double>(0.666f)));

    for (std::uint32_t level = 0; level < wavelet_refine_levels_;
         ++level) {
        float mean = 0.f;
        float sigma = 0.f;
        float maxp = 0.f;
        float meand = 0.f;
        float sigmad = 0.f;
        float maxpd = 0.f;
        aggregate(pre_stats, level, mean, sigma, maxp);
        aggregate(post_stats, level, meand, sigmad, maxpd);

        /*
         * The block enters the per-level arithmetic only when the pre-denoise
         * MaxP and mean/sigma are non-zero. A degenerate level is skipped,
         * not refused: the other levels still apply their factor.
         */
        const bool active = maxp > 0.f && mean != 0.f && sigma != 0.f;
        float* entry = tail + RT_LAB_WAVELET_REFINE_HEADER +
                       static_cast<std::size_t>(level) *
                           RT_LAB_WAVELET_REFINE_SCALARS;
        entry[0] = active ? 1.f : 0.f;
        if (!active) {
            continue;
        }
        /* siglh[level] is the expert pairing's sigmm14 or sigmm56. */
        const float siglh =
            wavelet_refine_expert_ ? (level > 3 ? sigmm56 : sigmm14) : sigm;

        /* intp(a, b, c) = a * (b - c) + c: post is b, pre is c. */
        const float tempmax = weight * (maxpd - maxp) + maxp;
        const float tempmean = weight * (meand - mean) + mean;
        const float tempsig = weight * (sigmad - sigma) + sigma;
        if (!(tempmax > 0.f) || !(tempsig > 0.f)) {
            /* The host would take log()/divide of a non-positive value and
             * produce a non-finite coefficient. Fail closed instead. */
            return 2;
        }
        const float logmax =
            static_cast<float>(std::log(static_cast<double>(tempmax)));
        const float rap_x = (tempmean + siglh * tempsig) / tempmax;
        if (!(rap_x > 0.f)) {
            return 2;
        }
        const float iny =
            static_cast<float>(std::log(static_cast<double>(rap_x)));
        const float rap = inx / iny;
        const float asig = 0.166f / (tempsig * siglh);
        const float bsig = 0.5f - asig * tempmean;
        const float amean = 1.f / tempmean;
        const float k =
            1.f / (siglh > 1.f ? SQR(siglh) : siglh);
        const float maxval = tempmean + siglh * tempsig;
        /* cp.denmet is hard-coded to 4: levels 0 and 1 get 0.3, the rest 1. */
        const float kc_factor =
            (level == 0 || level == 1) ? 0.3f : 1.f;

        entry[1] = weight;
        entry[2] = logmax;
        entry[3] = rap;
        entry[4] = asig;
        entry[5] = bsig;
        entry[6] = amean;
        entry[7] = k;
        entry[8] = maxval;
        entry[9] = tempmean;
        entry[10] = kc_factor;
    }
    tail[0] = 1.f;
    return 0;
}

int LabWaveletContext::computeSoftend(const float* stats_ll,
                                      const float* stats_dst,
                                      uint32_t levels)
{
    const uint32_t stride = static_cast<uint32_t>(wavelet_levels_);
    if (!wavelet_active_ || !softend_active_ || !softend_thrend_ ||
        stats_ll == nullptr || stats_dst == nullptr ||
        softend_thrend_offset_ == 0 || levels < 1 || levels > stride ||
        stride > RT_LAB_WAVELET_MAX_TRANSFORM_LEVELS) {
        return 1;
    }

    const auto aggregate = [](const float* stats, uint32_t level,
                              float& mean, float& sigma, float& maxp) {
        const auto value = [&](int direction, int entry) {
            return stats[(static_cast<std::size_t>(level) * 3 +
                          static_cast<std::size_t>(direction)) *
                             RT_LAB_WAVELET_STATS_PER_BAND +
                         static_cast<std::size_t>(entry)];
        };
        /* Eval2()'s accumulation order and its / 3. */
        mean = (value(0, 0) + value(1, 0) + value(2, 0)) / 3.f;
        sigma = (value(0, 4) + value(1, 4) + value(2, 4)) / 3.f;
        maxp = (value(0, 2) + value(1, 2) + value(2, 2)) / 3.f;
    };

    /* ip_wavelet()'s fixed `sig = 2.f`, so `k` is SQR(sig) and the amean
     * branch takes the sqrt(sqrt()) shortcut. */
    constexpr float sig = 2.f;
    /* ip_wavelet.cc calls the double log() and stores into a float. */
    const float inx =
        static_cast<float>(std::log(static_cast<double>(0.666f)));

    float* tail = wavelet_params_.data() + softend_thrend_offset_;
    if (tail[0] != static_cast<float>(levels)) {
        return 1;
    }

    /*
     * The block's loop stops one level short of the decomposition's count;
     * the last level's coefficients are left untouched.
     */
    for (uint32_t level = 0; level + 1 < levels; ++level) {
        float mean = 0.f;
        float sigma = 0.f;
        float maxp = 0.f;
        float meang = 0.f;
        float sigmag = 0.f;
        float maxpg = 0.f;
        aggregate(stats_dst, level, mean, sigma, maxp);
        aggregate(stats_ll, level, meang, sigmag, maxpg);

        float* entry = tail + RT_LAB_WAVELET_SOFTEND_THREND_HEADER +
                       static_cast<std::size_t>(level) *
                           RT_LAB_WAVELET_SOFTEND_LEVEL_SCALARS;
        /*
         * The block enters the per-level arithmetic only when the pre-filter
         * MaxP and mean/sigma are non-zero. A degenerate level is skipped,
         * not refused: the other levels still apply their factor.
         */
        const bool active = maxp > 0.f && mean != 0.f && sigma != 0.f;
        entry[0] = active ? 1.f : 0.f;
        if (!active) {
            continue;
        }

        const float tempmean = 0.3f * mean + 0.7f * meang;
        const float tempsig = 0.3f * sigma + 0.7f * sigmag;
        const float tempmax = 0.3f * maxp + 0.7f * maxpg;
        if (!(tempmax > 0.f) || !(tempsig > 0.f) || tempmean == 0.f) {
            /* The block would take log()/divide of a non-positive value and
             * produce a non-finite coefficient. Fail closed instead. */
            return 2;
        }
        const float logmax =
            static_cast<float>(std::log(static_cast<double>(tempmax)));
        const float rap_x = (tempmean + sig * tempsig) / tempmax;
        if (!(rap_x > 0.f)) {
            return 2;
        }
        const float iny =
            static_cast<float>(std::log(static_cast<double>(rap_x)));
        const float rap = inx / iny;
        const float asig = 0.166f / (tempsig * sig);
        const float bsig = 0.5f - asig * tempmean;
        const float amean = 1.f / tempmean;

        entry[1] = tempmean;
        entry[2] = tempmean + sig * tempsig;
        entry[3] = logmax;
        entry[4] = rap;
        entry[5] = asig;
        entry[6] = bsig;
        entry[7] = amean;
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
