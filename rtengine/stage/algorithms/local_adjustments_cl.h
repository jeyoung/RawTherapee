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

#include "stage/algorithms/lab_tools_cl.h"
#include "stage/algorithms/lab_wavelet_cl.h"
#include "stage/algorithms/lab_regions_cl.h"
#include "stage/algorithms/local_contrast_cl.h"
#include "stage/algorithms/munsell_lch_cl.h"
#include "stage/algorithms/vibrance_cl.h"

#include <string>

/*
 * The one-spot colour path of ImProcFunctions::Lab_Local().
 *
 * This program is concatenated after the lab.tools helpers (the Lab round
 * trip, the caches and the LUT access), the local-contrast IIR line kernel,
 * the Sleef xatan2f/xsincosf port and the lab.regions xlogf/xexpf port. It
 * adds the single-spot colour apply, the ROI copy, the per-pixel soft-light
 * curve, the local dehaze path, the local CBDL pyramid/blends, and the
 * full-image clamp.
 *
 * The record slots are the host's RT_LOCAL_ADJUSTMENTS_SLOT_* values; the
 * device source repeats the small subset it reads so the program is
 * self-contained. Keep the two lists in step.
 */
inline constexpr const char* RT_LOCAL_ADJUSTMENTS_CL = R"CLC(
/* RtLocalAdjustmentsResult record slots used here. */
#define RT_LA_XC 0
#define RT_LA_YC 1
#define RT_LA_LX 2
#define RT_LA_LY 3
#define RT_LA_LXL 4
#define RT_LA_LYT 5
#define RT_LA_TRANS 6
#define RT_LA_TRANSWEAK 7
#define RT_LA_TRANSGRAD 8
#define RT_LA_CHRO 11
#define RT_LA_CONT 12
#define RT_LA_LIGH 13
#define RT_LA_SENS 15
#define RT_LA_THR 16
#define RT_LA_ITERAT 17
#define RT_LA_BALANCE 18
#define RT_LA_BALANCEH 19
#define RT_LA_HUEREF 21
#define RT_LA_CHROMAREF 22
#define RT_LA_LUMAREF 23
#define RT_LA_SHAPE 26
#define RT_LA_ACTIV 27
#define RT_LA_ISLOCAL 28
#define RT_LA_COLORENA 29
#define RT_LA_DELTAE 30
#define RT_LA_QCURVE 31
#define RT_LA_MERGE 32
#define RT_LA_AVOIDGAMUT 33
#define RT_LA_AVOID_MUNS 4
#define RT_LA_CLAMPMODE 34
#define RT_LA_HRENABLED 35
#define RT_LA_AVOIDNEG 65
#define RT_LA_LL_OFFSET 43
#define RT_LA_CL_OFFSET 44
#define RT_LA_LC_OFFSET 45
#define RT_LA_CC_OFFSET 46
#define RT_LA_LH_OFFSET 47
#define RT_LA_HH_OFFSET 48
#define RT_LA_CH_OFFSET 49
#define RT_LA_SATUR_OFFSET 50
#define RT_LA_SATURMOINS_OFFSET 51
#define RT_LA_LIGHT_OFFSET 52
#define RT_LA_LL_ACTIVE 53
#define RT_LA_CL_ACTIVE 54
#define RT_LA_LC_ACTIVE 55
#define RT_LA_CC_ACTIVE 56
#define RT_LA_LH_ACTIVE 57
#define RT_LA_HH_ACTIVE 58
#define RT_LA_CH_ACTIVE 59
#define RT_LA_SATUR_ACTIVE 60
#define RT_LA_SATURMOINS_ACTIVE 61
#define RT_LA_LIGHT_ACTIVE 62
#define RT_LA_ADJUSTR 63
#define RT_LA_AMOUNTCHROM 64
#define RT_LA_EXPSOFT 94
#define RT_LA_SOFT_STRENGTH 95
#define RT_LA_SENSSOFT 96
#define RT_LA_EXPCBDL 97
#define RT_LA_SENSCB 98
#define RT_LA_CBDL_THRESHOLD 99
#define RT_LA_CBDL_CLARITY 100
#define RT_LA_CBDL_CHROMA 101
#define RT_LA_CBDL_MULT0 102
#define RT_LA_CBDL_MULT1 103
#define RT_LA_CBDL_MULT2 104
#define RT_LA_CBDL_MULT3 105
#define RT_LA_CBDL_MULT4 106
#define RT_LA_CBDL_MULT5 107
#define RT_LA_CBDL_LUMA_LEVELS 108
#define RT_LA_CBDL_CHROMA_LEVELS 109
#define RT_LA_CBDL_LUMA_OFFSET 110
#define RT_LA_CBDL_LUMA_ACTIVE 111
#define RT_LA_CBDL_CHROMA_OFFSET 112
#define RT_LA_CBDL_CHROMA_ACTIVE 113
#define RT_LA_EXPDENAZE 114
#define RT_LA_SENSH 115
#define RT_LA_DEHAZE_STRENGTH 116
#define RT_LA_DEHAZE_DEPTH 117
#define RT_LA_DEHAZE_SATURATION 118
#define RT_LA_EXPLOG 119
#define RT_LA_LOG_SENS 120
#define RT_LA_LOG_GRAY 121
#define RT_LA_LOG_SHADOWS_RANGE 122
#define RT_LA_LOG_DYNAMIC_RANGE 123
#define RT_LA_LOG_COMPRLOG 124
#define RT_LA_LOG_COMPRFACTOR 125
#define RT_LA_LOG_COMPRTHLOG 126
#define RT_LA_LOG_LINBASE 127
#define RT_LA_LOG_NOISE 128
#define RT_LA_LOG_LOG2 129
#define RT_LA_LOG_SATCONTROL 130
#define RT_LA_LOG_REPART 131
#define RT_LA_LOG_KAB_PRE 132

/*
 * The number of trailing columns the host takes through its scalar path.
 * lab2rgb() scans blocks of four while `j < W - 3` and avoidcolshi() does the
 * same, so exactly `width % 4` columns fall through (none when the width is
 * divisible by four). A build without SSE2/RT_SIMDE is scalar throughout.
 * The program defines RT_LA_HOST_SIMD from the host's compile-time choice.
 */
inline int rt_la_host_tail(const int width)
{
#if RT_LA_HOST_SIMD
    return width % 4;
#else
    return width;
#endif
}

/* iplocallab.cc's constants. */
#define RT_LA_MINSCOPE 0.025f
#define RT_LA_MAXSCOPE 1.25f
#define RT_LA_LIMSCOPE 80
#define RT_LA_PI_F 3.1415927f
/* dehazeloc()'s fixed tolerances; the constants live in the host context. */
#define RT_LA_DEHAZE_TEPS 1e-6f
#define RT_LA_DEHAZE_LUMINANCE_FLOOR 1e-5f

/* iplocallab.cc clipC(). */
inline float rt_la_clipC(const float x)
{
    return fmin(fmax(x, -100000.0f), 100000.0f);
}

/* LUTf::operator[] with LUT_CLIP_OFF, as complexCurvelocal() leaves the
 * lightness table. maxs is size - 2 and maxsf is maxs. */
/* LUTf::operator[] with LUT_CLIP_OFF. `maxs` is size - 2 (32768 for the
 * 32770-entry light table, 65534 for a 65536-entry curve table). */
inline float rt_la_lut_off(__global const float* table, const float maxs,
                           const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        idx = 0;
    } else if (index > maxs) {
        idx = (int)maxs;
    }
    const float diff = index - (float)idx;
    const float low = table[idx];
    return low + (table[idx + 1] - low) * diff;
}

inline float rt_la_light_lut(__global const float* table, const float index)
{
    return rt_la_lut_off(table, 32768.0f, index);
}

/* The host's color_satur/color_saturmoins DiagonalCurve, sampled by the context
 * at 65536 points and read here with the same [0,1] argument. */
inline float rt_la_satur_lut(__global const float* table, const float chroma)
{
    return rt_lt_lut(table, 65536, rt_lt_lim01(chroma / 35000.0f) * 65535.0f);
}

/* iplocallab.cc balancedeltaE(). */
inline float rt_la_balancedeltaE(const float kL)
{
    const float mincurs = 0.05f;
    const float maxcurs = 2.5f;
    const float maxkab = 1.475f;
    const float minkab = 0.25f;
    const float abal = (maxkab - minkab) / (mincurs - maxcurs);
    const float bbal = maxkab - mincurs * abal;
    return abal * kL + bbal;
}

/* iplocallab.cc calcreducdE(). The host calls std::pow(float,float), so the
 * device calls pow() on the same float arguments. */
inline float rt_la_calcreducdE(const float dE, const float maxdE,
                               const float mindE, const float maxdElim,
                               const float mindElim, const float iterat,
                               const int limscope, const int scope)
{
    if (scope > limscope) {
        if (dE > maxdElim) {
            return 0.0f;
        } else if (dE > mindElim) {
            const float reducdElim =
                pow((dE - maxdElim) / (mindElim - maxdElim), iterat);
            const float aalim = (1.0f - reducdElim) / 20.0f;
            const float bblim = 1.0f - 100.0f * aalim;
            return aalim * (float)scope + bblim;
        }
        return 1.0f;
    }

    if (dE > maxdE) {
        return 0.0f;
    } else if (dE > mindE) {
        return pow((dE - maxdE) / (mindE - maxdE), iterat);
    }
    return 1.0f;
}

