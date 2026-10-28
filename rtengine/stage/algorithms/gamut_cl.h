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

#include "color.h"
#include "iccstore.h"
#include "procparams.h"
#include "stage/stage_abi.h"
#include "stage/stage_buffer.h"

#include "opthelper.h"

#include <array>
#include <string>

/*
 * The end-of-process gamut node, shared by its call site, its device launcher
 * and its test.
 *
 * The kernel is one Lab -> linear working RGB -> optional gain -> ACES
 * reference gamut compression -> XYZ -> Lab pass, in place. The four matrices
 * are built on the host, in double, from the same helpers and in the same
 * order as ImProcFunctions::gamutcompr() with beginend == 1, so the device
 * cannot drift from the CPU path; only their upload is single precision.
 *
 * A namespace-scope constant rather than a macro: GCC 12, the portable
 * package baseline, rejects a multi-line raw string inside a #define.
 */
inline constexpr const char* RT_GAMUT_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF

/*
 * The end-of-process contract from ImProcFunctions::gamutcompr(). These are
 * fixed values, not user settings: the node only ever runs beginend == 1, so
 * the host's thc/thm/thy = 0.8/0.75/0.85 and dc/dm/dy = 1.15/1.25/1.5
 * assignments are part of the algorithm and are named here rather than
 * carried through the ABI. Threshold is the protected core of each channel;
 * distance limit is the distance that the rolloff maps to the gamut boundary.
 */
#define RT_GAMUT_THRESHOLD_CYAN 0.8f
#define RT_GAMUT_THRESHOLD_MAGENTA 0.75f
#define RT_GAMUT_THRESHOLD_YELLOW 0.85f
#define RT_GAMUT_DISTANCE_CYAN 1.15f
#define RT_GAMUT_DISTANCE_MAGENTA 1.25f
#define RT_GAMUT_DISTANCE_YELLOW 1.5f

/* 16-bit sample scale: LabImage holds Lab scaled to this range and rt_math.h
 * calls it MAXVALF. */
#define RT_GAMUT_MAXVAL 65535.0f

/* 116/16 and the CIE constants, the same float values Color uses. */
#define RT_GAMUT_C1_BY_116 0.008620689655172414f
#define RT_GAMUT_C16_BY_116 0.13793103448275862f
#define RT_GAMUT_EPSILON_N 0.20689655172413793f
#define RT_GAMUT_KAPPA_INV (27.0f / 24389.0f)
#define RT_GAMUT_KAPPA 903.2962962962963f
/* MAXVALF * eps, eps = 216/24389: the end of the CIE linear segment, the
 * same split Color::init() uses for cachef/cachefy. */
#define RT_GAMUT_EPS_MAX (RT_GAMUT_MAXVAL * 216.0f / 24389.0f)
#define RT_GAMUT_D50X 0.9642f
#define RT_GAMUT_D50Z 0.8249f

/* Color::f2xyz(float). */
inline float rt_gamut_f2xyz(const float f)
{
    return f > RT_GAMUT_EPSILON_N
        ? f * f * f
        : (116.0f * f - 16.0f) * RT_GAMUT_KAPPA_INV;
}

/* Color::Lab2XYZ(), identical operation order. */
inline void rt_gamut_lab2xyz(const float L, const float a, const float b,
                             float* x, float* y, float* z)
{
    const float LL = L / 327.68f;
    const float aa = a / 327.68f;
    const float bb = b / 327.68f;
    const float fy = (RT_GAMUT_C1_BY_116 * LL) + RT_GAMUT_C16_BY_116;
    const float fx = (0.002f * aa) + fy;
    const float fz = fy - (0.005f * bb);
    *x = RT_GAMUT_MAXVAL * rt_gamut_f2xyz(fx) * RT_GAMUT_D50X;
    *z = RT_GAMUT_MAXVAL * rt_gamut_f2xyz(fz) * RT_GAMUT_D50Z;
    *y = LL > 8.0f
        ? RT_GAMUT_MAXVAL * fy * fy * fy
        : RT_GAMUT_MAXVAL * LL / RT_GAMUT_KAPPA;
}

/* Color::xyz2rgb() and Color::rgbxyz(), the row-wise matrix product. */
inline void rt_gamut_xyz2rgb(const float x, const float y, const float z,
                             __global const float* m, float rgb[3])
{
    rgb[0] = m[0] * x + m[1] * y + m[2] * z;
    rgb[1] = m[3] * x + m[4] * y + m[5] * z;
    rgb[2] = m[6] * x + m[7] * y + m[8] * z;
}

