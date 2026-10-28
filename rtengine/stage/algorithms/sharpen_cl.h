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
#pragma once

#include "procparams.h"
#include "stage/stage_abi.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

/*
 * Device kernels for the unsupported-mask path. They are compiled as one
 * program and entered separately, so the node costs one build.
 *
 * The threshold evaluation uses double when the device advertises
 * cl_khr_fp64, mirroring Threshold::multiply(), and degrades to float
 * otherwise. The latter still only affects the final rounding of one sample.
 */
inline constexpr const char* RT_SHARPEN_CL = R"CLC(
#ifdef cl_khr_fp64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
typedef double rt_sharpen_real;
#else
typedef float rt_sharpen_real;
#endif

/* The CPU truncates the difference at this magnitude. */
#define RT_SHARPEN_THRESHOLD_UPPER_BOUND 2000.0f

/* buildBlendMask() with autoContrast off, including its border replication. */
__kernel void rt_sharpen_mask(__global const float* src,
                              __global float* mask,
                              const int width,
                              const int height,
                              const int pitch,
                              const float threshold,
                              const float scale)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    if (threshold == 0.0f) {
        mask[y * pitch + x] = 1.0f;
        return;
    }

    const int sx = x < 2 ? 2 : (x >= width - 2 ? width - 3 : x);
    const int sy = y < 2 ? 2 : (y >= height - 2 ? height - 3 : y);
    const int base = sy * pitch + sx;
    const float dx1 = src[base + 1] - src[base - 1];
    const float dy1 = src[base + pitch] - src[base - pitch];
    const float dx2 = src[base + 2] - src[base - 2];
    const float dy2 = src[base + 2 * pitch] - src[base - 2 * pitch];
    const float gradient = sqrt(dx1 * dx1 + dy1 * dy1 + dx2 * dx2 +
                                dy2 * dy2) * scale;
    const float xv = -16.0f + (16.0f / threshold) * gradient;
    mask[y * pitch + x] = 0.5f * (1.0f + xv / sqrt(1.0f + xv * xv));
}

/*
 * One 1-D Young-van Vliet IIR line in place at the destination. Horizontal
 * lines advance by one sample and start `pitch` samples apart; vertical lines
 * advance by `pitch` and start one sample apart. The coefficients arrive as
 * floats that the host derived in double.
 */
__kernel void rt_sharpen_iir_line(__global const float* src,
                                  __global float* dst,
                                  const int length,
                                  const int src_base_step,
                                  const int dst_base_step,
                                  const int src_stride,
                                  const int dst_stride,
                                  const float B,
                                  const float b1,
                                  const float b2,
                                  const float b3,
                                  const float m00,
                                  const float m01,
                                  const float m02,
                                  const float m10,
                                  const float m11,
                                  const float m12,
                                  const float m20,
                                  const float m21,
                                  const float m22)
{
    const int line = (int)get_global_id(0);
    const int sb = line * src_base_step;
    const int db = line * dst_base_step;

    const float s0 = src[sb];
    const float t0 = s0 * (B + b1 + b2 + b3);
    dst[db] = t0;
    const float t1 = B * src[sb + src_stride] + b1 * t0 + s0 * (b2 + b3);
    dst[db + dst_stride] = t1;
    const float t2 = B * src[sb + 2 * src_stride] + b1 * t1 + b2 * t0 +
                     b3 * s0;
    dst[db + 2 * dst_stride] = t2;

    for (int j = 3; j < length; ++j) {
        dst[db + j * dst_stride] =
            B * src[sb + j * src_stride] +
            b1 * dst[db + (j - 1) * dst_stride] +
            b2 * dst[db + (j - 2) * dst_stride] +
            b3 * dst[db + (j - 3) * dst_stride];
    }

    const int last = length - 1;
    const float sW = src[sb + last * src_stride];
    const float f1 = dst[db + last * dst_stride];
    const float f2 = dst[db + (last - 1) * dst_stride];
    const float f3 = dst[db + (last - 2) * dst_stride];
    const float c0 = sW + m00 * (f1 - sW) + m01 * (f2 - sW) + m02 * (f3 - sW);
    const float c1 = sW + m10 * (f1 - sW) + m11 * (f2 - sW) + m12 * (f3 - sW);
    const float c2 = sW + m20 * (f1 - sW) + m21 * (f2 - sW) + m22 * (f3 - sW);

    dst[db + last * dst_stride] = c0;
    dst[db + (last - 1) * dst_stride] =
        B * f2 + b1 * dst[db + last * dst_stride] + b2 * c1 + b3 * c2;
    dst[db + (last - 2) * dst_stride] =
        B * f3 + b1 * dst[db + (last - 1) * dst_stride] +
        b2 * dst[db + last * dst_stride] + b3 * c1;

    for (int j = last - 3; j >= 0; --j) {
        dst[db + j * dst_stride] =
            B * dst[db + j * dst_stride] +
            b1 * dst[db + (j + 1) * dst_stride] +
            b2 * dst[db + (j + 2) * dst_stride] +
            b3 * dst[db + (j + 3) * dst_stride];
    }
}