/* iplocallab.cc calcLocalFactor(): the host's xcosf is xsincosf(...).y. */
inline float rt_la_calc_local_factor(const float lox, const float loy,
                                     const float lcx, const float dx,
                                     const float lcy, const float dy,
                                     const float ach, const float gradient)
{
    const float kelip = dx / dy;
    const float belip = fmax(
        0.0001f,
        sqrt((lox - lcx) / kelip * ((lox - lcx) / kelip) +
             (loy - lcy) * (loy - lcy)));
    const float rapy = fabs((loy - lcy) / belip);
    const float aelip = belip * kelip;
    const float degrad = aelip / dx;
    const float gradreal = gradient * rapy + 1.0f;
    const float ap = RT_LA_PI_F / (1.0f - ach);
    const float bp = RT_LA_PI_F - ap;
    const float c = rt_lt_xsincosf(degrad * ap + bp).y;
    return pow(0.5f * (1.0f + c), gradreal * gradreal);
}

/* iplocallab.cc calcLocalFactorrect(). */
inline float rt_la_calc_local_factor_rect(const float lox, const float loy,
                                          const float lcx, const float dx,
                                          const float lcy, const float dy,
                                          const float ach, const float gradient)
{
    const float eps = 0.0001f;
    const float krap = fabs(dx / dy);
    const float kx = lox - lcx;
    const float ky = loy - lcy;
    float ref = 0.0f;
    if (fabs(kx / (ky + eps)) < krap) {
        ref = sqrt(dy * dy * (1.0f + (kx / (ky + eps)) *
                                         (kx / (ky + eps))));
    } else {
        ref = sqrt(dx * dx * (1.0f + (ky / (kx + eps)) *
                                         (ky / (kx + eps))));
    }
    const float rad = fmax(eps, sqrt(kx * kx + ky * ky));
    const float rapy = fabs((loy - lcy) / rad);
    const float gradreal = gradient * rapy + 1.0f;
    const float coef = rad / ref;
    const float fact = (coef - 1.0f) / (ach - 1.0f);
    return pow(fact, gradreal * gradreal);
}

/* iplocallab.cc calcTransitionrect(). */
inline void rt_la_transition_rect(const float lox, const float loy,
                                  const float ach, __global const float* rec,
                                  int* zone, float* localFactor)
{
    const float xc = rec[RT_LA_XC];
    const float yc = rec[RT_LA_YC];
    const float lx = rec[RT_LA_LX];
    const float ly = rec[RT_LA_LY];
    const float lxL = rec[RT_LA_LXL];
    const float lyT = rec[RT_LA_LYT];
    const float transgrad = rec[RT_LA_TRANSGRAD];
    const float transweak = rec[RT_LA_TRANSWEAK];

    *zone = 0;

    if (lox >= xc && lox < xc + lx) {
        if (loy >= yc && loy < yc + ly) {
            if (lox < xc + lx * ach && loy < yc + ly * ach) {
                *zone = 2;
            } else {
                *zone = 1;
                *localFactor = rt_lr_pow_F(
                    rt_la_calc_local_factor_rect(lox, loy, xc, lx, yc, ly,
                                                 ach, transgrad),
                    transweak);
            }
        } else if (loy < yc && loy > yc - lyT) {
            if (lox < xc + lx * ach && loy > yc - lyT * ach) {
                *zone = 2;
            } else {
                *zone = 1;
                *localFactor = rt_lr_pow_F(
                    rt_la_calc_local_factor_rect(lox, loy, xc, lx, yc, lyT,
                                                 ach, transgrad),
                    transweak);
            }
        }
    } else if (lox < xc && lox > xc - lxL) {
        if (loy <= yc && loy > yc - lyT) {
            if (lox > (xc - lxL * ach) && loy > (yc - lyT * ach)) {
                *zone = 2;
            } else {
                *zone = 1;
                *localFactor = rt_lr_pow_F(
                    rt_la_calc_local_factor_rect(lox, loy, xc, lxL, yc, lyT,
                                                 ach, transgrad),
                    transweak);
            }
        } else if (loy > yc && loy < yc + ly) {
            if (lox > (xc - lxL * ach) && loy < (yc + ly * ach)) {
                *zone = 2;
            } else {
                *zone = 1;
                *localFactor = rt_lr_pow_F(
                    rt_la_calc_local_factor_rect(lox, loy, xc, lxL, yc, ly,
                                                 ach, transgrad),
                    transweak);
            }
        }
    }
}

/*
 * The transition for the spot's shape: ELI is calcTransition(), RECT is
 * calcTransitionrect().
 */
inline void rt_la_transition(const float lox, const float loy,
                             const float ach, __global const float* rec,
                             int* zone, float* localFactor)
{
    if (rec[RT_LA_SHAPE] == 1.0f) {
        rt_la_transition_rect(lox, loy, ach, rec, zone, localFactor);
        return;
    }
    const float xc = rec[RT_LA_XC];
    const float yc = rec[RT_LA_YC];
    const float lx = rec[RT_LA_LX];
    const float ly = rec[RT_LA_LY];
    const float lxL = rec[RT_LA_LXL];
    const float lyT = rec[RT_LA_LYT];
    const float transgrad = rec[RT_LA_TRANSGRAD];
    const float transweak = rec[RT_LA_TRANSWEAK];

    *zone = 0;

    if (lox >= xc && lox < xc + lx) {
        if (loy >= yc && loy < yc + ly) {
            const float zoneVal =
                ((lox - xc) / (ach * lx)) * ((lox - xc) / (ach * lx)) +
                ((loy - yc) / (ach * ly)) * ((loy - yc) / (ach * ly));
            *zone = zoneVal < 1.0f ? 2 : 0;
            if (!*zone) {
                *zone = (zoneVal > 1.0f &&
                         (((lox - xc) / lx) * ((lox - xc) / lx) +
                          ((loy - yc) / ly) * ((loy - yc) / ly)) < 1.0f)
                            ? 1
                            : 0;
                if (*zone == 1) {
                    *localFactor = rt_lr_pow_F(
                        rt_la_calc_local_factor(lox, loy, xc, lx, yc, ly,
                                                ach, transgrad),
                        transweak);
                }
            }
        } else if (loy < yc && loy > yc - lyT) {
            const float zoneVal =
                ((lox - xc) / (ach * lx)) * ((lox - xc) / (ach * lx)) +
                ((loy - yc) / (ach * lyT)) * ((loy - yc) / (ach * lyT));
            *zone = zoneVal < 1.0f ? 2 : 0;
            if (!*zone) {
                *zone = (zoneVal > 1.0f &&
                         (((lox - xc) / lx) * ((lox - xc) / lx) +
                          ((loy - yc) / lyT) * ((loy - yc) / lyT)) < 1.0f)
                            ? 1
                            : 0;
                if (*zone == 1) {
                    *localFactor = rt_lr_pow_F(
                        rt_la_calc_local_factor(lox, loy, xc, lx, yc, lyT,
                                                ach, transgrad),
                        transweak);
                }
            }
        }
    } else if (lox < xc && lox > xc - lxL) {
        if (loy <= yc && loy > yc - lyT) {
            const float zoneVal =
                ((lox - xc) / (ach * lxL)) * ((lox - xc) / (ach * lxL)) +
                ((loy - yc) / (ach * lyT)) * ((loy - yc) / (ach * lyT));
            *zone = zoneVal < 1.0f ? 2 : 0;
            if (!*zone) {
                *zone = (zoneVal > 1.0f &&
                         (((lox - xc) / lxL) * ((lox - xc) / lxL) +
                          ((loy - yc) / lyT) * ((loy - yc) / lyT)) < 1.0f)
                            ? 1
                            : 0;
                if (*zone == 1) {
                    *localFactor = rt_lr_pow_F(
                        rt_la_calc_local_factor(lox, loy, xc, lxL, yc, lyT,
                                                ach, transgrad),
                        transweak);
                }
            }
        } else if (loy > yc && loy < yc + ly) {
            const float zoneVal =
                ((lox - xc) / (ach * lxL)) * ((lox - xc) / (ach * lxL)) +
                ((loy - yc) / (ach * ly)) * ((loy - yc) / (ach * ly));
            *zone = zoneVal < 1.0f ? 2 : 0;
            if (!*zone) {
                *zone = (zoneVal > 1.0f &&
                         (((lox - xc) / lxL) * ((lox - xc) / lxL) +
                          ((loy - yc) / ly) * ((loy - yc) / ly)) < 1.0f)
                            ? 1
                            : 0;
                if (*zone == 1) {
                    *localFactor = rt_lr_pow_F(
                        rt_la_calc_local_factor(lox, loy, xc, lxL, yc, ly,
                                                ach, transgrad),
                        transweak);
                }
            }
        }
    }
}

/*
 * LocLHCurve/LocHHCurve/LocCHCurve have a 501-entry LUTf: the host's
 * LUTf::operator[](float) clips at index 499, so the last half-entry returns
 * table[500] rather than interpolating. The context exports 65536 samples;
 * only the first 501 are the curve.
 */
inline float rt_la_loc_lut(__global const float* table, const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        return table[0];
    }
    if (index > 499.0f) {
        return table[500];
    }
    const float low = table[idx];
    return low + (table[idx + 1] - low) * (index - (float)idx);
}

/* iplocallab.cc's Color::huelab_to_huehsv2(), narrowed to float for the
 * 500-entry L/C/H tables. */
inline float rt_la_huehsv(const float HH)
{
    return (float)rt_lr_huelab_to_huehsv2(HH);
}

/*
 * Copy the spot ROI out of the full Lab image into a three-plane ROI buffer.
 * This is the host's bufcolorig/bufgb/tmp1 copy at the start of a tool block.
 * The destination is one buffer whose planes are `dst_pitch * bfh` apart, the
 * same layout the three-plane kernels read.
 */
