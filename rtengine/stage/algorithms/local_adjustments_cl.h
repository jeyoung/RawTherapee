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
 * adds the single-spot colour apply, the ROI copy, the ROI Gaussian and the
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

/* iplocallab.cc's constants. */
#define RT_LA_MINSCOPE 0.025f
#define RT_LA_MAXSCOPE 1.25f
#define RT_LA_LIMSCOPE 80
#define RT_LA_PI_F 3.1415927f

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
 * Copy the spot ROI out of the full Lab image into one plane per channel.
 * This is the host's bufcolorig/origblur copy at the start of the colour
 * block.
 */
__kernel void rt_local_adjustments_roi(__global const float* lab,
                                       __global float* roiL,
                                       __global float* roiA,
                                       __global float* roiB,
                                       const int xstart, const int ystart,
                                       const int bfw, const int bfh,
                                       const int lab_pitch,
                                       const int lab_plane,
                                       const int roi_pitch)
{
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= bfw || y >= bfh) {
        return;
    }
    const int src = (y + ystart) * lab_pitch + (x + xstart);
    const int dst = y * roi_pitch + x;
    roiL[dst] = lab[src];
    roiA[dst] = lab[lab_plane + src];
    roiB[dst] = lab[2 * lab_plane + src];
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

    const float refa = rec[RT_LA_CHROMAREF] * cos(rec[RT_LA_HUEREF]) * 327.68f;
    const float refb = rec[RT_LA_CHROMAREF] * sin(rec[RT_LA_HUEREF]) * 327.68f;
    const float refL = rec[RT_LA_LUMAREF] * 327.68f;

    const int bo = ry * blur_pitch + rx;
    const float ml = blurL[bo];
    const float ma = blurA[bo];
    const float mb = blurB[bo];

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

    const float varsens = rec[RT_LA_SENS];
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

    const float factor = localFactor;
    const float realstrdE = reducdE * (corrL - origL);
    const float realstradE = reducdE * (corrA - origA);
    const float realstrbdE = reducdE * (corrB - origB);

    /* clipLoc() is the identity in this build; keep the host's expression. */
    lab[offset] = origL + factor * realstrdE;
    lab[lab_plane + offset] = rt_la_clipC(origA + factor * realstradE);
    lab[2 * lab_plane + offset] = rt_la_clipC(origB + factor * realstrbdE);
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
    const float Chprov = sqrt(anew * anew + bnew * bnew) / 327.68f;

    float2 sincosval;
    sincosval.y = Chprov == 0.0f ? 1.0f : anew / (Chprov * 327.68f);
    sincosval.x = Chprov == 0.0f ? 0.0f : bnew / (Chprov * 327.68f);
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
    rt_lt_lab2xyz(l, a, b, &xyz[0], &xyz[1], &xyz[2]);

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
 * local-adjustments fragments. */
inline std::string local_adjustments_program()
{
    return std::string(RT_LAB_TOOLS_CL) + RT_MUNSELL_LCH_CL +
           RT_LOCAL_CONTRAST_CL + RT_VIBRANCE_CL + RT_LAB_REGIONS_CL +
           RT_LOCAL_ADJUSTMENTS_CL;
}
