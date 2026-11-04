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
#include "stage/algorithms/sharpen_cl.h"
#include "stage/algorithms/vibrance_cl.h"

#include <string>

/*
 * The one-spot colour path of ImProcFunctions::Lab_Local().
 *
 * This program is concatenated after the lab.tools helpers (the Lab round
 * trip, the caches and the LUT access), the local-contrast IIR line kernel,
 * the Sleef xatan2f/xsincosf port and the lab.regions xlogf/xexpf port. It
 * adds the single-spot colour apply, the ROI copy, the per-pixel soft-light
 * curve, the local dehaze path, the local CBDL pyramid/blends, the local
 * sharpening padded-plane copy and the full-image clamp.
 *
 * The record slots are the host's RT_LOCAL_ADJUSTMENTS_SLOT_* values; the
 * device source repeats the small subset it reads so the program is
 * self-contained. Keep the two lists in step.
 */
inline constexpr const char* RT_LOCAL_ADJUSTMENTS_CL = R"CLC(
#pragma OPENCL FP_CONTRACT OFF
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
#define RT_LA_EXPSHARP 133
#define RT_LA_SENSSHA 134
#define RT_LA_SHAR_AMOUNT 135
#define RT_LA_SHAR_RADIUS 136
#define RT_LA_SHAR_ITER 137
#define RT_LA_SHAR_CONTRAST 138
#define RT_LA_SHAR_BLUR 139
#define RT_LA_EXP_TONEMAP 148
#define RT_LA_TM_SENS 149
#define RT_LA_TM_REPART 150
#define RT_LA_TM_GAMMA 151
#define RT_LA_TM_COMPRESSION 152
#define RT_LA_TM_DETAILBOOST 153
#define RT_LA_TM_TEMP 154
#define RT_LA_TM_SAT 155
#define RT_LA_TM_EDGEST 156
#define RT_LA_TM_SCALE 157
#define RT_LA_TM_ITERATES 158
#define RT_LA_TM_REWEIGHT 159
#define RT_LA_TM_EQUIL 160
#define RT_LA_EXP_RETINEX 161
#define RT_LA_RETI_STRENGTH 162
#define RT_LA_RETI_SCALE 163
#define RT_LA_RETI_VART 164
#define RT_LA_RETI_DARK 165
#define RT_LA_RETI_LIGHT 166
#define RT_LA_RETI_VALUE 167
#define RT_LA_RETI_LIMD 168
#define RT_LA_RETI_ILIMD 169
#define RT_LA_RETI_CLIPTM 170
#define RT_LA_RETI_OFFS 171
#define RT_LA_RETI_LOGLIN 172
#define RT_LA_RETI_POND 173
#define RT_LA_RETI_SOFTRADIUS 174
#define RT_LA_RETI_STRCLI 175
#define RT_LA_RETI_SENSIBEFORE 176
#define RT_LA_RETI_GAIN_OFFSET 177
#define RT_LA_RETI_GAIN_ACTIVE 178
#define RT_LA_RETI_TRANS_OFFSET 179
#define RT_LA_RETI_TRANS_ACTIVE 180
#define RT_LA_RETI_BLUR_STAGES 181
#define RT_LA_RETI_STR 182
#define RT_LA_RETI_BLUR_BASE 200
#define RT_LA_RETI_BLUR_STRIDE 7
#define RT_LA_RETI_MAX_BOX_ITERATIONS 6
#define RT_LA_EXP_CIE 270
#define RT_LA_CIE_C 271
#define RT_LA_CIE_NC 272
#define RT_LA_CIE_AW 273
#define RT_LA_CIE_FL 274
#define RT_LA_CIE_WH 275
#define RT_LA_CIE_NBB 276
#define RT_LA_CIE_NCB 277
#define RT_LA_CIE_CZ 278
#define RT_LA_CIE_D 279
#define RT_LA_CIE_POW1 280
#define RT_LA_CIE_POW1N 281
#define RT_LA_CIE_COEFQ 282
#define RT_LA_CIE_QPRO 283
#define RT_LA_CIE_PFL 284
#define RT_LA_CIE_PLUM 285
#define RT_LA_CIE_XW1 286
#define RT_LA_CIE_YW1 287
#define RT_LA_CIE_ZW1 288
#define RT_LA_CIE_XW2 289
#define RT_LA_CIE_YW2 290
#define RT_LA_CIE_ZW2 291
#define RT_LA_CIE_C2 292
#define RT_LA_CIE_NC2 293
#define RT_LA_CIE_AWJ 294
#define RT_LA_CIE_FLJ 295
#define RT_LA_CIE_NBBJ 296
#define RT_LA_CIE_NCBJ 297
#define RT_LA_CIE_CZJ 298
#define RT_LA_CIE_DJ 299
#define RT_LA_CIE_RECCMCZ 300
#define RT_LA_CIE_COE 301
#define RT_LA_CIE_MCHR 302
#define RT_LA_CIE_SCHR 303
#define RT_LA_CIE_CCHR 304
#define RT_LA_CIE_HUE 305
#define RT_LA_CIE_RSTPROTECT 306
#define RT_LA_CIE_SENS 307
#define RT_LA_CIE_REPART 308
#define RT_LA_CIE_CAMJ_OFFSET 309
#define RT_LA_CIE_CAMQ_OFFSET 310
#define RT_LA_CIE_HUERED 311
#define RT_LA_CIE_SCHRRED 312
#define RT_LA_CIE_HUEGREEN 313
#define RT_LA_CIE_SCHRGREEN 314
#define RT_LA_CIE_HUEBLUE 315
#define RT_LA_CIE_SCHRBLUE 316
#define RT_LA_CIE_CAM_ENTRIES 32768
#define RT_LA_CIE_EPSIL 0.0001f
#define RT_LA_EXP_CAPTURE 331
#define RT_LA_CAP_SIGMA 332
#define RT_LA_CAP_ITER 333
#define RT_LA_CAP_CONTRAST 334
#define RT_LA_CAP_REPART 335
#define RT_LA_CAP_GAMMA 336
#define RT_LA_CAP_KERNEL_OFFSET 337
#define RT_LA_CAP_KERNEL_N 338
#define RT_LA_CAP_BORDER 339
#define RT_LA_CAP_CHECK_STOP 340
#define RT_LA_EXP_EXPOSURE 341
#define RT_LA_EXPCOMP 342
#define RT_LA_HLCOMP 343
#define RT_LA_HLCOMPTHR 344
#define RT_LA_LAPLACEXP 345
#define RT_LA_BALANEXP 346
#define RT_LA_EXP_LINEAR 347
#define RT_LA_EXP_GAM 348
#define RT_LA_EXP_BLUREXP 349
#define RT_LA_EXP_SENS 350
#define RT_LA_EXP_HL_OFFSET 351
#define RT_LA_EXP_SH_OFFSET 352
#define RT_LA_EXP_TONE_OFFSET 353
#define RT_LA_EXP_FWDW_OFFSET 354
#define RT_LA_EXP_FWDH_OFFSET 355
#define RT_LA_EXP_INVW_OFFSET 356
#define RT_LA_EXP_INVH_OFFSET 357
#define RT_LA_EXP_POISW_OFFSET 358
#define RT_LA_EXP_POISH_OFFSET 359
#define RT_LA_EXP_BFW 360
#define RT_LA_EXP_BFH 361
#define RT_LA_CAP_TILE 32
#define RT_LA_CAP_MAX_FULL 48
#define RT_LA_CAP_MIN_BLEND 0.01f
#define RT_LA_CAP_WORKGROUP 64
#define RT_LA_CIE_HUE_ATTENUATION 0.555f

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
 * The local sharpening input plane. Lab_Local()'s RL arm copies the image into
 * a zero-initialised `(lx + lxL + 3) x (ly + lyT + 3)` extent whose origin is
 * the spot's unclipped `(xc - lxL, yc - lyT)`, not the clipped image ROI. The
 * deconvolution therefore sees zero padding at the image border. This kernel
 * reproduces that copy: every padded sample outside the image or outside the
 * spot extent is zero.
 */
__kernel void rt_local_adjustments_sharpen_fill(
    __global const float* lab, __global float* dst,
    const int lab_pitch, const int dst_pitch,
    const int bfw, const int bfh, const int begx, const int begy,
    const int xend, const int yend, const int width, const int height)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }
    const int imgx = x + begx;
    const int imgy = y + begy;
    float value = 0.0f;
    if (imgx >= 0 && imgx < width && imgx < xend &&
        imgy >= 0 && imgy < height && imgy < yend) {
        value = lab[imgy * lab_pitch + imgx];
    }
    dst[y * dst_pitch + x] = value;
}

/* dcdamping()'s per-pixel factor, in place on the blurred estimate. `value`
 * holds the Gaussian blur of the estimate, `luminance` the original padded L
 * plane. The host guards non-positive samples by writing zero; the scalar and
 * SSE arms agree on the factor `(O - I) / I * U + 1` with U the fourth-order
 * damping polynomial. */
__kernel void rt_local_adjustments_sharpen_damp(
    __global float* value, __global const float* luminance,
    const int width, const int height, const int pitch,
    const float damping_fac)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * pitch + x;
    const float image = value[index];
    const float original = luminance[index];
    if (image <= 0.0f || original <= 0.0f) {
        value[index] = 0.0f;
        return;
    }
    /* Match dcdamping()'s SIMD/scalar split. The SSE arm handles the columns
     * below `width - width % 4` and the scalar tail the rest; they group the
     * U polynomial and the log sum differently. */
    const float w = original - image;
    const float log_ratio = rt_lr_xlogf(image / original);
    float u;
    if (x < width - (width % 4)) {
        u = (original * log_ratio + w) * damping_fac;
        u = u < 1.0f ? u : 1.0f;
        const float u2 = u * u;
        const float u4 = u2 * u2;
        u = u4 * (5.0f - u * 4.0f);
    } else {
        u = (original * log_ratio - image + original) * damping_fac;
        u = u < 1.0f ? u : 1.0f;
        u = u * u * u * u * (5.0f - u * 4.0f);
    }
    value[index] = (w / image) * u + 1.0f;
}

/* Copy one plane of the full Lab image into a one-plane blur buffer. */
__kernel void rt_local_adjustments_plane_copy(
    __global const float* src, __global float* dst,
    const int src_plane, const int src_pitch, const int dst_plane,
    const int dst_pitch, const int width, const int height)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    dst[dst_plane + y * dst_pitch + x] =
        src[src_plane + y * src_pitch + x];
}

/*
 * transit_shapedetect2()'s per-pixel tail, shared by the fused colour apply,
 * the standalone blend and the blur arm. `ml`/`ma`/`mb` are the blurred
 * original the deltaE scope compares against, `orig` is the image before the
 * tool, `fin` is the tool's output and `varsens` the tool's own sensitivity
 * (`lp.sens`, `lp.sensv`, `lp.senslc` or `lp.sensbn`). `scope_base` is the
 * mindE constant: two for transit_shapedetect2() and four for
 * BlurNoise_Local().
 */
inline void rt_la_scope_blend(const float origL, const float origA,
                              const float origB, const float finL,
                              const float finA, const float finB,
                              const float ml, const float ma, const float mb,
                              const float localFactor, const float varsens,
                              __global const float* rec, const int raw_scope,
                              const float scope_base, float* outL,
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
    const float mindE = scope_base + RT_LA_MINSCOPE * varsens * thr;
    const float maxdE = 5.0f + RT_LA_MAXSCOPE * varsens * (1.0f + 0.1f * thr);
    const float mindElim = 2.0f + RT_LA_MINSCOPE * (float)RT_LA_LIMSCOPE * thr;
    const float maxdElim =
        5.0f + RT_LA_MAXSCOPE * (float)RT_LA_LIMSCOPE * (1.0f + 0.1f * thr);

    float reducdE = rt_la_calcreducdE(
        dE, maxdE, mindE, maxdElim, mindElim, rec[RT_LA_ITERAT],
        RT_LA_LIMSCOPE, (int)varsens);
    if (varsens == 100.0f && raw_scope == 0) {
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
                      0, 2.0f, &blendedL, &blendedA, &blendedB);
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
                                         const float varsens,
                                         const int clip_luminance,
                                         const int blur_is_full,
                                         const int raw_scope,
                                         const int fin_is_padded,
                                         const int fin_l_only,
                                         const int begx, const int begy,
                                         const float scope_base)
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
    const int fo = fin_is_padded != 0
                       ? (y - begy) * fin_pitch + (x - begx)
                       : ry * fin_pitch + rx;
    const int bo = blur_is_full != 0 ? y * blur_pitch + x
                                     : ry * blur_pitch + rx;

    const float finL = fin[fo];
    const float finA =
        fin_l_only != 0 ? lab[lab_plane + offset] : fin[fin_plane + fo];
    const float finB =
        fin_l_only != 0 ? lab[2 * lab_plane + offset]
                        : fin[2 * fin_plane + fo];
    float blendedL = 0.0f;
    float blendedA = 0.0f;
    float blendedB = 0.0f;
    rt_la_scope_blend(lab[offset], lab[lab_plane + offset],
                      lab[2 * lab_plane + offset], finL, finA, finB,
                      blurL[bo], blurA[bo], blurB[bo], localFactor, varsens,
                      rec, raw_scope, scope_base, &blendedL, &blendedA,
                      &blendedB);
    lab[offset] = clip_luminance != 0
                      ? fmin(fmax(blendedL, 0.0f), 65535.0f)
                      : blendedL;
    lab[lab_plane + offset] = blendedA;
    lab[2 * lab_plane + offset] = blendedB;
}

/*
 * Median_Denoise()'s interior, one plane and one pass. The host runs a
 * `(2 * border + 1) ^ 2` window over `[border, height - border) x
 * [border, width - border)`, copies the border rows and columns from the
 * input, and repeats with the buffers swapped; the border keeps the original
 * values on every pass because it is copied from the previous buffer, whose
 * border is original. The median is the value at index count / 2 of the
 * sorted window, which an insertion sort reproduces bit-exactly; only the
 * VALUE is read, so no sorting network is needed. `src` and `dst` are the
 * same ROI buffer in the host, so the launcher ping-pongs them.
 */
__kernel void rt_local_adjustments_median(
    __global const float* src, const int src_pitch, const int src_plane,
    __global float* dst, const int dst_pitch, const int dst_plane,
    const int width, const int height, const int border)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int so = src_plane + y * src_pitch + x;
    const int doff = dst_plane + y * dst_pitch + x;
    /* Median::TYPE_9X9 is the widest admitted window: 9 x 9. */
    if (border < 1 || border > 4 || x < border || y < border ||
        x >= width - border || y >= height - border) {
        dst[doff] = src[so];
        return;
    }
    float window[81];
    int count = 0;
    for (int row = -border; row <= border; ++row) {
        const int line = src_plane + (y + row) * src_pitch + x;
        for (int col = -border; col <= border; ++col) {
            window[count++] = src[line + col];
        }
    }
    for (int index = 1; index < count; ++index) {
        const float value = window[index];
        int slot = index - 1;
        while (slot >= 0 && window[slot] > value) {
            window[slot + 1] = window[slot];
            --slot;
        }
        window[slot + 1] = value;
    }
    dst[doff] = window[count / 2];
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