__kernel void rt_local_adjustments_roi(__global const float* lab,
                                       __global float* dst,
                                       const int xstart, const int ystart,
                                       const int bfw, const int bfh,
                                       const int lab_pitch,
                                       const int lab_plane,
                                       const int dst_pitch,
                                       const int dst_plane)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }
    const int src = (y + ystart) * lab_pitch + (x + xstart);
    const int out = y * dst_pitch + x;
    dst[out] = lab[src];
    dst[dst_plane + out] = lab[lab_plane + src];
    dst[2 * dst_plane + out] = lab[2 * lab_plane + src];
}

/*
 * transit_shapedetect2()'s per-pixel tail, shared by the fused colour apply and
 * the standalone blend the vibrance and local-contrast tools use. `ml`/`ma`/
 * `mb` are the blurred original the deltaE scope compares against, `orig` is
 * the image before the tool, `fin` is the tool's output and `varsens` the
 * tool's own sensitivity (`lp.sens`, `lp.sensv` or `lp.senslc`).
 */
inline void rt_la_scope_blend(const float origL, const float origA,
                              const float origB, const float finL,
                              const float finA, const float finB,
                              const float ml, const float ma, const float mb,
                              const float localFactor, const float varsens,
                              __global const float* rec, float* outL,
                              float* outA, float* outB)
{
    const float refa = rec[RT_LA_CHROMAREF] * cos(rec[RT_LA_HUEREF]) * 327.68f;
    const float refb = rec[RT_LA_CHROMAREF] * sin(rec[RT_LA_HUEREF]) * 327.68f;
    const float refL = rec[RT_LA_LUMAREF] * 327.68f;

    const float abdelta2 = (refa - ma) * (refa - ma) +
                           (refb - mb) * (refb - mb);
    const float chrodelta2 =
        (sqrt(ma * ma + mb * mb) - rec[RT_LA_CHROMAREF] * 327.68f) *
        (sqrt(ma * ma + mb * mb) - rec[RT_LA_CHROMAREF] * 327.68f);
    const float huedelta2 = abdelta2 - chrodelta2;

    const float kL = rec[RT_LA_BALANCE] / (327.68f * 327.68f);
    const float kab = rt_la_balancedeltaE(rec[RT_LA_BALANCE]) /
                      (327.68f * 327.68f);
    const float kH = rec[RT_LA_BALANCEH];
    const float kch = rt_la_balancedeltaE(rec[RT_LA_BALANCEH]);
    const float dE = sqrt(kab * (kch * chrodelta2 + kH * huedelta2) +
                          kL * (refL - ml) * (refL - ml));

    const float thr = rec[RT_LA_THR];
    const float mindE = 2.0f + RT_LA_MINSCOPE * varsens * thr;
    const float maxdE = 5.0f + RT_LA_MAXSCOPE * varsens * (1.0f + 0.1f * thr);
    const float mindElim = 2.0f + RT_LA_MINSCOPE * (float)RT_LA_LIMSCOPE * thr;
    const float maxdElim =
        5.0f + RT_LA_MAXSCOPE * (float)RT_LA_LIMSCOPE * (1.0f + 0.1f * thr);

    float reducdE = rt_la_calcreducdE(
        dE, maxdE, mindE, maxdElim, mindElim, rec[RT_LA_ITERAT],
        RT_LA_LIMSCOPE, (int)varsens);
    if (varsens == 100.0f) {
        reducdE = 1.0f;
    }

    const float realstrdE = reducdE * (finL - origL);
    const float realstradE = reducdE * (finA - origA);
    const float realstrbdE = reducdE * (finB - origB);

    /* clipLoc() is the identity in this build; keep the host's expression. */
    *outL = origL + localFactor * realstrdE;
    *outA = rt_la_clipC(origA + localFactor * realstradE);
    *outB = rt_la_clipC(origB + localFactor * realstrbdE);
}

/*
 * The colour core, the elliptical transition, the deltaE scope against the
 * blurred original ROI and the blend. `lab` is the full image, read and
 * written in place; the blur planes are the ROI-sized original, already
 * Gaussian-blurred with the host's coefficients.
 */
__kernel void rt_local_adjustments_apply(__global float* lab,
                                         __global const float* blurL,
                                         __global const float* blurA,
                                         __global const float* blurB,
                                         __global const float* rec,
                                         __global const float* luts,
                                         __global const float* light_lut,
                                         __global const float* satur_lut,
                                         __global const float* saturmoins_lut,
                                         const int lab_pitch,
                                         const int lab_plane,
                                         const int bfw, const int bfh,
                                         const int xstart, const int ystart,
                                         const int blur_pitch)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }

    const int x = rx + xstart;
    const int y = ry + ystart;
    const int offset = y * lab_pitch + x;

    const float origL = lab[offset];
    const float origA = lab[lab_plane + offset];
    const float origB = lab[2 * lab_plane + offset];

    float corrL = origL;
    float corrA = origA;
    float corrB = origB;

    const float chro = rec[RT_LA_CHRO];
    const float ligh = rec[RT_LA_LIGH];
    const float cont = rec[RT_LA_CONT];

    if (chro != 0.0f) {
        float chroma = sqrt(corrA * corrA + corrB * corrB);
        float2 sincosval;
        sincosval.y = chroma == 0.0f ? 1.0f : corrA / chroma;
        sincosval.x = chroma == 0.0f ? 0.0f : corrB / chroma;
        if (chro > 0.0f) {
            chroma = rt_la_satur_lut(satur_lut, chroma) * 35000.0f;
        } else {
            chroma = rt_la_satur_lut(saturmoins_lut, chroma) * 35000.0f;
        }
        if (chro == -100.0f) {
            chroma = 0.0f;
        }
        corrA = chroma * sincosval.y;
        corrB = chroma * sincosval.x;
    }

    const float qcurve = rec[RT_LA_QCURVE];
    const float adjustr = rec[RT_LA_ADJUSTR];
    const float amountchrom = rec[RT_LA_AMOUNTCHROM];

    if (qcurve != 0.0f) {
        if (rec[RT_LA_CC_ACTIVE] != 0.0f) { /* C=f(C) */
            const float chromat = sqrt(corrA * corrA + corrB * corrB);
            const float ch = rt_la_lut_off(
                luts + (int)rec[RT_LA_CC_OFFSET], 65534.0f,
                chromat * adjustr) / ((chromat + 0.00001f) * adjustr);
            corrA *= ch;
            corrB *= ch;
        }
        if (rec[RT_LA_CL_ACTIVE] != 0.0f) { /* C=f(L) */
            const float factor = rt_la_lut_off(
                luts + (int)rec[RT_LA_CL_OFFSET], 65534.0f,
                corrL * 2.0f) / (corrL * 2.0f);
            corrA *= factor;
            corrB *= factor;
        }
        if (rec[RT_LA_LC_ACTIVE] != 0.0f) { /* L=f(C) */
            const float chromat = sqrt(corrA * corrA + corrB * corrB);
            float lc = rt_la_lut_off(
                luts + (int)rec[RT_LA_LC_OFFSET], 65534.0f,
                chromat * adjustr) / ((chromat + 0.00001f) * adjustr);
            lc = lc > 1.0f ? (lc - 1.0f) * 0.1f + 1.0f
                           : (lc - 1.0f) * 0.3f + 1.0f;
            corrL *= lc;
        }
        if (rec[RT_LA_HH_ACTIVE] != 0.0f) { /* H=f(H) */
            const float chromat = sqrt(corrA * corrA + corrB * corrB);
            const float hh = rt_lt_xatan2f(corrB, corrA);
            const float table = rt_la_loc_lut(
                luts + (int)rec[RT_LA_HH_OFFSET],
                500.0f * rt_la_huehsv(hh));
            const float valparam = 2.0f * (table - 0.5f) + hh;
            const float2 sincosval = rt_lt_xsincosf(valparam);
            corrA = chromat * sincosval.y;
            corrB = chromat * sincosval.x;
        }
    }

    if (ligh != 0.0f || cont != 0.0f) {
        corrL = rt_la_light_lut(light_lut, corrL);
    }

    if (qcurve != 0.0f) {
        if (rec[RT_LA_LL_ACTIVE] != 0.0f) { /* L=f(L) */
            corrL = 0.5f * rt_la_lut_off(
                luts + (int)rec[RT_LA_LL_OFFSET], 65534.0f, corrL * 2.0f);
        }
        if (rec[RT_LA_LH_ACTIVE] != 0.0f) { /* L=f(H) */
            const float rhue = rt_lt_xatan2f(corrB, corrA);
            const float chromat =
                sqrt(corrA * corrA + corrB * corrB) / 32768.0f;
            float lr = rt_lt_lim01(corrL / 32768.0f);
            const float table = rt_la_loc_lut(
                luts + (int)rec[RT_LA_LH_OFFSET],
                500.0f * rt_la_huehsv(rhue));
            float valparam = table - 0.5f;
            float valparamneg = valparam;
            const float kc = amountchrom;
            const double kccd = (double)chromat / (double)kc;
            const float kcc = (float)(kccd * kccd);
            valparam *= 2.0f * kcc;
            valparamneg *= kcc;
            if (valparam > 0.0f) {
                const float one_minus = 1.0f - fmin(lr, 1.0f);
                lr = (1.0f - valparam) * lr +
                     valparam * (1.0f - one_minus * one_minus *
                                           one_minus * one_minus);
            } else {
                lr *= (1.0f + 1.9f * valparamneg);
            }
            corrL = lr * 32768.0f;
        }
        if (rec[RT_LA_CH_ACTIVE] != 0.0f) { /* C=f(H) */
            const float rhue = rt_lt_xatan2f(corrB, corrA);
            const float table = rt_la_loc_lut(
                luts + (int)rec[RT_LA_CH_OFFSET],
                500.0f * rt_la_huehsv(rhue));
            const float factor = 1.0f + (table - 0.5f);
            corrA *= factor;
            corrB *= factor;
        }
    }

    int zone = 0;
    float localFactor = 1.0f;
    const float achm = rec[RT_LA_TRANS] / 100.0f;
    rt_la_transition((float)x, (float)y, achm, rec, &zone, &localFactor);
    if (zone == 0) {
        return;
    }

    const int bo = ry * blur_pitch + rx;
    float blendedL = 0.0f;
    float blendedA = 0.0f;
    float blendedB = 0.0f;
    rt_la_scope_blend(origL, origA, origB, corrL, corrA, corrB, blurL[bo],
                      blurA[bo], blurB[bo], localFactor, rec[RT_LA_SENS], rec,
                      &blendedL, &blendedA, &blendedB);
    lab[offset] = blendedL;
    lab[lab_plane + offset] = blendedA;
    lab[2 * lab_plane + offset] = blendedB;
}

