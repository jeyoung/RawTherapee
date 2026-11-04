/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 The RawTherapee Team
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

#include "lab_sharpening_context.h"

#include "curves.h"
#include "flatcurvetypes.h"
#include "improcfun.h"
#include "labimage.h"
#include "procparams.h"
#include "stage_buffer.h"
#include "stage_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace rtengine::stage
{

namespace
{

bool tight_plane(const RtStageBuffer& buffer)
{
    if (buffer.size != sizeof(RtStageBuffer) || buffer.data == nullptr ||
        buffer.format != RT_STAGE_F32 || buffer.planes != 1 ||
        buffer.width <= 0 || buffer.height <= 0) {
        return false;
    }
    const std::size_t row =
        static_cast<std::size_t>(buffer.width) * sizeof(float);
    return buffer.row_pitch == row &&
           buffer.plane_pitch == row * static_cast<std::size_t>(buffer.height);
}

RtStageBuffer plane_buffer(float* data, int width, int height)
{
    RtStageBuffer buffer{};
    buffer.size = sizeof(RtStageBuffer);
    buffer.data = data;
    buffer.width = width;
    buffer.height = height;
    buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
    buffer.plane_pitch =
        buffer.row_pitch * static_cast<std::size_t>(height);
    buffer.planes = 1;
    buffer.format = RT_STAGE_F32;
    buffer.flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;
    return buffer;
}

/*
 * The defringe hue table. FlatCurve::getVal() returns a piecewise-linear
 * polyline, so a uniform sample with linear interpolation follows it to well
 * below float precision. The builder measures the worst interpolation error
 * and refuses a curve that would move the fringe decision.
 */
class HueTableBuilder
{
public:
    HueTableBuilder(const FlatCurve& curve, std::vector<float>& table)
        : curve_(curve), table_(table)
    {
    }

    bool build(std::string& error)
    {
        const int entries = static_cast<int>(RT_LAB_SHARPENING_HUE_ENTRIES);
        table_.resize(static_cast<std::size_t>(entries));
        for (int i = 0; i < entries; ++i) {
            table_[i] = static_cast<float>(
                curve_.getVal(static_cast<double>(i) /
                              static_cast<double>(entries - 1)));
        }

        /*
         * Independent sample points: the table nodes and their midpoints are
         * valid for a linear function by construction, so they would hide a
         * curve sampled too coarsely.
         */
        double worst = 0.0;
        const std::size_t checks = RT_LAB_SHARPENING_HUE_CHECK_POINTS;
        for (std::size_t i = 0; i < checks; ++i) {
            const double t =
                (static_cast<double>(i) + 0.3819660112501051) /
                static_cast<double>(checks);
            worst = std::max(worst, std::fabs(lookup(t) - curve_.getVal(t)));
        }
        if (worst > RT_LAB_SHARPENING_HUE_MAX_ERROR) {
            error = "lab.sharpening: the defringe hue curve is too steep for "
                    "the device table";
            return false;
        }
        return true;
    }

    int entries() const
    {
        return static_cast<int>(RT_LAB_SHARPENING_HUE_ENTRIES);
    }

private:
    double lookup(double t) const
    {
        const int entries = static_cast<int>(RT_LAB_SHARPENING_HUE_ENTRIES);
        const double pos = t * static_cast<double>(entries - 1);
        int index = static_cast<int>(pos);
        if (index < 0) {
            index = 0;
        }
        if (index > entries - 2) {
            index = entries - 2;
        }
        const double fraction = pos - static_cast<double>(index);
        const double y0 = static_cast<double>(table_[index]);
        const double y1 = static_cast<double>(table_[index + 1]);
        return y0 + (y1 - y0) * fraction;
    }

    const FlatCurve& curve_;
    std::vector<float>& table_;
};

void copy_plane_in(const RtStageBuffer& buffer, float** plane)
{
    for (int y = 0; y < buffer.height; ++y) {
        std::memcpy(plane[y], static_cast<const float*>(buffer.data) +
                                static_cast<std::size_t>(y) * buffer.width,
                    static_cast<std::size_t>(buffer.width) * sizeof(float));
    }
}

void copy_plane_out(const float* const* plane, RtStageBuffer& buffer)
{
    for (int y = 0; y < buffer.height; ++y) {
        std::memcpy(static_cast<float*>(buffer.data) +
                        static_cast<std::size_t>(y) * buffer.width,
                    plane[y],
                    static_cast<std::size_t>(buffer.width) * sizeof(float));
    }
}

} // namespace

bool lab_sharpening_decode(const RtStageArg* args, uint32_t nargs,
                           LabSharpeningNodeParams& params,
                           RtStageBuffer& luminance, RtStageBuffer& a,
                           RtStageBuffer& b, RtStageBuffer& hue,
                           std::string& error)
{
    if (args == nullptr || nargs != LAB_SHARPENING_NODE_ARGS) {
        error = "lab.sharpening expects " +
                std::to_string(LAB_SHARPENING_NODE_ARGS) + " arguments";
        return false;
    }

    const auto is_int = [args](LabSharpeningArg index) {
        return args[index].kind == RT_STAGE_ARG_INT;
    };

    const LabSharpeningArg buffers[] = {LAB_SHARPENING_ARG_LUMINANCE,
                                        LAB_SHARPENING_ARG_A,
                                        LAB_SHARPENING_ARG_B};
    for (LabSharpeningArg index : buffers) {
        if (args[index].kind != RT_STAGE_ARG_BUFFER) {
            error = "lab.sharpening expects three Lab planes first";
            return false;
        }
    }
    luminance = args[LAB_SHARPENING_ARG_LUMINANCE].u.buffer;
    a = args[LAB_SHARPENING_ARG_A].u.buffer;
    b = args[LAB_SHARPENING_ARG_B].u.buffer;
    if (!tight_plane(luminance) || !tight_plane(a) || !tight_plane(b)) {
        error = "lab.sharpening needs tightly packed one-plane F32 L, a and b "
                "buffers";
        return false;
    }
    if (luminance.width != a.width || luminance.width != b.width ||
        luminance.height != a.height || luminance.height != b.height) {
        error = "lab.sharpening needs three planes of the same size";
        return false;
    }

    const LabSharpeningArg intArgs[] = {
        LAB_SHARPENING_ARG_IMPULSE_ENABLED,
        LAB_SHARPENING_ARG_IMPULSE_THRESHOLD,
        LAB_SHARPENING_ARG_DEFRINGE_ENABLED,
        LAB_SHARPENING_ARG_DEFRINGE_THRESHOLD,
        LAB_SHARPENING_ARG_HUE_ENTRIES,
        LAB_SHARPENING_ARG_HUE_CURVE,
        LAB_SHARPENING_ARG_EDGES_ENABLED,
        LAB_SHARPENING_ARG_EDGES_PASSES,
        LAB_SHARPENING_ARG_EDGES_THREECHANNELS,
        LAB_SHARPENING_ARG_MICRO_ENABLED,
        LAB_SHARPENING_ARG_MICRO_MATRIX,
        LAB_SHARPENING_ARG_MICRO_UNIFORMITY};
    for (LabSharpeningArg index : intArgs) {
        if (!is_int(index)) {
            error = "lab.sharpening argument " + std::to_string(index) +
                    " is not an int";
            return false;
        }
    }
    const LabSharpeningArg doubleArgs[] = {
        LAB_SHARPENING_ARG_DEFRINGE_RADIUS,
        LAB_SHARPENING_ARG_EDGES_AMOUNT,
        LAB_SHARPENING_ARG_MICRO_AMOUNT,
        LAB_SHARPENING_ARG_MICRO_CONTRAST,
        LAB_SHARPENING_ARG_SCALE};
    for (LabSharpeningArg index : doubleArgs) {
        if (args[index].kind != RT_STAGE_ARG_DOUBLE) {
            error = "lab.sharpening argument " + std::to_string(index) +
                    " is not a double";
            return false;
        }
    }
    if (args[LAB_SHARPENING_ARG_HUE_TABLE].kind != RT_STAGE_ARG_BUFFER ||
        args[LAB_SHARPENING_ARG_CONTEXT].kind != RT_STAGE_ARG_PTR) {
        error = "lab.sharpening expects a hue table and a host context";
        return false;
    }

    const auto flag = [args](LabSharpeningArg index) -> int {
        const std::int64_t value = args[index].u.i;
        if (value != 0 && value != 1) {
            return -1;
        }
        return static_cast<int>(value);
    };
    params.impulse_enabled = flag(LAB_SHARPENING_ARG_IMPULSE_ENABLED);
    params.defringe_enabled = flag(LAB_SHARPENING_ARG_DEFRINGE_ENABLED);
    params.hue_curve = flag(LAB_SHARPENING_ARG_HUE_CURVE);
    params.edges_enabled = flag(LAB_SHARPENING_ARG_EDGES_ENABLED);
    params.edges_threechannels =
        flag(LAB_SHARPENING_ARG_EDGES_THREECHANNELS);
    params.micro_enabled = flag(LAB_SHARPENING_ARG_MICRO_ENABLED);
    params.micro_matrix = flag(LAB_SHARPENING_ARG_MICRO_MATRIX);
    if (params.impulse_enabled < 0 || params.defringe_enabled < 0 ||
        params.hue_curve < 0 || params.edges_enabled < 0 ||
        params.edges_threechannels < 0 || params.micro_enabled < 0 ||
        params.micro_matrix < 0) {
        error = "lab.sharpening flags must be 0 or 1";
        return false;
    }
    params.impulse_threshold =
        static_cast<int>(args[LAB_SHARPENING_ARG_IMPULSE_THRESHOLD].u.i);
    params.defringe_threshold =
        static_cast<int>(args[LAB_SHARPENING_ARG_DEFRINGE_THRESHOLD].u.i);
    params.defringe_radius = args[LAB_SHARPENING_ARG_DEFRINGE_RADIUS].u.f;
    params.edges_passes =
        static_cast<int>(args[LAB_SHARPENING_ARG_EDGES_PASSES].u.i);
    params.edges_amount = args[LAB_SHARPENING_ARG_EDGES_AMOUNT].u.f;
    params.micro_amount = args[LAB_SHARPENING_ARG_MICRO_AMOUNT].u.f;
    params.micro_contrast = args[LAB_SHARPENING_ARG_MICRO_CONTRAST].u.f;
    params.micro_uniformity =
        static_cast<int>(args[LAB_SHARPENING_ARG_MICRO_UNIFORMITY].u.i);
    params.scale = args[LAB_SHARPENING_ARG_SCALE].u.f;

    hue = args[LAB_SHARPENING_ARG_HUE_TABLE].u.buffer;
    const std::int64_t hue_entries =
        args[LAB_SHARPENING_ARG_HUE_ENTRIES].u.i;
    if (!tight_plane(hue) || hue.height != 1 || hue_entries <= 0 ||
        hue.width != hue_entries) {
        error = "lab.sharpening needs a packed F32 hue table of "
                "'HUE_ENTRIES' entries";
        return false;
    }

    return true;
}

bool lab_sharpening_admitted(const LabSharpeningNodeParams& params, int width,
                             int height, bool has_fp64, std::string& error)
{
    if (width < 8 || height < 8) {
        error = "lab.sharpening needs at least 8x8 pixels";
        return false;
    }
    /*
     * The export prescales the impulse threshold and defringe radius before
     * the block runs. The scale itself must be usable: a non-positive or
     * non-finite factor cannot describe a resize, and above the point where
     * the widest GUI radius reaches gaussianBlur()'s double-precision IIR
     * limit the host and device recurrences differ, so the block is a host
     * residual.
     */
    if (!std::isfinite(params.scale) ||
        params.scale <= LAB_SHARPENING_MIN_SCALE ||
        params.scale > LAB_SHARPENING_MAX_SCALE) {
        error = "lab.sharpening export scale is outside the device range";
        return false;
    }
    if (params.impulse_enabled == 0 && params.defringe_enabled == 0 &&
        params.edges_enabled == 0 && params.micro_enabled == 0) {
        error = "lab.sharpening has no enabled tool";
        return false;
    }
    /*
     * adjust_procparams() multiplies the GUI threshold by the scale and
     * truncates the product into the int field, so the prescaled maximum is
     * the same truncation of the GUI maximum. A legal GUI value therefore
     * stays inside the derived bound at every supported scale.
     */
    if (params.impulse_enabled != 0 &&
        (params.impulse_threshold <
             static_cast<int>(LAB_SHARPENING_MIN_THRESHOLD * params.scale) ||
         params.impulse_threshold >
             static_cast<int>(LAB_SHARPENING_MAX_THRESHOLD * params.scale))) {
        error = "lab.sharpening impulse threshold is outside 0..100 * scale";
        return false;
    }
    if (params.edges_enabled != 0) {
        if (params.edges_passes < LAB_SHARPENING_EDGES_MIN_PASSES ||
            params.edges_passes > LAB_SHARPENING_EDGES_MAX_PASSES) {
            error = "lab.sharpening edge pass count is outside 0..4";
            return false;
        }
        if (!std::isfinite(params.edges_amount) ||
            params.edges_amount < LAB_SHARPENING_EDGES_MIN_AMOUNT ||
            params.edges_amount > LAB_SHARPENING_EDGES_MAX_AMOUNT) {
            error = "lab.sharpening edge amount is outside 0..100";
            return false;
        }
    }
    if (params.micro_enabled != 0) {
        if (!std::isfinite(params.micro_amount) ||
            params.micro_amount < LAB_SHARPENING_MICRO_MIN_AMOUNT ||
            params.micro_amount > LAB_SHARPENING_MICRO_MAX_AMOUNT) {
            error = "lab.sharpening microcontrast amount is outside 0..100";
            return false;
        }
        if (!std::isfinite(params.micro_contrast) ||
            params.micro_contrast < LAB_SHARPENING_MICRO_MIN_CONTRAST ||
            params.micro_contrast > LAB_SHARPENING_MICRO_MAX_CONTRAST) {
            error = "lab.sharpening microcontrast contrast is outside 0..200";
            return false;
        }
        if (params.micro_uniformity < LAB_SHARPENING_MICRO_MIN_UNIFORMITY ||
            params.micro_uniformity > LAB_SHARPENING_MICRO_MAX_UNIFORMITY) {
            error = "lab.sharpening microcontrast uniformity is outside 0..10";
            return false;
        }
    }
    if (params.defringe_enabled == 0) {
        return true;
    }

    /*
     * adjust_procparams() maps the GUI radius r to
     * 2 + (r - 2) * scale. Invert that mapping into the same bounds so a
     * prescaled radius inside the GUI range is admitted while a value the
     * export could not have produced from a GUI radius stays a residual. The
     * lower bound uses the admitted no-op floor (0), not the GUI's 0.5,
     * because a stored radius below 0.5 still runs on the host.
     */
    const double radius_lo =
        LAB_SHARPENING_DEFRINGE_DEFAULT_RADIUS +
        (LAB_SHARPENING_MIN_RADIUS -
         LAB_SHARPENING_DEFRINGE_DEFAULT_RADIUS) *
            params.scale;
    const double radius_hi =
        LAB_SHARPENING_DEFRINGE_DEFAULT_RADIUS +
        (LAB_SHARPENING_GUI_MAX_RADIUS -
         LAB_SHARPENING_DEFRINGE_DEFAULT_RADIUS) *
            params.scale;
    if (!std::isfinite(params.defringe_radius) ||
        params.defringe_radius < radius_lo - LAB_SHARPENING_SCALE_EPSILON ||
        params.defringe_radius > radius_hi + LAB_SHARPENING_SCALE_EPSILON ||
        params.defringe_radius >= LAB_SHARPENING_MAX_RADIUS) {
        error = "lab.sharpening defringe radius is outside the device range";
        return false;
    }
    if (params.defringe_threshold < LAB_SHARPENING_MIN_THRESHOLD ||
        params.defringe_threshold > LAB_SHARPENING_MAX_THRESHOLD) {
        error = "lab.sharpening defringe threshold is outside 0..100";
        return false;
    }
    if (params.defringe_radius < LAB_SHARPENING_RADIUS_NOOP) {
        /* gaussianBlur() copies the plane, the chromaticity is zero and the
         * host early-out leaves the image untouched. */
        return true;
    }
    if (!has_fp64) {
        error = "lab.sharpening defringe needs device fp64 for its double "
                "chromaticity mean";
        return false;
    }
    /*
     * The host's defringe windows read x+halfwin-1 without a width clamp in
     * the left tail. Requiring the CPU-safe size keeps the device from reading
     * past the plane for the same configuration.
     */
    const double halfwin_value = std::ceil(2.0 * params.defringe_radius) + 1.0;
    if (!(halfwin_value <
          static_cast<double>(std::numeric_limits<int>::max()))) {
        error = "lab.sharpening defringe radius is too large";
        return false;
    }
    const int halfwin = static_cast<int>(halfwin_value);
    if (width < 2 * halfwin - 2 || height < 2 * halfwin - 2) {
        error = "lab.sharpening defringe window does not fit the image";
        return false;
    }
    return true;
}

bool lab_sharpening_active(const LabSharpeningNodeParams& params)
{
    /* An enabled tool is active even when the host would turn it into a
     * no-op: the node still has to reproduce that no-op, and treating it as
     * inactive would move the decision out of the device set. */
    return params.impulse_enabled != 0 || params.defringe_enabled != 0 ||
           params.edges_enabled != 0 || params.micro_enabled != 0;
}

void labSharpeningScaleParams(procparams::ProcParams& params, double scale)
{
    /*
     * The production operator is `int *= double`, which truncates towards
     * zero. Keep the same assignment here rather than an explicit cast so the
     * call site and the admission bound cannot drift apart.
     */
    params.impulseDenoise.thresh *= scale;
    params.defringe.radius =
        LAB_SHARPENING_DEFRINGE_DEFAULT_RADIUS +
        (params.defringe.radius - LAB_SHARPENING_DEFRINGE_DEFAULT_RADIUS) *
            scale;
    if (scale < LAB_SHARPENING_IMPULSE_MIN_SCALE) {
        params.impulseDenoise.enabled = false;
    }
}

LabSharpeningNodeParams
labSharpeningNodeParams(const procparams::ProcParams& params,
                        bool ciecam_arm, double scale)
{
    LabSharpeningNodeParams node;
    /* The CIE-CAM pipeline owns these tools on the CieImage. ML sharpen edges
     * is a Lab pass in both arms, so it stays enabled. */
    node.impulse_enabled =
        !ciecam_arm && params.impulseDenoise.enabled ? 1 : 0;
    node.impulse_threshold = params.impulseDenoise.thresh;
    node.defringe_enabled =
        !ciecam_arm && params.defringe.enabled ? 1 : 0;
    node.defringe_radius = params.defringe.radius;
    node.defringe_threshold = params.defringe.threshold;
    node.scale = scale;
    /*
     * The active hue curve is the block's own condition: a stored curve whose
     * type is beyond FCT_Linear.
     */
    if (params.defringe.huecurve.size() &&
        FlatCurveType(params.defringe.huecurve[0]) > FCT_Linear) {
        node.hue_curve = 1;
    }
    node.edges_enabled = params.sharpenEdge.enabled ? 1 : 0;
    node.edges_passes = params.sharpenEdge.passes;
    node.edges_amount = params.sharpenEdge.amount;
    node.edges_threechannels = params.sharpenEdge.threechannels ? 1 : 0;
    node.micro_enabled =
        !ciecam_arm && params.sharpenMicro.enabled ? 1 : 0;
    node.micro_matrix = params.sharpenMicro.matrix ? 1 : 0;
    node.micro_amount = params.sharpenMicro.amount;
    node.micro_contrast = params.sharpenMicro.contrast;
    node.micro_uniformity = params.sharpenMicro.uniformity;
    return node;
}

void labSharpeningArgs(LabImage& lab, const LabSharpeningNodeParams& node,
                       LabSharpeningContext& context,
                       RtLabSharpeningContext& descriptor,
                       std::vector<RtStageArg>& args)
{
    args.assign(LAB_SHARPENING_NODE_ARGS, RtStageArg{});
    args[LAB_SHARPENING_ARG_LUMINANCE].kind = RT_STAGE_ARG_BUFFER;
    args[LAB_SHARPENING_ARG_LUMINANCE].u.buffer =
        plane_buffer(lab.L[0], lab.W, lab.H);
    args[LAB_SHARPENING_ARG_A].kind = RT_STAGE_ARG_BUFFER;
    args[LAB_SHARPENING_ARG_A].u.buffer = plane_buffer(lab.a[0], lab.W, lab.H);
    args[LAB_SHARPENING_ARG_B].kind = RT_STAGE_ARG_BUFFER;
    args[LAB_SHARPENING_ARG_B].u.buffer = plane_buffer(lab.b[0], lab.W, lab.H);
    args[LAB_SHARPENING_ARG_IMPULSE_ENABLED].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_IMPULSE_ENABLED].u.i = node.impulse_enabled;
    args[LAB_SHARPENING_ARG_IMPULSE_THRESHOLD].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_IMPULSE_THRESHOLD].u.i = node.impulse_threshold;
    args[LAB_SHARPENING_ARG_DEFRINGE_ENABLED].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_DEFRINGE_ENABLED].u.i = node.defringe_enabled;
    args[LAB_SHARPENING_ARG_DEFRINGE_RADIUS].kind = RT_STAGE_ARG_DOUBLE;
    args[LAB_SHARPENING_ARG_DEFRINGE_RADIUS].u.f = node.defringe_radius;
    args[LAB_SHARPENING_ARG_DEFRINGE_THRESHOLD].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_DEFRINGE_THRESHOLD].u.i = node.defringe_threshold;

    RtStageBuffer hue{};
    hue.size = sizeof(RtStageBuffer);
    hue.data = const_cast<float*>(context.hueTable().data());
    hue.width = context.hueEntries();
    hue.height = 1;
    hue.row_pitch = static_cast<std::size_t>(hue.width) * sizeof(float);
    hue.plane_pitch = hue.row_pitch;
    hue.planes = 1;
    hue.format = RT_STAGE_F32;
    hue.flags = RT_STAGE_BUFFER_READ;
    args[LAB_SHARPENING_ARG_HUE_TABLE].kind = RT_STAGE_ARG_BUFFER;
    args[LAB_SHARPENING_ARG_HUE_TABLE].u.buffer = hue;
    args[LAB_SHARPENING_ARG_HUE_ENTRIES].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_HUE_ENTRIES].u.i = context.hueEntries();
    /* The flag describes the table that was actually built, not the profile:
     * a pass that cannot read it must not make the launcher upload one. */
    args[LAB_SHARPENING_ARG_HUE_CURVE].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_HUE_CURVE].u.i = context.hueCurve() ? 1 : 0;
    args[LAB_SHARPENING_ARG_EDGES_ENABLED].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_EDGES_ENABLED].u.i = node.edges_enabled;
    args[LAB_SHARPENING_ARG_EDGES_PASSES].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_EDGES_PASSES].u.i = node.edges_passes;
    args[LAB_SHARPENING_ARG_EDGES_AMOUNT].kind = RT_STAGE_ARG_DOUBLE;
    args[LAB_SHARPENING_ARG_EDGES_AMOUNT].u.f = node.edges_amount;
    args[LAB_SHARPENING_ARG_EDGES_THREECHANNELS].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_EDGES_THREECHANNELS].u.i =
        node.edges_threechannels;
    args[LAB_SHARPENING_ARG_MICRO_ENABLED].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_MICRO_ENABLED].u.i = node.micro_enabled;
    args[LAB_SHARPENING_ARG_MICRO_MATRIX].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_MICRO_MATRIX].u.i = node.micro_matrix;
    args[LAB_SHARPENING_ARG_MICRO_AMOUNT].kind = RT_STAGE_ARG_DOUBLE;
    args[LAB_SHARPENING_ARG_MICRO_AMOUNT].u.f = node.micro_amount;
    args[LAB_SHARPENING_ARG_MICRO_CONTRAST].kind = RT_STAGE_ARG_DOUBLE;
    args[LAB_SHARPENING_ARG_MICRO_CONTRAST].u.f = node.micro_contrast;
    args[LAB_SHARPENING_ARG_MICRO_UNIFORMITY].kind = RT_STAGE_ARG_INT;
    args[LAB_SHARPENING_ARG_MICRO_UNIFORMITY].u.i = node.micro_uniformity;
    args[LAB_SHARPENING_ARG_SCALE].kind = RT_STAGE_ARG_DOUBLE;
    args[LAB_SHARPENING_ARG_SCALE].u.f = node.scale;
    args[LAB_SHARPENING_ARG_CONTEXT].kind = RT_STAGE_ARG_PTR;
    args[LAB_SHARPENING_ARG_CONTEXT].u.ptr = &descriptor;
}