/*
 * Tone Mapping: ImProcFunctions::EPDToneMaplocal() followed by
 * transit_shapedetect2()'s `equiltm` normalization and its senstype-8
 * recovery blend.
 *
 * EdgePreservingDecomposition is a fixed-iteration conjugate gradient on a
 * five-diagonal finite-element Laplacian, preconditioned by an incomplete
 * Cholesky factorisation with one level of fill. Every one of its steps is
 * order-sensitive: the dot products accumulate in double with the even and
 * odd samples in separate accumulators, the factorisation is a column
 * recurrence, and the matrix-vector product takes three different summation
 * orders by region. The kernel therefore runs the whole decomposition in one
 * work item, reproducing the host's arithmetic and its order, and leaves only
 * the exact ROI minimum and maximum to a reduction.
 *
 * Scratch plane layout, one plane per array of bfw * bfh floats:
 *   0 source (log domain, then the compressed result)
 *   1 blur (the CG iterate, warm-started between reweighting passes)
 *   2 r, 3 s, 4 direction, 5 matrix product
 *   6 edge-stopping weights
 *   7..11 the five system diagonals (offsets 0, 1, w - 1, w, w + 1)
 *   12..17 the six incomplete-Cholesky diagonals
 * The ROI is at least 128 px on both axes, so the factorisation always has
 * six diagonals at offsets {0, 1, w - 3, w - 2, w, w + 1} and the file's fill
 * map has exactly the four entries worked out below.
 */
#define RT_LA_TM_EPS 0.0001f
#define RT_LA_TM_SQREPS 0.0004f
#define RT_LA_TM_PLANE_SRC 0
#define RT_LA_TM_PLANE_BLUR 1
#define RT_LA_TM_PLANE_R 2
#define RT_LA_TM_PLANE_S 3
#define RT_LA_TM_PLANE_DIR 4
#define RT_LA_TM_PLANE_AX 5
#define RT_LA_TM_PLANE_WEIGHTS 6
#define RT_LA_TM_PLANE_DIAG 7
#define RT_LA_TM_PLANE_IC 12

/* rt_algo.cc's accumulateProduct() single-threaded: two double accumulators
 * over the even and the odd samples, the odd tail folded into the first. */
inline double rt_la_tm_product(__global const float* first,
                               __global const float* second, const int n)
{
    if (n == 0) {
        return 0.0;
    }
    double acc1 = 0.0;
    double acc2 = 0.0;
    for (int i = 0; i < n - 1; i += 2) {
        acc1 += (double)first[i] * (double)second[i];
        acc2 += (double)first[i + 1] * (double)second[i + 1];
    }
    if ((n & 1) != 0) {
        acc1 += (double)first[n - 1] * (double)second[n - 1];
    }
    return acc1 + acc2;
}

/* sleef.h EPD's SSE loops evaluate xexpf() with the vector form of
 * sleefsseavx.h, whose last polynomial step groups as ((s*s)*u + s) + 1
 * instead of the scalar form's s*((s*u) + 1) + 1. EdgePreservingDecomposition
 * takes the vector form for every column below 4 * (w / 4) and every sample
 * below 4 * (n / 4), and the difference reaches the tone-map result through
 * the conjugate gradient, so the kernel reproduces both forms. */
inline float rt_la_tm_xexpf_vector(const float d)
{
    const int q = rt_lr_xrintf(d * RT_LR_RLN2);
    float s = (float)q * -RT_LR_L2Uf + d;
    s = (float)q * -RT_LR_L2Lf + s;
    float u = 0.00136324646882712841033936f;
    u = u * s + 0.00836596917361021041870117f;
    u = u * s + 0.0416710823774337768554688f;
    u = u * s + 0.166665524244308471679688f;
    u = u * s + 0.499999850988388061523438f;
    u = ((s * s) * u + s) + 1.0f;
    const float value = rt_lr_ldexpkf(u, q);
    return -104.0f > d ? 0.0f : value;
}

inline float rt_la_tm_pow_vector(const float a, const float b)
{
    return rt_la_tm_xexpf_vector(b * rt_lr_xlogf(a));
}

/*
 * MultiDiagonalSymmetricMatrix::VectorProduct(). `diag` is the base of the
 * five system diagonals and `plane` their plane stride. The host writes the
 * middle block with the diagonals taken in descending order and the borders
 * in ascending order, so the three regions keep their own summation order.
 */
inline void rt_la_tm_vector_product(__global float* product,
                                    __global const float* x,
                                    __global const float* diag,
                                    const int plane, const int n,
                                    const int w)
{
    const int srm = w + 1;
    const int lm = n - (w + 1);
    const int sr[5] = {0, 1, w - 1, w, w + 1};
    for (int j = 0; j < n; ++j) {
        float p = diag[j] * x[j];
        if (j >= srm && j < lm) {
            for (int i = 4; i > 0; --i) {
                const int s = sr[i];
                p += (diag[i * plane + j - s] * x[j - s]) +
                     (diag[i * plane + j] * x[j + s]);
            }
        } else if (j < srm) {
            for (int i = 1; i < 5; ++i) {
                const int s = sr[i];
                if (j < s) {
                    p += diag[i * plane + j] * x[j + s];
                } else {
                    p += (diag[i * plane + j - s] * x[j - s]) +
                         (diag[i * plane + j] * x[j + s]);
                }
            }
        } else {
            for (int i = 1; i < 5; ++i) {
                const int s = sr[i];
                if (j < n - s) {
                    p += (diag[i * plane + j - s] * x[j - s]) +
                         (diag[i * plane + j] * x[j + s]);
                } else {
                    p += diag[i * plane + j - s] * x[j - s];
                }
            }
        }
        product[j] = p;
    }
}

/*
 * MultiDiagonalSymmetricMatrix::CholeskyBackSolve(): solve L D Lt x = b for
 * the six-diagonal factor. The forward substitution walks j upwards and the
 * back substitution downwards, both in the host's order.
 */
inline void rt_la_tm_backsolve(__global float* x, __global const float* b,
                               __global const float* ic, const int plane,
                               const int n, const int w)
{
    const int s[6] = {0, 1, w - 2, w - 1, w, w + 1};
    /* The host splits the forward substitution at s[5]: the short prefix keeps
     * the ascending `while` form, the rest takes the unrolled descending
     * order. */
    for (int j = 0; j <= s[5] && j < n; ++j) {
        float sub = b[j];
        for (int i = 1; i <= 5 && j - s[i] >= 0; ++i) {
            const int c = j - s[i];
            sub -= ic[i * plane + c] * x[c];
        }
        x[j] = sub;
    }
    for (int j = s[5] + 1; j < n; ++j) {
        float sub = b[j];
        for (int i = 5; i > 0; --i) {
            const int c = j - s[i];
            sub -= ic[i * plane + c] * x[c];
        }
        x[j] = sub;
    }
    for (int j = 0; j < n; ++j) {
        x[j] = x[j] / ic[j];
    }
    for (int j = n - 1; j >= (n - 1) - s[5]; --j) {
        float sub = x[j];
        for (int i = 1; i <= 5 && j + s[i] < n; ++i) {
            sub -= ic[i * plane + j] * x[j + s[i]];
        }
        x[j] = sub;
    }
    for (int j = (n - 2) - s[5]; j >= 0; --j) {
        float sub = x[j];
        for (int i = 5; i > 0; --i) {
            sub -= ic[i * plane + j] * x[j + s[i]];
        }
        x[j] = sub;
    }
}

/*
 * MultiDiagonalSymmetricMatrix::CreateIncompleteCholeskyFactorization(1) for
 * the five-diagonal system. The source start rows are {0, 1, w - 1, w, w + 1};
 * one level of fill produces the factor's rows {0, 1, w - 2, w - 1, w, w + 1}
 * (two fill diagonals below the w - 1 row, one below each of the others).
 * The fill map pairs the factor's own indices whose offsets sum to another
 * factor offset: (1, w - 2), (1, w - 1), (1, w), (w - 2, 1), (w - 1, 1) and
 * (w, 1). The host scans that map as a single prefix from one column to the
 * next, so an entry with a larger offset than the current column blocks every
 * later one; the loop below keeps that behaviour. Returns 1 on the host's
 * division-by-zero failure, where the block zeroes the blur.
 */
inline int rt_la_tm_factor(__global const float* diag,
                           __global float* ic, const int plane,
                           const int n, const int w)
{
    const int sw[6] = {0, 1, w - 2, w - 1, w, w + 1};
    const int map_ss[6] = {2, 3, 4, 1, 1, 1};
    const int map_sss[6] = {3, 4, 5, 3, 4, 5};
    const int map_k[6] = {w - 2, w - 1, w, 1, 1, 1};
    const int map_max[6] = {-1, 2, 3, 4, 5, 5};
    const int find_map[6] = {-1, 1, -1, 2, 3, 4};
    __global float* d0 = ic;
    for (int j = 0; j < n; ++j) {
        float dv = diag[j];
        for (int s = 1; s <= 5; ++s) {
            const int k = sw[s];
            if (k <= j) {
                const float l = ic[s * plane + j - k];
                dv -= l * l * d0[j - k];
            }
        }
        d0[j] = dv;
        if (dv == 0.0f) {
            return 1;
        }
        const float id = 1.0f / dv;
        const int j_max = n - j;
        int map_index = 0;
        for (int s = 1; s <= 5; ++s) {
            if (sw[s] >= j_max) {
                break;
            }
            float temp = 0.0f;
            while (map_index <= map_max[s] && map_k[map_index] <= j) {
                const int c = j - map_k[map_index];
                temp -= (ic[map_sss[map_index] * plane + c] *
                         ic[map_ss[map_index] * plane + c]) * d0[c];
                ++map_index;
            }
            const int a = find_map[s];
            ic[s * plane + j] =
                a < 0 ? id * temp : id * (diag[a * plane + j] + temp);
        }
    }
    return 0;
}

/*
 * SparseConjugateGradient() with RMSResidual zero, so the iteration count is
 * exactly `iterates` unless the step is exactly singular. `x` is the
 * warm-start guess and the result; `b` is the right-hand side.
 */
inline void rt_la_tm_conjugate_gradient(__global float* x,
                                        __global const float* b,
                                        __global float* work,
                                        __global const float* diag,
                                        __global const float* ic,
                                        const int plane, const int n,
                                        const int w, const int iterates)
{
    __global float* r = work;
    __global float* s = work + plane;
    __global float* dir = work + 2 * plane;
    __global float* ax = work + 3 * plane;
    rt_la_tm_vector_product(ax, x, diag, plane, n, w);
    for (int i = 0; i < n; ++i) {
        r[i] = b[i] - ax[i];
    }
    rt_la_tm_backsolve(s, r, ic, plane, n, w);
    double rs = rt_la_tm_product(r, s, n);
    for (int i = 0; i < n; ++i) {
        dir[i] = s[i];
    }
    for (int it = 0; it < iterates; ++it) {
        rt_la_tm_vector_product(ax, dir, diag, plane, n, w);
        double ab = rt_la_tm_product(dir, ax, n);
        if (ab == 0.0) {
            break;
        }
        ab = rs / ab;
        const float abf = (float)ab;
        for (int i = 0; i < n; ++i) {
            x[i] += abf * dir[i];
            r[i] -= abf * ax[i];
        }
        rt_la_tm_backsolve(s, r, ic, plane, n, w);
        ab = rs;
        rs = rt_la_tm_product(r, s, n);
        ab = rs / ab;
        const float abf2 = (float)ab;
        for (int i = 0; i < n; ++i) {
            dir[i] = s[i] + abf2 * dir[i];
        }
    }
}

/*
 * EdgePreservingDecomposition::CreateBlur(). `g` is the image the edge
 * stopping reads (the source, or the previous blur for a reweighting pass),
 * `x` the iterate to warm-start from and overwrite. The weights are written
 * for the interior only; the assembly below reads no border weight.
 */
inline void rt_la_tm_create_blur(__global const float* source,
                                 __global const float* g,
                                 __global float* x,
                                 __global float* weights,
                                 __global float* diag,
                                 __global float* ic,
                                 __global float* work,
                                 const int plane, const int w, const int h,
                                 const float scale, const float edgest,
                                 const int iterates, const int warm)
{
    const int n = w * h;
    const int w1 = w - 1;
    const int h1 = h - 1;
    /* The host's SSE loop covers every column below 4 * (w1 / 4); the rest
     * takes the scalar xexpf() form. */
    const int vector_columns = 4 * (w1 / 4);
    for (int y = 0; y < h1; ++y) {
        for (int xx = 0; xx < w1; ++xx) {
            const int i = y * w + xx;
            const float gx =
                (g[i + 1] - g[i]) + (g[i + w + 1] - g[i + w]);
            const float gy =
                (g[i + w] - g[i]) + (g[i + w + 1] - g[i + 1]);
            const float base =
                0.5f * sqrt(gx * gx + gy * gy + RT_LA_TM_SQREPS);
            weights[i] = xx < vector_columns
                             ? scale * rt_la_tm_pow_vector(base, -edgest)
                             : scale * rt_lr_pow_F(base, -edgest);
        }
    }

    for (int i = 0; i < n - 1; ++i) {
        diag[plane + i] = 0.0f;
    }
    for (int i = 0; i < n - (w - 1); ++i) {
        diag[2 * plane + i] = 0.0f;
    }
    for (int i = 0; i < n - w; ++i) {
        diag[3 * plane + i] = 0.0f;
    }
    for (int i = 0; i < n - (w + 1); ++i) {
        diag[4 * plane + i] = 0.0f;
    }
    for (int y = 0; y < h; ++y) {
        for (int xx = 0; xx < w; ++xx) {
            const int i = y * w + xx;
            float acc = 0.25f;
            if (xx > 0 && y > 0) {
                const float ac = weights[i - w - 1] / 6.0f;
                diag[4 * plane + i - w - 1] -= 2.0f * ac;
                diag[3 * plane + i - w] -= ac;
                diag[plane + i - 1] -= ac;
                acc += ac;
            }
            if (xx < w1 && y > 0) {
                const float ac = weights[i - w] / 6.0f;
                diag[3 * plane + i - w] -= ac;
                diag[2 * plane + i - w + 1] -= 2.0f * ac;
                acc += ac;
            }
            if (xx > 0 && y < h1) {
                const float ac = weights[i - 1] / 6.0f;
                diag[plane + i - 1] -= ac;
                acc += ac;
            }
            if (xx < w1 && y < h1) {
                acc += weights[i] / 6.0f;
            }
            diag[i] = 4.0f * acc;
        }
    }

    for (int p = 0; p < 6; ++p) {
        for (int i = 0; i < n; ++i) {
            ic[p * plane + i] = 0.0f;
        }
    }
    if (rt_la_tm_factor(diag, ic, plane, n, w) != 0) {
        for (int i = 0; i < n; ++i) {
            x[i] = 0.0f;
        }
        return;
    }
    if (warm == 0) {
        for (int i = 0; i < n; ++i) {
            x[i] = source[i];
        }
    }
    rt_la_tm_conjugate_gradient(x, source, work, diag, ic, plane, n, w,
                                iterates);
}

/*
 * normalize_mean_dt()'s two moment reductions, single-threaded:
 * mean_dt() accumulates the values and their float squares into doubles in
 * ascending order.
 */