/*
 * The standalone blend for the vibrance and local-contrast tools: the tool has
 * already written its output into the ROI buffer `fin`, and the original is
 * still the full-image pixel. `varsens` is lp.sensv or lp.senslc.
 */
__kernel void rt_local_adjustments_blend(__global float* lab,
                                         __global const float* fin,
                                         __global const float* blurL,
                                         __global const float* blurA,
                                         __global const float* blurB,
                                         __global const float* rec,
                                         const int lab_pitch,
                                         const int lab_plane,
                                         const int fin_pitch,
                                         const int fin_plane,
                                         const int bfw, const int bfh,
                                         const int xstart, const int ystart,
                                         const int blur_pitch,
                                         const float varsens)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }

    const int x = rx + xstart;
    const int y = ry + ystart;
    int zone = 0;
    float localFactor = 1.0f;
    const float achm = rec[RT_LA_TRANS] / 100.0f;
    rt_la_transition((float)x, (float)y, achm, rec, &zone, &localFactor);
    if (zone == 0) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const int fo = ry * fin_pitch + rx;
    const int bo = ry * blur_pitch + rx;

    float blendedL = 0.0f;
    float blendedA = 0.0f;
    float blendedB = 0.0f;
    rt_la_scope_blend(lab[offset], lab[lab_plane + offset],
                      lab[2 * lab_plane + offset], fin[fo],
                      fin[fin_plane + fo], fin[2 * fin_plane + fo],
                      blurL[bo], blurA[bo], blurB[bo], localFactor, varsens,
                      rec, &blendedL, &blendedA, &blendedB);
    lab[offset] = blendedL;
    lab[lab_plane + offset] = blendedA;
    lab[2 * lab_plane + offset] = blendedB;
}

/*
 * transit_shapedetect2()'s senstype-11 arm for the log tool. The sensitivity
 * is lp.sensilog. Below 50 the host raises the threshold and iteration count
 * by a fifth and divides the lightness balance by 1.2; at 50 and above it adds
 * 10 to the threshold, subtracts 2.3 from the balance and replaces the
 * reduction with a sensitivity ramp, clamped to 0.1..1. The record carries the
 * adjusted threshold, iteration count and balance (the host mutation persists
 * for later tools) and the pre-adjustment balancedeltaE() for kab.
 */
__kernel void rt_local_adjustments_blend_log(
    __global float* lab, __global const float* fin,
    __global const float* blurL, __global const float* blurA,
    __global const float* blurB, __global const float* rec,
    const int lab_pitch, const int lab_plane, const int fin_pitch,
    const int fin_plane, const int bfw, const int bfh, const int xstart,
    const int ystart, const int blur_pitch)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }

    const int x = rx + xstart;
    const int y = ry + ystart;
    int zone = 0;
    float localFactor = 1.0f;
    rt_la_transition((float)x, (float)y, rec[RT_LA_TRANS] / 100.0f, rec,
                     &zone, &localFactor);
    if (zone == 0) {
        return;
    }

    const int bo = ry * blur_pitch + rx;
    const float ml = blurL[bo];
    const float ma = blurA[bo];
    const float mb = blurB[bo];
    const float varsens = rec[RT_LA_LOG_SENS];
    const float thr = rec[RT_LA_THR];

    const float refa = rec[RT_LA_CHROMAREF] * cos(rec[RT_LA_HUEREF]) * 327.68f;
    const float refb = rec[RT_LA_CHROMAREF] * sin(rec[RT_LA_HUEREF]) * 327.68f;
    const float refL = rec[RT_LA_LUMAREF] * 327.68f;

    const float abdelta2 = (refa - ma) * (refa - ma) +
                           (refb - mb) * (refb - mb);
    const float chroma = sqrt(ma * ma + mb * mb);
    const float chromadelta = chroma - rec[RT_LA_CHROMAREF] * 327.68f;
    const float chrodelta2 = chromadelta * chromadelta;
    const float huedelta2 = abdelta2 - chrodelta2;

    /* kL is the adjusted balance; kab is the pre-adjustment value. */
    const float scale = 327.68f * 327.68f;
    float kL = rec[RT_LA_BALANCE] / scale;
    if (varsens < 50.0f) {
        kL /= 1.2f;
    }
    const float kab = rec[RT_LA_LOG_KAB_PRE];
    const float kH = rec[RT_LA_BALANCEH];
    const float kch = rt_la_balancedeltaE(kH);
    const float dE = sqrt(kab * (kch * chrodelta2 + kH * huedelta2) +
                          kL * (refL - ml) * (refL - ml));

    const float mindE = 2.0f + RT_LA_MINSCOPE * varsens * thr;
    const float maxdE = 5.0f + RT_LA_MAXSCOPE * varsens * (1.0f + 0.1f * thr);
    const float mindElim =
        2.0f + RT_LA_MINSCOPE * (float)RT_LA_LIMSCOPE * thr;
    const float maxdElim =
        5.0f + RT_LA_MAXSCOPE * (float)RT_LA_LIMSCOPE * (1.0f + 0.1f * thr);
    float reducdE = rt_la_calcreducdE(
        dE, maxdE, mindE, maxdElim, mindElim, rec[RT_LA_ITERAT],
        RT_LA_LIMSCOPE, (int)varsens);
    if (varsens >= 50.0f) {
        const float maxvarsens = 90.0f;
        const float ared = (1.0f - reducdE) / (maxvarsens - 50.0f);
        const float bred = 1.0f - ared * maxvarsens;
        reducdE = ared * varsens + bred;
        reducdE = fmin(fmax(reducdE, 0.1f), 1.0f);
    }
    if (varsens == 100.0f) {
        reducdE = 1.0f;
    }
    const int offset = y * lab_pitch + x;
    const int fo = ry * fin_pitch + rx;
    const float origL = lab[offset];
    const float origA = lab[lab_plane + offset];
    const float origB = lab[2 * lab_plane + offset];
    const float realstrdE =
        reducdE * (fin[fo] - origL);
    const float realstradE =
        reducdE * (fin[fin_plane + fo] - origA);
    const float realstrbdE =
        reducdE * (fin[2 * fin_plane + fo] - origB);

    lab[offset] = origL + localFactor * realstrdE;
    lab[lab_plane + offset] = rt_la_clipC(origA + localFactor * realstradE);
    lab[2 * lab_plane + offset] = rt_la_clipC(origB + localFactor * realstrbdE);
}

/*
 * Local CBDL. cbdl_local_temp() is the lab.wavelet node's directional
 * pyramid: dirpyr_channel() and idirpyr_eq_channel_loc() are the
 * rt_lab_wavelet_decompose/rt_lab_wavelet_equalize kernels with the block's
 * host-built range tables, and skin protection is always zero in the local
 * tool. These kernels add the ROI source, the final level merge and the two
 * transit_shapedetect() blends (senstype 6 for L, 7 for chroma).
 */

/* The tool's source plane: 0 copies L, 1 is sqrt(a * a + b * b). */
__kernel void rt_local_adjustments_cbdl_fill(__global const float* work,
                                             const int work_pitch,
                                             const int work_plane,
                                             __global float* dst,
                                             const int dst_pitch,
                                             const int bfw, const int bfh,
                                             const int mode)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }
    const int in = y * work_pitch + x;
    const float value =
        mode == 0
            ? work[in]
            : sqrt(work[work_plane + in] * work[work_plane + in] +
                   work[2 * work_plane + in] * work[2 * work_plane + in]);
    dst[y * dst_pitch + x] = value;
}

/* residbuff's seed: the coarsest pyramid level, before reconstruction. */
__kernel void rt_local_adjustments_cbdl_copy(__global const float* src,
                                             const int src_pitch,
                                             __global float* dst,
                                             const int dst_pitch,
                                             const int bfw, const int bfh)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }
    dst[y * dst_pitch + x] = src[y * src_pitch + x];
}

/*
 * cbdl_local_temp()'s tail: loct is residbuff clamped to [0, 32768], and the
 * clarity merge is (1 + clar) * loct - clar * resid5, clamped again. `resid5`
 * is the untouched coarsest level; the reconstruction writes a separate copy.
 */
