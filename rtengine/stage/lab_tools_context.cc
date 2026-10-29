#include "stage/lab_tools_context.h"

#include "color.h"
#include "curves.h"
#include "iccstore.h"
#include "improcfun.h"
#include "ipshadowshighlights.h"
#include "labimage.h"
#include "procparams.h"
#include "stage/stage_buffer.h"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace rtengine::stage
{

namespace
{

/* chromiLuminanceCurve()'s own construction: a flat curve that is non-identity
 * only when its points move away from 0.5. The three flat-curve families are
 * the chromaticity path, which the node does not port. */
bool flatCurveActive(const std::vector<double>& points)
{
    if (points.empty() || points[0] == 0) {
        return false;
    }
    FlatCurve curve(points);
    return !curve.isIdentity();
}

/* Color::init() fills the Munsell tables MunsellLch() reads. A process
 * without them cannot run the host block either, so the block is a residual. */
bool munsellTablesAvailable()
{
    const LUTf* first = Color::munsellLut(0);
    return Color::munsellLutCount() ==
               static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES) &&
           first != nullptr && first->getSize() != 0;
}

} // namespace

bool labToolsShadowsHighlightsActive(const procparams::ProcParams& params)
{
    return params.sh.enabled && (params.sh.highlights > 0 || params.sh.shadows > 0);
}

bool labToolsVibranceActive(const procparams::ProcParams& params)
{
    if (!params.vibrance.enabled) {
        return false;
    }
    if (params.vibrance.pastels != 0 || params.vibrance.saturated != 0) {
        return true;
    }
    /*
     * ImProcFunctions::vibrance() keeps the hue curve only when it is not the
     * identity, so an enabled but flat curve with both sliders at zero is a
     * no-op there too.
     */
    DiagonalCurve curve(params.vibrance.skintonescurve, CURVES_MIN_POLY_POINTS);
    return !curve.isIdentity();
}

bool labToolsLabCurveActive(const procparams::ProcParams& params)
{
    /*
     * chromiLuminanceCurve() owns the hist16 refill when `contrast` is
     * non-zero; that refill is a side effect a later node reads, so the
     * contrast path is never a device pass. With contrast at zero the pass
     * always runs when the curves are enabled, even if every table is the
     * identity: the pass still rewrites L, a and b through the block's own
     * divides, which is why it is not a no-op.
     */
    return params.labCurve.enabled && params.labCurve.contrast == 0;
}