inline void rt_la_tm_mean(__global const float* data, const int n,
                          double* mean, double* deviation)
{
    double sum = 0.0;
    double squares = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += (double)data[i];
        squares += (double)(data[i] * data[i]);
    }
    sum /= n;
    squares /= n;
    squares -= sum * sum;
    *mean = sum;
    *deviation = sqrt(squares);
}

__kernel void rt_local_adjustments_tonemap(
    __global const float* lab, const int lab_pitch, const int lab_plane,
    __global float* fin, const int fin_pitch, const int fin_plane,
    __global float* scratch, const int plane,
    const int bfw, const int bfh, const int xstart, const int ystart,
    __global const float* rec)
{
    const int w = bfw;
    const int h = bfh;
    const int n = w * h;
    __global float* src = scratch + RT_LA_TM_PLANE_SRC * plane;
    __global float* blur = scratch + RT_LA_TM_PLANE_BLUR * plane;
    __global float* work = scratch + RT_LA_TM_PLANE_R * plane;
    __global float* weights = scratch + RT_LA_TM_PLANE_WEIGHTS * plane;
    __global float* diag = scratch + RT_LA_TM_PLANE_DIAG * plane;
    __global float* ic = scratch + RT_LA_TM_PLANE_IC * plane;

    /* EPDToneMaplocal()'s range reduction. min() and max() are the host's
     * std::min/std::max, which are exact in any order. */
    float min_l = lab[ystart * lab_pitch + xstart];
    float max_l = min_l;
    for (int y = 0; y < h; ++y) {
        const int row = (ystart + y) * lab_pitch + xstart;
        for (int x = 0; x < w; ++x) {
            const float v = lab[row + x];
            min_l = v < min_l ? v : min_l;
            max_l = v > max_l ? v : max_l;
        }
    }
    if (min_l > 0.0f) {
        min_l = 0.0f;
    }
    if (max_l == 0.0f) {
        max_l = 1.0f;
    }
    const float gamma = rec[RT_LA_TM_GAMMA];
    const float mult = gamma / max_l;

    /* The block's own scale, then CompressDynamicRange()'s logarithm. */
    for (int y = 0; y < h; ++y) {
        const int row = (ystart + y) * lab_pitch + xstart;
        for (int x = 0; x < w; ++x) {
            const float scaled =
                (lab[row + x] - min_l) * mult;
            const float positive = scaled > 0.0f ? scaled : 0.0f;
            src[y * w + x] = rt_lr_xlogf(positive + RT_LA_TM_EPS);
        }
    }

    const float scale = rec[RT_LA_TM_SCALE];
    const float edgest = rec[RT_LA_TM_EDGEST];
    const int iterates = (int)rec[RT_LA_TM_ITERATES];
    int reweight = (int)rec[RT_LA_TM_REWEIGHT];
    if (w < 300 && h < 300) {
        reweight = 0;
    }
    if (reweight == 0) {
        rt_la_tm_create_blur(src, src, blur, weights, diag, ic, work, plane,
                             w, h, scale, edgest, iterates, 0);
    } else {
        for (int i = 0; i < n; ++i) {
            blur[i] = src[i];
        }
        for (int pass = 0; pass <= reweight; ++pass) {
            rt_la_tm_create_blur(src, blur, blur, weights, diag, ic, work,
                                 plane, w, h, scale, edgest, iterates, 1);
        }
    }

    /* CompressDynamicRange()'s compression, detail boost and unlogging. The
     * host's SSE loop covers the samples below 4 * (n / 4) and its scalar
     * tail the rest; both xexpf() forms appear. */
    const float detail = rec[RT_LA_TM_DETAILBOOST];
    const float temp = rec[RT_LA_TM_TEMP];
    const int vector_samples = 4 * (n / 4);
    for (int i = 0; i < vector_samples; ++i) {
        const float ce = rt_la_tm_xexpf_vector(src[i] + blur[i] * temp) -
                         RT_LA_TM_EPS;
        const float ue = rt_la_tm_xexpf_vector(blur[i]) - RT_LA_TM_EPS;
        const float raw = rt_la_tm_xexpf_vector(src[i]) - RT_LA_TM_EPS;
        src[i] = ce + detail * (raw - ue);
    }
    for (int i = vector_samples; i < n; ++i) {
        const float ce = rt_lr_xexpf(src[i] + blur[i] * temp) -
                         RT_LA_TM_EPS;
        const float ue = rt_lr_xexpf(blur[i]) - RT_LA_TM_EPS;
        const float raw = rt_lr_xexpf(src[i]) - RT_LA_TM_EPS;
        src[i] = ce + detail * (raw - ue);
    }

    /* EPDToneMaplocal()'s range restore. The equiltm normalization below runs
     * on the restored lightness, not on the compressed scale. */
    const float inv_gamma = 1.0f / gamma;
    for (int i = 0; i < n; ++i) {
        work[i] = src[i] * max_l * inv_gamma + min_l;
    }

    /* transit_shapedetect2()'s `equiltm` arm: normalize_mean_dt() with
     * mod = 1 and no supplied moments, so the restored lightness becomes
     * a * data + b against the pre-tool ROI lightness. */
    if (rec[RT_LA_TM_EQUIL] != 0.0f) {
        __global float* ref = blur;
        for (int y = 0; y < h; ++y) {
            const int row = (ystart + y) * lab_pitch + xstart;
            for (int x = 0; x < w; ++x) {
                ref[y * w + x] = lab[row + x];
            }
        }
        double mean_ref = 0.0;
        double dev_ref = 0.0;
        double mean_data = 0.0;
        double dev_data = 0.0;
        rt_la_tm_mean(ref, n, &mean_ref, &dev_ref);
        rt_la_tm_mean(work, n, &mean_data, &dev_data);
        const double ratio = dev_ref / dev_data;
        const double offset = mean_ref - ratio * mean_data;
        const float modma = (float)(1.0 * ratio);
        const float modmb = (float)(1.0 * 1.0 * offset);
        const float onesmod = 1.0f - 1.0f;
        for (int i = 0; i < n; ++i) {
            work[i] = (modma * work[i] + modmb) + onesmod * ref[i];
        }
    }

    /* EPDToneMaplocal()'s colour correction and transit_shapedetect2()'s
     * senstype-8 recovery blend. */
    const float sat = rec[RT_LA_TM_SAT];
    const float repart = rec[RT_LA_TM_REPART];
    for (int y = 0; y < h; ++y) {
        const int row = (ystart + y) * lab_pitch + xstart;
        const int frow = y * fin_pitch;
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            const int o = row + x;
            const float orig_l = lab[o];
            const float orig_a = lab[lab_plane + o];
            const float orig_b = lab[2 * lab_plane + o];
            const float epd_l = work[i];
            const float epd_a = sat * orig_a;
            const float epd_b = sat * orig_b;
            fin[frow + x] = repart * (orig_l - epd_l) + epd_l;
            fin[fin_plane + frow + x] =
                repart * (orig_a - epd_a) + epd_a;
            fin[2 * fin_plane + frow + x] =
                repart * (orig_b - epd_b) + epd_b;
        }
    }
}

/*
 * Retinex. LocretigainCurve/LocretitransCurve are 501-entry LUTf tables; the
 * host's operator[] clamps below zero and above index 499 and interpolates
 * linearly.
 */
#define RT_LA_RETI_LUT_LAST 499.0f
inline float rt_la_reti_lut(__global const float* table, const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        return table[0];
    }
    if (index > RT_LA_RETI_LUT_LAST) {
        return table[500];
    }
    const float diff = index - (float)idx;
    const float low = table[idx];
    return low + (table[idx + 1] - low) * diff;
}

/* iplocallab.cc clipDE(). */
inline float rt_la_reti_clip_de(const float x)
{
    return fmin(fmax(x, 0.3f), 1.0f);
}

/*
 * MSRLocal()'s per-spot setup: the ROI copy, the deltaE reduction the block
 * applies before the multi-scale pass, orig/orig1, src = orig + 2 and the
 * zeroed luminance accumulator. The host's call==2 arm reads the retinex
 * strength gate and the ROI itself; a smaller ROI never reaches here.
 */
__kernel void rt_local_adjustments_reti_prepare(
    __global float* lab, __global float* work, __global float* reduc,
    __global float* orig1, __global float* src, __global float* lum,
    __global const float* rec, const int lab_pitch, const int lab_plane,
    const int work_pitch, const int work_plane, const int xstart,
    const int ystart, const int bfw, const int bfh)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }
    const int o = (ystart + ry) * lab_pitch + (xstart + rx);
    const int r = ry * work_pitch + rx;
    const float L = lab[o];
    const float a = lab[lab_plane + o];
    const float b = lab[2 * lab_plane + o];
    work[r] = L;
    work[work_plane + r] = a;
    work[2 * work_plane + r] = b;
    orig1[r] = L;
    src[r] = L + 2.0f;
    lum[r] = 0.0f;

    /* iplocallab.cc's pre-MSR reduction. The lightness term reads the b
     * channel in the host's call==2 arm; keep the expression verbatim. */
    const float refa = rec[RT_LA_CHROMAREF] * cos(rec[RT_LA_HUEREF]);
    const float refb = rec[RT_LA_CHROMAREF] * sin(rec[RT_LA_HUEREF]);
    const float refL = rec[RT_LA_LUMAREF];
    const float dE = sqrt((refa - a / 327.68f) * (refa - a / 327.68f) +
                          (refb - b / 327.68f) * (refb - b / 327.68f) +
                          (refL - b / 327.68f) * (refL - b / 327.68f));
    const float sens = rec[RT_LA_SENSH];
    const float thr = rec[RT_LA_THR];
    const float mindE = 4.0f + RT_LA_MINSCOPE * sens * thr;
    const float maxdE = 5.0f + RT_LA_MAXSCOPE * sens * (1.0f + 0.1f * thr);
    const float mindElim = 2.0f + RT_LA_MINSCOPE * (float)RT_LA_LIMSCOPE * thr;
    const float maxdElim =
        5.0f + RT_LA_MAXSCOPE * (float)RT_LA_LIMSCOPE * (1.0f + 0.1f * thr);
    const int scope = (int)rec[RT_LA_RETI_SENSIBEFORE];
    const float reducdE = rt_la_calcreducdE(
        dE, maxdE, mindE, maxdElim, mindElim, rec[RT_LA_ITERAT],
        RT_LA_LIMSCOPE, scope);
    reduc[r] = rt_la_reti_clip_de(reducdE);
}

/*
 * One scale's accumulation. At scale one the host first applies the darkness
 * and lightness multipliers to the current blur estimate; the accumulator
 * then adds pond times the clamped ratio, in log or linear form.
 */
__kernel void rt_local_adjustments_reti_accum(
    __global float* lum, __global const float* src, __global float* out,
    __global const float* rec, const int pitch, const int width,
    const int height, const int scale_is_one)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int r = y * pitch + x;
    float o = out[r];
    if (scale_is_one != 0) {
        float buf = (src[r] - o) * rec[RT_LA_RETI_VALUE];
        buf *= (buf > 0.0f) ? rec[RT_LA_RETI_LIGHT] : rec[RT_LA_RETI_DARK];
        o = fmin(fmax(o + buf, -100000.0f), 100000.0f);
        out[r] = o;
    }
    const float ratio =
        fmin(fmax(src[r] / o, rec[RT_LA_RETI_ILIMD]), rec[RT_LA_RETI_LIMD]);
    if (rec[RT_LA_RETI_LOGLIN] != 0.0f) {
        lum[r] += rec[RT_LA_RETI_POND] * rt_lr_xlogf(ratio);
    } else {
        lum[r] += rec[RT_LA_RETI_POND] * ratio;
    }
}

/*
 * MSRLocal()'s reductions and normalisation. One work item walks the ROI in
 * the host's row-major order three times: before the transmission curve,
 * after it, and after subtracting mini when the gain table is active. The
 * results go to a small parameter buffer for the gain kernel.
 */
__kernel void rt_local_adjustments_reti_stats(
    __global float* lum, __global const float* rec,
    __global const float* trans, __global float* params, const int pitch,
    const int width, const int height)
{
    if (get_global_id(0) != 0) {
        return;
    }
    const int count = width * height;
    double sum = 0.0;
    double squared = 0.0;
    float lmax = -999999.0f;
    float lmin = 999999.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = lum[y * pitch + x];
            sum += (double)v;
            squared += (double)v * (double)v;
            lmax = v > lmax ? v : lmax;
            lmin = v < lmin ? v : lmin;
        }
    }
    const float mean = (float)(sum / (double)count);
    float variance = squared / (double)count;
    variance -= (double)mean * (double)mean;
    const float stddv = (float)sqrt(variance);
    const float maxtr = lmax;
    const float mintr = lmin;

    if (rec[RT_LA_RETI_TRANS_ACTIVE] != 0.0f) {
        float asig = 0.166666f / stddv;
        float bsig = 0.5f - asig * mean;
        float amax = 0.333333f / (maxtr - mean - stddv);
        float bmax = 1.0f - amax * maxtr;
        float amin = 0.333333f / (mean - stddv - mintr);
        float bmin = -amin * mintr;
        asig *= 500.0f;
        bsig *= 500.0f;
        amax *= 500.0f;
        bmax *= 500.0f;
        amin *= 500.0f;
        bmin *= 500.0f;
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            const int i = y * pitch + x;
            const float v = lum[i];
            float absciss;
            if (fabs(v - mean) < stddv) {
                absciss = asig * v + bsig;
            } else if (v >= mean) {
                absciss = amax * v + bmax;
            } else {
                absciss = amin * v + bmin;
            }
            lum[i] = v * (-1.0f + 4.0f * rt_la_reti_lut(trans, absciss));
          }
        }
    }

    sum = 0.0;
    squared = 0.0;
    lmax = -999999.0f;
    lmin = 999999.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = lum[y * pitch + x];
            sum += (double)v;
            squared += (double)v * (double)v;
            lmax = v > lmax ? v : lmax;
            lmin = v < lmin ? v : lmin;
        }
    }
    const float mean2 = (float)(sum / (double)count);
    variance = squared / (double)count;
    variance -= (double)mean2 * (double)mean2;
    const float stddv2 = (float)sqrt(variance);
    const float maxtr2 = lmax;
    const float mintr2 = lmin;

    const float vart = rec[RT_LA_RETI_VART];
    float mini = mean2 - vart * stddv2;
    if (mini < mintr2) {
        mini = mintr2 + 0.1f;
    }
    float maxi = mean2 + vart * stddv2;
    if (maxi > maxtr2) {
        maxi = maxtr2 - 0.1f;
    }
    float delta = maxi - mini;
    if (delta == 0.0f) {
        delta = 1.0f;
    }
    float cdfactor = (rec[RT_LA_RETI_CLIPTM] * 32768.0f) / delta;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            lum[y * pitch + x] -= mini;
        }
    }

    float mean3 = 0.0f;
    float stddv3 = 0.0f;
    float maxtr3 = 0.0f;
    float mintr3 = 0.0f;
    int has_gain = 0;
    if (rec[RT_LA_RETI_GAIN_ACTIVE] != 0.0f) {
        sum = 0.0;
        squared = 0.0;
        lmax = -999999.0f;
        lmin = 999999.0f;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float v = lum[y * pitch + x];
                sum += (double)v;
                squared += (double)v * (double)v;
                lmax = v > lmax ? v : lmax;
                lmin = v < lmin ? v : lmin;
            }
        }
        mean3 = (float)(sum / (double)count);
        variance = squared / (double)count;
        variance -= (double)mean3 * (double)mean3;
        stddv3 = (float)sqrt(variance);
        maxtr3 = lmax;
        mintr3 = lmin;
        has_gain = mean3 != 0.0f && stddv3 != 0.0f ? 1 : 0;
    }

    float asig = 0.0f;
    float bsig = 0.0f;
    float amax = 0.0f;
    float bmax = 0.0f;
    float amin = 0.0f;
    float bmin = 0.0f;
    if (has_gain != 0) {
        asig = 0.166666f / stddv3;
        bsig = 0.5f - asig * mean3;
        amax = 0.333333f / (maxtr3 - mean3 - stddv3);
        bmax = 1.0f - amax * maxtr3;
        amin = 0.333333f / (mean3 - stddv3 - mintr3);
        bmin = -amin * mintr3;
        asig *= 500.0f;
        bsig *= 500.0f;
        amax *= 500.0f;
        bmax *= 500.0f;
        amin *= 500.0f;
        bmin *= 500.0f;
        cdfactor *= 2.0f;
    }

    params[0] = cdfactor;
    params[1] = mean3;
    params[2] = stddv3;
    params[3] = (float)has_gain;
    params[4] = asig;
    params[5] = bsig;
    params[6] = amax;
    params[7] = bmax;
    params[8] = amin;
    params[9] = bmin;
}

