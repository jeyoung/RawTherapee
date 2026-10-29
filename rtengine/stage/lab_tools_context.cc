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

} // namespace

bool labToolsShadowsHighlightsActive(const procparams::ProcParams& params)
{
    return params.sh.enabled && (params.sh.highlights > 0 || params.sh.shadows > 0);
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
     * Lab curves own the histogram refill and the Lab LUTs, and
     * chromiLuminanceCurve() applies them. A contrast value with the curves
     * switched off still refills hist16, which a later block reads, so it is a
     * residual too.
     */
    if (params.labCurve.enabled || params.labCurve.contrast != 0) {
        return false;
    }

    if (params.localContrast.enabled || params.epd.enabled ||
        params.vibrance.enabled) {
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
           labToolsShadowsHighlightsActive(params);
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

    out.labgrid = labgrid_;
    out.lut_highlight = lut_highlight_.data();
    out.lut_shadow = lut_shadow_.data();
    out.lut_entries = static_cast<uint32_t>(entries);
    out.ws = ws_;
    out.iws = iws_;
    out.cachef = cachef_.data();
    out.cachefy = cachefy_.data();
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