__kernel void rt_local_adjustments_cbdl_finish(__global const float* resid,
                                               const int resid_pitch,
                                               __global const float* resid5,
                                               const int resid5_pitch,
                                               __global float* out,
                                               const int out_pitch,
                                               const int bfw, const int bfh,
                                               const float clar)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }
    const float loct = fmax(0.0f, fmin(resid[y * resid_pitch + x], 32768.0f));
    float value = loct;
    if (clar > 0.0f) {
        value = (1.0f + clar) * loct -
                clar * resid5[y * resid5_pitch + x];
        value = fmax(0.0f, fmin(value, 32768.0f));
    }
    out[y * out_pitch + x] = value;
}

/*
 * The chroma pass's normaliser. bufchrom is
 * loctemp->L - sqrt(a * a + b * b); the block scales it by
 * 1 / (0.01 * max |value|). One work item per row scans the row and writes its
 * max magnitude, which the host reduces with the block's own expression.
 */
__kernel void rt_local_adjustments_cbdl_metric(__global const float* out,
                                               const int out_pitch,
                                               __global const float* lab,
                                               const int lab_pitch,
                                               const int lab_plane,
                                               const int xstart,
                                               const int ystart,
                                               const int bfw, const int bfh,
                                               __global float* rowmax)
{
    const int ry = (int)get_global_id(0);
    if (ry >= bfh) {
        return;
    }
    float magnitude = 0.0f;
    for (int rx = 0; rx < bfw; ++rx) {
        const int offset = (ry + ystart) * lab_pitch + (rx + xstart);
        const float a = lab[lab_plane + offset];
        const float b = lab[2 * lab_plane + offset];
        const float raw =
            out[ry * out_pitch + rx] - sqrt(a * a + b * b);
        magnitude = fmax(magnitude, fabs(raw));
    }
    rowmax[ry] = magnitude;
}

/* transit_shapedetect()'s deltaE for the CBDL passes. The reference is the
 * blurred original ROI; only the lightness balance weights are read, exactly
 * as in the senstype 6/7 arm. */
inline float rt_la_cbdl_deltae(__global const float* rec, const float blurL,
                               const float blurA, const float blurB)
{
    const float refa = rec[RT_LA_CHROMAREF] * cos(rec[RT_LA_HUEREF]) * 327.68f;
    const float refb = rec[RT_LA_CHROMAREF] * sin(rec[RT_LA_HUEREF]) * 327.68f;
    const float refL = rec[RT_LA_LUMAREF] * 327.68f;
    const float kL = rec[RT_LA_BALANCE] / (327.68f * 327.68f);
    const float kab = rt_la_balancedeltaE(rec[RT_LA_BALANCE]) /
                      (327.68f * 327.68f);
    return sqrt(kab * ((refa - blurA) * (refa - blurA) +
                       (refb - blurB) * (refb - blurB)) +
                kL * (refL - blurL) * (refL - blurL));
}

inline float rt_la_cbdl_reducdE(__global const float* rec, const float dE)
{
    const float varsens = rec[RT_LA_SENSCB];
    /* The scope thresholds use the spot's deltaE threshold (lp.thr), not the
     * CBDL range-table threshold. */
    const float thr = rec[RT_LA_THR];
    const float mindE = 2.0f + RT_LA_MINSCOPE * varsens * thr;
    const float maxdE = 5.0f + RT_LA_MAXSCOPE * varsens * (1.0f + 0.1f * thr);
    const float mindElim = 2.0f + RT_LA_MINSCOPE * (float)RT_LA_LIMSCOPE * thr;
    const float maxdElim =
        5.0f + RT_LA_MAXSCOPE * (float)RT_LA_LIMSCOPE * (1.0f + 0.1f * thr);
    return rt_la_calcreducdE(dE, maxdE, mindE, maxdElim, mindElim,
                             rec[RT_LA_ITERAT], RT_LA_LIMSCOPE,
                             (int)varsens);
}

/* transit_shapedetect(6): the luminance change with the block's CLIP to
 * 65535. The chroma table is zero for this pass, so a and b do not move. */
__kernel void rt_local_adjustments_cbdl_blend_luma(
    __global float* lab, __global const float* fin,
    __global const float* blurL, __global const float* blurA,
    __global const float* blurB, __global const float* rec,
    const int lab_pitch, const int lab_plane, const int fin_pitch,
    const int bfw, const int bfh, const int xstart, const int ystart,
    const int blur_pitch)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }
    const int x = rx + xstart;
    const int y = ry + ystart;
    int zone = 0;
    float localFactor = 1.0f;
    rt_la_transition((float)x, (float)y, rec[RT_LA_TRANS] / 100.0f, rec,
                     &zone, &localFactor);
    if (zone == 0) {
        return;
    }
    const int bo = ry * blur_pitch + rx;
    const float blur_l = blurL[bo];
    const float blur_a = blurA[bo];
    const float blur_b = blurB[bo];
    if (!(blur_l / 327.68f > 0.1f)) {
        return;
    }
    const float dE = rt_la_cbdl_deltae(rec, blur_l, blur_a, blur_b);
    const float reducdE = rt_la_cbdl_reducdE(rec, dE);
    const int offset = y * lab_pitch + x;
    const float origL = lab[offset];
    const float difL =
        (fin[ry * fin_pitch + rx] - origL) * localFactor * reducdE;
    lab[offset] = fmax(0.0f, fmin(origL + difL, 65535.0f));
}

/* transit_shapedetect(7): the chroma change. `coefC` is the block's
 * 1 / (0.01 * max |bufchrom|), applied to the raw difference. */
__kernel void rt_local_adjustments_cbdl_blend_chroma(
    __global float* lab, __global const float* fin,
    __global const float* blurL, __global const float* blurA,
    __global const float* blurB, __global const float* rec,
    const int lab_pitch, const int lab_plane, const int fin_pitch,
    const int bfw, const int bfh, const int xstart, const int ystart,
    const int blur_pitch, const float coefC)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }
    const int x = rx + xstart;
    const int y = ry + ystart;
    int zone = 0;
    float localFactor = 1.0f;
    rt_la_transition((float)x, (float)y, rec[RT_LA_TRANS] / 100.0f, rec,
                     &zone, &localFactor);
    if (zone == 0) {
        return;
    }
    const int bo = ry * blur_pitch + rx;
    const float blur_l = blurL[bo];
    const float blur_a = blurA[bo];
    const float blur_b = blurB[bo];
    if (!(blur_l / 327.68f > 0.1f)) {
        return;
    }
    const int offset = y * lab_pitch + x;
    const float origA = lab[lab_plane + offset];
    const float origB = lab[2 * lab_plane + offset];
    const float raw =
        fin[ry * fin_pitch + rx] - sqrt(origA * origA + origB * origB);
    const float dE = rt_la_cbdl_deltae(rec, blur_l, blur_a, blur_b);
    const float reducdE = rt_la_cbdl_reducdE(rec, dE);
    const float clc = raw * coefC;
    const float realstr = reducdE * clc;
    const float rhue = rt_lt_xatan2f(blur_b, blur_a);
    const float2 sincosval = rt_lt_xsincosf(rhue);
    float difa = raw * sincosval.y;
    float difb = raw * sincosval.x;
    difa *= localFactor * (100.0f + realstr) / 100.0f;
    difb *= localFactor * (100.0f + realstr) / 100.0f;
    lab[lab_plane + offset] = rt_la_clipC(origA + difa);
    lab[2 * lab_plane + offset] = rt_la_clipC(origB + difb);
}

/*
 * Color::AllMunsellLch(true, Lprov1, Lprov2, HH, Chprov1, CC,
 * correctionHue, correctlum). The fragment's rt_lt_munsell_lch() is the
 * host MunsellLch() port; this wrapper is the lumaMuns branch.
 */
inline void rt_la_all_munsell_lch(const float Lprov1, const float Loldd,
                                  const float HH, const float Chprov1_in,
                                  const float CC, float* correctionHuechroma,
                                  float* correctlum,
                                  __global const float* tables,
                                  __global const float* sizes)
{
    *correctionHuechroma = 0.0f;
    *correctlum = 0.0f;
    if (CC < 6.0f || CC >= 140.0f) {
        return;
    }
    const float huelimit[8] = {-2.48f, -0.55f, 0.44f, 1.52f,
                               1.87f, 3.09f, -0.27f, 0.44f};
    const float chrom = fmin(fmax(Chprov1_in, 6.0f), 140.0f);
    for (int zo = 1; zo <= 4; ++zo) {
        if (HH > huelimit[2 * zo - 2] && HH < huelimit[2 * zo - 1]) {
            int correctL = 0;
            float correctionHue = 0.0f;
            float correctionHueLum = 0.0f;
            rt_lt_munsell_lch(Lprov1, HH, chrom, CC, &correctionHue, zo,
                              &correctionHueLum, &correctL, tables, sizes);
            *correctionHuechroma = correctionHue;

            bool contin1 = false;
            float correctlumprov = 0.0f;
            if (correctL != 0) {
                correctlumprov = correctionHueLum;
                contin1 = true;
                correctL = 0;
            }
            if (fabs(Lprov1 - Loldd) > 6.0f) {
                correctionHueLum = 0.0f;
                correctionHue = 0.0f;
                rt_lt_munsell_lch(Loldd, HH, chrom, chrom, &correctionHue, zo,
                                  &correctionHueLum, &correctL, tables, sizes);
                if (correctL != 0 && contin1) {
                    *correctlum = correctionHueLum - correctlumprov;
                }
            }
            break;
        }
    }
}

/*
 * ImProcFunctions::avoidcolshi()'s MUNS branch. `reserved` is the fixed
 * start-of-block Lab image; `lab` is the current transformed image.
 */