/* gauss3x3() for distinct source and destination, including its borders. */
__kernel void rt_sharpen_fir3(__global const float* src,
                              __global float* dst,
                              const int width,
                              const int height,
                              const int src_pitch,
                              const int dst_pitch,
                              const float c0,
                              const float c1,
                              const float c2,
                              const float b0,
                              const float b1)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int src_index = y * src_pitch + x;
    const int dst_index = y * dst_pitch + x;
    float value;
    if (y == 0 || y == height - 1) {
        if (x == 0 || x == width - 1) {
            value = src[src_index];
        } else {
            value = b1 * (src[src_index - 1] + src[src_index + 1]) +
                    b0 * src[src_index];
        }
    } else if (x == 0 || x == width - 1) {
        value = b1 * (src[src_index - src_pitch] +
                      src[src_index + src_pitch]) +
                b0 * src[src_index];
    } else {
        value = c2 * (src[src_index - src_pitch - 1] +
                      src[src_index - src_pitch + 1] +
                      src[src_index + src_pitch - 1] +
                      src[src_index + src_pitch + 1]) +
                c1 * (src[src_index - src_pitch] +
                      src[src_index - 1] +
                      src[src_index + 1] +
                      src[src_index + src_pitch]) +
                c0 * src[src_index];
    }
    dst[dst_index] = value;
}

/* Threshold::multiply() with its branch order and double evaluation. */
float rt_sharpen_threshold(const float x,
                           const float y_max,
                           const int bl,
                           const int tl,
                           const int br,
                           const int tr,
                           const int is_double,
                           const int start_at_one)
{
    const rt_sharpen_real val = (rt_sharpen_real)x;
    const rt_sharpen_real y = (rt_sharpen_real)y_max;

    if (start_at_one) {
        if (is_double) {
            if (val == (rt_sharpen_real)br &&
                (rt_sharpen_real)br == (rt_sharpen_real)tr) {
                return 0.0f;
            }
            if (val >= (rt_sharpen_real)tr) {
                return y_max;
            }
            if (val > (rt_sharpen_real)br) {
                return (float)(y * (val - (rt_sharpen_real)br) /
                               ((rt_sharpen_real)tr - (rt_sharpen_real)br));
            }
        }
        if (val >= (rt_sharpen_real)bl) {
            return 0.0f;
        }
        if (val > (rt_sharpen_real)tl) {
            return (float)(y * (1.0 - (val - (rt_sharpen_real)bl) /
                                      ((rt_sharpen_real)tl -
                                       (rt_sharpen_real)bl)));
        }
        return y_max;
    }

    if (is_double) {
        if (val == (rt_sharpen_real)br &&
            (rt_sharpen_real)br == (rt_sharpen_real)tr) {
            return y_max;
        }
        if (val >= (rt_sharpen_real)br) {
            return 0.0f;
        }
        if (val > (rt_sharpen_real)tr) {
            return (float)(y * (1.0 - (val - (rt_sharpen_real)tr) /
                                      ((rt_sharpen_real)br -
                                       (rt_sharpen_real)tr)));
        }
    }
    if (val >= (rt_sharpen_real)tl) {
        return y_max;
    }
    if (val > (rt_sharpen_real)bl) {
        return (float)(y * (val - (rt_sharpen_real)bl) /
                       ((rt_sharpen_real)tl - (rt_sharpen_real)bl));
    }
    return 0.0f;
}