/* MSRLocal()'s final intp() against the original lightness. */
__kernel void rt_local_adjustments_reti_gain(
    __global float* lum, __global const float* reduc,
    __global const float* orig1, __global const float* rec,
    __global const float* gain, __global const float* params,
    const int pitch, const int width, const int height)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int i = y * pitch + x;
    const float v = lum[i];
    float gan = 0.5f;
    if (params[3] != 0.0f) {
        float absciss;
        if (fabs(v - params[1]) < params[2]) {
            absciss = params[4] * v + params[5];
        } else if (v >= params[1]) {
            absciss = params[6] * v + params[7];
        } else {
            absciss = params[8] * v + params[9];
        }
        gan = rt_la_reti_lut(gain, absciss);
    }
    float cd = gan * params[0] * v + rec[RT_LA_RETI_OFFS];
    cd = fmin(fmax(cd, 0.0f), 32768.0f);
    const float t = rec[RT_LA_RETI_STR] * reduc[i];
    lum[i] = t * (cd - orig1[i]) + orig1[i];
}

/*
 * The lightness difference MSRLocal leaves for the blend, normalised by one
 * percent of its largest magnitude. One work item reduces the ROI and writes
 * the scaled plane.
 */
__kernel void rt_local_adjustments_reti_diff(
    __global const float* lum, __global const float* orig1,
    __global float* flight, const int pitch, const int width,
    const int height)
{
    if (get_global_id(0) != 0) {
        return;
    }
    float minL = lum[0] - orig1[0];
    float maxL = minL;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int i = y * pitch + x;
            const float d = lum[i] - orig1[i];
            minL = d < minL ? d : minL;
            maxL = d > maxL ? d : maxL;
        }
    }
    float coef = 0.01f * fmax(fabs(minL), fabs(maxL));
    if (coef > 0.0f) {
        coef = 1.0f / coef;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int i = y * pitch + x;
            flight[i] = (lum[i] - orig1[i]) * coef;
        }
    }
}

/*
 * transit_shapedetect_retinex()'s senstype-4 blend. origblur is the full-image
 * 3/sk Gaussian, indexed at the image coordinate; the tool's own ROI copy is
 * `work` and the normalised lightness difference `flight`. `lab` is updated
 * in place. The block's `rL > 0.1` guard is a per-pixel switch the host takes
 * on the blurred lightness.
 */
__kernel void rt_local_adjustments_reti_blend(
    __global float* lab, __global const float* flight,
    __global const float* blurL, __global const float* blurA,
    __global const float* blurB, __global const float* rec,
    const int lab_pitch, const int lab_plane, const int work_pitch,
    const int blur_pitch, const int bfw, const int bfh, const int xstart,
    const int ystart)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }
    const int x = xstart + rx;
    const int y = ystart + ry;
    int zone = 0;
    float localFactor = 1.0f;
    const float ach = rec[RT_LA_TRANS] / 100.0f;
    rt_la_transition((float)x, (float)y, ach, rec, &zone, &localFactor);
    if (zone == 0) {
        return;
    }

    const int ro = (ystart + ry) * blur_pitch + x;
    const float ml = blurL[ro];
    const float ma = blurA[ro];
    const float mb = blurB[ro];
    const float refa = rec[RT_LA_CHROMAREF] * cos(rec[RT_LA_HUEREF]) * 327.68f;
    const float refb = rec[RT_LA_CHROMAREF] * sin(rec[RT_LA_HUEREF]) * 327.68f;
    const float refL = rec[RT_LA_LUMAREF] * 327.68f;
    const float chromaref = rec[RT_LA_CHROMAREF] * 327.68f;
    const float abdelta2 = (refa - ma) * (refa - ma) +
                           (refb - mb) * (refb - mb);
    const float chroma2 = sqrt(ma * ma + mb * mb) - chromaref;
    const float chrodelta2 = chroma2 * chroma2;
    const float huedelta2 = abdelta2 - chrodelta2;
    const float kL = rec[RT_LA_BALANCE] / (327.68f * 327.68f);
    const float kab = rt_la_balancedeltaE(rec[RT_LA_BALANCE]) /
                      (327.68f * 327.68f);
    const float kH = rec[RT_LA_BALANCEH];
    const float kch = rt_la_balancedeltaE(rec[RT_LA_BALANCEH]);
    const float dE = sqrt(kab * (kch * chrodelta2 + kH * huedelta2) +
                          kL * (refL - ml) * (refL - ml));
    const float varsens = rec[RT_LA_SENSH];
    const float thr = rec[RT_LA_THR];
    const float mindE = 2.0f + RT_LA_MINSCOPE * varsens * thr;
    const float maxdE = 5.0f + RT_LA_MAXSCOPE * varsens * (1.0f + 0.1f * thr);
    const float mindElim = 2.0f + RT_LA_MINSCOPE * (float)RT_LA_LIMSCOPE * thr;
    const float maxdElim =
        5.0f + RT_LA_MAXSCOPE * (float)RT_LA_LIMSCOPE * (1.0f + 0.1f * thr);
    float reducdE = rt_la_calcreducdE(
        dE, maxdE, mindE, maxdElim, mindElim, rec[RT_LA_ITERAT],
        RT_LA_LIMSCOPE, (int)varsens);
    reducdE /= 100.0f;

    const int index = ry * work_pitch + rx;
    float cli = flight[index] * reducdE * (1.0f + rec[RT_LA_RETI_STRCLI]);

    const float rL = ml / 327.68f;
    if (rL > 0.1f) {
        /* `bufexporig` is the ROI copy taken before MSRLocal(), and no mask
         * is admitted, so its lightness equals the pixel's own and its
         * chroma planes are untouched: the host's difa/difb are exactly
         * zero and only the lightness blend writes the image. */
        const int o = y * lab_pitch + x;
        const float originalL = lab[o];
        float diflc = originalL * (1.0f + cli) - originalL;
        diflc *= localFactor;
        lab[o] = fmin(fmax(originalL + diflc, 0.0f), 65535.0f);
    }
}
/*
 * CIE CAM (CAM16). ciecamloc_02float()'s call==0 arm: the forward CAM16
 * transform under the scene viewing conditions, the tone and colour sliders,
 * and the inverse transform under the viewing conditions. The host builds the
 * viewing-condition scalars and the two brightness tables; this program only
 * reproduces the per-pixel arithmetic. The `c16 == 16` (CAT16) arm is the only
 * one admitted; the PQ arm above pqremap 100 is a host residual.
 */
inline void rt_la_cie_xyz_cat16(float* r, float* g, float* b, const float x,
                                const float y, const float z)
{
    *r = 0.401288f * x + 0.650173f * y - 0.051461f * z;
    *g = -0.250268f * x + 1.204414f * y + 0.045854f * z;
    *b = -0.002079f * x + 0.048952f * y + 0.953127f * z;
}

/* Ciecam02::cat02_to_xyzfloat()'s c16 == 16 arm. The host's per-pixel loop
 * runs the vector form, whose y row carries 0.621447744f. */
inline void rt_la_cie_cat16_xyz(float* x, float* y, float* z, const float r,
                                const float g, const float b)
{
    *x = 1.86206786f * r - 1.01125463f * g + 0.14918677f * b;
    *y = 0.38752654f * r + 0.621447744f * g - 0.00897398f * b;
    *z = -0.0158415f * r - 0.03412294f * g + 1.04996444f * b;
}

/*
 * opthelper.h pow_F(a, b) = xexpf(b * xlogf(a)). The block's per-pixel loop
 * calls the SIMD form, whose xexpf evaluates `1 + (s * s) * u + s`; the shared
 * rt_lr_pow_F port keeps the scalar grouping `s * (s * u + 1) + 1`, which
 * rounds differently in the last bit. The CAM16 chain amplifies that
 * difference through the chroma and saturation sliders, so this program uses
 * the vector grouping.
 */
inline float rt_la_cie_pow(const float a, const float b)
{
    const float d = b * rt_lr_xlogf(a);
    const int q = rt_lr_xrintf(d * RT_LR_RLN2);
    float s = (float)q * -RT_LR_L2Uf + d;
    s = (float)q * -RT_LR_L2Lf + s;

    float u = 0.00136324646882712841033936f;
    u = u * s + 0.00836596917361021041870117f;
    u = u * s + 0.0416710823774337768554688f;
    u = u * s + 0.166665524244308471679688f;
    u = u * s + 0.499999850988388061523438f;

    u = 1.0f + ((s * s) * u + s);
    return rt_lr_ldexpkf(u, q);
}

inline float rt_la_cie_nonlin(const float c, const float fl)
{
    if (c < 0.0f) {
        const float p = rt_la_cie_pow((-1.0f * fl * c) / 100.0f, 0.42f);
        return ((-1.0f * 400.0f * p) / (27.13f + p)) + 0.1f;
    }
    const float p = rt_la_cie_pow((fl * c) / 100.0f, 0.42f);
    return ((400.0f * p) / (27.13f + p)) + 0.1f;
}

inline float rt_la_cie_inv_nonlin(float c, float fl)
{
    c -= 0.1f;
    if (c < 0.0f) {
        fl *= -1.0f;
        if (c < -399.99f) {
            c = -399.99f;
        }
    } else if (c > 399.99f) {
        c = 399.99f;
    }
    return (100.0f / fl) * rt_la_cie_pow(
                                (27.13f * fabs(c)) / (400.0f - fabs(c)),
                                2.38095238f);
}

inline float rt_la_cie_sign(const float x)
{
    return (x > 0.0f) - (x < 0.0f);
}

inline void rt_la_cie_calc_ab(float* aa, float* bb, const float h,
                              const float e, const float t, const float nbb,
                              const float a)
{
    const float2 sincosval =
        rt_lt_xsincosf((h * RT_LA_PI_F) / 180.0f);
    float sinh = sincosval.x;
    float cosh = sincosval.y;
    const float x = (a / nbb) + 0.305f;
    const float p3 = 1.05f;
    const bool swapValues = fabs(sinh) > fabs(cosh);
    if (swapValues) {
        const float tmp = sinh;
        sinh = cosh;
        cosh = tmp;
    }
    float c1 = 1.0f;
    float c2 = sinh / cosh;
    if (swapValues) {
        const float tmp = c1;
        c1 = c2;
        c2 = tmp;
    }
    float div = ((e / (t * cosh)) -
                 (-0.31362f - (p3 * 0.15681f)) * c1 -
                 ((0.01924f - (p3 * 4.49038f)) * c2));
    if (rt_la_cie_sign(div) != rt_la_cie_sign(cosh) ||
        fabs(div) <= fabs(cosh) * 2.0f) {
        div = cosh * 2.0f;
    }
    *aa = ((0.32787f * x) * (2.0f + p3)) / div;
    *bb = (*aa * sinh) / cosh;
    if (swapValues) {
        const float tmp = *aa;
        *aa = *bb;
        *bb = tmp;
    }
}

inline void rt_la_cie_aab_to_rgb(float* r, float* g, float* b, const float A,
                                 const float aa, const float bb,
                                 const float nbb)
{
    const float x = (A / nbb) + 0.305f;
    *r = (0.32787f * x) + (0.32145f * aa) + (0.20527f * bb);
    *g = (0.32787f * x) - (0.63507f * aa) - (0.18603f * bb);
    *b = (0.32787f * x) - (0.15681f * aa) - (4.49038f * bb);
}

inline void rt_la_cie_xyz2jchqms(float* J, float* C, float* h, float* Q,
                                 float* M, float* s, const float aw,
                                 const float fl, const float wh,
                                 const float x, const float y,
                                 const float z, const float xw,
                                 const float yw, const float zw,
                                 const float c, const float nc,
                                 const float pow1, const float nbb,
                                 const float ncb, const float pfl,
                                 const float cz, const float d)
{
    float r, g, b;
    float rw, gw, bw;
    rt_la_cie_xyz_cat16(&r, &g, &b, x, y, z);
    rt_la_cie_xyz_cat16(&rw, &gw, &bw, xw, yw, zw);
    const float rc = r * (((yw * d) / rw) + (1.0f - d));
    const float gc = g * (((yw * d) / gw) + (1.0f - d));
    const float bc = b * (((yw * d) / bw) + (1.0f - d));
    /* cat02_to_hpefloat() is the identity for CAT16. */
    float rp = fmax(rc, 0.0f);
    float gp = fmax(gc, 0.0f);
    float bp = fmax(bc, 0.0f);
    const float rpa = rt_la_cie_nonlin(rp, fl);
    const float gpa = rt_la_cie_nonlin(gp, fl);
    const float bpa = rt_la_cie_nonlin(bp, fl);
    const float ca = rpa - ((12.0f * gpa) - bpa) / 11.0f;
    const float cb = (0.11111111f) * (rpa + gpa - (2.0f * bpa));
    float myh = rt_lt_xatan2f(cb, ca);
    if (myh < 0.0f) {
        myh += 2.0f * RT_LA_PI_F;
    }
    float a = ((2.0f * rpa) + gpa + (0.05f * bpa) - 0.305f) * nbb;
    a = fmax(a, 0.0f);
    *J = rt_la_cie_pow(a / aw, c * cz * 0.5f);
    const float e =
        ((961.53846f) * nc * ncb) *
        (rt_lt_xsincosf(myh + 2.0f).y + 3.8f);
    const float t =
        (e * sqrt((ca * ca) + (cb * cb))) / (rpa + gpa + (1.05f * bpa));
    *C = rt_la_cie_pow(t, 0.9f) * (*J) * pow1;
    *Q = wh * (*J);
    *J *= (*J) * 100.0f;
    *M = (*C) * pfl;
    *Q = fmax(*Q, 0.0001f);
    *s = 100.0f * sqrt((*M) / (*Q));
    *h = (myh * 180.0f) / RT_LA_PI_F;
}