__kernel void rt_local_adjustments_munsell(__global float* lab,
                                           __global const float* reserved,
                                           __global const float* rec,
                                           __global const float* tables,
                                           __global const float* sizes,
                                           const int width, const int height,
                                           const int lab_pitch,
                                           const int lab_plane)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    if (rec[RT_LA_ISLOCAL] == 0.0f ||
        rec[RT_LA_AVOIDGAMUT] != (float)RT_LA_AVOID_MUNS) {
        return;
    }

    const float yc = rec[RT_LA_YC];
    const float ly = rec[RT_LA_LY];
    const float lyT = rec[RT_LA_LYT];
    const bool isZone0 = (float)y > yc + ly || (float)y < yc - lyT;
    if (isZone0) {
        return;
    }

    int zone = 0;
    float localFactor = 1.0f;
    const float achm = rec[RT_LA_TRANS] / 100.0f;
    rt_la_transition((float)x, (float)y, achm, rec, &zone, &localFactor);
    if (zone == 0) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float anew = lab[lab_plane + offset];
    const float bnew = lab[2 * lab_plane + offset];
    const float Lprov1 = lab[offset] / 327.68f;
    const float raw = sqrt(anew * anew + bnew * bnew);
    const float Chprov = raw / 327.68f;

    /*
     * avoidcolshi() normalises the sine and cosine differently in its SIMD
     * loop and its scalar tail: the SIMD loop divides by the raw chroma,
     * while the scalar tail divides by the chroma after the 327.68 scale.
     * The clamp already splits at width % 4 for the same reason; reproduce
     * the split here so the chroma normalisation follows the host's order.
     */
    const int tail = rt_la_host_tail(width);
    const bool scalar_tail = tail != 0 && x >= width - tail;
    float2 sincosval;
    if (raw == 0.0f) {
        sincosval.y = 1.0f;
        sincosval.x = 0.0f;
    } else if (scalar_tail) {
        sincosval.y = anew / (Chprov * 327.68f);
        sincosval.x = bnew / (Chprov * 327.68f);
    } else {
        sincosval.y = anew / raw;
        sincosval.x = bnew / raw;
    }
    float HH = rt_lt_xatan2f(bnew, anew);

    const float Lprov2 = reserved[offset] / 327.68f;
    const float rA = reserved[lab_plane + offset];
    const float rB = reserved[2 * lab_plane + offset];
    const float memChprov = sqrt(rA * rA + rB * rB) / 327.68f;

    float correctionHue = 0.0f;
    float correctlum = 0.0f;
    rt_la_all_munsell_lch(Lprov1, Lprov2, HH, Chprov, memChprov,
                          &correctionHue, &correctlum, tables, sizes);

    if (correctionHue != 0.0f || correctlum != 0.0f) {
        if (fabs(correctionHue) < 0.015f) {
            HH += correctlum;
        }
        sincosval = rt_lt_xsincosf(HH + correctionHue);
    }

    lab[lab_plane + offset] = 327.68f * Chprov * sincosval.y;
    lab[2 * lab_plane + offset] = 327.68f * Chprov * sincosval.x;
}

/*
 * Color::Lab2XYZ() as lab2rgb() evaluates it. The host's SIMD loop computes
 * `fy * fy * fy` and then scales by 65535, while its scalar tail multiplies
 * by 65535 first; the two orders round differently. The clamp must reproduce
 * that split because a one-LSB Y difference moves Munsell's zone thresholds.
 */
inline void rt_la_lab2xyz(const float L, const float a, const float b,
                          const int x, const int width,
                          float* px, float* py, float* pz)
{
    const float LL = L / 327.68f;
    const float aa = a / 327.68f;
    const float bb = b / 327.68f;
    const float fy = (RT_LT_C1BY116 * LL) + RT_LT_C16BY116;
    const float fx = (0.002f * aa) + fy;
    const float fz = fy - (0.005f * bb);
    const float fx3 = fx * fx * fx;
    const float fz3 = fz * fz * fz;
    const float fxl = (116.0f * fx - 16.0f) * RT_LT_KAPPAINV;
    const float fzl = (116.0f * fz - 16.0f) * RT_LT_KAPPAINV;
    *px = 65535.0f * ((fx > RT_LT_EPSINV3) ? fx3 : fxl) * RT_LT_D50X;
    *pz = 65535.0f * ((fz > RT_LT_EPSINV3) ? fz3 : fzl) * RT_LT_D50Z;

    const int tail = rt_la_host_tail(width);
    if (tail != 0 && x >= width - tail) {
        *py = (LL > RT_LT_EPSKAP) ? 65535.0f * fy * fy * fy
                                  : 65535.0f * LL / RT_LT_KAPPA;
    } else {
        const float cube = fy * fy * fy;
        *py = ((LL > RT_LT_EPSKAP) ? cube : LL / RT_LT_KAPPA) * 65535.0f;
    }
}

/* Color::cachef[]/cachefy[] are LUT_CLIP_BELOW only, so a value above the
 * second-to-last entry extrapolates from data[65534], while the shared
 * lab.tools helper clips above at the last entry. The clamp uses this
 * local, host-faithful copy. */
inline float rt_la_cache_lookup(__global const float* table, const float value)
{
    if (value < 0.0f) {
        return table[0];
    }
    int idx = (int)value;
    if (value > 65534.0f) {
        idx = 65534;
    }
    const float diff = value - (float)idx;
    const float low = table[idx];
    return low + (table[idx + 1] - low) * diff;
}

inline float rt_la_compute_f(const float value, __global const float* cache)
{
    if (value < 0.0f) {
        return (float)(327.68 * ((903.2962962962963 * (double)value /
                                  65535.0 + 16.0) / 116.0));
    }
    if (value > RT_LT_MAXVAL) {
        return 327.68f * rt_lt_cbrt(value / RT_LT_MAXVAL);
    }
    return rt_la_cache_lookup(cache, value);
}

inline float rt_la_compute_fy(const float value, __global const float* cache)
{
    if (value < 0.0f) {
        return (float)(327.68 * (903.2962962962963 * (double)value /
                                 65535.0));
    }
    if (value > RT_LT_MAXVAL) {
        return 327.68f * (116.0f * rt_lt_cbrt(value / RT_LT_MAXVAL) - 16.0f);
    }
    return rt_la_cache_lookup(cache, value);
}

inline void rt_la_xyz2lab(const float X, const float Y, const float Z,
                          __global const float* cachef,
                          __global const float* cachefy,
                          float* L, float* a, float* b)
{
    const float fx = rt_la_compute_f(X / RT_LT_D50X, cachef);
    const float fy = rt_la_compute_f(Y, cachef);
    const float fz = rt_la_compute_f(Z / RT_LT_D50Z, cachef);
    *L = rt_la_compute_fy(Y, cachefy);
    *a = 500.0f * (fx - fy);
    *b = 200.0f * (fy - fz);
}

/*
 * ipsoftlight.cc's sl(): Pegtop's soft-light curve on one working-RGB
 * channel. The two gamma tables are Color::gammatab_srgb and
 * Color::igammatab_srgb, read with the same LUTf interpolation the host uses.
 * A channel outside the 16-bit range is returned unchanged, as OOG() does.
 */
inline float rt_la_soft_sl(const float blend, const float x,
                           __global const float* gamma_tab,
                           __global const float* igamma_tab)
{
    if (x < 0.0f || x > 65535.0f) {
        return x;
    }
    const float v = rt_la_lut_off(gamma_tab, 65534.0f, x) / 65535.0f;
    const float curved = rt_la_lut_off(
        igamma_tab, 65534.0f,
        v * v * (3.0f - 2.0f * v) * 65535.0f);
    return blend * (curved - x) + x;
}

/*
 * ImProcFunctions::softLight() on the ROI: Lab -> XYZ -> working RGB, the
 * per-channel soft-light curve, then back to XYZ -> Lab. The working
 * matrices, the Lab caches and the sRGB gamma pair are host-built. The
 * lab2rgb() scalar-tail multiplication order is reproduced by
 * rt_la_lab2xyz() so the last columns round like the host.
 */
__kernel void rt_local_adjustments_softlight(
    __global float* work, __global const float* gamma_tab,
    __global const float* igamma_tab, __global const float* ws,
    __global const float* iws, __global const float* cachef,
    __global const float* cachefy, const int bfw, const int bfh,
    const int work_pitch, const int work_plane, const float strength)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }

    const int offset = ry * work_pitch + rx;
    const float l = work[offset];
    const float a = work[work_plane + offset];
    const float b = work[2 * work_plane + offset];

    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    rt_la_lab2xyz(l, a, b, rx, bfw, &X, &Y, &Z);

    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    rt_lt_xyz2rgb(X, Y, Z, iws, &R, &G, &B);

    const float blend = strength / 100.0f;
    R = rt_la_soft_sl(blend, R, gamma_tab, igamma_tab);
    G = rt_la_soft_sl(blend, G, gamma_tab, igamma_tab);
    B = rt_la_soft_sl(blend, B, gamma_tab, igamma_tab);

    X = ws[0] * R + ws[1] * G + ws[2] * B;
    Y = ws[3] * R + ws[4] * G + ws[5] * B;
    Z = ws[6] * R + ws[7] * G + ws[8] * B;

    float out_l = 0.0f;
    float out_a = 0.0f;
    float out_b = 0.0f;
    rt_la_xyz2lab(X, Y, Z, cachef, cachefy, &out_l, &out_a, &out_b);
    work[offset] = out_l;
    work[work_plane + offset] = out_a;
    work[2 * work_plane + offset] = out_b;
}