/* Threshold, delta and the intp(a, b, c) = a * (b - c) + c blend, in place. */
__kernel void rt_sharpen_apply(__global float* luminance,
                               __global const float* blur,
                               __global const float* mask,
                               const int width,
                               const int height,
                               const int pitch,
                               const int use_blur,
                               const int amount,
                               const int bl,
                               const int tl,
                               const int br,
                               const int tr,
                               const int is_double,
                               const int start_at_one)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float base = luminance[index];
    const float diff = use_blur != 0 ? base - blur[index] : 0.0f;
    const float y_max = (float)amount * diff * 0.01f;
    const float magnitude = fabs(diff) < RT_SHARPEN_THRESHOLD_UPPER_BOUND
                                ? fabs(diff)
                                : RT_SHARPEN_THRESHOLD_UPPER_BOUND;
    const float delta = rt_sharpen_threshold(magnitude, y_max, bl, tl, br, tr,
                                             is_double, start_at_one);
    const float updated = base + delta;
    luminance[index] = mask[index] * (updated - base) + base;
}
)CLC";

/*
 * The output sharpening node, shared by its call site, its host builtin, its
 * device launcher and its test.
 *
 * Argument order:
 *   [0]      one-plane F32 L buffer, read and written in place
 *   [1]      method, SharpenMethod as an int
 *   [2]      amount
 *   [3]      contrast
 *   [4]      radius
 *   [5]      blurradius
 *   [6..9]   threshold bottom-left, top-left, bottom-right, top-right
 *   [10]     threshold uses the four-value form
 *   [11]     threshold starts at one
 *   [12]     edges only
 *   [13]     edge detection radius
 *   [14]     edge tolerance
 *   [15]     halo control
 *   [16]     halo control amount
 *   [17]     deconvolution amount
 *   [18]     deconvolution radius
 *   [19]     deconvolution iterations
 *   [20]     deconvolution damping
 *   [21]     automatic deconvolution radius
 *   [22]     deconvolution corner boost
 *   [23]     deconvolution corner latitude
 *   [24]     scale
 *   [25]     show mask
 *
 * Every parameter the core reads is encoded, including the residuals, so the
 * host builtin reproduces the CPU path for the whole parameter set.
 */

namespace rtengine::stage
{

enum SharpenMethod : int {
    SHARPEN_METHOD_USM = 0,
    SHARPEN_METHOD_RLD = 1
};

enum SharpenArg : std::size_t {
    SHARPEN_ARG_BUFFER = 0,
    SHARPEN_ARG_METHOD,
    SHARPEN_ARG_AMOUNT,
    SHARPEN_ARG_CONTRAST,
    SHARPEN_ARG_RADIUS,
    SHARPEN_ARG_BLUR_RADIUS,
    SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT,
    SHARPEN_ARG_THRESHOLD_TOP_LEFT,
    SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT,
    SHARPEN_ARG_THRESHOLD_TOP_RIGHT,
    SHARPEN_ARG_THRESHOLD_IS_DOUBLE,
    SHARPEN_ARG_THRESHOLD_START_AT_ONE,
    SHARPEN_ARG_EDGES_ONLY,
    SHARPEN_ARG_EDGES_RADIUS,
    SHARPEN_ARG_EDGES_TOLERANCE,
    SHARPEN_ARG_HALO_CONTROL,
    SHARPEN_ARG_HALO_CONTROL_AMOUNT,
    SHARPEN_ARG_DECONV_AMOUNT,
    SHARPEN_ARG_DECONV_RADIUS,
    SHARPEN_ARG_DECONV_ITERATIONS,
    SHARPEN_ARG_DECONV_DAMPING,
    SHARPEN_ARG_DECONV_AUTO_RADIUS,
    SHARPEN_ARG_DECONV_CORNER_BOOST,
    SHARPEN_ARG_DECONV_CORNER_LATITUDE,
    SHARPEN_ARG_SCALE,
    SHARPEN_ARG_SHOW_MASK,
    SHARPEN_NODE_ARGS
};

/*
 * Everything the core reads, in a form both the ABI encoder and the device
 * launcher can inspect without touching the production parameter struct.
 */
struct SharpenNodeParams {
    SharpenMethod method = SHARPEN_METHOD_USM;
    int amount = 0;
    double contrast = 0.0;
    double radius = 0.0;
    double blurradius = 0.0;
    int threshold_bottom_left = 0;
    int threshold_top_left = 0;
    int threshold_bottom_right = 0;
    int threshold_top_right = 0;
    bool threshold_is_double = false;
    bool threshold_start_at_one = false;
    bool edgesonly = false;
    double edges_radius = 0.0;
    int edges_tolerance = 0;
    bool halocontrol = false;
    int halocontrol_amount = 0;
    int deconvamount = 0;
    double deconvradius = 0.0;
    int deconviter = 0;
    int deconvdamping = 0;
    bool deconvAutoRadius = false;
    double deconvCornerBoost = 0.0;
    int deconvCornerLatitude = 0;
    double scale = 1.0;
    bool showMask = false;
};

constexpr double SHARPEN_BLUR_RADIUS_LIMIT = 0.25;
/* Mirrors GAUSS_SKIP and GAUSS_3X3_LIMIT in gauss.cc. */
constexpr double SHARPEN_GAUSS_SKIP = 0.25;
constexpr double SHARPEN_FIR_LIMIT = 0.6;
constexpr double SHARPEN_SIGMA_LIMIT = 25.0;
constexpr double SHARPEN_MASK_SIGMA = 2.0;
constexpr double SHARPEN_MASK_GRADIENT_SCALE = 0.0625 / 327.68;

/*
 * Coefficients of the 1-D Young-van Vliet recurrence, computed in double on
 * the host from the same formulae as calculateYvVFactors() in gauss.cc. The
 * M scaling matches the SSE implementation that ships on x86-64, because that
 * is the production CPU path the device is compared against.
 */
struct SharpenIirFactors {
    float B = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float b3 = 0.0f;
    float m[3][3] = {};
};

inline void sharpen_iir_factors(double sigma, SharpenIirFactors& out)
{
    double q;
    if (sigma < 2.5) {
        q = 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sigma);
    } else {
        q = 0.98711 * sigma - 0.96330;
    }