inline void rt_la_cie_jch2xyz(float* x, float* y, float* z, const float J,
                              const float C, const float h, const float xw,
                              const float yw, const float zw, const float nc,
                              const float pow1, const float nbb,
                              const float ncb, const float fl, const float d,
                              const float aw, const float reccmcz)
{
    float rw, gw, bw;
    rt_la_cie_xyz_cat16(&rw, &gw, &bw, xw, yw, zw);
    const float e =
        ((961.53846f) * nc * ncb) *
        (rt_lt_xsincosf(((h * RT_LA_PI_F) / 180.0f) + 2.0f).y + 3.8f);
    const float a = rt_la_cie_pow(J / 100.0f, reccmcz) * aw;
    const float t =
        rt_la_cie_pow((10.0f * C) / (sqrt(J) * pow1), 1.1111111f);
    float ca, cb;
    rt_la_cie_calc_ab(&ca, &cb, h, e, t, nbb, a);
    float rpa, gpa, bpa;
    rt_la_cie_aab_to_rgb(&rpa, &gpa, &bpa, a, ca, cb, nbb);
    float rp = rt_la_cie_inv_nonlin(rpa, fl);
    float gp = rt_la_cie_inv_nonlin(gpa, fl);
    float bp = rt_la_cie_inv_nonlin(bpa, fl);
    const float r = rp / (((yw * d) / rw) + (1.0f - d));
    const float g = gp / (((yw * d) / gw) + (1.0f - d));
    const float b = bp / (((yw * d) / bw) + (1.0f - d));
    rt_la_cie_cat16_xyz(x, y, z, r, g, b);
}

/* Ciecam02::curvecolorfloat(). */
inline float rt_la_cie_curve_color(const float satind, const float satval,
                                   const float parsat)
{
    float sres;
    if (satind > 0.0f) {
        if (satval >= 1.0f) {
            sres = satval;
        } else {
            const float one = 1.0f - fmin(satval, 1.0f);
            const float pow4 = one * one * one * one;
            sres = (1.0f - satind / 100.0f) * satval +
                   (satind / 100.0f) * (1.0f - pow4);
        }
        if (sres > parsat) {
            sres = fmax(parsat, satval);
        }
    } else if (satind < 0.0f) {
        sres = satval * (1.0f + satind / 100.0f);
    } else {
        sres = satval;
    }
    return sres;
}

/* Color::scalered() and Color::transitred(). */
inline void rt_la_cie_scalered(const float rstprotection, const float param,
                               const float limit, const float HH,
                               const float deltaHH, float* scale,
                               float* scaleext)
{
    if (rstprotection < 99.9999f) {
        if (param > limit) {
            *scale = rstprotection / 100.1f;
        }
        if (HH < (1.3f + deltaHH) && HH >= 1.3f) {
            *scaleext = (HH * (1.0f - *scale) + deltaHH -
                         (1.3f + deltaHH) * (1.0f - *scale)) /
                        deltaHH;
        } else if (HH < 0.15f && HH > (0.15f - deltaHH)) {
            *scaleext = (HH * (*scale - 1.0f) + deltaHH -
                         (0.15f - deltaHH) * (*scale - 1.0f)) /
                        deltaHH;
        }
    }
}

inline void rt_la_cie_transitred(const float HH, const float Chprov1,
                                 const float dred, const float factorskin,
                                 const float protect_red,
                                 const float factorskinext,
                                 const float deltaHH,
                                 const float factorsat, float* factor)
{
    if (HH >= 0.15f && HH < 1.3f) {
        if (Chprov1 < dred) {
            *factor = factorskin;
        } else if (Chprov1 < (dred + protect_red)) {
            *factor = ((factorsat - factorskin) * Chprov1 +
                       factorsat * protect_red -
                       (dred + protect_red) * (factorsat - factorskin)) /
                      protect_red;
        }
    } else if (HH > (0.15f - deltaHH) && HH < (1.3f + deltaHH)) {
        if (Chprov1 < dred) {
            *factor = factorskinext;
        } else if (Chprov1 < (dred + protect_red)) {
            *factor = ((factorsat - factorskinext) * Chprov1 +
                       factorsat * protect_red -
                       (dred + protect_red) * (factorsat - factorskinext)) /
                      protect_red;
        }
    }
}

/* Color::skinredfloat(). `s` is the value the caller passes and receives. */
inline float rt_la_cie_skinred(const float J, const float h, const float sres,
                               const float Sp, float dred,
                               float protect_red, const int sk,
                               const float rstprotection, const float ko,
                               const float s)
{
    float result = s;
    float HH = 0.0f;
    bool doskin = false;
    if (h > 8.6f && h <= 74.0f) {
        HH = (1.15f / 65.4f) * h - 0.0012f;
        doskin = true;
    } else if (h > 0.0f && h <= 8.6f) {
        HH = (0.19f / 8.6f) * h - 0.04f;
        doskin = true;
    } else if (h > 355.0f && h <= 360.0f) {
        HH = (0.11f / 5.0f) * h - 7.96f;
        doskin = true;
    } else if (h > 74.0f && h < 95.0f) {
        HH = (0.30f / 21.0f) * h + 0.24285f;
        doskin = true;
    }
    if (doskin) {
        const float deltaHH = 0.3f;
        const float chromapro = sres / Sp;
        if (sk == 1) {
            if (J < 16.0f) {
                dred = 40.0f;
            } else if (J < 22.0f) {
                dred = 2.5f * J;
            } else if (J < 60.0f) {
                dred = 55.0f;
            } else if (J < 70.0f) {
                dred = 145.0f - 1.5f * J;
            } else {
                dred = 40.0f;
            }
        }
        float factorskin;
        float factorskinext;
        if (chromapro > 1.0f) {
            float scale = 0.999000999f;
            float scaleext = 1.0f;
            rt_la_cie_scalered(rstprotection, chromapro, 0.0f, HH, deltaHH,
                               &scale, &scaleext);
            const float interm = chromapro - 1.0f;
            factorskin = 1.0f + (interm * scale);
            factorskinext = 1.0f + (interm * scaleext);
        } else {
            factorskin = chromapro;
            factorskinext = chromapro;
        }
        float factor = chromapro;
        rt_la_cie_transitred(HH, result, dred, factorskin, protect_red,
                             factorskinext, deltaHH, chromapro, &factor);
        result *= factor;
    } else {
        result = ko * sres;
    }
    return result;
}

/*
 * The CIE CAM pass: the ROI copy `work` is the block's bufexpfin (a copy of
 * the image as the earlier tools left it), transformed in place. The launcher
 * then blends it over the image with transit_shapedetect2()'s senstype-31
 * arm.
 */
__kernel void rt_local_adjustments_cie(__global float* work,
                                       __global const float* rec,
                                       __global const float* luts,
                                       __global const double* ws_double,
                                       __global const double* iws_double,
                                       __global const float* cachef,
                                       __global const float* cachefy,
                                       const int width, const int height,
                                       const int pitch, const int plane)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int offset = y * pitch + x;
    const float origL = work[offset];
    const float origA = work[plane + offset];
    const float origB = work[2 * plane + offset];

    float X, Y, Z;
    rt_lt_lab2xyz(origL, origA, origB, &X, &Y, &Z);

    const float c = rec[RT_LA_CIE_C];
    const float nc = rec[RT_LA_CIE_NC];
    const float aw = rec[RT_LA_CIE_AW];
    const float fl = rec[RT_LA_CIE_FL];
    const float wh = rec[RT_LA_CIE_WH];
    const float nbb = rec[RT_LA_CIE_NBB];
    const float ncb = rec[RT_LA_CIE_NCB];
    const float cz = rec[RT_LA_CIE_CZ];
    const float d = rec[RT_LA_CIE_D];
    const float pow1 = rec[RT_LA_CIE_POW1];
    const float pfl = rec[RT_LA_CIE_PFL];
    const float coefQ = rec[RT_LA_CIE_COEFQ];
    const float qpro = rec[RT_LA_CIE_QPRO];

    float J, C, h, Q, M, s;
    rt_la_cie_xyz2jchqms(&J, &C, &h, &Q, &M, &s, aw, fl, wh,
                         X / 655.35f, Y / 655.35f, Z / 655.35f,
                         rec[RT_LA_CIE_XW1], rec[RT_LA_CIE_YW1],
                         rec[RT_LA_CIE_ZW1], c, nc, pow1, nbb, ncb, pfl,
                         cz, d);

    float Jpro = J;
    float Cpro = C;
    float hpro = h;
    float Qpro = Q;
    float Mpro = M;
    float spro = s;

    /* The per-hue brightness sliders are at their defaults, so only the
     * block's chroma/colourfulness recomputation runs. */
    if ((hpro > 340.0f && hpro <= 360.0f) ||
        (hpro > 0.0f && hpro <= 100.0f)) {
        hpro = hpro + RT_LA_CIE_HUE_ATTENUATION * rec[RT_LA_CIE_HUERED];
        spro = spro * (1.0f + rec[RT_LA_CIE_SCHRRED] / 100.0f);
        const float Cp = (spro * spro * Qpro) / (1000000.0f);
        Mpro = (spro * spro * Qpro) / 10000.0f;
        Cpro = Cp * 100.0f;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }
    if (hpro > 100.0f && hpro <= 190.0f) {
        hpro = hpro + RT_LA_CIE_HUE_ATTENUATION * rec[RT_LA_CIE_HUEGREEN];
        spro = spro * (1.0f + rec[RT_LA_CIE_SCHRGREEN] / 100.0f);
        const float Cp = (spro * spro * Qpro) / (1000000.0f);
        Mpro = (spro * spro * Qpro) / 10000.0f;
        Cpro = Cp * 100.0f;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }
    if (hpro > 190.0f && hpro <= 340.0f) {
        hpro = hpro + RT_LA_CIE_HUE_ATTENUATION * rec[RT_LA_CIE_HUEBLUE];
        spro = spro * (1.0f + rec[RT_LA_CIE_SCHRBLUE] / 100.0f);
        const float Cp = (spro * spro * Qpro) / (1000000.0f);
        Mpro = (spro * spro * Qpro) / 10000.0f;
        Cpro = Cp * 100.0f;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }

    const float mchr = rec[RT_LA_CIE_MCHR];
    const float schr = rec[RT_LA_CIE_SCHR];
    const float cchr = rec[RT_LA_CIE_CCHR];
    const float hue = rec[RT_LA_CIE_HUE];
    const float rstprotect = rec[RT_LA_CIE_RSTPROTECT];
    const float coe = rec[RT_LA_CIE_COE];

    Qpro = rt_lt_lut(luts + (int)rec[RT_LA_CIE_CAMQ_OFFSET],
                     RT_LA_CIE_CAM_ENTRIES, Qpro * coefQ) / coefQ;

    const float Mp = Mpro / 100.0f;
    float sres = rt_la_cie_curve_color(mchr, Mp, 2.5f);
    Mpro = rt_la_cie_skinred(Jpro, hpro, sres, Mp, 100.0f * coe,
                             80.0f * coe, 0, rstprotect, 100.0f, Mpro);
    Jpro = (10.0f * Qpro) / wh;
    Jpro *= Jpro;
    Qpro = (Qpro == 0.0f ? RT_LA_CIE_EPSIL : Qpro);
    spro = 100.0f * sqrt(Mpro / Qpro);
    Jpro = rt_lt_lut(luts + (int)rec[RT_LA_CIE_CAMJ_OFFSET],
                     RT_LA_CIE_CAM_ENTRIES, Jpro * 327.68f);
    const float Sp = spro / 100.0f;
    sres = rt_la_cie_curve_color(schr, Sp, 1.5f);
    spro = rt_la_cie_skinred(
        Jpro, hpro, sres, Sp, 100.0f * sqrt((100.0f * coe) / Q),
        100.0f * sqrt((80.0f * coe) / Q), 0, rstprotect, 100.0f, spro);
    Qpro = qpro * sqrt(Jpro);
    const float Cp = (spro * spro * Qpro) / 1000000.0f;
    Cpro = Cp * 100.0f;
    sres = rt_la_cie_curve_color(cchr, Cp, 1.8f);
    Cpro = rt_la_cie_skinred(Jpro, hpro, sres, Cp, 55.0f, 30.0f, 1,
                             rstprotect, 100.0f, Cpro);
    hpro = hpro + hue;
    if (hpro < 0.0f) {
        hpro += 360.0f;
    }

    float xx, yy, zz;
    rt_la_cie_jch2xyz(&xx, &yy, &zz, Jpro, Cpro, hpro,
                      rec[RT_LA_CIE_XW2], rec[RT_LA_CIE_YW2],
                      rec[RT_LA_CIE_ZW2], rec[RT_LA_CIE_NC2],
                      rec[RT_LA_CIE_POW1N], rec[RT_LA_CIE_NBBJ],
                      rec[RT_LA_CIE_NCBJ], rec[RT_LA_CIE_FLJ],
                      rec[RT_LA_CIE_DJ], rec[RT_LA_CIE_AWJ],
                      rec[RT_LA_CIE_RECCMCZ]);
    const float outX = fmin(fmax(xx * 655.35f, 0.0f), 65535.0f);
    const float outY = fmin(fmax(yy * 655.35f, 0.0f), 65535.0f);
    const float outZ = fmin(fmax(zz * 655.35f, 0.0f), 65535.0f);
    float L, a, b;
    rt_lt_xyz2lab(outX, outY, outZ, cachef, cachefy, &L, &a, &b);

    /* The block's recovery blend: `intp(repart, original, result)`. */
    const float repart = rec[RT_LA_CIE_REPART];
    L = rt_lt_intp(repart, origL, L);
    a = rt_lt_intp(repart, origA, a);
    b = rt_lt_intp(repart, origB, b);

    /* The final gain and gamut round trip at its neutral settings: Lab to
     * XYZ, XYZ to RGB through the inverse matrix, back through the working
     * matrix, and XYZ to Lab. The host evaluates both products in double. */
    float rx, ry, rz;
    rt_lt_lab2xyz(L, a, b, &rx, &ry, &rz);
    const float rr = (float)(iws_double[0] * (double)rx +
                             iws_double[1] * (double)ry +
                             iws_double[2] * (double)rz);
    const float gg = (float)(iws_double[3] * (double)rx +
                             iws_double[4] * (double)ry +
                             iws_double[5] * (double)rz);
    const float bb = (float)(iws_double[6] * (double)rx +
                             iws_double[7] * (double)ry +
                             iws_double[8] * (double)rz);
    const float fx = (float)(ws_double[0] * (double)rr +
                             ws_double[1] * (double)gg +
                             ws_double[2] * (double)bb);
    const float fy = (float)(ws_double[3] * (double)rr +
                             ws_double[4] * (double)gg +
                             ws_double[5] * (double)bb);
    const float fz = (float)(ws_double[6] * (double)rr +
                             ws_double[7] * (double)gg +
                             ws_double[8] * (double)bb);
    rt_lt_xyz2lab(fx, fy, fz, cachef, cachefy, &L, &a, &b);

    work[offset] = L;
    work[plane + offset] = a;
    work[2 * plane + offset] = b;
}
/*
 * Common mask. maskcalccol() builds the mask the log-encoding and CIE CAM
 * tools pass to transit_shapedetect2() as their deltaE reference. The admitted
 * arm keeps the luminance curve and zeroes the chroma/hue curves, so the mask's
 * a/b channels are zero and the mask is a pure luminance field. The first
 * kernel samples LocLLmaskCurve with the host's 501-point interpolation; the
 * finish kernel reproduces blendmask()'s copy and clip; the box pair replays
 * boxblur(); the recovery pair replays maskrecov().
 */