inline void rt_gamut_rgbxyz(const float r, const float g, const float b,
                            __global const float* m, float xyz[3])
{
    xyz[0] = m[0] * r + m[1] * g + m[2] * b;
    xyz[1] = m[3] * r + m[4] * g + m[5] * b;
    xyz[2] = m[6] * r + m[7] * g + m[8] * b;
}

/*
 * Color::computeXYZ2Lab() and Color::computeXYZ2LabY(). The host answers the
 * 0..65535 range from its interpolated cachef/cachefy tables; those tables
 * hold the same CIE curve that is evaluated here, and the recorded parity
 * shows the difference stays far below one 16-bit LSB.
 */
inline float rt_gamut_compute_f(const float value)
{
    if (value < 0.0f) {
        return 327.68f * ((RT_GAMUT_KAPPA * value / RT_GAMUT_MAXVAL
                           + 16.0f) / 116.0f);
    }
    if (value > RT_GAMUT_MAXVAL) {
        return 327.68f * cbrt(value / RT_GAMUT_MAXVAL);
    }
    /* The host table is linear below the CIE epsilon and a cube root above. */
    return value > RT_GAMUT_EPS_MAX
        ? 327.68f * cbrt(value / RT_GAMUT_MAXVAL)
        : 327.68f * ((RT_GAMUT_KAPPA * value / RT_GAMUT_MAXVAL + 16.0f)
                     / 116.0f);
}

inline float rt_gamut_compute_fy(const float value)
{
    if (value < 0.0f) {
        return 327.68f * (RT_GAMUT_KAPPA * value / RT_GAMUT_MAXVAL);
    }
    if (value > RT_GAMUT_MAXVAL) {
        return 327.68f * (116.0f * cbrt(value / RT_GAMUT_MAXVAL) - 16.0f);
    }
    return value > RT_GAMUT_EPS_MAX
        ? 327.68f * (116.0f * cbrt(value / RT_GAMUT_MAXVAL) - 16.0f)
        : 327.68f * (RT_GAMUT_KAPPA * value / RT_GAMUT_MAXVAL);
}

/* Color::XYZ2Lab(), identical operation order. */
inline void rt_gamut_xyz2lab(const float X, const float Y, const float Z,
                             float* L, float* a, float* b)
{
    const float x = X / RT_GAMUT_D50X;
    const float z = Z / RT_GAMUT_D50Z;
    const float y = Y;
    const float fx = rt_gamut_compute_f(x);
    const float fy = rt_gamut_compute_f(y);
    const float fz = rt_gamut_compute_f(z);
    *L = rt_gamut_compute_fy(y);
    *a = 500.0f * (fx - fy);
    *b = 200.0f * (fy - fz);
}

/* Color::mult3(): row i is the dot product of row i of the matrix with the
 * colour vector, as the host accumulates it. */
inline void rt_gamut_mult3(const float in[3], __global const float* m,
                           float out[3])
{
    float buf[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            buf[i] += in[j] * m[j * 3 + i];
        }
    }
    out[0] = buf[0];
    out[1] = buf[1];
    out[2] = buf[2];
}

/*
 * Color::aces_reference_gamut_compression() with the fixed end-of-process
 * parameters. Only the rolloff branch exists here because gamutcompr() sets
 * roll = true for beginend == 1; the parabolic branch is unreachable for this
 * node. Inputs and outputs are in the 0..1 working range, as the host divides
 * and multiplies by 65535 around the call.
 */
inline void rt_gamut_compress(float rgb[3], __global const float* to_out,
                              __global const float* from_out, const float pwr)
{
    const float threshold[3] = {
        RT_GAMUT_THRESHOLD_CYAN, RT_GAMUT_THRESHOLD_MAGENTA,
        RT_GAMUT_THRESHOLD_YELLOW};
    const float distance_limit[3] = {
        RT_GAMUT_DISTANCE_CYAN, RT_GAMUT_DISTANCE_MAGENTA,
        RT_GAMUT_DISTANCE_YELLOW};

    float target[3];
    rt_gamut_mult3(rgb, to_out, target);

    /* Achromatic axis and inverse RGB ratios: distance from that axis. */
    const float ac = fmax(target[0], fmax(target[1], target[2]));
    float d[3] = {0.0f, 0.0f, 0.0f};
    if (ac != 0.0f) {
        for (int i = 0; i < 3; ++i) {
            d[i] = (ac - target[i]) / fabs(ac);
        }
    }

    float cd[3] = {d[0], d[1], d[2]};
    for (int i = 0; i < 3; ++i) {
        if (d[i] >= threshold[i]) {
            const float limit = distance_limit[i];
            const float thres = threshold[i];
            /* Scale factor for y = 1 at x = limit. */
            const float scale = (limit - thres)
                / pow(pow((1.0f - thres) / (limit - thres), -pwr) - 1.0f,
                      1.0f / pwr);
            const float nd = (d[i] - thres) / scale;
            const float po = pow(nd, pwr);
            cd[i] = thres + scale * nd / pow(1.0f + po, 1.0f / pwr);
        }
    }

    for (int i = 0; i < 3; ++i) {
        target[i] = ac - cd[i] * fabs(ac);
    }

    float compressed[3];
    rt_gamut_mult3(target, from_out, compressed);
    rgb[0] = compressed[0];
    rgb[1] = compressed[1];
    rgb[2] = compressed[2];
}