    const double b0 =
        1.57825 + 2.44413 * q + 1.4281 * q * q + 0.422205 * q * q * q;
    double b1 = 2.44413 * q + 2.85619 * q * q + 1.26661 * q * q * q;
    double b2 = -1.4281 * q * q - 1.26661 * q * q * q;
    double b3 = 0.422205 * q * q * q;
    const double B = 1.0 - (b1 + b2 + b3) / b0;

    b1 /= b0;
    b2 /= b0;
    b3 /= b0;

    double M[3][3];
    M[0][0] = -b3 * b1 + 1.0 - b3 * b3 - b2;
    M[0][1] = (b3 + b1) * (b2 + b3 * b1);
    M[0][2] = b3 * (b1 + b3 * b2);
    M[1][0] = b1 + b3 * b2;
    M[1][1] = -(b2 - 1.0) * (b2 + b3 * b1);
    M[1][2] = -(b3 * b1 + b3 * b3 + b2 - 1.0) * b3;
    M[2][0] = b3 * b1 + b2 + b1 * b1 - b2 * b2;
    M[2][1] = b1 * b2 + b3 * b2 * b2 - b1 * b3 * b3 - b3 * b3 * b3 -
              b3 * b2 + b3;
    M[2][2] = b3 * (b1 + b3 * b2);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            M[i][j] *= (1.0 + b2 + (b1 - b3) * b3);
            M[i][j] /=
                (1.0 + b1 - b2 + b3) * (1.0 - b1 - b2 - b3);
        }
    }

    out.B = static_cast<float>(B);
    out.b1 = static_cast<float>(b1);
    out.b2 = static_cast<float>(b2);
    out.b3 = static_cast<float>(b3);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.m[i][j] = static_cast<float>(M[i][j]);
        }
    }
}

/* The normalized 3x3 kernel gaussianBlur() uses below sigma 0.6. */
struct SharpenFirFactors {
    float c0 = 0.0f;
    float c1 = 0.0f;
    float c2 = 0.0f;
    float b0 = 0.0f;
    float b1 = 0.0f;
};