#define RT_LA_MASK_BOX_MAX 16

/* LocLLmaskCurve::operator[]: LUT_CLIP_BELOW/ABOVE with linear
 * interpolation on a 501-entry table (maxs = size - 2). */
inline float rt_la_mask_curve(__global const float* table, const float index)
{
    if (index < 0.0f) {
        return table[0];
    }
    if (index > 499.0f) {
        return table[500];
    }
    const int idx = (int)index;
    const float diff = index - (float)idx;
    const float low = table[idx];
    return low + (table[idx + 1] - low) * diff;
}

/* maskcalccol()'s luminance mask before its own 1 / sk Gaussian:
 * `32768 * LIM01(1 - curve[(500 / 32768) * L])`; the identity table is the
 * gamma-1 / slope-0 LUT, which passes kmaskL through unchanged. */
__kernel void rt_local_adjustments_mask_curve(
    __global const float* src, __global float* dst,
    __global const float* luts, const float offset, const int active,
    const int width, const int height, const int src_pitch,
    const int dst_pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const float l = src[y * src_pitch + x];
    float value = 0.0f;
    if (active != 0) {
        value = rt_la_mask_curve(luts + (int)offset,
                                 (500.0f / 32768.0f) * l);
    }
    dst[y * dst_pitch + x] = 32768.0f * rt_lt_lim01(1.0f - value);
}

/* blendmask() with blendm = blendmab = 0: the mask copy stays unclipped
 * outside the spot, inside it the image is CLIP/clipC'd and the mask is the
 * clipped image minus the blurred mask (its a/b are the clipped image). */
__kernel void rt_local_adjustments_mask_finish(
    __global const float* orig, __global const float* mask,
    __global float* out, __global float* lab,
    __global const float* rec,
    const int lab_pitch, const int lab_plane,
    const int orig_pitch, const int orig_plane,
    const int out_pitch, const int out_plane,
    const int mask_pitch, const int bfw, const int bfh,
    const int xstart, const int ystart)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= bfw || ry >= bfh) {
        return;
    }
    const int oo = ry * orig_pitch + rx;
    const int mo = ry * mask_pitch + rx;
    const int outo = ry * out_pitch + rx;
    const int lx = rx + xstart;
    const int ly = ry + ystart;
    int zone = 0;
    float localFactor = 1.0f;
    rt_la_transition((float)lx, (float)ly, rec[RT_LA_TRANS] / 100.0f, rec,
                     &zone, &localFactor);
    const float origL = orig[oo];
    const float origA = orig[orig_plane + oo];
    const float origB = orig[2 * orig_plane + oo];
    if (zone > 0) {
        const float l = fmin(fmax(origL, 0.0f), 65535.0f);
        const float a = rt_la_clipC(origA);
        const float b = rt_la_clipC(origB);
        out[outo] = fmin(fmax(l - mask[mo], 0.0f), 65535.0f);
        out[out_plane + outo] = rt_la_clipC(a);
        out[2 * out_plane + outo] = rt_la_clipC(b);
        const int lo = ly * lab_pitch + lx;
        lab[lo] = l;
        lab[lab_plane + lo] = a;
        lab[2 * lab_plane + lo] = b;
    } else {
        out[outo] = origL;
        out[out_plane + outo] = origA;
        out[2 * out_plane + outo] = origB;
    }
}

/* boxblur()'s horizontal pass, one work item per row. The host runs this
 * pass scalar; the running mean keeps its float operation order and its
 * shrinking/growing border windows. */
__kernel void rt_local_adjustments_mask_box_h(
    __global float* plane, const int width, const int height,
    const int pitch, const int radius)
{
    const int row = (int)get_global_id(0);
    if (row >= height) {
        return;
    }
    float* line = plane + row * pitch;
    if (radius <= 0) {
        return;
    }
    float lineBuffer[RT_LA_MASK_BOX_MAX + 1];
    float len = (float)(radius + 1);
    float tempval = line[0];
    lineBuffer[0] = tempval;
    for (int j = 1; j <= radius; ++j) {
        tempval += line[j];
    }
    tempval /= len;
    line[0] = tempval;
    for (int col = 1; col <= radius; ++col) {
        lineBuffer[col] = line[col];
        tempval = (tempval * len + line[col + radius]) / (len + 1.0f);
        line[col] = tempval;
        len += 1.0f;
    }
    int pos = 0;
    for (int col = radius + 1; col < width - radius; ++col) {
        const float oldVal = lineBuffer[pos];
        lineBuffer[pos] = line[col];
        tempval = tempval + (line[col + radius] - oldVal) / len;
        line[col] = tempval;
        ++pos;
        pos = pos <= radius ? pos : 0;
    }
    for (int col = width - radius; col < width; ++col) {
        tempval = (tempval * len - lineBuffer[pos]) / (len - 1.0f);
        line[col] = tempval;
        len -= 1.0f;
        ++pos;
        pos = pos <= radius ? pos : 0;
    }
}

/* boxblur()'s vertical pass. The host's SSE groups of eight columns use a
 * reciprocal multiply in the interior loop; the scalar tail divides. Both
 * are lane-wise recurrences, so one work item per column reproduces each
 * column's own arithmetic. */
__kernel void rt_local_adjustments_mask_box_v(
    __global float* plane, const int width, const int height,
    const int pitch, const int radius)
{
    const int col = (int)get_global_id(0);
    if (col >= width) {
        return;
    }
    if (radius <= 0) {
        return;
    }
    float rowBuffer[RT_LA_MASK_BOX_MAX + 1];
    const int vector_end = width - (width % 8);
    const int vector = col < vector_end ? 1 : 0;
    float len = (float)(radius + 1);
    float tempval = plane[col];
    rowBuffer[0] = tempval;
    for (int i = 1; i <= radius; ++i) {
        tempval += plane[i * pitch + col];
    }
    tempval /= len;
    plane[col] = tempval;
    for (int row = 1; row <= radius; ++row) {
        rowBuffer[row] = plane[row * pitch + col];
        tempval = (tempval * len + plane[(row + radius) * pitch + col]) /
                  (len + 1.0f);
        plane[row * pitch + col] = tempval;
        len += 1.0f;
    }
    int pos = 0;
    if (vector != 0) {
        const float rlen = 1.0f / len;
        for (int row = radius + 1; row < height - radius; ++row) {
            const float oldVal = rowBuffer[pos];
            rowBuffer[pos] = plane[row * pitch + col];
            tempval = tempval +
                      (plane[(row + radius) * pitch + col] - oldVal) * rlen;
            plane[row * pitch + col] = tempval;
            ++pos;
            pos = pos <= radius ? pos : 0;
        }
    } else {
        for (int row = radius + 1; row < height - radius; ++row) {
            const float oldVal = rowBuffer[pos];
            rowBuffer[pos] = plane[row * pitch + col];
            tempval = tempval +
                      (plane[(row + radius) * pitch + col] - oldVal) / len;
            plane[row * pitch + col] = tempval;
            ++pos;
            pos = pos <= radius ? pos : 0;
        }
    }
    for (int row = height - radius; row < height; ++row) {
        tempval = (tempval * len - rowBuffer[pos]) / (len - 1.0f);
        plane[row * pitch + col] = tempval;
        len -= 1.0f;
        ++pos;
        pos = pos <= radius ? pos : 0;
    }
}

/* iplocallab.cc calcdif(): the sRGB/Lab dark-and-light breakpoints. */
inline float rt_la_mask_calcdif(const float lmr)
{
    if (lmr < 11.6f) {
        return (7.6f / 11.6f) * lmr;
    } else if (lmr < 60.0f) {
        const float a1 = (62.0f - 7.6f) / (60.0f - 11.6f);
        return a1 * lmr + (62.0f - a1 * 60.0f);
    }
    const float a2 = (100.0f - 62.0f) / (100.0f - 60.0f);
    return a2 * lmr + (100.0f - a2 * 100.0f);
}

/* maskrecov()'s masklum: the low/high ramps and `1 - pow(k, decay)` for the
 * block's `invmask == false`. */
__kernel void rt_local_adjustments_mask_recov_mask(
    __global const float* mask, __global float* masklum,
    const int width, const int height, const int mask_pitch,
    const int dst_pitch, const float recoth, const float low,
    const float high, const float decay)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const float higc0 = rt_la_mask_calcdif(high);
    const float lowc = rt_la_mask_calcdif(low);
    const float higc = higc0 < lowc ? lowc + 0.01f : higc0;
    const float th = recoth - 1.0f;
    const float ahigh = th / (higc - 100.0f);
    const float bhigh = 1.0f - higc * ahigh;
    const float alow = th / lowc;
    const float blow = 1.0f - th;
    const float lM = mask[y * mask_pitch + x];
    const float lmr = lM / 327.68f;
    float k = 1.0f;
    if (lM < 327.68f * lowc) {
        k = alow * lmr + blow;
    } else if (lM >= 327.68f * higc) {
        k = ahigh * lmr + bhigh;
    }
    masklum[y * dst_pitch + x] = 1.0f - pow(k, decay);
}

/* maskrecov()'s final blend: `(original - result) * LIM01(masklum) + result`. */
__kernel void rt_local_adjustments_mask_recov_apply(
    __global const float* lab, __global float* work,
    __global const float* masklum,
    const int lab_pitch, const int lab_plane,
    const int work_pitch, const int work_plane,
    const int mask_pitch, const int width, const int height,
    const int xstart, const int ystart)
{
    const int rx = (int)get_global_id(0);
    const int ry = (int)get_global_id(1);
    if (rx >= width || ry >= height) {
        return;
    }
    const float k = rt_lt_lim01(masklum[ry * mask_pitch + rx]);
    const int lo = (ry + ystart) * lab_pitch + (rx + xstart);
    const int wo = ry * work_pitch + rx;
    work[wo] = (lab[lo] - work[wo]) * k + work[wo];
    work[work_plane + wo] =
        (lab[lab_plane + lo] - work[work_plane + wo]) * k +
        work[work_plane + wo];
    work[2 * work_plane + wo] =
        (lab[2 * lab_plane + lo] - work[2 * work_plane + wo]) * k +
        work[2 * work_plane + wo];
}

/*
 * Capture sharpening. doCapture_Sharpening_SE() converts the clipped ROI to
 * working RGB, builds a contrast mask from RGB2L(), runs
 * CaptureDeconvSharpening_SE() on the Rec2020 luminance and multiplies each
 * RGB channel by the new/old luminance ratio. The admitted arm has no gamma
 * wrapper and no corner radius boost, so the RGB values are used unchanged
 * and the kernel is chosen by sigma alone.
 */

/* Color::RGB2Y()'s Rec2020 luminance: each channel floored at zero, then the
 * host's left-to-right sum. */
inline float rt_la_capture_y(__global const float* rgb, const int pitch,
                             const int plane, const int index)
{
    const float r = fmax(rgb[index], 0.0f);
    const float g = fmax(rgb[plane + index], 0.0f);
    const float b = fmax(rgb[2 * plane + index], 0.0f);
    return 0.2627f * r + 0.6780f * g + 0.0593f * b;
}

/* RGB2L()'s working-matrix luminance through Color::computeXYZ2LabY(). */
__kernel void rt_local_adjustments_capture_luma(
    __global const float* rgb, const int rgb_pitch, const int rgb_plane,
    __global float* luma, const int luma_pitch,
    __global const float* ws, __global const float* cachefy,
    const int width, const int height)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int index = y * rgb_pitch + x;
    const float r = rgb[index];
    const float g = rgb[rgb_plane + index];
    const float b = rgb[2 * rgb_plane + index];
    const float value = ws[3] * r + ws[4] * g + ws[5] * b;
    luma[y * luma_pitch + x] = rt_la_compute_fy(value, cachefy);
}

/* CaptureDeconvSharpening_SE()'s grouped Gaussian sums. The host reads one
 * representative of each symmetric coefficient pair and groups the taps in a
 * fixed order; the helper repeats that order so the recurrence's rounding
 * matches. `src` is the tile estimate and `kernel` the host-built N x N
 * matrix in row-major order. */