/*
 * Local log encoding. ImProcFunctions::log_encode()'s per-pixel arm:
 *
 *   norm2(r, g, b) -> the apply lambda -> the per-channel scale and its
 *   saturation control -> the tool's `repar` recovery blend.
 *
 * The apply scalars are host-built (gray, shadows_range, dynamic_range,
 * comprthlog, linbase, noise, log2), and find_gray() stays on the host. The
 * block's guided-filter local-contrast arm (detail != 0) and its gradient are
 * residuals, so this kernel is per pixel. The sRGB RGB copy before the encode
 * is `orig`; `rgb` holds the encoded result and receives the intp() recovery
 * blend.
 */

/* std::numeric_limits<float>::max() / 100.f, as norm2() uses. */
#define RT_LA_LOG_FLOAT_MAX 3.4028234663852886e+36f

/* iplocallab.cc power_norm(). */
inline float rt_la_log_power_norm(const float r, const float g,
                                  const float b)
{
    const float ar = fabs(r);
    const float ag = fabs(g);
    const float ab = fabs(b);
    const float r2 = ar * ar;
    const float g2 = ag * ag;
    const float b2 = ab * ab;
    const float d = r2 + g2 + b2;
    const float n = ar * r2 + ag * g2 + ab * b2;
    return n / fmax(d, 1e-12f);
}

/* Color::rgbLuminance(r, g, b, TMatrix), which accumulates in double. */
inline float rt_la_log_luminance(const float r, const float g,
                                 const float b,
                                 __global const double* ws_double)
{
    const double luminance = (double)r * ws_double[3] +
                             (double)g * ws_double[4] +
                             (double)b * ws_double[5];
    return (float)luminance;
}

/* iplocallab.cc norm2(). */
inline float rt_la_log_norm2(const float r, const float g, const float b,
                             __global const double* ws_double)
{
    return fmin(RT_LA_LOG_FLOAT_MAX,
                rt_la_log_power_norm(r, g, b) / 2.0f +
                    rt_la_log_luminance(r, g, b, ws_double) / 2.0f);
}

/* sleef.h xlog2lin(). */
inline float rt_la_log_xlog2lin(const float x, const float base)
{
    return (rt_lr_pow_F(base, x) - 1.0f) / (base - 1.0f);
}

/* log_encode()'s apply lambda. */
inline float rt_la_log_apply(const float value, __global const float* rec)
{
    const float noise = rec[RT_LA_LOG_NOISE];
    float x = value / 65535.0f;
    x = fmax(x, noise);
    x = fmax(x / rec[RT_LA_LOG_GRAY], noise);
    const float comprthlog = rec[RT_LA_LOG_COMPRTHLOG];
    if (rec[RT_LA_LOG_COMPRLOG] != 0.0f && x >= comprthlog) {
        x = rt_lt_intp(rec[RT_LA_LOG_COMPRFACTOR],
                       (tanh((x - comprthlog) / comprthlog) + 1.0f) *
                           comprthlog,
                       x);
    }
    x = fmax((rt_lr_xlogf(x) / rec[RT_LA_LOG_LOG2] -
              rec[RT_LA_LOG_SHADOWS_RANGE]) /
                 rec[RT_LA_LOG_DYNAMIC_RANGE],
             noise);
    if (rec[RT_LA_LOG_LINBASE] > 0.0f) {
        x = rt_la_log_xlog2lin(x, rec[RT_LA_LOG_LINBASE]);
    }
    return x * 65535.0f;
}

/* log_encode()'s sf() and apply_sat(). */
inline float rt_la_log_sf(const float s, const float c, const float noise)
{
    if (c > noise) {
        return 1.0f - fmin(fabs(s) / c, 1.0f);
    }
    return 0.0f;
}

inline void rt_la_log_apply_sat(float* r, float* g, float* b, const float f,
                                __global const float* rec,
                                __global const double* ws_double)
{
    const float noise = rec[RT_LA_LOG_NOISE];
    const float ll = rt_la_log_luminance(*r, *g, *b, ws_double);
    const float rl = *r - ll;
    const float gl = *g - ll;
    const float bl = *b - ll;
    const float s = rt_lt_intp(
        fmax(fmax(rt_la_log_sf(rl, *r, noise),
                  rt_la_log_sf(gl, *g, noise)),
             rt_la_log_sf(bl, *b, noise)),
        rt_lr_pow_F(f, 0.3f) * 0.6f + 0.4f, 1.0f);
    *r = ll + s * rl;
    *g = ll + s * gl;
    *b = ll + s * bl;
}

__kernel void rt_local_adjustments_log(
    __global float* rgb, __global const float* orig,
    __global const float* rec, __global const double* ws_double,
    const int bfw, const int bfh, const int rgb_pitch,
    const int rgb_plane, const int orig_pitch, const int orig_plane)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }

    const int offset = y * rgb_pitch + x;
    const int original = y * orig_pitch + x;
    const float noise = rec[RT_LA_LOG_NOISE];

    float r = rgb[offset];
    float g = rgb[rgb_plane + offset];
    float b = rgb[2 * rgb_plane + offset];
    const float m = rt_la_log_norm2(r, g, b, ws_double);

    if (m > noise) {
        const float mm = rt_la_log_apply(m, rec);
        float f = mm / m;
        f = fmin(f, 1000000.0f);

        r *= f;
        b *= f;
        g *= f;

        if (rec[RT_LA_LOG_SATCONTROL] != 0.0f && f < 1.0f) {
            rt_la_log_apply_sat(&r, &g, &b, f, rec, ws_double);
        }

        r = fmax(0.0f, fmin(r, 65535.0f));
        g = fmax(0.0f, fmin(g, 65535.0f));
        b = fmax(0.0f, fmin(b, 65535.0f));
    }

    /* intp(repart, tmpImageorig, tmpImage): the tool's recovery blend. */
    const float repart = rec[RT_LA_LOG_REPART];
    rgb[offset] = rt_lt_intp(repart, orig[original], r);
    rgb[rgb_plane + offset] =
        rt_lt_intp(repart, orig[orig_plane + original], g);
    rgb[2 * rgb_plane + offset] =
        rt_lt_intp(repart, orig[2 * orig_plane + original], b);
}

/*
 * Local dehaze. ImProcFunctions::dehazeloc() on the ROI:
 *
 *   Lab -> RGB, normalize, three guided filters of the RGB planes,
 *   ambient-light reduction (host, see dehaze_ambient.h), dark channel,
 *   guided filter of the dark channel, per-pixel recovery, restore,
 *   RGB -> Lab.
 *
 * The guided-filter kernels are the lab.tools fragments
 * (rt_lab_tools_subsample/box_h/box_v/combine/upsample) with the block's own
 * automatic subsampling and box radius, computed on the host. The two extra
 * kernels here only bridge the multi-plane ROI buffers to the single-plane
 * fragments.
 */

/* lab2rgb() over the ROI. `rt_la_lab2xyz` reproduces lab2rgb()'s SIMD/scalar
 * split for the Y cubic; the inverse working matrix is the block's float
 * one. */
__kernel void rt_local_adjustments_dehaze_lab2rgb(
    __global const float* lab, __global float* rgb, const int width,
    const int height, const int lab_pitch, const int lab_plane,
    const int rgb_pitch, const int rgb_plane, __global const float* iws)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int in = y * lab_pitch + x;
    const int out = y * rgb_pitch + x;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    rt_la_lab2xyz(lab[in], lab[lab_plane + in], lab[2 * lab_plane + in], x,
                  width, &X, &Y, &Z);
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    rt_lt_xyz2rgb(X, Y, Z, iws, &R, &G, &B);
    rgb[out] = R;
    rgb[rgb_plane + out] = G;
    rgb[2 * rgb_plane + out] = B;
}

/* normalize()'s maximum over the three channels, one work item per row. The
 * host reduces these maxima; a maximum is order independent. */
__kernel void rt_local_adjustments_dehaze_rowmax(
    __global const float* rgb, const int width, const int height,
    const int rgb_pitch, const int rgb_plane, __global float* rowmax)
{
    const int y = (int)get_global_id(0);
    if (y >= height) {
        return;
    }
    const int row = y * rgb_pitch;
    float value = -INFINITY;
    for (int x = 0; x < width; ++x) {
        value = fmax(value, fmax(rgb[row + x],
                                 fmax(rgb[rgb_plane + row + x],
                                      rgb[2 * rgb_plane + row + x])));
    }
    rowmax[y] = value;
}

/* normalize()'s division and restore()'s multiplication. */
__kernel void rt_local_adjustments_dehaze_scale(
    __global float* rgb, const int width, const int height, const int rgb_pitch,
    const int rgb_plane, const float factor, const int multiply)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int out = y * rgb_pitch + x;
    for (int plane = 0; plane < 3; ++plane) {
        const int offset = plane * rgb_plane + out;
        rgb[offset] = multiply != 0 ? rgb[offset] * factor
                                    : rgb[offset] / factor;
    }
}

/* guidedFilter()'s f_subsample() for a guide plane and a source plane.
 * `rt_lt_bilinear` is rescaleBilinear()'s getBilinearValue(); with a scale of
 * one it is the host's exact copy branch. */
__kernel void rt_local_adjustments_dehaze_subsample(
    __global const float* guide, const int guide_pitch,
    const int guide_plane, __global const float* src, const int src_pitch,
    const int src_plane, __global float* i1, __global float* p1,
    const int sub_pitch, const int sw, const int sh, const int width,
    const int height, const float col_scale, const float row_scale)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= sw || y >= sh) {
        return;
    }
    const float fx = x * col_scale;
    const float fy = y * row_scale;
    const int out = y * sub_pitch + x;
    i1[out] = rt_lt_bilinear(guide + guide_plane, width, height,
                             guide_pitch, fx, fy);
    p1[out] = rt_lt_bilinear(src + src_plane, width, height, src_pitch, fx,
                             fy);
}