inline void sharpen_fir_factors(double sigma, SharpenFirFactors& out)
{
    const double c = std::exp(-0.5 * (1.0 / (sigma * sigma)));
    const double c2 = std::exp(-(1.0 / (sigma * sigma)));
    const double sum = 1.0 + 4.0 * (c + c2);
    out.c0 = static_cast<float>(1.0 / sum);
    out.c1 = static_cast<float>(c / sum);
    out.c2 = static_cast<float>(c2 / sum);

    const double edge = std::exp(-1.0 / (2.0 * sigma * sigma));
    const double edge_sum = 2.0 * edge + 1.0;
    out.b1 = static_cast<float>(edge / edge_sum);
    out.b0 = static_cast<float>(1.0 / edge_sum);
}

/*
 * The device accepts only the USM path with no residual switch and with the
 * blur radii whose kernels the launcher implements. Everything else returns
 * false with a reason, so the host builtin runs and the record says host.
 */
inline bool sharpen_device_supported(const SharpenNodeParams& p,
                                     std::string& error)
{
    if (p.method != SHARPEN_METHOD_USM) {
        error = "output.sharpen: the RLD method is a host residual";
        return false;
    }
    if (p.edgesonly) {
        error = "output.sharpen: edges only is a host residual";
        return false;
    }
    if (p.halocontrol) {
        error = "output.sharpen: halo control is a host residual";
        return false;
    }
    if (p.showMask) {
        error = "output.sharpen: show mask is a host residual";
        return false;
    }
    if (p.blurradius >= SHARPEN_BLUR_RADIUS_LIMIT) {
        error = "output.sharpen: the blur radius is a host residual";
        return false;
    }
    if (!(p.scale > 0.0) || !std::isfinite(p.scale) ||
        !std::isfinite(p.radius) || p.radius < 0.0) {
        error = "output.sharpen: invalid radius or scale";
        return false;
    }
    const double sigma = p.radius / p.scale;
    if (!(sigma < SHARPEN_SIGMA_LIMIT)) {
        error = "output.sharpen: sigma is outside the device range";
        return false;
    }
    return true;
}

/* The node parameters a profile selects. Rejects a method outside the enum. */
inline bool sharpen_node_params(const procparams::SharpeningParams& p,
                                double scale, bool showMask,
                                SharpenNodeParams& out, std::string& error)
{
    if (p.method == "usm") {
        out.method = SHARPEN_METHOD_USM;
    } else if (p.method == "rld") {
        out.method = SHARPEN_METHOD_RLD;
    } else {
        error = "sharpen: unknown method '" + p.method + "'";
        return false;
    }

    out.amount = p.amount;
    out.contrast = p.contrast;
    out.radius = p.radius;
    out.blurradius = p.blurradius;
    out.threshold_bottom_left = p.threshold.getBottomLeft();
    out.threshold_top_left = p.threshold.getTopLeft();
    out.threshold_bottom_right = p.threshold.getBottomRight();
    out.threshold_top_right = p.threshold.getTopRight();
    out.threshold_is_double = p.threshold.isDouble();
    /*
     * Threshold keeps its start-at-one flag private and exposes no getter.
     * SharpeningParams constructs it with false and setValues() never changes
     * it, so every profile the editor writes uses the start-at-zero form. The
     * field is still encoded so the ABI is complete for reconstructed values.
     */
    out.threshold_start_at_one = false;
    out.edgesonly = p.edgesonly;
    out.edges_radius = p.edges_radius;
    out.edges_tolerance = p.edges_tolerance;
    out.halocontrol = p.halocontrol;
    out.halocontrol_amount = p.halocontrol_amount;
    out.deconvamount = p.deconvamount;
    out.deconvradius = p.deconvradius;
    out.deconviter = p.deconviter;
    out.deconvdamping = p.deconvdamping;
    out.deconvAutoRadius = p.deconvAutoRadius;
    out.deconvCornerBoost = p.deconvCornerBoost;
    out.deconvCornerLatitude = p.deconvCornerLatitude;
    out.scale = scale;
    out.showMask = showMask;
    return true;
}