#define RT_LA_CAP_S(di, dj) src[base + (di) * ft + (dj)]
inline float rt_la_capture_gauss(__local float* src, const int ft,
                                 const int k, const int l, const int n,
                                 __global const float* coeff)
{
    const int base = k * ft + l;
    if (n == 3) {
        const float c11 = coeff[0];
        const float c10 = coeff[1];
        const float c00 = coeff[4];
        return c11 * (RT_LA_CAP_S(-1, -1) + RT_LA_CAP_S(-1, 1) +
                      RT_LA_CAP_S(1, -1) + RT_LA_CAP_S(1, 1)) +
               c10 * (RT_LA_CAP_S(-1, 0) + RT_LA_CAP_S(0, -1) +
                      RT_LA_CAP_S(0, 1) + RT_LA_CAP_S(1, 0)) +
               c00 * RT_LA_CAP_S(0, 0);
    }
    if (n == 5) {
        const float c21 = coeff[1];
        const float c20 = coeff[2];
        const float c11 = coeff[6];
        const float c10 = coeff[7];
        const float c00 = coeff[12];
        return c21 * ((RT_LA_CAP_S(-2, -1) + RT_LA_CAP_S(-2, 1)) +
                      (RT_LA_CAP_S(-1, -2) + RT_LA_CAP_S(-1, 2)) +
                      (RT_LA_CAP_S(1, -2) + RT_LA_CAP_S(1, 2)) +
                      (RT_LA_CAP_S(2, -1) + RT_LA_CAP_S(2, 1))) +
               c20 * (RT_LA_CAP_S(-2, 0) + RT_LA_CAP_S(0, -2) +
                      RT_LA_CAP_S(0, 2) + RT_LA_CAP_S(2, 0)) +
               c11 * (RT_LA_CAP_S(-1, -1) + RT_LA_CAP_S(-1, 1) +
                      RT_LA_CAP_S(1, -1) + RT_LA_CAP_S(1, 1)) +
               c10 * (RT_LA_CAP_S(-1, 0) + RT_LA_CAP_S(0, -1) +
                      RT_LA_CAP_S(0, 1) + RT_LA_CAP_S(1, 0)) +
               c00 * RT_LA_CAP_S(0, 0);
    }
    if (n == 7) {
        const float c31 = coeff[2];
        const float c30 = coeff[3];
        const float c22 = coeff[8];
        const float c21 = coeff[9];
        const float c20 = coeff[10];
        const float c11 = coeff[16];
        const float c10 = coeff[17];
        const float c00 = coeff[24];
        return c31 * ((RT_LA_CAP_S(-3, -1) + RT_LA_CAP_S(-3, 1)) +
                      (RT_LA_CAP_S(-1, -3) + RT_LA_CAP_S(-1, 3)) +
                      (RT_LA_CAP_S(1, -3) + RT_LA_CAP_S(1, 3)) +
                      (RT_LA_CAP_S(3, -1) + RT_LA_CAP_S(3, 1))) +
               c30 * (RT_LA_CAP_S(-3, 0) + RT_LA_CAP_S(0, -3) +
                      RT_LA_CAP_S(0, 3) + RT_LA_CAP_S(3, 0)) +
               c22 * (RT_LA_CAP_S(-2, -2) + RT_LA_CAP_S(-2, 2) +
                      RT_LA_CAP_S(2, -2) + RT_LA_CAP_S(2, 2)) +
               c21 * ((RT_LA_CAP_S(-2, -1) + RT_LA_CAP_S(-2, 1)) +
                      (RT_LA_CAP_S(-1, -2) + RT_LA_CAP_S(-1, 2)) +
                      (RT_LA_CAP_S(1, -2) + RT_LA_CAP_S(1, 2)) +
                      (RT_LA_CAP_S(2, -1) + RT_LA_CAP_S(2, 1))) +
               c20 * (RT_LA_CAP_S(-2, 0) + RT_LA_CAP_S(0, -2) +
                      RT_LA_CAP_S(0, 2) + RT_LA_CAP_S(2, 0)) +
               c11 * (RT_LA_CAP_S(-1, -1) + RT_LA_CAP_S(-1, 1) +
                      RT_LA_CAP_S(1, -1) + RT_LA_CAP_S(1, 1)) +
               c10 * (RT_LA_CAP_S(-1, 0) + RT_LA_CAP_S(0, -1) +
                      RT_LA_CAP_S(0, 1) + RT_LA_CAP_S(1, 0)) +
               c00 * RT_LA_CAP_S(0, 0);
    }
    if (n == 9) {
        const float c42 = coeff[2];
        const float c41 = coeff[3];
        const float c40 = coeff[4];
        const float c33 = coeff[10];
        const float c32 = coeff[11];
        const float c31 = coeff[12];
        const float c30 = coeff[13];
        const float c22 = coeff[20];
        const float c21 = coeff[21];
        const float c20 = coeff[22];
        const float c11 = coeff[30];
        const float c10 = coeff[31];
        const float c00 = coeff[40];
        return c42 * ((RT_LA_CAP_S(-4, -2) + RT_LA_CAP_S(-4, 2)) +
                      (RT_LA_CAP_S(-2, -4) + RT_LA_CAP_S(-2, 4)) +
                      (RT_LA_CAP_S(2, -4) + RT_LA_CAP_S(2, 4)) +
                      (RT_LA_CAP_S(4, -2) + RT_LA_CAP_S(4, 2))) +
               c41 * ((RT_LA_CAP_S(-4, -1) + RT_LA_CAP_S(-4, 1)) +
                      (RT_LA_CAP_S(-1, -4) + RT_LA_CAP_S(-1, 4)) +
                      (RT_LA_CAP_S(1, -4) + RT_LA_CAP_S(1, 4)) +
                      (RT_LA_CAP_S(4, -1) + RT_LA_CAP_S(4, 1))) +
               c40 * (RT_LA_CAP_S(-4, 0) + RT_LA_CAP_S(0, -4) +
                      RT_LA_CAP_S(0, 4) + RT_LA_CAP_S(4, 0)) +
               c33 * (RT_LA_CAP_S(-3, -3) + RT_LA_CAP_S(-3, 3) +
                      RT_LA_CAP_S(3, -3) + RT_LA_CAP_S(3, 3)) +
               c32 * ((RT_LA_CAP_S(-3, -2) + RT_LA_CAP_S(-3, 2)) +
                      (RT_LA_CAP_S(-2, -3) + RT_LA_CAP_S(-2, 3)) +
                      (RT_LA_CAP_S(2, -3) + RT_LA_CAP_S(2, 3)) +
                      (RT_LA_CAP_S(3, -2) + RT_LA_CAP_S(3, 2))) +
               c31 * ((RT_LA_CAP_S(-3, -1) + RT_LA_CAP_S(-3, 1)) +
                      (RT_LA_CAP_S(-1, -3) + RT_LA_CAP_S(-1, 3)) +
                      (RT_LA_CAP_S(1, -3) + RT_LA_CAP_S(1, 3)) +
                      (RT_LA_CAP_S(3, -1) + RT_LA_CAP_S(3, 1))) +
               c30 * (RT_LA_CAP_S(-3, 0) + RT_LA_CAP_S(0, -3) +
                      RT_LA_CAP_S(0, 3) + RT_LA_CAP_S(3, 0)) +
               c22 * (RT_LA_CAP_S(-2, -2) + RT_LA_CAP_S(-2, 2) +
                      RT_LA_CAP_S(2, -2) + RT_LA_CAP_S(2, 2)) +
               c21 * ((RT_LA_CAP_S(-2, -1) + RT_LA_CAP_S(-2, 1)) +
                      (RT_LA_CAP_S(-1, -2) + RT_LA_CAP_S(-1, 2)) +
                      (RT_LA_CAP_S(1, -2) + RT_LA_CAP_S(1, 2)) +
                      (RT_LA_CAP_S(2, -1) + RT_LA_CAP_S(2, 1))) +
               c20 * (RT_LA_CAP_S(-2, 0) + RT_LA_CAP_S(0, -2) +
                      RT_LA_CAP_S(0, 2) + RT_LA_CAP_S(2, 0)) +
               c11 * (RT_LA_CAP_S(-1, -1) + RT_LA_CAP_S(-1, 1) +
                      RT_LA_CAP_S(1, -1) + RT_LA_CAP_S(1, 1)) +
               c10 * (RT_LA_CAP_S(-1, 0) + RT_LA_CAP_S(0, -1) +
                      RT_LA_CAP_S(0, 1) + RT_LA_CAP_S(1, 0)) +
               c00 * RT_LA_CAP_S(0, 0);
    }
    const float c60 = coeff[6];
    const float c53 = coeff[16];
    const float c52 = coeff[17];
    const float c51 = coeff[18];
    const float c50 = coeff[19];
    const float c44 = coeff[28];
    const float c42 = coeff[30];
    const float c41 = coeff[31];
    const float c40 = coeff[32];
    const float c33 = coeff[42];
    const float c32 = coeff[43];
    const float c31 = coeff[44];
    const float c30 = coeff[45];
    const float c22 = coeff[56];
    const float c21 = coeff[57];
    const float c20 = coeff[58];
    const float c11 = coeff[70];
    const float c10 = coeff[71];
    const float c00 = coeff[84];
    return c60 * (RT_LA_CAP_S(-6, 0) + RT_LA_CAP_S(0, -6) +
                  RT_LA_CAP_S(0, 6) + RT_LA_CAP_S(6, 0)) +
           c53 * ((RT_LA_CAP_S(-5, -3) + RT_LA_CAP_S(-5, 3)) +
                  (RT_LA_CAP_S(-3, -5) + RT_LA_CAP_S(-3, 5)) +
                  (RT_LA_CAP_S(3, -5) + RT_LA_CAP_S(3, 5)) +
                  (RT_LA_CAP_S(5, -3) + RT_LA_CAP_S(5, 3))) +
           c52 * ((RT_LA_CAP_S(-5, -2) + RT_LA_CAP_S(-5, 2)) +
                  (RT_LA_CAP_S(-2, -5) + RT_LA_CAP_S(-2, 5)) +
                  (RT_LA_CAP_S(2, -5) + RT_LA_CAP_S(2, 5)) +
                  (RT_LA_CAP_S(5, -2) + RT_LA_CAP_S(5, 2))) +
           c51 * ((RT_LA_CAP_S(-5, -1) + RT_LA_CAP_S(-5, 1)) +
                  (RT_LA_CAP_S(-1, -5) + RT_LA_CAP_S(-1, 5)) +
                  (RT_LA_CAP_S(1, -5) + RT_LA_CAP_S(1, 5)) +
                  (RT_LA_CAP_S(5, -1) + RT_LA_CAP_S(5, 1))) +
           c50 * ((RT_LA_CAP_S(-5, 0) + RT_LA_CAP_S(0, -5) +
                   RT_LA_CAP_S(0, 5) + RT_LA_CAP_S(5, 0)) +
                  ((RT_LA_CAP_S(-4, -3) + RT_LA_CAP_S(-4, 3)) +
                   (RT_LA_CAP_S(-3, -4) + RT_LA_CAP_S(-3, 4)) +
                   (RT_LA_CAP_S(3, -4) + RT_LA_CAP_S(3, 4)) +
                   (RT_LA_CAP_S(4, -3) + RT_LA_CAP_S(4, 3)))) +
           c44 * (RT_LA_CAP_S(-4, -4) + RT_LA_CAP_S(-4, 4) +
                  RT_LA_CAP_S(4, -4) + RT_LA_CAP_S(4, 4)) +
           c42 * ((RT_LA_CAP_S(-4, -2) + RT_LA_CAP_S(-4, 2)) +
                  (RT_LA_CAP_S(-2, -4) + RT_LA_CAP_S(-2, 4)) +
                  (RT_LA_CAP_S(2, -4) + RT_LA_CAP_S(2, 4)) +
                  (RT_LA_CAP_S(4, -2) + RT_LA_CAP_S(4, 2))) +
           c41 * ((RT_LA_CAP_S(-4, -1) + RT_LA_CAP_S(-4, 1)) +
                  (RT_LA_CAP_S(-1, -4) + RT_LA_CAP_S(-1, 4)) +
                  (RT_LA_CAP_S(1, -4) + RT_LA_CAP_S(1, 4)) +
                  (RT_LA_CAP_S(4, -1) + RT_LA_CAP_S(4, 1))) +
           c40 * (RT_LA_CAP_S(-4, 0) + RT_LA_CAP_S(0, -4) +
                  RT_LA_CAP_S(0, 4) + RT_LA_CAP_S(4, 0)) +
           c33 * (RT_LA_CAP_S(-3, -3) + RT_LA_CAP_S(-3, 3) +
                  RT_LA_CAP_S(3, -3) + RT_LA_CAP_S(3, 3)) +
           c32 * ((RT_LA_CAP_S(-3, -2) + RT_LA_CAP_S(-3, 2)) +
                  (RT_LA_CAP_S(-2, -3) + RT_LA_CAP_S(-2, 3)) +
                  (RT_LA_CAP_S(2, -3) + RT_LA_CAP_S(2, 3)) +
                  (RT_LA_CAP_S(3, -2) + RT_LA_CAP_S(3, 2))) +
           c31 * ((RT_LA_CAP_S(-3, -1) + RT_LA_CAP_S(-3, 1)) +
                  (RT_LA_CAP_S(-1, -3) + RT_LA_CAP_S(-1, 3)) +
                  (RT_LA_CAP_S(1, -3) + RT_LA_CAP_S(1, 3)) +
                  (RT_LA_CAP_S(3, -1) + RT_LA_CAP_S(3, 1))) +
           c30 * (RT_LA_CAP_S(-3, 0) + RT_LA_CAP_S(0, -3) +
                  RT_LA_CAP_S(0, 3) + RT_LA_CAP_S(3, 0)) +
           c22 * (RT_LA_CAP_S(-2, -2) + RT_LA_CAP_S(-2, 2) +
                  RT_LA_CAP_S(2, -2) + RT_LA_CAP_S(2, 2)) +
           c21 * ((RT_LA_CAP_S(-2, -1) + RT_LA_CAP_S(-2, 1)) +
                  (RT_LA_CAP_S(-1, -2) + RT_LA_CAP_S(-1, 2)) +
                  (RT_LA_CAP_S(1, -2) + RT_LA_CAP_S(1, 2)) +
                  (RT_LA_CAP_S(2, -1) + RT_LA_CAP_S(2, 1))) +
           c20 * (RT_LA_CAP_S(-2, 0) + RT_LA_CAP_S(0, -2) +
                  RT_LA_CAP_S(0, 2) + RT_LA_CAP_S(2, 0)) +
           c11 * (RT_LA_CAP_S(-1, -1) + RT_LA_CAP_S(-1, 1) +
                  RT_LA_CAP_S(1, -1) + RT_LA_CAP_S(1, 1)) +
           c10 * (RT_LA_CAP_S(-1, 0) + RT_LA_CAP_S(0, -1) +
                  RT_LA_CAP_S(0, 1) + RT_LA_CAP_S(1, 0)) +
           c00 * RT_LA_CAP_S(0, 0);
}
#undef RT_LA_CAP_S

/* The host initialises the deconvolution output to the Rec2020 luminance
 * before its tile loop, so skipped and never-covered pixels keep that value.
 * The device plane needs the same initial state. */
__kernel void rt_local_adjustments_capture_init(
    __global const float* rgb, const int rgb_pitch, const int rgb_plane,
    __global float* y, const int width, const int height)
{
    const int x = (int)get_global_id(0);
    const int yy = (int)get_global_id(1);
    if (x >= width || yy >= height) {
        return;
    }
    const int index = yy * rgb_pitch + x;
    y[index] = rt_la_capture_y(rgb, rgb_pitch, rgb_plane, index);
}

/*
 * CaptureDeconvSharpening_SE()'s tile recurrence. One workgroup owns one tile
 * and processes it in the host's order. Tiles overlap only at the end of a
 * row or column (the host's end tile starts at `size - full`); the launcher
 * splits the work into four phases so those tiles run after the interior
 * ones, reproducing the host's row-major last-writer order without races.
 * `phase` 0 is interior, 1 the last column, 2 the last row and 3 the corner.
 */