bool labToolsSupported(const procparams::ProcParams& params)
{
    /*
     * The LabGrid pass is the only colour toning this node implements, and
     * every other colour toning method is a residual. LabRegions is covered by
     * the method test: labColorCorrectionRegions() only runs for it.
     */
    if (params.colorToning.enabled && params.colorToning.method != "LabGrid") {
        return false;
    }

    /*
     * The Lab contrast value refills hist16, which a later block reads. The
     * refill stays on the host, so a non-zero contrast is a residual.
     */
    if (params.labCurve.contrast != 0) {
        return false;
    }

    /*
     * The Lab-curve pass is a device case only when everything it reads is
     * ported. The chromaticity slider and the three flat curves (C=f(H),
     * L=f(H) and H=f(H)) are a separate family; the LAB, XYZ and XYZREL
     * Munsell methods need gamutLchonly() and gamutmap(); factnoise reads the
     * denoise tool. Each is a residual.
     */
    if (params.labCurve.enabled) {
        if (params.labCurve.chromaticity != 0 ||
            flatCurveActive(params.labCurve.chcurve) ||
            flatCurveActive(params.labCurve.lhcurve) ||
            flatCurveActive(params.labCurve.hhcurve) ||
            params.dirpyrDenoise.enabled) {
            return false;
        }
        const bool munsell =
            params.labCurve.gamutmunselmethod == "MUN";
        if (!munsell && params.labCurve.gamutmunselmethod != "NONE") {
            return false;
        }
        if (munsell && !munsellTablesAvailable()) {
            return false;
        }
        if (!(params.labCurve.rstprotection >= 0.0 &&
              params.labCurve.rstprotection <= 100.0)) {
            return false;
        }
    }

    if (params.localContrast.enabled || params.epd.enabled) {
        return false;
    }

    /*
     * chromiLuminanceCurve() zeroes a and b for a black-and-white output even
     * when the Lab curves are off, so it is outside the node.
     */
    if (params.blackwhite.enabled) {
        return false;
    }

    /*
     * Vibrance runs on the device only when its parameters are inside the
     * slider ranges the ABI validates and, for the avoidcolorshift path, the
     * Munsell tables exist. A no-op vibrance is not a device case: the node
     * would only be the other tools, or nothing.
     */
    const bool vibrance = labToolsVibranceActive(params);
    if (params.vibrance.enabled) {
        const auto in_range = [](int value, int low, int high) {
            return value >= low && value <= high;
        };
        if (!in_range(params.vibrance.pastels, -100, 100) ||
            !in_range(params.vibrance.saturated, -100, 100) ||
            !in_range(params.vibrance.psthreshold.getTopLeft(), 0, 100) ||
            !in_range(params.vibrance.psthreshold.getBottomLeft(), 0, 100)) {
            return false;
        }
        if (!vibrance) {
            return false;
        }
        if (params.vibrance.avoidcolorshift && !munsellTablesAvailable()) {
            return false;
        }
    }

    /*
     * Slider ranges the device ABI validates. Keeping them here makes an
     * out-of-range profile a recorded residual instead of a device refusal.
     */
    if (params.sh.enabled) {
        const auto in_slider = [](int value) {
            return value >= 0 && value <= 100;
        };
        const auto in_amount = [](int value) {
            return value >= 0 && value <= 10000;
        };
        if (!in_amount(params.sh.highlights) ||
            !in_amount(params.sh.shadows) ||
            !in_slider(params.sh.htonalwidth) ||
            !in_slider(params.sh.stonalwidth) ||
            !in_slider(params.sh.radius)) {
            return false;
        }
    }

    return (params.colorToning.enabled &&
            params.colorToning.method == "LabGrid") ||
           labToolsShadowsHighlightsActive(params) || vibrance ||
           labToolsLabCurveActive(params);
}


void labToolsHostBlock(ImProcFunctions& ipf,
                       const procparams::ProcParams& params,
                       LabImage& lab,
                       LabToolsLuts& luts,
                       bool& autili,
                       bool& butili)
{
    /* ImageProcessor::usesCieCam02(), inlined for the extracted block. */
    const bool cam02 = params.colorappearance.modelmethod == "02" &&
                       params.colorappearance.enabled;

    if (params.labCurve.contrast != 0) { //only use hist16 for contrast
        luts.hist16->clear();

#ifdef _OPENMP
        #pragma omp parallel
#endif
        {
            LUTu hist16thr(luts.hist16->getSize());   // one temporary lookup table per thread
            hist16thr.clear();
#ifdef _OPENMP
            #pragma omp for schedule(static) nowait
#endif

            for (int i = 0; i < lab.H; i++)
                for (int j = 0; j < lab.W; j++) {
                    hist16thr[(int)((lab.L[i][j]))]++;
                }

#ifdef _OPENMP
            #pragma omp critical
#endif
            {
                *luts.hist16 += hist16thr;
            }
        }
    }

    bool utili;
    CurveFactory::complexLCurve(params.labCurve.brightness, params.labCurve.contrast, params.labCurve.lcurve, *luts.hist16, *luts.lumacurve, *luts.dummy, 1, utili);

    const bool clcutili = CurveFactory::diagonalCurve2Lut(params.labCurve.clcurve, *luts.clcurve, 1);

    bool ccutili, cclutili;
    CurveFactory::complexsgnCurve(autili, butili, ccutili, cclutili, params.labCurve.acurve, params.labCurve.bcurve, params.labCurve.cccurve,
                                  params.labCurve.lccurve, *luts.curve1, *luts.curve2, *luts.satcurve, *luts.lhskcurve, 1);


    if (params.colorToning.enabled && params.colorToning.method == "LabGrid") {
        ipf.colorToningLabGrid(&lab, 0, lab.W, 0, lab.H, false);
    }

    ipf.shadowsHighlights(&lab, params.sh.enabled, params.sh.lab, params.sh.highlights, params.sh.shadows, params.sh.radius, LAB_TOOLS_EXPORT_SCALE, params.sh.htonalwidth, params.sh.stonalwidth);

    if (params.localContrast.enabled) {
        // Alberto's local contrast
        ipf.localContrast(&lab, lab.L, params.localContrast, false, 1);//scale);
    }

    ipf.chromiLuminanceCurve(nullptr, 1, &lab, &lab, *luts.curve1, *luts.curve2, *luts.satcurve, *luts.lhskcurve, *luts.clcurve, *luts.lumacurve, utili, autili, butili, ccutili, cclutili, clcutili, *luts.dummy, *luts.dummy);

    if ((params.colorappearance.enabled && !params.colorappearance.tonecie) || (!cam02)) {
        ipf.EPDToneMap(&lab, 0, 1);
    }

    ipf.vibrance(&lab, params.vibrance, params.toneCurve.hrenabled, params.icm.workingProfile);
    ipf.labColorCorrectionRegions(&lab);
}