/*
 * One pixel in place. `pitch` and `plane_pitch` are the device allocation's
 * row and plane pitches, so padded rows and the three planes of one Lab image
 * are addressed exactly as the caller laid them out.
 */
__kernel void rt_gamut(__global float* lab,
                       __global const float* wip,
                       __global const float* wp,
                       __global const float* to_out,
                       __global const float* from_out,
                       const float gainev, const float pwr,
                       const int compress,
                       const int width, const int height,
                       const int pitch, const int plane_pitch)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * pitch + x;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    rt_gamut_lab2xyz(lab[offset], lab[plane_pitch + offset],
                     lab[2 * plane_pitch + offset], &X, &Y, &Z);

    float rgb[3];
    rt_gamut_xyz2rgb(X, Y, Z, wip, rgb);
    /* The host skips the gain loop when wgamgain is zero, where gainev is
     * exactly 1, so an unconditional multiply is the same result. */
    rgb[0] *= gainev;
    rgb[1] *= gainev;
    rgb[2] *= gainev;

    if (compress != 0) {
        rgb[0] /= RT_GAMUT_MAXVAL;
        rgb[1] /= RT_GAMUT_MAXVAL;
        rgb[2] /= RT_GAMUT_MAXVAL;
        rt_gamut_compress(rgb, to_out, from_out, pwr);
        rgb[0] *= RT_GAMUT_MAXVAL;
        rgb[1] *= RT_GAMUT_MAXVAL;
        rgb[2] *= RT_GAMUT_MAXVAL;
    }

    float xyz[3];
    rt_gamut_rgbxyz(rgb[0], rgb[1], rgb[2], wp, xyz);

    float L = 0.0f;
    float a = 0.0f;
    float b = 0.0f;
    rt_gamut_xyz2lab(xyz[0], xyz[1], xyz[2], &L, &a, &b);

    lab[offset] = L;
    lab[plane_pitch + offset] = a;
    lab[2 * plane_pitch + offset] = b;
}
)CLC";

namespace rtengine::stage
{

/*
 * Host-side node parameters. The four matrices are the double matrices
 * gamutcompr() derives for beginend == 1; gamut_node_params() reproduces that
 * derivation so call site, launcher and test share one definition.
 */
struct GamutNodeParams {
    double wip[3][3];
    double wp[3][3];
    double to_out[3][3];
    double from_out[3][3];
    /* pow_F(2, wgamgain): the final gain in EV. */
    double gainev = 1.0;
    /* icm.wgampower, the compression aggressiveness. */
    double pwr = 1.0;
    /* 1 when wgamut selects a gamut, 0 for the gain-only case. */
    int compress = 0;
};

/*
 * The output gamut matrices of ImProcFunctions::gamutcompr(), beginend == 1.
 * They are local constants there, so they are repeated here; the device parity
 * test and the end-to-end gate fail if they drift apart.
 */
inline const Color::Matrix& gamut_output_matrix(
    procparams::ColorManagementParams::Wwgamut gamut)
{
    /* DCI-P3 as gamutcompr carries it: D63 adapted to D50. */
    static const Color::Matrix dcip3 = {{
        {0.4861607, 0.3238514, 0.1541879},
        {0.2266839, 0.7103336, 0.0629826},
        {-0.0008016, 0.0432353, 0.7824663}}};
    static const Color::Matrix rec2020 = {{
        {0.6734241, 0.1656411, 0.1251286},
        {0.2790177, 0.6753402, 0.0456377},
        {-0.0019300, 0.0299784, 0.7973330}}};
    static const Color::Matrix srgb = {{
        {0.4360747, 0.3850649, 0.1430804},
        {0.2225045, 0.7168786, 0.0606169},
        {0.0139322, 0.0971045, 0.7141733}}};
    static const Color::Matrix adobe = {{
        {0.6097559, 0.2052401, 0.1492240},
        {0.3111242, 0.6256560, 0.0632197},
        {0.0194811, 0.0608902, 0.7448387}}};
    static const Color::Matrix identity = {{
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}}};