bool labSharpeningSupported(const procparams::ProcParams& params, int width,
                            int height, bool ciecam_arm, std::string* error,
                            double scale)
{
    const auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const LabSharpeningNodeParams node =
        labSharpeningNodeParams(params, ciecam_arm, scale);
    if (!lab_sharpening_active(node)) {
        return fail("lab.sharpening has no tool that would change the image");
    }
    std::string reason;
    if (!lab_sharpening_admitted(node, width, height, true, reason)) {
        return fail(reason.c_str());
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void labSharpeningHostBlock(ImProcFunctions& ipf,
                            procparams::ProcParams& params, LabImage& lab,
                            bool ciecam_arm)
{
    (void)params;
    /* The same order as the call sites had before the device node existed. */
    if (!ciecam_arm) {
        ipf.impulsedenoise(&lab);
        ipf.defringe(&lab);
    }
    ipf.MLsharpen(&lab);
    if (!ciecam_arm) {
        ipf.MLmicrocontrast(&lab);
    }
}

void labSharpeningRun(ImProcFunctions& ipf, procparams::ProcParams& params,
                      LabImage& lab, bool ciecam_arm, double scale)
{
    LabSharpeningNodeParams node =
        labSharpeningNodeParams(params, ciecam_arm, scale);
    if (!lab_sharpening_active(node)) {
        return;
    }

    if (node_device("lab.sharpening") != StageNodeDevice::OpenCL) {
        labSharpeningHostBlock(ipf, params, lab, ciecam_arm);
        return;
    }

    LabSharpeningContext context(params, ipf, lab.W, lab.H, ciecam_arm);
    std::string error;
    if (context.build(&error) != 0) {
        labSharpeningHostBlock(ipf, params, lab, ciecam_arm);
        return;
    }

    std::string support_error;
    const bool supported = labSharpeningSupported(
        params, lab.W, lab.H, ciecam_arm, &support_error, scale);
    RtLabSharpeningContext descriptor = context.descriptor(supported);

    std::vector<RtStageArg> args;
    labSharpeningArgs(lab, node, context, descriptor, args);

    const RtStageStatus status = run("lab.sharpening", args);
    if (status != RT_STAGE_OK) {
        labSharpeningHostBlock(ipf, params, lab, ciecam_arm);
    }
}

LabSharpeningContext::LabSharpeningContext(procparams::ProcParams& params,
                                           ImProcFunctions& ipf, int width,
                                           int height, bool ciecam_arm)
    : params_(params), ipf_(ipf), width_(width), height_(height),
      ciecam_arm_(ciecam_arm)
{
}

LabSharpeningContext::~LabSharpeningContext() = default;

int LabSharpeningContext::build(std::string* error)
{
    hue_table_.clear();
    hue_entries_ = 0;
    hue_curve_ = false;

    /*
     * The table is only read by a defringe pass that runs on the device. In
     * the CIE-CAM arm the pass belongs to the CIE-CAM pipeline on the
     * CieImage, and below the host's Gaussian identity it is a no-op, so the
     * profile's curve must not refuse the tools that do run here.
     */
    const procparams::DefringeParams& defringe = params_.defringe;
    const bool table_needed =
        !ciecam_arm_ && defringe.enabled &&
        defringe.radius >= LAB_SHARPENING_RADIUS_NOOP;
    if (table_needed && defringe.huecurve.size() &&
        FlatCurveType(defringe.huecurve[0]) > FCT_Linear) {
        FlatCurve curve(defringe.huecurve);
        std::string reason;
        HueTableBuilder builder(curve, hue_table_);
        if (!builder.build(reason)) {
            if (error != nullptr) {
                *error = reason;
            }
            hue_table_.clear();
            return 1;
        }
        hue_entries_ = builder.entries();
        hue_curve_ = true;
        return 0;
    }

    /* The device ignores the table without an active curve; two identity
     * entries keep the buffer descriptor and the lookup valid. */
    hue_table_.assign(2, 0.5f);
    hue_entries_ = 2;
    return 0;
}

int LabSharpeningContext::runHostThunk(void* self, RtStageBuffer* luminance,
                                       RtStageBuffer* a, RtStageBuffer* b)
{
    return static_cast<LabSharpeningContext*>(self)->runHost(luminance, a, b);
}

RtLabSharpeningContext LabSharpeningContext::descriptor(bool supported)
{
    RtLabSharpeningContext context{};
    context.size = sizeof(RtLabSharpeningContext);
    context.version = RT_LAB_SHARPENING_CONTEXT_VERSION;
    context.magic = RT_LAB_SHARPENING_CONTEXT_MAGIC;
    context.supported = supported ? 1u : 0u;
    context.self = this;
    context.run_host = &LabSharpeningContext::runHostThunk;
    return context;
}

int LabSharpeningContext::runHost(RtStageBuffer* luminance, RtStageBuffer* a,
                                  RtStageBuffer* b)
{
    if (luminance == nullptr || a == nullptr || b == nullptr ||
        !tight_plane(*luminance) || !tight_plane(*a) || !tight_plane(*b) ||
        luminance->width != a->width || luminance->width != b->width ||
        luminance->height != a->height || luminance->height != b->height) {
        return 1;
    }

    LabImage lab(luminance->width, luminance->height);
    copy_plane_in(*luminance, lab.L);
    copy_plane_in(*a, lab.a);
    copy_plane_in(*b, lab.b);

    labSharpeningHostBlock(ipf_, params_, lab, ciecam_arm_);

    copy_plane_out(lab.L, *luminance);
    copy_plane_out(lab.a, *a);
    copy_plane_out(lab.b, *b);
    return 0;
}

} // namespace rtengine::stage