LabToolsContext::LabToolsContext(const procparams::ProcParams& params,
                                 ImProcFunctions& ipf,
                                 LabToolsLuts luts,
                                 bool& autili,
                                 bool& butili)
    : params_(params),
      ipf_(ipf),
      luts_(luts),
      autili_(&autili),
      butili_(&butili)
{
}

LabToolsContext::~LabToolsContext() = default;

RtLabToolsContext LabToolsContext::descriptor(bool supported)
{
    RtLabToolsContext descriptor{};
    descriptor.size = sizeof(RtLabToolsContext);
    descriptor.version = RT_LAB_TOOLS_CONTEXT_VERSION;
    descriptor.magic = RT_LAB_TOOLS_CONTEXT_MAGIC;
    descriptor.supported = supported ? 1u : 0u;
    descriptor.self = this;
    descriptor.build = [](void* self, RtLabToolsResult* out) {
        return static_cast<LabToolsContext*>(self)->build(*out);
    };
    descriptor.run_host = [](void* self, RtStageBuffer* lab) {
        return static_cast<LabToolsContext*>(self)->runHost(*lab);
    };
    return descriptor;
}

int LabToolsContext::build(RtLabToolsResult& out)
{
    const bool labgrid =
        params_.colorToning.enabled &&
        params_.colorToning.method == "LabGrid";

    /*
     * ImProcFunctions::colorToningLabGrid(): the same doubles, the same
     * division order, so the four floats match the block bit for bit.
     */
    const double factor = procparams::ColorToningParams::LABGRID_CORR_MAX * 3.0;
    const double scaling = procparams::ColorToningParams::LABGRID_CORR_SCALE;
    float a_scale = 0.f;
    float a_base = 0.f;
    float b_scale = 0.f;
    float b_base = 0.f;
    if (labgrid) {
        a_scale = static_cast<float>(
            (params_.colorToning.labgridAHigh -
             params_.colorToning.labgridALow) / factor / scaling);
        a_base = static_cast<float>(params_.colorToning.labgridALow / scaling);
        b_scale = static_cast<float>(
            (params_.colorToning.labgridBHigh -
             params_.colorToning.labgridBLow) / factor / scaling);
        b_base = static_cast<float>(params_.colorToning.labgridBLow / scaling);
    }
    labgrid_[0] = a_scale;
    labgrid_[1] = a_base;
    labgrid_[2] = b_scale;
    labgrid_[3] = b_base;

    const bool lab_mode = params_.sh.lab;
    const std::size_t entries = lab_mode ? RT_LAB_TOOLS_LAB_LUT_ENTRIES
                                         : RT_LAB_TOOLS_RGB_LUT_ENTRIES;
    if (lut_highlight_.size() != entries) {
        lut_highlight_.assign(entries, 0.f);
        lut_shadow_.assign(entries, 0.f);
    }

    const TMatrix ws =
        ICCStore::getInstance()->workingSpaceMatrix(params_.icm.workingProfile);
    const TMatrix iws = ICCStore::getInstance()->workingSpaceInverseMatrix(
        params_.icm.workingProfile);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            ws_[row * 3 + col] = static_cast<float>(ws[row][col]);
            iws_[row * 3 + col] = static_cast<float>(iws[row][col]);
        }
    }

    /*
     * The tone tables. A round that does not run keeps an identity table: the
     * kernel never reads it, and the launcher still validates one fixed shape.
     */
    LUTf highlight(static_cast<unsigned int>(entries));
    LUTf shadow(static_cast<unsigned int>(entries));
    const bool active = labToolsShadowsHighlightsActive(params_);
    if (active && params_.sh.highlights > 0) {
        shadowsHighlightsLut(
            ws, iws, lab_mode,
            shadowsHighlightsAmount(params_.sh.highlights, true), true, true,
            highlight);
    }
    if (active && params_.sh.shadows > 0) {
        shadowsHighlightsLut(
            ws, iws, lab_mode,
            shadowsHighlightsAmount(params_.sh.shadows, false), false, true,
            shadow);
    }
    for (std::size_t index = 0; index < entries; ++index) {
        const float identity = static_cast<float>(index);
        lut_highlight_[index] =
            active && params_.sh.highlights > 0
                ? highlight[static_cast<int>(index)]
                : identity;
        lut_shadow_[index] =
            active && params_.sh.shadows > 0
                ? shadow[static_cast<int>(index)]
                : identity;
    }

    /*
     * RGB mode closes the blend through lab2rgb() and rgb2lab(). The host
     * evaluates those two colour-matrix products in double and the device in
     * float, so the channel that indexes the tone table differs by up to one
     * ulp of a 16-bit value. The table's steepest adjacent step turns that
     * difference into the output error the step can add; refuse a table where
     * the product can exceed one LSB16 and let the builtin run the block.
     * Lab mode applies the table to the exact L value and needs no bound.
     */
    if (!lab_mode && active) {
        /*
         * One ulp of a 16-bit channel: 65535 * 2^-23, the rounding unit of the
         * float the Lab-to-RGB round trip narrows to.
         */
        constexpr float CHANNEL_ULP = 0.0078125f;
        constexpr float MAX_ERROR = 1.0f; /* LSB16 */
        const auto slope = [](const std::vector<float>& table) {
            float worst = 0.f;
            for (std::size_t index = 0; index + 1 < table.size(); ++index) {
                const float step = table[index + 1] - table[index];
                worst = std::max(worst, step < 0.f ? -step : step);
            }
            return worst;
        };
        if ((params_.sh.highlights > 0 &&
             slope(lut_highlight_) * CHANNEL_ULP > MAX_ERROR) ||
            (params_.sh.shadows > 0 &&
             slope(lut_shadow_) * CHANNEL_ULP > MAX_ERROR)) {
            return 1;
        }
    }

    /*
     * Color::XYZ2Lab() reads these two caches in RGB mode. They are static
     * globals filled by Color::init(); a process without them cannot run the
     * block either, so the setup refuses instead of reading a null table.
     */
    if (Color::cachef.getSize() < RT_LAB_TOOLS_CACHE_ENTRIES ||
        Color::cachefy.getSize() < RT_LAB_TOOLS_CACHE_ENTRIES) {
        return 1;
    }
    if (cachef_.size() != RT_LAB_TOOLS_CACHE_ENTRIES) {
        cachef_.resize(RT_LAB_TOOLS_CACHE_ENTRIES);
        cachefy_.resize(RT_LAB_TOOLS_CACHE_ENTRIES);
    }
    for (std::size_t index = 0; index < RT_LAB_TOOLS_CACHE_ENTRIES;
         ++index) {
        cachef_[index] = Color::cachef[static_cast<int>(index)];
        cachefy_[index] = Color::cachefy[static_cast<int>(index)];
    }

    /*
     * The two host-built table families: the Munsell set the vibrance
     * avoidcolorshift path and the MUN Lab-curve method read, and vibrance's
     * skin-hue curve.
     */
    static_assert(Color::MUNSELL_LUT_COUNT ==
                      static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES),
                  "the stage ABI and Color must agree on the Munsell table count");
    vibrance_active_ = labToolsVibranceActive(params_);
    skin_curve_set_ = false;

    /*
     * The Lab-curve pass. `labToolsLabCurveActive()` is the block's
     * `labCurve.enabled` with the residual contrast at zero; the six activity
     * flags come from the host LUT build below. The method is mapped here so
     * the Munsell export knows whether the pass needs the tables.
     */
    lab_curve_active_ = labToolsLabCurveActive(params_);
    lab_curve_munsell_ = RT_LAB_TOOLS_MUNSELL_NONE;
    if (lab_curve_active_) {
        if (params_.labCurve.gamutmunselmethod == "NONE") {
            lab_curve_munsell_ = RT_LAB_TOOLS_MUNSELL_NONE;
        } else if (params_.labCurve.gamutmunselmethod == "MUN") {
            lab_curve_munsell_ = RT_LAB_TOOLS_MUNSELL_MUN;
        } else {
            /* The predicate must have refused it; never guess a method. */
            return 1;
        }
    }

    const bool need_munsell =
        vibrance_active_ ||
        (lab_curve_active_ && lab_curve_munsell_ == RT_LAB_TOOLS_MUNSELL_MUN);

    if (need_munsell) {
        const std::size_t table_count = RT_LAB_TOOLS_MUNSELL_TABLES;
        const std::size_t stride = RT_LAB_TOOLS_MUNSELL_STRIDE;
        if (Color::munsellLutCount() != static_cast<int>(table_count)) {
            return 1;
        }
        if (munsell_.size() != table_count * stride) {
            munsell_.assign(table_count * stride, 0.f);
            munsell_sizes_.assign(table_count, 0.f);
        }
        for (std::size_t table = 0; table < table_count; ++table) {
            const LUTf* lut = Color::munsellLut(static_cast<int>(table));
            if (lut == nullptr || lut->getSize() == 0 ||
                lut->getSize() > stride) {
                return 1;
            }
            munsell_sizes_[table] = static_cast<float>(lut->getSize());
            for (std::size_t index = 0; index < stride; ++index) {
                munsell_[table * stride + index] =
                    index < lut->getSize()
                        ? (*lut)[static_cast<int>(index)]
                        : 0.f;
            }
        }
    }

    if (vibrance_active_) {
        /*
         * ImProcFunctions::vibrance()'s own construction: a diagonal curve,
         * kept only when it is not the identity, filled by
         * fillCurveArrayVib() and then divided by `ask`. The one-liner is
         * duplicated here because fillCurveArrayVib() is file-local in
         * ipvibrance.cc; the arithmetic is the same expression in the same
         * order.
         */
        DiagonalCurve curve(params_.vibrance.skintonescurve,
                            CURVES_MIN_POLY_POINTS);
        skin_curve_set_ = !curve.isIdentity();
        if (skin_curve_.size() != RT_LAB_TOOLS_SKIN_CURVE_ENTRIES) {
            skin_curve_.assign(RT_LAB_TOOLS_SKIN_CURVE_ENTRIES, 0.f);
        }
        if (skin_curve_set_) {
            /* ipvibrance.cc's skbeg, skend and ask. */
            constexpr float SKIN_HUE_BEGIN = -0.05f;
            constexpr float SKIN_HUE_END = 1.60f;
            const float ask = 65535.f / (SKIN_HUE_END - SKIN_HUE_BEGIN);
            for (std::size_t index = 0;
                 index < RT_LAB_TOOLS_SKIN_CURVE_ENTRIES; ++index) {
                skin_curve_[index] = static_cast<float>(
                    65535.0 * curve.getVal(static_cast<double>(index) /
                                           65535.0)) /
                    ask;
            }
        } else {
            std::fill(skin_curve_.begin(), skin_curve_.end(), 0.f);
        }
    }

    /*
     * The Lab-curve tables. The block's own CurveFactory calls build them, so
     * the device reads exactly the host's LUTs and never re-derives a curve.
     * `complexsgnCurve()` writes the a/b/cc/lc activity flags through to the
     * caller's members, as the host block does.
     */
    lab_curve_l_active_ = false;
    lab_curve_a_active_ = false;
    lab_curve_b_active_ = false;
    lab_curve_cc_active_ = false;
    lab_curve_lc_active_ = false;
    lab_curve_cl_active_ = false;
    lab_curve_lcredsk_ = params_.labCurve.lcredsk;
    lab_curve_adjust_ = 1.f;
    if (lab_curve_active_) {
        if (settings == nullptr) {
            return 1;
        }
        if (luts_.lumacurve == nullptr ||
            luts_.lumacurve->getSize() != RT_LAB_TOOLS_LUMA_LUT_ENTRIES ||
            luts_.clcurve == nullptr ||
            luts_.clcurve->getSize() != RT_LAB_TOOLS_CURVE_LUT_ENTRIES ||
            luts_.curve1 == nullptr ||
            luts_.curve1->getSize() != RT_LAB_TOOLS_CURVE_LUT_ENTRIES ||
            luts_.curve2 == nullptr ||
            luts_.curve2->getSize() != RT_LAB_TOOLS_CURVE_LUT_ENTRIES ||
            luts_.satcurve == nullptr ||
            luts_.satcurve->getSize() != RT_LAB_TOOLS_CURVE_LUT_ENTRIES ||
            luts_.lhskcurve == nullptr ||
            luts_.lhskcurve->getSize() != RT_LAB_TOOLS_CURVE_LUT_ENTRIES ||
            luts_.hist16 == nullptr || luts_.dummy == nullptr) {
            return 1;
        }

        bool utili = false;
        CurveFactory::complexLCurve(
            params_.labCurve.brightness, params_.labCurve.contrast,
            params_.labCurve.lcurve, *luts_.hist16, *luts_.lumacurve,
            *luts_.dummy, 1, utili);
        const bool clcutili = CurveFactory::diagonalCurve2Lut(
            params_.labCurve.clcurve, *luts_.clcurve, 1);
        bool ccutili = false;
        bool cclutili = false;
        CurveFactory::complexsgnCurve(
            *autili_, *butili_, ccutili, cclutili, params_.labCurve.acurve,
            params_.labCurve.bcurve, params_.labCurve.cccurve,
            params_.labCurve.lccurve, *luts_.curve1, *luts_.curve2,
            *luts_.satcurve, *luts_.lhskcurve, 1);

        const auto copy = [](const LUTf& lut, std::vector<float>& flat) {
            const std::size_t size = lut.getSize();
            if (flat.size() != size) {
                flat.resize(size);
            }
            for (std::size_t index = 0; index < size; ++index) {
                flat[index] = lut[static_cast<int>(index)];
            }
        };
        copy(*luts_.lumacurve, lab_curve_l_);
        copy(*luts_.curve1, lab_curve_a_);
        copy(*luts_.curve2, lab_curve_b_);
        copy(*luts_.satcurve, lab_curve_cc_);
        copy(*luts_.lhskcurve, lab_curve_lc_);
        copy(*luts_.clcurve, lab_curve_cl_);

        lab_curve_l_active_ = utili;
        lab_curve_a_active_ = *autili_;
        lab_curve_b_active_ = *butili_;
        lab_curve_cc_active_ = ccutili;
        lab_curve_lc_active_ = cclutili;
        lab_curve_cl_active_ = clcutili;

        /*
         * chromiLuminanceCurve()'s working-profile chroma scale, for the
         * C=f(C) and C=f(L) table index.
         */
        const auto& profile = params_.icm.workingProfile;
        if (profile == "ProPhoto") {
            lab_curve_adjust_ = 1.2f;
        } else if (profile == "Adobe RGB") {
            lab_curve_adjust_ = 1.8f;
        } else if (profile == "sRGB") {
            lab_curve_adjust_ = 2.0f;
        } else if (profile == "WideGamut") {
            lab_curve_adjust_ = 1.2f;
        } else if (profile == "Beta RGB") {
            lab_curve_adjust_ = 1.4f;
        } else if (profile == "BestRGB") {
            lab_curve_adjust_ = 1.4f;
        } else if (profile == "BruceRGB") {
            lab_curve_adjust_ = 1.8f;
        }

        /*
         * The red and skin protection inputs, read from the same globals the
         * block reads. A non-finite value would make the kernel's comparisons
         * meaningless, so the setup refuses.
         */
        lab_curve_rst_protection_ =
            static_cast<float>(100.0 - params_.labCurve.rstprotection);
        lab_curve_protect_red_ = static_cast<float>(settings->protectred);
        lab_curve_protect_red_h_ = static_cast<float>(settings->protectredh);
        lab_curve_amount_chroma_ = static_cast<float>(settings->amchroma);
        if (!std::isfinite(lab_curve_rst_protection_) ||
            !std::isfinite(lab_curve_protect_red_) ||
            !std::isfinite(lab_curve_protect_red_h_) ||
            !std::isfinite(lab_curve_amount_chroma_)) {
            return 1;
        }
    }

    out.labgrid = labgrid_;
    out.lut_highlight = lut_highlight_.data();
    out.lut_shadow = lut_shadow_.data();
    out.lut_entries = static_cast<uint32_t>(entries);
    out.ws = ws_;
    out.iws = iws_;
    out.cachef = cachef_.data();
    out.cachefy = cachefy_.data();
    out.munsell = need_munsell ? munsell_.data() : nullptr;
    out.munsell_sizes = need_munsell ? munsell_sizes_.data() : nullptr;
    out.skin_curve = vibrance_active_ ? skin_curve_.data() : nullptr;
    out.munsell_tables =
        need_munsell ? static_cast<uint32_t>(RT_LAB_TOOLS_MUNSELL_TABLES)
                     : 0u;
    out.skin_curve_set = skin_curve_set_ ? 1u : 0u;
    out.lab_curve_l = lab_curve_active_ ? lab_curve_l_.data() : nullptr;
    out.lab_curve_a = lab_curve_active_ ? lab_curve_a_.data() : nullptr;
    out.lab_curve_b = lab_curve_active_ ? lab_curve_b_.data() : nullptr;
    out.lab_curve_cc = lab_curve_active_ ? lab_curve_cc_.data() : nullptr;
    out.lab_curve_lc = lab_curve_active_ ? lab_curve_lc_.data() : nullptr;
    out.lab_curve_cl = lab_curve_active_ ? lab_curve_cl_.data() : nullptr;
    out.lab_curve_l_entries =
        lab_curve_active_
            ? static_cast<uint32_t>(RT_LAB_TOOLS_LUMA_LUT_ENTRIES)
            : 0u;
    out.lab_curve_entries =
        lab_curve_active_
            ? static_cast<uint32_t>(RT_LAB_TOOLS_CURVE_LUT_ENTRIES)
            : 0u;
    out.lab_curve_active = lab_curve_active_ ? 1u : 0u;
    out.lab_curve_l_active = lab_curve_l_active_ ? 1u : 0u;
    out.lab_curve_a_active = lab_curve_a_active_ ? 1u : 0u;
    out.lab_curve_b_active = lab_curve_b_active_ ? 1u : 0u;
    out.lab_curve_cc_active = lab_curve_cc_active_ ? 1u : 0u;
    out.lab_curve_lc_active = lab_curve_lc_active_ ? 1u : 0u;
    out.lab_curve_cl_active = lab_curve_cl_active_ ? 1u : 0u;
    out.lab_curve_munsell = lab_curve_munsell_;
    out.lab_curve_rst_protection = lab_curve_rst_protection_;
    out.lab_curve_protect_red = lab_curve_protect_red_;
    out.lab_curve_protect_red_h = lab_curve_protect_red_h_;
    out.lab_curve_amount_chroma = lab_curve_amount_chroma_;
    out.lab_curve_adjust = lab_curve_adjust_;
    out.lab_curve_lcredsk = lab_curve_lcredsk_ ? 1u : 0u;
    built_ = true;
    return 0;
}