    using Wwgamut = procparams::ColorManagementParams::Wwgamut;
    switch (gamut) {
    case Wwgamut::REC2020:
        return rec2020;
    case Wwgamut::ADOBE:
        return adobe;
    case Wwgamut::SRGB:
        return srgb;
    case Wwgamut::DCIP3:
        return dcip3;
    case Wwgamut::NONE:
        return identity;
    }
    /* An out-of-range value must not fall back to a guessed gamut. */
    return identity;
}

/*
 * Derive the node parameters. Returns false for a gamut value outside the
 * enum, so the launcher never receives a guessed matrix.
 */
inline bool gamut_node_params(const procparams::ColorManagementParams& icm,
                              GamutNodeParams& params, std::string& error)
{
    using Wwgamut = procparams::ColorManagementParams::Wwgamut;

    if (icm.wgamut != Wwgamut::NONE && icm.wgamut != Wwgamut::REC2020 &&
        icm.wgamut != Wwgamut::ADOBE && icm.wgamut != Wwgamut::SRGB &&
        icm.wgamut != Wwgamut::DCIP3) {
        error = "gamut: unknown working gamut value";
        return false;
    }

    const Color::Matrix out = gamut_output_matrix(icm.wgamut);
    Color::Matrix inv_out{};
    if (!invertMatrix(out, inv_out)) {
        error = "gamut: the output gamut matrix is not invertible";
        return false;
    }

    const TMatrix wprof =
        ICCStore::getInstance()->workingSpaceMatrix(icm.workingProfile);
    Color::Matrix wpro{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            wpro[r][c] = wprof[r][c];
        }
    }

    Color::Matrix prov{};
    Color::multip(inv_out, wpro, prov);

    Color::Matrix to_out{};
    Color::transpose(prov, to_out);

    Color::Matrix from_out{};
    if (!invertMatrix(to_out, from_out)) {
        error = "gamut: the transposed matrix is not invertible";
        return false;
    }

    const TMatrix wiprof =
        ICCStore::getInstance()->workingSpaceInverseMatrix(icm.workingProfile);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            params.wip[r][c] = wiprof[r][c];
            params.wp[r][c] = wprof[r][c];
            params.to_out[r][c] = to_out[r][c];
            params.from_out[r][c] = from_out[r][c];
        }
    }

    params.gainev = pow_F(2.0f, static_cast<float>(icm.wgamgain));
    params.pwr = icm.wgampower;
    params.compress = icm.wgamut == Wwgamut::NONE ? 0 : 1;
    return true;
}

/*
 * The eight node arguments, in the order the launcher documents: the Lab
 * buffer in place, the four 9-entry matrices, the gain, the power and the
 * compression flag. The matrix storage lives in this struct because the
 * argument descriptors point into it; keep it alive until the node returns.
 */
struct GamutNodeArgs {
    std::array<std::array<float, 9>, 4> tables;
    RtStageArg args[8];
};

inline void gamut_node_args(LabImage& lab, const GamutNodeParams& params,
                            GamutNodeArgs& out)
{
    const double* sources[4] = {
        &params.wip[0][0], &params.wp[0][0],
        &params.to_out[0][0], &params.from_out[0][0]};
    for (int table = 0; table < 4; ++table) {
        for (int entry = 0; entry < 9; ++entry) {
            out.tables[table][entry] =
                static_cast<float>(sources[table][entry]);
        }
    }

    out.args[0].kind = RT_STAGE_ARG_BUFFER;
    out.args[0].u.buffer = view(lab);
    for (int table = 0; table < 4; ++table) {
        RtStageBuffer buffer{};
        buffer.size = sizeof(RtStageBuffer);
        buffer.data = out.tables[table].data();
        buffer.width = 9;
        buffer.height = 1;
        buffer.row_pitch = 9 * sizeof(float);
        buffer.plane_pitch = buffer.row_pitch;
        buffer.planes = 1;
        buffer.format = RT_STAGE_F32;
        buffer.flags = RT_STAGE_BUFFER_READ;
        out.args[1 + table].kind = RT_STAGE_ARG_BUFFER;
        out.args[1 + table].u.buffer = buffer;
    }
    out.args[5].kind = RT_STAGE_ARG_DOUBLE;
    out.args[5].u.f = params.gainev;
    out.args[6].kind = RT_STAGE_ARG_DOUBLE;
    out.args[6].u.f = params.pwr;
    out.args[7].kind = RT_STAGE_ARG_INT;
    out.args[7].u.i = params.compress;
}

} // namespace rtengine::stage