/* Copy the encoded parameters back into the production parameter struct. */
inline void sharpen_apply_params(const SharpenNodeParams& node,
                                 procparams::SharpeningParams& p)
{
    p.enabled = true;
    p.method = node.method == SHARPEN_METHOD_RLD ? "rld" : "usm";
    p.amount = node.amount;
    p.contrast = node.contrast;
    p.radius = node.radius;
    p.blurradius = node.blurradius;
    if (node.threshold_is_double) {
        p.threshold = procparams::Threshold<int>(
            node.threshold_bottom_left, node.threshold_top_left,
            node.threshold_bottom_right, node.threshold_top_right,
            node.threshold_start_at_one);
    } else {
        p.threshold = procparams::Threshold<int>(
            node.threshold_bottom_left, node.threshold_top_left,
            node.threshold_start_at_one);
    }
    p.edgesonly = node.edgesonly;
    p.edges_radius = node.edges_radius;
    p.edges_tolerance = node.edges_tolerance;
    p.halocontrol = node.halocontrol;
    p.halocontrol_amount = node.halocontrol_amount;
    p.deconvamount = node.deconvamount;
    p.deconvradius = node.deconvradius;
    p.deconviter = node.deconviter;
    p.deconvdamping = node.deconvdamping;
    p.deconvAutoRadius = node.deconvAutoRadius;
    p.deconvCornerBoost = node.deconvCornerBoost;
    p.deconvCornerLatitude = node.deconvCornerLatitude;
}

inline RtStageBuffer sharpen_l_buffer(float* L, int width, int height)
{
    RtStageBuffer buffer{};
    buffer.size = sizeof(RtStageBuffer);
    buffer.data = L;
    buffer.width = width;
    buffer.height = height;
    buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
    buffer.plane_pitch = buffer.row_pitch * static_cast<std::size_t>(height);
    buffer.planes = 1;
    buffer.format = RT_STAGE_F32;
    buffer.flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;
    return buffer;
}

/*
 * The argument descriptors point into this struct for the scalar values, but
 * the scalars are copied into the union, so only the array itself must stay
 * alive until the node returns.
 */
struct SharpenNodeArgs {
    RtStageArg args[SHARPEN_NODE_ARGS];
};

inline void sharpen_node_args(float* L, int width, int height,
                              const SharpenNodeParams& p, SharpenNodeArgs& out)
{
    out.args[SHARPEN_ARG_BUFFER].kind = RT_STAGE_ARG_BUFFER;
    out.args[SHARPEN_ARG_BUFFER].u.buffer = sharpen_l_buffer(L, width, height);

    const auto set_int = [&out](SharpenArg index, std::int64_t value) {
        out.args[index].kind = RT_STAGE_ARG_INT;
        out.args[index].u.i = value;
    };
    const auto set_double = [&out](SharpenArg index, double value) {
        out.args[index].kind = RT_STAGE_ARG_DOUBLE;
        out.args[index].u.f = value;
    };

    set_int(SHARPEN_ARG_METHOD, static_cast<std::int64_t>(p.method));
    set_int(SHARPEN_ARG_AMOUNT, p.amount);
    set_double(SHARPEN_ARG_CONTRAST, p.contrast);
    set_double(SHARPEN_ARG_RADIUS, p.radius);
    set_double(SHARPEN_ARG_BLUR_RADIUS, p.blurradius);
    set_int(SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT, p.threshold_bottom_left);
    set_int(SHARPEN_ARG_THRESHOLD_TOP_LEFT, p.threshold_top_left);
    set_int(SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT, p.threshold_bottom_right);
    set_int(SHARPEN_ARG_THRESHOLD_TOP_RIGHT, p.threshold_top_right);
    set_int(SHARPEN_ARG_THRESHOLD_IS_DOUBLE, p.threshold_is_double ? 1 : 0);
    set_int(SHARPEN_ARG_THRESHOLD_START_AT_ONE,
            p.threshold_start_at_one ? 1 : 0);
    set_int(SHARPEN_ARG_EDGES_ONLY, p.edgesonly ? 1 : 0);
    set_double(SHARPEN_ARG_EDGES_RADIUS, p.edges_radius);
    set_int(SHARPEN_ARG_EDGES_TOLERANCE, p.edges_tolerance);
    set_int(SHARPEN_ARG_HALO_CONTROL, p.halocontrol ? 1 : 0);
    set_int(SHARPEN_ARG_HALO_CONTROL_AMOUNT, p.halocontrol_amount);
    set_int(SHARPEN_ARG_DECONV_AMOUNT, p.deconvamount);
    set_double(SHARPEN_ARG_DECONV_RADIUS, p.deconvradius);
    set_int(SHARPEN_ARG_DECONV_ITERATIONS, p.deconviter);
    set_int(SHARPEN_ARG_DECONV_DAMPING, p.deconvdamping);
    set_int(SHARPEN_ARG_DECONV_AUTO_RADIUS, p.deconvAutoRadius ? 1 : 0);
    set_double(SHARPEN_ARG_DECONV_CORNER_BOOST, p.deconvCornerBoost);
    set_int(SHARPEN_ARG_DECONV_CORNER_LATITUDE, p.deconvCornerLatitude);
    set_double(SHARPEN_ARG_SCALE, p.scale);
    set_int(SHARPEN_ARG_SHOW_MASK, p.showMask ? 1 : 0);
}