int LabToolsContext::runHost(RtStageBuffer& lab)
{
    if (lab.data == nullptr || lab.format != RT_STAGE_F32 ||
        lab.planes != 3 || lab.width <= 0 || lab.height <= 0) {
        return 1;
    }

    LabImage image(lab.width, lab.height);
    const auto* base = static_cast<const unsigned char*>(lab.data);
    for (int y = 0; y < lab.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            const auto* row = reinterpret_cast<const float*>(
                base + plane * lab.plane_pitch + y * lab.row_pitch);
            float* dst = plane == 0 ? image.L[y]
                        : plane == 1 ? image.a[y]
                                     : image.b[y];
            std::memcpy(dst, row, static_cast<std::size_t>(lab.width) *
                                      sizeof(float));
        }
    }

    labToolsHostBlock(ipf_, params_, image, luts_, *autili_, *butili_);

    auto* out = static_cast<unsigned char*>(lab.data);
    for (int y = 0; y < lab.height; ++y) {
        for (uint32_t plane = 0; plane < 3; ++plane) {
            auto* row = reinterpret_cast<float*>(
                out + plane * lab.plane_pitch + y * lab.row_pitch);
            const float* src = plane == 0 ? image.L[y]
                               : plane == 1 ? image.a[y]
                                            : image.b[y];
            std::memcpy(row, src, static_cast<std::size_t>(lab.width) *
                                     sizeof(float));
        }
    }
    return 0;
}

} // namespace rtengine::stage