/* guidedFilter()'s readout: q = bilinear(meana) * guide + bilinear(meanb). */
__kernel void rt_local_adjustments_dehaze_upsample(
    __global const float* meana, __global const float* meanb,
    __global const float* guide, const int guide_plane, __global float* dst,
    const int dst_plane, const int sw, const int sh, const int width,
    const int height, const int sub_pitch, const int guide_pitch,
    const int dst_pitch, const float col_scale, const float row_scale)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const float fx = x * col_scale;
    const float fy = y * row_scale;
    const float upper = rt_lt_bilinear(meana, sw, sh, sub_pitch, fx, fy);
    const float lower = rt_lt_bilinear(meanb, sw, sh, sub_pitch, fx, fy);
    dst[dst_plane + y * dst_pitch + x] =
        upper * guide[guide_plane + y * guide_pitch + x] + lower;
}

/* rescaleNearest() for one plane. The block's integer index arithmetic is
 * reproduced exactly; the caller refuses an aspect that would overflow it. */
__kernel void rt_local_adjustments_dehaze_rescale(
    __global const float* src, const int src_pitch, const int src_plane,
    __global float* dst, const int dst_pitch, const int dst_plane,
    const int sw, const int sh, const int dw, const int dh)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= dw || y >= dh) {
        return;
    }
    const int sx = x * sw / dw;
    const int sy = y * sh / dh;
    dst[dst_plane + y * dst_pitch + x] =
        src[src_plane + sy * src_pitch + sx];
}

/* get_dark_channel(): one work item per patch, the block minimum over the
 * three guided-filtered channels, then the block's normalized transmission. */
__kernel void rt_local_adjustments_dehaze_dark(
    __global const float* rgb, const int rgb_pitch, const int rgb_plane,
    __global float* dark, const int dark_pitch, const int width,
    const int height, const int patchsize, const float ambient0,
    const float ambient1, const float ambient2, const float strength)
{
    const int bx = (int)get_global_id(0);
    const int by = (int)get_global_id(1);
    const int x = bx * patchsize;
    const int y = by * patchsize;
    if (x >= width || y >= height) {
        return;
    }
    const int pW = min(x + patchsize, width);
    const int pH = min(y + patchsize, height);
    float minR = INFINITY;
    float minG = INFINITY;
    float minB = INFINITY;
    for (int yy = y; yy < pH; ++yy) {
        for (int xx = x; xx < pW; ++xx) {
            const int offset = yy * rgb_pitch + xx;
            minR = fmin(minR, rgb[offset]);
            minG = fmin(minG, rgb[rgb_plane + offset]);
            minB = fmin(minB, rgb[2 * rgb_plane + offset]);
        }
    }
    float value =
        fmin(fmin(minR / ambient0, minG / ambient1), minB / ambient2);
    value = 1.0f - strength * rt_lt_lim01(value);
    for (int yy = y; yy < pH; ++yy) {
        for (int xx = x; xx < pW; ++xx) {
            dark[yy * dark_pitch + xx] = value;
        }
    }
}

/* dehazeloc()'s per-pixel recovery on the normalized channels. The ambient
 * luminance is the block's double-precision Color::rgbLuminance() and is
 * passed in; `t0`, `satBlend` and `addHaze` are host-built from the block's
 * expressions so the device does not re-evaluate exp() or the float casts. */
__kernel void rt_local_adjustments_dehaze_recover(
    __global float* rgb, const int rgb_pitch, const int rgb_plane,
    __global const float* dark, const int dark_pitch, const int width,
    const int height, __global const float* ws, const float ambient0,
    const float ambient1, const float ambient2, const float ambientY,
    const float t0, const float satBlend, const int addHaze)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int offset = y * rgb_pitch + x;
    const float rIn = rgb[offset];
    const float gIn = rgb[rgb_plane + offset];
    const float bIn = rgb[2 * rgb_plane + offset];

    const float tl = 1.0f + RT_LA_DEHAZE_TEPS -
                     fmin(fmin(rIn / ambient0, gIn / ambient1),
                          bIn / ambient2);
    const float mt = fmax(fmax(dark[y * dark_pitch + x], t0), tl);

    /* Color::rgbLuminance(r, g, b, TMatrix) accumulates in double. */
    const double Yd = (double)rIn * ws[3] + (double)gIn * ws[4] +
                      (double)bIn * ws[5];
    const float Y = (float)Yd;
    float f = 1.0f;
    if (Y > RT_LA_DEHAZE_LUMINANCE_FLOOR) {
        float YY = (Y - ambientY) / mt + ambientY;
        if (addHaze != 0) {
            YY = Y + Y - YY;
        }
        f = YY / Y;
    }
    const float r1 = rIn * f;
    const float g1 = gIn * f;
    const float b1 = bIn * f;

    float r2 = ((rIn - ambient0) / mt + ambient0);
    float g2 = ((gIn - ambient1) / mt + ambient1);
    float b2 = ((bIn - ambient2) / mt + ambient2);
    if (addHaze != 0) {
        r2 = rIn + rIn - r2;
        g2 = gIn + gIn - g2;
        b2 = bIn + bIn - b2;
    }
    /* intp(a, b, c) = a * (b - c) + c. */
    rgb[offset] = satBlend * (r2 - r1) + r1;
    rgb[rgb_plane + offset] = satBlend * (g2 - g1) + g1;
    rgb[2 * rgb_plane + offset] = satBlend * (b2 - b1) + b1;
}

/* rgb2lab() over the ROI, with the block's Lab caches. */
__kernel void rt_local_adjustments_dehaze_rgb2lab(
    __global const float* rgb, const int rgb_pitch, const int rgb_plane,
    __global float* lab, const int lab_pitch, const int lab_plane,
    const int width, const int height, __global const float* ws,
    __global const float* cachef, __global const float* cachefy)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int in = y * rgb_pitch + x;
    const int out = y * lab_pitch + x;
    const float R = rgb[in];
    const float G = rgb[rgb_plane + in];
    const float B = rgb[2 * rgb_plane + in];
    const float X = ws[0] * R + ws[1] * G + ws[2] * B;
    const float Y = ws[3] * R + ws[4] * G + ws[5] * B;
    const float Z = ws[6] * R + ws[7] * G + ws[8] * B;
    float L = 0.0f;
    float A = 0.0f;
    float BB = 0.0f;
    rt_la_xyz2lab(X, Y, Z, cachef, cachefy, &L, &A, &BB);
    lab[out] = L;
    lab[lab_plane + out] = A;
    lab[2 * lab_plane + out] = BB;
}

/*
 * Lab_Local()'s two full-image negative-value round trips: lab2rgb(), the
 * per-channel max with epsi (and clipR for the Laplacian arm), rgb2lab().
 * mode 0 is the tail's no-clamp arm and is not used by a supported single
 * spot; mode 2 is the Laplacian arm.
 */
__kernel void rt_local_adjustments_clamp(__global float* lab,
                                         __global const float* ws,
                                         __global const float* iws,
                                         __global const float* cachef,
                                         __global const float* cachefy,
                                         const int width, const int height,
                                         const int lab_pitch,
                                         const int lab_plane,
                                         const int mode, const float epsi)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;
    const float l = lab[offset];
    const float a = lab[lab_plane + offset];
    const float b = lab[2 * lab_plane + offset];

    float xyz[3] = {0.0f, 0.0f, 0.0f};
    rt_la_lab2xyz(l, a, b, x, width, &xyz[0], &xyz[1], &xyz[2]);

    float rgb[3] = {0.0f, 0.0f, 0.0f};
    rt_lt_xyz2rgb(xyz[0], xyz[1], xyz[2], iws, &rgb[0], &rgb[1], &rgb[2]);
    for (int i = 0; i < 3; ++i) {
        rgb[i] = fmax(rgb[i], epsi);
        if (mode == 2) {
            rgb[i] = fmin(fmax(rgb[i], 0.0f), 65535.0f);
        }
    }

    const float out_x = ws[0] * rgb[0] + ws[1] * rgb[1] + ws[2] * rgb[2];
    const float out_y = ws[3] * rgb[0] + ws[4] * rgb[1] + ws[5] * rgb[2];
    const float out_z = ws[6] * rgb[0] + ws[7] * rgb[1] + ws[8] * rgb[2];

    float out_l = 0.0f;
    float out_a = 0.0f;
    float out_b = 0.0f;
    rt_la_xyz2lab(out_x, out_y, out_z, cachef, cachefy, &out_l, &out_a,
                  &out_b);
    lab[offset] = out_l;
    lab[lab_plane + offset] = out_a;
    lab[2 * lab_plane + offset] = out_b;
}
)CLC";

/* The one-spot colour program: the shared helpers and kernels plus the
 * local-adjustments fragments. The leading define records whether the host
 * takes lab2rgb()'s and avoidcolshi()'s SIMD branch. */
inline std::string local_adjustments_program()
{
#if defined(__SSE2__) || defined(RT_SIMDE)
    static const std::string simd = "#define RT_LA_HOST_SIMD 1\n";
#else
    static const std::string simd = "#define RT_LA_HOST_SIMD 0\n";
#endif
    return simd + RT_LAB_TOOLS_CL + RT_MUNSELL_LCH_CL +
           RT_LOCAL_CONTRAST_CL + RT_VIBRANCE_CL + RT_LAB_REGIONS_CL +
           RT_LAB_WAVELET_CL + RT_LOCAL_ADJUSTMENTS_CL;
}