__kernel void rt_local_adjustments_capture(
    __global const float* rgb, const int rgb_pitch, const int rgb_plane,
    __global float* y, __global const float* mask, const int mask_pitch,
    __global const float* coeff, const int kernel_offset,
    const int phase, const int tiles_x, const int tiles_y,
    const int tile_size, const int border, const int full,
    const int radius, const int n,
    const int bfw, const int bfh, const int iterations, const int check_stop)
{
    __local float tile0[RT_LA_CAP_MAX_FULL * RT_LA_CAP_MAX_FULL];
    __local float tile1[RT_LA_CAP_MAX_FULL * RT_LA_CAP_MAX_FULL];
    __local int any_low;
    __local int skip;

    const int group = (int)get_group_id(0);
    const int lid = (int)get_local_id(0);
    const int wg = (int)get_local_size(0);
    __global const float* kptr = coeff + kernel_offset;
    int ti;
    int tj;
    if (phase == 0) {
        const int cols = tiles_x - 1;
        ti = group / cols;
        tj = group - ti * cols;
    } else if (phase == 1) {
        ti = group;
        tj = tiles_x - 1;
    } else if (phase == 2) {
        ti = tiles_y - 1;
        tj = group;
    } else {
        ti = tiles_y - 1;
        tj = tiles_x - 1;
    }
    const int i = border + ti * tile_size;
    const int j = border + tj * tile_size;
    const int end_col = (i + tile_size + border) >= bfh ? 1 : 0;
    const int end_row = (j + tile_size + border) >= bfw ? 1 : 0;
    const int base_y = end_col ? bfh - full : i - border;
    const int base_x = end_row ? bfw - full : j - border;
    const int full_elems = full * full;

    /* The host fills tmpThr with one before its tile loop and never resets
     * it; the border therefore stays one for every tile and iteration. */
    for (int p = lid; p < full_elems; p += wg) {
        tile0[p] = 0.0f;
        tile1[p] = 1.0f;
    }
    if (lid == 0) {
        const int my = end_col ? bfh - full + border : i;
        const int mx = end_row ? bfw - full + border : j;
        float maxv = 0.0f;
        for (int k = 0; k < tile_size; ++k) {
            for (int l = 0; l < tile_size; ++l) {
                maxv = fmax(maxv, mask[(my + k) * mask_pitch + mx + l]);
            }
        }
        skip = maxv < RT_LA_CAP_MIN_BLEND ? 1 : 0;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    if (skip != 0) {
        return;
    }

    for (int p = lid; p < full_elems; p += wg) {
        const int k = p / full;
        const int l = p - k * full;
        const int index = (base_y + k) * rgb_pitch + base_x + l;
        tile0[p] = rt_la_capture_y(rgb, rgb_pitch, rgb_plane, index);
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    int stop = 0;
    for (int iter = 0; iter < iterations && stop == 0; ++iter) {
        for (int p = lid; p < full_elems; p += wg) {
            const int k = p / full;
            const int l = p - k * full;
            if (k >= radius && k < full - radius &&
                l >= radius && l < full - radius) {
                const float val =
                    rt_la_capture_gauss(tile0, full, k, l, n, kptr);
                const int index = (base_y + k) * rgb_pitch + base_x + l;
                const float old =
                    rt_la_capture_y(rgb, rgb_pitch, rgb_plane, index);
                tile1[p] = old / fmax(val, 0.00001f);
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int p = lid; p < full_elems; p += wg) {
            const int k = p / full;
            const int l = p - k * full;
            if (k >= radius && k < full - radius &&
                l >= radius && l < full - radius) {
                tile0[p] *=
                    rt_la_capture_gauss(tile1, full, k, l, n, kptr);
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        if (check_stop != 0 && iter < iterations - 1) {
            if (lid == 0) {
                any_low = 0;
            }
            barrier(CLK_LOCAL_MEM_FENCE);
            for (int p = lid; p < tile_size * tile_size; p += wg) {
                const int kk = p / tile_size;
                const int ll = p - kk * tile_size;
                const int k = border + kk;
                const int l = border + ll;
                const int index = (base_y + k) * rgb_pitch + base_x + l;
                const float old =
                    rt_la_capture_y(rgb, rgb_pitch, rgb_plane, index);
                if (tile0[k * full + l] < old * mask[index] * 0.5f) {
                    atomic_or(&any_low, 1);
                }
            }
            barrier(CLK_LOCAL_MEM_FENCE);
            if (any_low != 0) {
                stop = 1;
            }
        }
    }

    for (int p = lid; p < tile_size * tile_size; p += wg) {
        const int kk = p / tile_size;
        const int ll = p - kk * tile_size;
        const int k = border + kk;
        const int l = border + ll;
        const int index = (base_y + k) * rgb_pitch + base_x + l;
        /* The host's output write blends with `luminance`, which the end
         * tile has already seen modified by the preceding tile in the
         * overlap. `y` was seeded with the Rec2020 luminance, so it carries
         * the same previous writer's value. */
        const float current = y[index];
        y[index] = mask[index] * (tile0[k * full + l] - current) + current;
    }
}

/* doCapture_Sharpening_SE()'s RGB recovery: each channel is scaled by the
 * deconvolved/old luminance ratio. */
__kernel void rt_local_adjustments_capture_finish(
    __global float* rgb, const int rgb_pitch, const int rgb_plane,
    __global const float* y, const int width, const int height)
{
    const int x = (int)get_global_id(0);
    const int yy = (int)get_global_id(1);
    if (x >= width || yy >= height) {
        return;
    }
    const int index = yy * rgb_pitch + x;
    const float r = rgb[index];
    const float g = rgb[rgb_plane + index];
    const float b = rgb[2 * rgb_plane + index];
    const float old = rt_la_capture_y(rgb, rgb_pitch, rgb_plane, index);
    const float factor = y[index] / fmax(old, 0.00001f);
    rgb[index] = r * factor;
    rgb[rgb_plane + index] = g * factor;
    rgb[2 * rgb_plane + index] = b * factor;
}

/* intp(repart, bufexporig, rgb2lab(output)): `repart == 0` keeps the
 * sharpened result and `repart == 1` the original. */
__kernel void rt_local_adjustments_capture_mix(
    __global float* fin, const int fin_pitch, const int fin_plane,
    __global const float* lab, const int lab_pitch, const int lab_plane,
    const int width, const int height, const int xstart, const int ystart,
    const float repart)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int fo = y * fin_pitch + x;
    const int lo = (y + ystart) * lab_pitch + (x + xstart);
    fin[fo] = repart * (lab[lo] - fin[fo]) + fin[fo];
    fin[fin_plane + fo] =
        repart * (lab[lab_plane + lo] - fin[fin_plane + fo]) +
        fin[fin_plane + fo];
    fin[2 * fin_plane + fo] =
        repart * (lab[2 * lab_plane + lo] - fin[2 * fin_plane + fo]) +
        fin[2 * fin_plane + fo];
}

/*
 * Local exposure's LUTf access. The host's operator[](float) truncates toward
 * zero, clips at the configured end and interpolates the two nearest samples.
 * hlCurve is LUT_CLIP_BELOW, shCurve LUT_CLIP_ABOVE and the tone curve is
 * LUT_CLIP_OFF; the 65536-entry tables make maxsf 65534.
 */
inline float rt_la_exposure_lut_lo(__global const float* table,
                                   const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        return table[0];
    }
    if (index > 65534.0f) {
        idx = 65534;
    }
    const float low = table[idx];
    return low + (table[idx + 1] - low) * (index - (float)idx);
}

inline float rt_la_exposure_lut_hi(__global const float* table,
                                   const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        idx = 0;
    } else if (index > 65534.0f) {
        return table[65535];
    }
    const float low = table[idx];
    return low + (table[idx + 1] - low) * (index - (float)idx);
}

/* LUT_CLIP_OFF: neither end clips, so an index above the last sample
 * extrapolates from the final pair. */
inline float rt_la_exposure_lut_off(__global const float* table,
                                    const float index)
{
    int idx = (int)index;
    if (index < 0.0f) {
        idx = 0;
    } else if (index > 65534.0f) {
        idx = 65534;
    }
    const float low = table[idx];
    return low + (table[idx + 1] - low) * (index - (float)idx);
}

/* curves.h CurveFactory::hlcurve(). */
inline float rt_la_exposure_hlcurve(const float exp_scale,
                                    const float comp,
                                    const float hlrange,
                                    const float level)
{
    if (comp > 0.0f) {
        float val = level + (hlrange - 65536.0f);
        if (val == 0.0f) {
            val = 0.000001f;
        }
        float Y = val * exp_scale / hlrange;
        Y *= comp;
        if (Y <= -1.0f) {
            Y = -0.999999f;
        }
        const float R = hlrange / (val * comp);
        return log1p(Y) * R;
    }
    return exp_scale;
}

/*
 * exlabLocal()'s PDE branch: the per-pixel exposure, highlight and shadow
 * curve. `work` holds the block's ROI; only L is read and rewritten, exactly
 * as bufexporig -> bufexpfin. The curve mode is fixed by the admitted arm.
 */
__kernel void rt_local_adjustments_exposure_tone(
    __global float* work, __global const float* luts,
    const int pitch, const int plane, const int width, const int height,
    const float hl_offset, const float sh_offset, const float tone_offset,
    const float expcomp, const float hlcomp, const float hlcompthr,
    const float linear)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int offset = plane + y * pitch + x;
    float L = work[offset];
    const float Llin = fmax(0.0f, fmin(L / 32768.0f, 1.0f));
    const float addcomp = linear * (-Llin + 1.0f);
    const float exp_scale = rt_lr_pow_F(2.0f, expcomp + addcomp);
    const float shoulder =
        (65536.0f / fmax(1.0f, exp_scale)) * (hlcompthr / 200.0f) +
        0.1f;
    const float comp =
        (fmax(0.0f, expcomp + addcomp) + 1.0f) * (hlcomp / 100.0f);
    const float hlrange = 65536.0f - shoulder;
    const float hlfactor =
        2.0f * L < 65535.0f
            ? rt_la_exposure_lut_lo(luts + (int)hl_offset, 2.0f * L)
            : rt_la_exposure_hlcurve(exp_scale, comp, hlrange, 2.0f * L);
    L *= hlfactor * rt_lr_pow_F(2.0f, addcomp);
    L *= rt_la_exposure_lut_hi(luts + (int)sh_offset, 2.0f * L);
    work[offset] =
        0.5f * rt_la_exposure_lut_off(luts + (int)tone_offset, 2.0f * L);
}

/* The Laplacian input plane: LIM01(L / 32768) ^ gam * 32768. The same plane
 * is kept as normalize_mean_dt()'s reference. */
__kernel void rt_local_adjustments_exposure_input(
    __global const float* work, __global float* input,
    __global float* reference, const int pitch, const int plane,
    const int input_pitch_arg, const int ref_pitch_arg,
    const int width, const int height, const float gam)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const int source = plane + y * pitch + x;
    const float L = work[source];
    const float value =
        rt_lr_pow_F(fmax(0.0f, fmin(L / 32768.0f, 1.0f)), gam) *
        32768.0f;
    input[y * input_pitch_arg + x] = value;
    reference[y * ref_pitch_arg + x] = value;
}

/* ImProcFunctions::discrete_laplacian_threshold(). */
__kernel void rt_local_adjustments_exposure_laplacian(
    __global const float* in, __global float* out,
    const int pitch, const int width, const int height,
    const float threshold)
{
    const int i = (int)get_global_id(0);
    const int j = (int)get_global_id(1);
    if (i >= width || j >= height) {
        return;
    }
    const int index = j * pitch + i;
    float val = 0.0f;
    if (i > 0) {
        const float diff = in[index] - in[index - 1];
        val += fabs(diff) > threshold ? diff : 0.0f;
    }
    if (i < width - 1) {
        const float diff = in[index] - in[index + 1];
        val += fabs(diff) > threshold ? diff : 0.0f;
    }
    if (j > 0) {
        const float diff = in[index] - in[index - pitch];
        val += fabs(diff) > threshold ? diff : 0.0f;
    }
    if (j < height - 1) {
        const float diff = in[index] - in[index + pitch];
        val += fabs(diff) > threshold ? diff : 0.0f;
    }
    out[index] = val;
}

/* One axis of FFTW's REDFT10 (inverse == 0) or REDFT01 (inverse != 0). The
 * direct transform accumulates in double; the double-precision result is
 * within FFTW's own single-precision error. */
__kernel void rt_local_adjustments_exposure_dct_rows(
    __global const float* src, __global float* dst,
    __global const float* tables, const int offset,
    const int length, const int lines,
    const int src_pitch, const int dst_pitch, const int inverse)
{
    const int k = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (k >= length || y >= lines) {
        return;
    }
    __global const float* table = tables + offset;
    double sum = 0.0;
    if (inverse == 0) {
        for (int n = 0; n < length; ++n) {
            sum += (double)src[y * src_pitch + n] *
                   (double)table[k * length + n];
        }
        sum *= 2.0;
    } else {
        sum = (double)src[y * src_pitch];
        for (int n = 1; n < length; ++n) {
            sum += 2.0 * (double)src[y * src_pitch + n] *
                   (double)table[k * length + n];
        }
    }
    dst[y * dst_pitch + k] = (float)sum;
}

__kernel void rt_local_adjustments_exposure_dct_cols(
    __global const float* src, __global float* dst,
    __global const float* tables, const int offset,
    const int length, const int lines,
    const int src_pitch, const int dst_pitch, const int inverse)
{
    const int k = (int)get_global_id(0);
    const int x = (int)get_global_id(1);
    if (k >= length || x >= lines) {
        return;
    }
    __global const float* table = tables + offset;
    double sum = 0.0;
    if (inverse == 0) {
        for (int n = 0; n < length; ++n) {
            sum += (double)src[n * src_pitch + x] *
                   (double)table[k * length + n];
        }
        sum *= 2.0;
    } else {
        sum = (double)src[x];
        for (int n = 1; n < length; ++n) {
            sum += 2.0 * (double)src[n * src_pitch + x] *
                   (double)table[k * length + n];
        }
    }
    dst[k * dst_pitch + x] = (float)sum;
}

/* ImProcFunctions::rex_poisson_dct()'s diagonal scaling, plus data[0] = 0. */
__kernel void rt_local_adjustments_exposure_poisson(
    __global float* data, __global const float* tables,
    const int offset_x, const int offset_y, const int pitch,
    const int width, const int height, const float m2)
{
    const int j = (int)get_global_id(0);
    const int i = (int)get_global_id(1);
    if (j >= width || i >= height) {
        return;
    }
    const int index = i * pitch + j;
    data[index] *=
        m2 / (tables[offset_x + j] + tables[offset_y + i]);
    if (index == 0) {
        data[0] = 0.0f;
    }
}

/* normalize_mean_dt(data, reference, size, 1, 1, 0, 0, 0, 0, 1): the two
 * double reductions, then the host's float a*data + b. */
__kernel void rt_local_adjustments_exposure_normalize(
    __global float* data, __global const float* reference,
    const int data_pitch, const int ref_pitch,
    const int width, const int height, const float mod)
{
    if (get_global_id(0) != 0) {
        return;
    }
    double mean_ref = 0.0;
    double dt_ref = 0.0;
    double mean_data = 0.0;
    double dt_data = 0.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float ref = reference[y * ref_pitch + x];
            const float value = data[y * data_pitch + x];
            mean_ref += (double)ref;
            dt_ref += (double)ref * (double)ref;
            mean_data += (double)value;
            dt_data += (double)value * (double)value;
        }
    }
    const int size = width * height;
    mean_ref /= size;
    dt_ref /= size;
    dt_ref -= mean_ref * mean_ref;
    mean_data /= size;
    dt_data /= size;
    dt_data -= mean_data * mean_data;
    dt_ref = sqrt(dt_ref);
    dt_data = sqrt(dt_data);
    const double a = dt_ref / dt_data;
    const double b = mean_ref - a * mean_data;
    const float modma = (float)((double)mod * a);
    const float sigmmmodmb = (float)((double)mod * b);
    const float onesmod = 1.0f - mod;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int index = y * data_pitch + x;
            const float ref = reference[y * ref_pitch + x];
            data[index] = (modma * data[index] + sigmmmodmb) +
                          onesmod * ref;
        }
    }
}

/* The inverse Laplacian gamma: pow_F(Y, 1 / gam) * 32768 back into L. */
__kernel void rt_local_adjustments_exposure_gamma(
    __global float* work, __global const float* data,
    const int pitch, const int plane, const int data_pitch,
    const int width, const int height, const float gam)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }
    const float Y = data[y * data_pitch + x] / 32768.0f;
    work[plane + y * pitch + x] =
        rt_lr_pow_F(Y, 1.0f / gam) * 32768.0f;
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
           RT_LAB_WAVELET_CL + RT_SHARPEN_CL + RT_LOCAL_ADJUSTMENTS_CL;
}