/* Validate the buffer, the argument kinds and the method enum. */
inline bool sharpen_node_decode(const RtStageArg* args, std::uint32_t nargs,
                                SharpenNodeParams& out, RtStageBuffer& buffer,
                                std::string& error)
{
    if (args == nullptr || nargs != SHARPEN_NODE_ARGS) {
        error = "sharpen expects " + std::to_string(SHARPEN_NODE_ARGS) +
                " arguments";
        return false;
    }

    const auto is_int = [args](SharpenArg index) {
        return args[index].kind == RT_STAGE_ARG_INT;
    };
    const auto is_double = [args](SharpenArg index) {
        return args[index].kind == RT_STAGE_ARG_DOUBLE;
    };

    if (args[SHARPEN_ARG_BUFFER].kind != RT_STAGE_ARG_BUFFER) {
        error = "sharpen expects an L buffer first";
        return false;
    }
    buffer = args[SHARPEN_ARG_BUFFER].u.buffer;
    if (buffer.size != sizeof(RtStageBuffer) || buffer.data == nullptr ||
        buffer.format != RT_STAGE_F32 || buffer.planes != 1 ||
        buffer.width <= 0 || buffer.height <= 0) {
        error = "sharpen needs a tightly packed one-plane F32 L buffer";
        return false;
    }
    /*
     * The extracted CPU core addresses the plane with a W-sample stride and
     * the host builtin passes its raw pointer, so the host cannot honour a
     * padded row or plane. The in-tree producer hands over a tight LabImage
     * plane. Requiring exact pitches keeps the host and device interpretations
     * identical; a padded descriptor is rejected rather than silently misread
     * on the host.
     */
    const std::size_t tight_row =
        static_cast<std::size_t>(buffer.width) * sizeof(float);
    if (tight_row >
        std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(buffer.height)) {
        error = "sharpen plane size overflows";
        return false;
    }
    const std::size_t tight_plane =
        tight_row * static_cast<std::size_t>(buffer.height);
    if (buffer.row_pitch != tight_row || buffer.plane_pitch != tight_plane) {
        error = "sharpen needs a tightly packed one-plane F32 L buffer";
        return false;
    }

    const SharpenArg intArgs[] = {
        SHARPEN_ARG_METHOD,
        SHARPEN_ARG_AMOUNT,
        SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT,
        SHARPEN_ARG_THRESHOLD_TOP_LEFT,
        SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT,
        SHARPEN_ARG_THRESHOLD_TOP_RIGHT,
        SHARPEN_ARG_THRESHOLD_IS_DOUBLE,
        SHARPEN_ARG_THRESHOLD_START_AT_ONE,
        SHARPEN_ARG_EDGES_ONLY,
        SHARPEN_ARG_EDGES_TOLERANCE,
        SHARPEN_ARG_HALO_CONTROL,
        SHARPEN_ARG_HALO_CONTROL_AMOUNT,
        SHARPEN_ARG_DECONV_AMOUNT,
        SHARPEN_ARG_DECONV_ITERATIONS,
        SHARPEN_ARG_DECONV_DAMPING,
        SHARPEN_ARG_DECONV_AUTO_RADIUS,
        SHARPEN_ARG_DECONV_CORNER_LATITUDE,
        SHARPEN_ARG_SHOW_MASK};
    for (SharpenArg index : intArgs) {
        if (!is_int(index)) {
            error = "sharpen argument " + std::to_string(index) +
                    " is not an int";
            return false;
        }
    }

    const SharpenArg doubleArgs[] = {
        SHARPEN_ARG_CONTRAST, SHARPEN_ARG_RADIUS,
        SHARPEN_ARG_BLUR_RADIUS, SHARPEN_ARG_EDGES_RADIUS,
        SHARPEN_ARG_DECONV_RADIUS, SHARPEN_ARG_DECONV_CORNER_BOOST,
        SHARPEN_ARG_SCALE};
    for (SharpenArg index : doubleArgs) {
        if (!is_double(index)) {
            error = "sharpen argument " + std::to_string(index) +
                    " is not a double";
            return false;
        }
    }

    const std::int64_t method = args[SHARPEN_ARG_METHOD].u.i;
    if (method != SHARPEN_METHOD_USM && method != SHARPEN_METHOD_RLD) {
        error = "sharpen method must be usm or rld";
        return false;
    }
    out.method = static_cast<SharpenMethod>(method);

    const auto flag = [args](SharpenArg index) {
        return args[index].u.i != 0;
    };
    const auto flag_value = [args](SharpenArg index) {
        return args[index].u.i;
    };

    out.amount = static_cast<int>(args[SHARPEN_ARG_AMOUNT].u.i);
    out.contrast = args[SHARPEN_ARG_CONTRAST].u.f;
    out.radius = args[SHARPEN_ARG_RADIUS].u.f;
    out.blurradius = args[SHARPEN_ARG_BLUR_RADIUS].u.f;
    out.threshold_bottom_left =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_BOTTOM_LEFT].u.i);
    out.threshold_top_left =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_TOP_LEFT].u.i);
    out.threshold_bottom_right =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_BOTTOM_RIGHT].u.i);
    out.threshold_top_right =
        static_cast<int>(args[SHARPEN_ARG_THRESHOLD_TOP_RIGHT].u.i);
    out.threshold_is_double = flag(SHARPEN_ARG_THRESHOLD_IS_DOUBLE);
    out.threshold_start_at_one = flag(SHARPEN_ARG_THRESHOLD_START_AT_ONE);
    out.edgesonly = flag(SHARPEN_ARG_EDGES_ONLY);
    out.edges_radius = args[SHARPEN_ARG_EDGES_RADIUS].u.f;
    out.edges_tolerance =
        static_cast<int>(args[SHARPEN_ARG_EDGES_TOLERANCE].u.i);
    out.halocontrol = flag(SHARPEN_ARG_HALO_CONTROL);
    out.halocontrol_amount =
        static_cast<int>(args[SHARPEN_ARG_HALO_CONTROL_AMOUNT].u.i);
    out.deconvamount = static_cast<int>(args[SHARPEN_ARG_DECONV_AMOUNT].u.i);
    out.deconvradius = args[SHARPEN_ARG_DECONV_RADIUS].u.f;
    out.deconviter =
        static_cast<int>(args[SHARPEN_ARG_DECONV_ITERATIONS].u.i);
    out.deconvdamping =
        static_cast<int>(args[SHARPEN_ARG_DECONV_DAMPING].u.i);
    out.deconvAutoRadius = flag(SHARPEN_ARG_DECONV_AUTO_RADIUS);
    out.deconvCornerBoost = args[SHARPEN_ARG_DECONV_CORNER_BOOST].u.f;
    out.deconvCornerLatitude =
        static_cast<int>(args[SHARPEN_ARG_DECONV_CORNER_LATITUDE].u.i);
    out.scale = args[SHARPEN_ARG_SCALE].u.f;
    out.showMask = flag(SHARPEN_ARG_SHOW_MASK);

    /*
     * The boolean slots are ints on the wire. Keep the decoder strict so a
     * malformed descriptor cannot smuggle an ambiguous flag into the core.
     */
    const SharpenArg flagArgs[] = {
        SHARPEN_ARG_THRESHOLD_IS_DOUBLE,
        SHARPEN_ARG_THRESHOLD_START_AT_ONE,
        SHARPEN_ARG_EDGES_ONLY,
        SHARPEN_ARG_HALO_CONTROL,
        SHARPEN_ARG_DECONV_AUTO_RADIUS,
        SHARPEN_ARG_SHOW_MASK};
    for (SharpenArg index : flagArgs) {
        const std::int64_t value = flag_value(index);
        if (value != 0 && value != 1) {
            error = "sharpen flag " + std::to_string(index) +
                    " is not 0 or 1";
            return false;
        }
    }

    return true;
}

} // namespace rtengine::stage
