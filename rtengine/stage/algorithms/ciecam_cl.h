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

/*
 * The `ciecam` node: CIE-CAM 02 (Ciecam02) as one full-image kernel.
 *
 * The host builds every viewing-condition scalar and the two brightness
 * tables; the kernel reproduces the per-pixel arithmetic of
 * ImProcFunctions::ciecam_02float()'s single-pass, non-EPD path. Only the
 * classical CIECAM02 model (c16 == 1) is admitted here. The forward and
 * inverse transforms, CAT02, HPE, the four algorithm arms, the colourfulness,
 * saturation and hue controls, Color::skinredfloat() and the optional Lab
 * gamut control are ports, not approximations.
 *
 * pow_F() is the sleef chain the host calls. The host's per-pixel loop takes
 * the SIMD form on x86-64, whose xexpf evaluates `1 + (s * s) * u + s`; the
 * scalar grouping rounds differently in the last bit, so this file keeps the
 * vector grouping like local_adjustments_cl.h does.
 *
 * Rec slot order. The host context fills the same table in ciecam_context.cc;
 * the two definitions are asserted against each other by the residency test.
 */
#define RT_CIECAM_REC_C 0
#define RT_CIECAM_REC_NC 1
#define RT_CIECAM_REC_AW 2
#define RT_CIECAM_REC_FL 3
#define RT_CIECAM_REC_WH 4
#define RT_CIECAM_REC_NBB 5
#define RT_CIECAM_REC_NCB 6
#define RT_CIECAM_REC_CZ 7
#define RT_CIECAM_REC_D 8
#define RT_CIECAM_REC_POW1 9
#define RT_CIECAM_REC_POW1N 10
#define RT_CIECAM_REC_COEFQ 11
#define RT_CIECAM_REC_QPRO 12
#define RT_CIECAM_REC_PFL 13
#define RT_CIECAM_REC_PLUM 14
#define RT_CIECAM_REC_XW1 15
#define RT_CIECAM_REC_YW1 16
#define RT_CIECAM_REC_ZW1 17
#define RT_CIECAM_REC_XW2 18
#define RT_CIECAM_REC_YW2 19
#define RT_CIECAM_REC_ZW2 20
#define RT_CIECAM_REC_C2 21
#define RT_CIECAM_REC_NC2 22
#define RT_CIECAM_REC_AWJ 23
#define RT_CIECAM_REC_FLJ 24
#define RT_CIECAM_REC_NBBJ 25
#define RT_CIECAM_REC_NCBJ 26
#define RT_CIECAM_REC_CZJ 27
#define RT_CIECAM_REC_DJ 28
#define RT_CIECAM_REC_RECCMCZ 29
#define RT_CIECAM_REC_COE 30
#define RT_CIECAM_REC_MCHR 31
#define RT_CIECAM_REC_SCHR 32
#define RT_CIECAM_REC_CHR 33
#define RT_CIECAM_REC_HUE 34
#define RT_CIECAM_REC_RSTPROTECT 35
#define RT_CIECAM_REC_GAMU 36
#define RT_CIECAM_REC_EPSIL 37
#define RT_CIECAM_REC_ALG 38
#define RT_CIECAM_REC_HIGHLIGHT 39
#define RT_CIECAM_REC_HUERED 40
#define RT_CIECAM_REC_SCHRRED 41
#define RT_CIECAM_REC_HUEGREEN 42
#define RT_CIECAM_REC_SCHRGREEN 43
#define RT_CIECAM_REC_HUEBLUE 44
#define RT_CIECAM_REC_SCHRBLUE 45
#define RT_CIECAM_REC_COUNT 46

/* The two brightness tables. `curveJfloat()` writes 32768 samples. */
#define RT_CIECAM_TABLE_ENTRIES 32768

/* The host's Lab caches: Color::XYZ2Lab() reads 65536 samples. */
#define RT_CIECAM_CACHE_ENTRIES 65536

inline constexpr const char* RT_CIE_CAM_CL = R"CLC(

/* The rec slots the host fills. Kept beside the C++ list above; the residency
 * test checks the two agree through a full parity run. */
#define RT_CIECAM_REC_C 0
#define RT_CIECAM_REC_NC 1
#define RT_CIECAM_REC_AW 2
#define RT_CIECAM_REC_FL 3
#define RT_CIECAM_REC_WH 4
#define RT_CIECAM_REC_NBB 5
#define RT_CIECAM_REC_NCB 6
#define RT_CIECAM_REC_CZ 7
#define RT_CIECAM_REC_D 8
#define RT_CIECAM_REC_POW1 9
#define RT_CIECAM_REC_POW1N 10
#define RT_CIECAM_REC_COEFQ 11
#define RT_CIECAM_REC_QPRO 12
#define RT_CIECAM_REC_PFL 13
#define RT_CIECAM_REC_PLUM 14
#define RT_CIECAM_REC_XW1 15
#define RT_CIECAM_REC_YW1 16
#define RT_CIECAM_REC_ZW1 17
#define RT_CIECAM_REC_XW2 18
#define RT_CIECAM_REC_YW2 19
#define RT_CIECAM_REC_ZW2 20
#define RT_CIECAM_REC_C2 21
#define RT_CIECAM_REC_NC2 22
#define RT_CIECAM_REC_AWJ 23
#define RT_CIECAM_REC_FLJ 24
#define RT_CIECAM_REC_NBBJ 25
#define RT_CIECAM_REC_NCBJ 26
#define RT_CIECAM_REC_CZJ 27
#define RT_CIECAM_REC_DJ 28
#define RT_CIECAM_REC_RECCMCZ 29
#define RT_CIECAM_REC_COE 30
#define RT_CIECAM_REC_MCHR 31
#define RT_CIECAM_REC_SCHR 32
#define RT_CIECAM_REC_CHR 33
#define RT_CIECAM_REC_HUE 34
#define RT_CIECAM_REC_RSTPROTECT 35
#define RT_CIECAM_REC_GAMU 36
#define RT_CIECAM_REC_EPSIL 37
#define RT_CIECAM_REC_ALG 38
#define RT_CIECAM_REC_HIGHLIGHT 39
#define RT_CIECAM_REC_HUERED 40
#define RT_CIECAM_REC_SCHRRED 41
#define RT_CIECAM_REC_HUEGREEN 42
#define RT_CIECAM_REC_SCHRGREEN 43
#define RT_CIECAM_REC_HUEBLUE 44
#define RT_CIECAM_REC_SCHRBLUE 45
#define RT_CIECAM_TABLE_ENTRIES 32768

/* sleef.h's L2Uf/L2Lf and R_LN2f, the constants pow_F() needs. */
#define RT_CC_L2Uf 0.693145751953125f
#define RT_CC_L2Lf 1.428606765330187045e-06f
#define RT_CC_RLN2 1.442695040888963407359924681001892137426645954152985934135449406931f

/* sleef.h ilogbp1f(): the exponent is one above ilogb(), which keeps the
 * mantissa in [0.707, 1.414) for the polynomial below. */
inline int rt_cc_ilogbp1f(float d)
{
    const int m = d < 5.421010862427522E-20f;
    d = m ? 1.8446744073709552E19f * d : d;
    int q = (as_int(d) >> 23) & 0xff;
    q = m ? q - (64 + 0x7e) : q - 0x7e;
    return q;
}

/* sleef.h ldexpkf(). */
inline float rt_cc_ldexpkf(float x, int q)
{
    int m = q >> 31;
    m = (((m + q) >> 6) - m) << 4;
    q = q - (m << 2);
    float u = as_float(((int)(m + 0x7f)) << 23);
    u = u * u;
    x = x * u * u;
    u = as_float(((int)(q + 0x7f)) << 23);
    return x * u;
}

/* sleef.h xlogf(), the scalar chain the host's pow_F() calls. */
inline float rt_cc_xlogf(const float d)
{
    const int e = rt_cc_ilogbp1f(d * 0.7071f);
    const float m = rt_cc_ldexpkf(d, -e);
    float x = (m - 1.0f) / (m + 1.0f);
    const float x2 = x * x;

    float t = 0.2371599674224853515625f;
    t = t * x2 + 0.285279005765914916992188f;
    t = t * x2 + 0.400005519390106201171875f;
    t = t * x2 + 0.666666567325592041015625f;
    t = t * x2 + 2.0f;

    x = x * t + 0.693147180559945286226764f * (float)e;
    if (isinf(d) && d > 0.0f) {
        x = INFINITY;
    }
    if (d < 0.0f) {
        x = NAN;
    }
    if (d == 0.0f) {
        x = -INFINITY;
    }
    return x;
}

/* opthelper.h pow_F() with the SIMD xexpf grouping the host uses. */
inline float rt_cc_pow(const float a, const float b)
{
    const float d = b * rt_cc_xlogf(a);
    const int q = (int)rint(d * RT_CC_RLN2);
    float s = (float)q * -RT_CC_L2Uf + d;
    s = (float)q * -RT_CC_L2Lf + s;

    float u = 0.00136324646882712841033936f;
    u = u * s + 0.00836596917361021041870117f;
    u = u * s + 0.0416710823774337768554688f;
    u = u * s + 0.166665524244308471679688f;
    u = u * s + 0.499999850988388061523438f;

    u = 1.0f + ((s * s) * u + s);
    return rt_cc_ldexpkf(u, q);
}

/* Ciecam02::xyz_to_cat02float(), c16 == 1. */
inline void rt_cc_xyz_cat02(float* r, float* g, float* b, const float x,
                            const float y, const float z)
{
    *r = 1.007245f * x + 0.011136f * y - 0.018381f * z;
    *g = -0.318061f * x + 1.314589f * y + 0.003471f * z;
    *b = z;
}

/* Ciecam02::cat02_to_hpefloat(), c16 == 1. */
inline void rt_cc_cat02_hpe(float* rh, float* gh, float* bh, const float r,
                            const float g, const float b)
{
    *rh = 0.550930835f * r + 0.519435987f * g - 0.070356303f * b;
    *gh = 0.055954056f * r + 0.89973132f * g + 0.044315524f * b;
    *bh = b;
}

/* Ciecam02::cat02_to_xyzfloat(), c16 == 1. */
inline void rt_cc_cat02_xyz(float* x, float* y, float* z, const float r,
                            const float g, const float b)
{
    *x = 0.99015849f * r - 0.00838772f * g + 0.018229217f * b;
    *y = 0.239565979f * r + 0.758664642f * g + 0.001770137f * b;
    *z = b;
}

/* Ciecam02::hpe_to_xyzfloat(). */
inline void rt_cc_hpe_xyz(float* x, float* y, float* z, const float r,
                          const float g, const float b)
{
    *x = 1.910197f * r - 1.112124f * g + 0.201908f * b;
    *y = 0.370950f * r + 0.629054f * g - 0.000008f * b;
    *z = b;
}

inline float rt_cc_nonlinear(const float c, const float fl)
{
    if (c < 0.0f) {
        const float p = rt_cc_pow((-1.0f * fl * c) / 100.0f, 0.42f);
        return ((-1.0f * 400.0f * p) / (27.13f + p)) + 0.1f;
    }
    const float p = rt_cc_pow((fl * c) / 100.0f, 0.42f);
    return ((400.0f * p) / (27.13f + p)) + 0.1f;
}

inline float rt_cc_inverse_nonlinear(float c, float fl)
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
    return (100.0f / fl) * rt_cc_pow(
                                (27.13f * fabs(c)) / (400.0f - fabs(c)),
                                2.38095238f);
}

inline float rt_cc_sign(const float x)
{
    return (x > 0.0f) - (x < 0.0f);
}

/* Ciecam02::calculate_abfloat(). */
inline void rt_cc_calc_ab(float* aa, float* bb, const float h, const float e,
                          const float t, const float nbb, const float a)
{
    const float2 sincosval = rt_lt_xsincosf((h * RT_LT_PI_F) / 180.0f);
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
    float div = ((e / (t * cosh)) - (-0.31362f - (p3 * 0.15681f)) * c1 -
                 ((0.01924f - (p3 * 4.49038f)) * c2));
    if (rt_cc_sign(div) != rt_cc_sign(cosh) ||
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

/* Ciecam02::Aab_to_rgbfloat(). */
inline void rt_cc_aab_rgb(float* r, float* g, float* b, const float A,
                          const float aa, const float bb, const float nbb)
{
    const float x = (A / nbb) + 0.305f;
    *r = (0.32787f * x) + (0.32145f * aa) + (0.20527f * bb);
    *g = (0.32787f * x) - (0.63507f * aa) - (0.18603f * bb);
    *b = (0.32787f * x) - (0.15681f * aa) - (4.49038f * bb);
}

/* Ciecam02::xyz2jchqms_ciecam02float(), c16 == 1. */
inline void rt_cc_xyz2jchqms(float* J, float* C, float* h, float* Q, float* M,
                             float* s, const float aw, const float fl,
                             const float wh, const float x, const float y,
                             const float z, const float xw, const float yw,
                             const float zw, const float c, const float nc,
                             const float pow1, const float nbb,
                             const float ncb, const float pfl,
                             const float cz, const float d)
{
    float r, g, b;
    float rw, gw, bw;
    rt_cc_xyz_cat02(&r, &g, &b, x, y, z);
    rt_cc_xyz_cat02(&rw, &gw, &bw, xw, yw, zw);
    const float rc = r * (((yw * d) / rw) + (1.0f - d));
    const float gc = g * (((yw * d) / gw) + (1.0f - d));
    const float bc = b * (((yw * d) / bw) + (1.0f - d));

    float rp, gp, bp;
    rt_cc_cat02_hpe(&rp, &gp, &bp, rc, gc, bc);
    rp = fmax(rp, 0.0f);
    gp = fmax(gp, 0.0f);
    bp = fmax(bp, 0.0f);

    const float rpa = rt_cc_nonlinear(rp, fl);
    const float gpa = rt_cc_nonlinear(gp, fl);
    const float bpa = rt_cc_nonlinear(bp, fl);

    const float ca = rpa - ((12.0f * gpa) - bpa) / 11.0f;
    const float cb = (0.11111111f) * (rpa + gpa - (2.0f * bpa));
    float myh = rt_lt_xatan2f(cb, ca);
    if (myh < 0.0f) {
        myh += 2.0f * RT_LT_PI_F;
    }
    float a = ((2.0f * rpa) + gpa + (0.05f * bpa) - 0.305f) * nbb;
    a = fmax(a, 0.0f);
    *J = rt_cc_pow(a / aw, c * cz * 0.5f);
    const float e = 961.53846f * nc * ncb *
                    (rt_lt_xsincosf(myh + 2.0f).y + 3.8f);
    const float t =
        (e * sqrt((ca * ca) + (cb * cb))) / (rpa + gpa + (1.05f * bpa));
    *C = rt_cc_pow(t, 0.9f) * (*J) * pow1;
    *Q = wh * (*J);
    *J *= (*J) * 100.0f;
    *M = (*C) * pfl;
    *Q = fmax(*Q, 0.0001f);
    *s = 100.0f * sqrt((*M) / (*Q));
    *h = (myh * 180.0f) / RT_LT_PI_F;
}

/* Ciecam02::jch2xyz_ciecam02float(), c16 == 1. */
inline void rt_cc_jch2xyz(float* x, float* y, float* z, const float J,
                          const float C, const float h, const float xw,
                          const float yw, const float zw, const float nc,
                          const float pow1, const float nbb, const float ncb,
                          const float fl, const float d, const float aw,
                          const float reccmcz)
{
    float rw, gw, bw;
    rt_cc_xyz_cat02(&rw, &gw, &bw, xw, yw, zw);
    const float e = 961.53846f * nc * ncb *
                    (rt_lt_xsincosf(((h * RT_LT_PI_F) / 180.0f) + 2.0f).y +
                     3.8f);
    const float a = rt_cc_pow(J / 100.0f, reccmcz) * aw;
    const float t = rt_cc_pow((10.0f * C) / (sqrt(J) * pow1), 1.1111111f);

    float ca, cb;
    rt_cc_calc_ab(&ca, &cb, h, e, t, nbb, a);
    float rpa, gpa, bpa;
    rt_cc_aab_rgb(&rpa, &gpa, &bpa, a, ca, cb, nbb);

    const float rp = rt_cc_inverse_nonlinear(rpa, fl);
    const float gp = rt_cc_inverse_nonlinear(gpa, fl);
    const float bp = rt_cc_inverse_nonlinear(bpa, fl);

    float xx, yy, zz;
    rt_cc_hpe_xyz(&xx, &yy, &zz, rp, gp, bp);
    float rc, gc, bc;
    rt_cc_xyz_cat02(&rc, &gc, &bc, xx, yy, zz);
    const float r = rc / (((yw * d) / rw) + (1.0f - d));
    const float g = gc / (((yw * d) / gw) + (1.0f - d));
    const float b = bc / (((yw * d) / bw) + (1.0f - d));
    rt_cc_cat02_xyz(x, y, z, r, g, b);
}

/* Ciecam02::curvecolorfloat(). */
inline float rt_cc_curve_color(const float satind, const float satval,
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
inline void rt_cc_scalered(const float rstprotection, const float param,
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

inline void rt_cc_transitred(const float HH, const float Chprov1,
                             const float dred, const float factorskin,
                             const float protect_red, const float factorskinext,
                             const float deltaHH, const float factorsat,
                             float* factor)
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
inline float rt_cc_skinred(const float J, const float h, const float sres,
                           const float Sp, float dred, float protect_red,
                           const int sk, const float rstprotection,
                           const float ko, const float s)
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
            rt_cc_scalered(rstprotection, chromapro, 0.0f, HH, deltaHH,
                           &scale, &scaleext);
            const float interm = chromapro - 1.0f;
            factorskin = 1.0f + (interm * scale);
            factorskinext = 1.0f + (interm * scaleext);
        } else {
            factorskin = chromapro;
            factorskinext = chromapro;
        }
        float factor = chromapro;
        rt_cc_transitred(HH, result, dred, factorskin, protect_red,
                         factorskinext, deltaHH, chromapro, &factor);
        result *= factor;
    } else {
        result = ko * sres;
    }
    return result;
}

/*
 * Color::gamutLchonly(float2 sincosval, ...): the Lab overload the host calls
 * with lowerCoef 0.15 and higherCoef 0.96, ClipLevel 65535. The host passes
 * the working-space inverse as float and the three planes stay in Lab scale,
 * so the correction needs no double.
 */
inline void rt_cc_gamut_lch(float* L_out, float* a_out, float* b_out,
                            __global const float* wip, const int isHLEnabled)
{
    const float clip_level = 65535.0f;
    const float higher_coef = 0.96f;
    const float lower_coef = 0.15f;
    float Lprov1 = *L_out / 327.68f;
    float Chprov1 = sqrt((*a_out) * (*a_out) + (*b_out) * (*b_out)) / 327.68f;
    float2 sincosval;
    if (Chprov1 == 0.0f) {
        sincosval.y = 1.0f;
        sincosval.x = 0.0f;
    } else {
        sincosval.y = *a_out / (Chprov1 * 327.68f);
        sincosval.x = *b_out / (Chprov1 * 327.68f);
    }
    int inGamut;
    do {
        inGamut = 1;
        const float aprov1 = Chprov1 * sincosval.y;
        const float bprov1 = Chprov1 * sincosval.x;
        const float fy = RT_LT_C1BY116 * Lprov1 + RT_LT_C16BY116;
        const float fx = 0.002f * aprov1 + fy;
        const float fz = fy - 0.005f * bprov1;
        const float x_ = 65535.0f * rt_lt_f2xyz(fx) * RT_LT_D50X;
        const float z_ = 65535.0f * rt_lt_f2xyz(fz) * RT_LT_D50Z;
        const float y_ = (Lprov1 > RT_LT_EPSKAP)
                             ? 65535.0f * fy * fy * fy
                             : 65535.0f * Lprov1 / RT_LT_KAPPA;
        float R, G, B;
        rt_lt_xyz2rgb(x_, y_, z_, wip, &R, &G, &B);
        if (R < 0.0f || G < 0.0f || B < 0.0f) {
            if (Lprov1 < 0.01f) {
                Lprov1 = 0.01f;
            }
            Chprov1 *= higher_coef;
            if (Chprov1 <= 3.0f) {
                Lprov1 += lower_coef;
            }
            inGamut = 0;
        } else if (!isHLEnabled && fmax(R, fmax(G, B)) > clip_level &&
                   fmin(R, fmin(G, B)) <= clip_level) {
            if (Lprov1 > 99.999f) {
                Lprov1 = 99.98f;
            }
            Chprov1 *= higher_coef;
            if (Chprov1 <= 3.0f) {
                Lprov1 -= lower_coef;
            }
            inGamut = 0;
        }
    } while (!inGamut);
    *L_out = Lprov1 * 327.68f;
    *a_out = 327.68f * Chprov1 * sincosval.y;
    *b_out = 327.68f * Chprov1 * sincosval.x;
}

__kernel void rt_ciecam(__global float* lab,
                        __global const float* rec,
                        __global const float* camj,
                        __global const float* camq,
                        __global const float* wip,
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

    float X, Y, Z;
    rt_lt_lab2xyz(lab[offset], lab[plane + offset], lab[2 * plane + offset],
                  &X, &Y, &Z);

    float J, C, h, Q, M, s;
    rt_cc_xyz2jchqms(&J, &C, &h, &Q, &M, &s,
                     rec[RT_CIECAM_REC_AW], rec[RT_CIECAM_REC_FL],
                     rec[RT_CIECAM_REC_WH], X / 655.35f, Y / 655.35f,
                     Z / 655.35f, rec[RT_CIECAM_REC_XW1],
                     rec[RT_CIECAM_REC_YW1], rec[RT_CIECAM_REC_ZW1],
                     rec[RT_CIECAM_REC_C], rec[RT_CIECAM_REC_NC],
                     rec[RT_CIECAM_REC_POW1], rec[RT_CIECAM_REC_NBB],
                     rec[RT_CIECAM_REC_NCB], rec[RT_CIECAM_REC_PFL],
                     rec[RT_CIECAM_REC_CZ], rec[RT_CIECAM_REC_D]);

    float Jpro = J;
    float Cpro = C;
    float hpro = h;
    float Qpro = Q;
    float Mpro = M;
    float spro = s;

    /* The six user colour curves are admitted only at their identity, so the
     * per-hue brightness arms reduce to the plain hue/saturation rotation. */
    if ((hpro > 340.0f && hpro <= 360.0f) ||
        (hpro > 0.0f && hpro <= 100.0f)) {
        hpro = hpro + 0.555f * rec[RT_CIECAM_REC_HUERED];
        spro = spro * (1.0f + rec[RT_CIECAM_REC_SCHRRED] / 100.0f);
        const float Cp = (spro * spro * Qpro) / 1000000.0f;
        Mpro = (spro * spro * Qpro) / 10000.0f;
        Cpro = Cp * 100.0f;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }
    if (hpro > 100.0f && hpro <= 190.0f) {
        hpro = hpro + 0.555f * rec[RT_CIECAM_REC_HUEGREEN];
        spro = spro * (1.0f + rec[RT_CIECAM_REC_SCHRGREEN] / 100.0f);
        const float Cp = (spro * spro * Qpro) / 1000000.0f;
        Mpro = (spro * spro * Qpro) / 10000.0f;
        Cpro = Cp * 100.0f;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }
    if (hpro > 190.0f && hpro <= 340.0f) {
        hpro = hpro + 0.555f * rec[RT_CIECAM_REC_HUEBLUE];
        spro = spro * (1.0f + rec[RT_CIECAM_REC_SCHRBLUE] / 100.0f);
        const float Cp = (spro * spro * Qpro) / 1000000.0f;
        Mpro = (spro * spro * Qpro) / 10000.0f;
        Cpro = Cp * 100.0f;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }

    const float mchr = rec[RT_CIECAM_REC_MCHR];
    const float schr = rec[RT_CIECAM_REC_SCHR];
    const float chr = rec[RT_CIECAM_REC_CHR];
    const float hue = rec[RT_CIECAM_REC_HUE];
    const float rstprotect = rec[RT_CIECAM_REC_RSTPROTECT];
    const float coe = rec[RT_CIECAM_REC_COE];
    const float qpro_factor = rec[RT_CIECAM_REC_QPRO];
    const float coefq = rec[RT_CIECAM_REC_COEFQ];
    const float wh = rec[RT_CIECAM_REC_WH];
    const float epsil = rec[RT_CIECAM_REC_EPSIL];
    const int alg = (int)rec[RT_CIECAM_REC_ALG];

    if (alg == 0) {
        Jpro = rt_lt_lut(camj, RT_CIECAM_TABLE_ENTRIES, Jpro * 327.68f);
        Qpro = qpro_factor * sqrt(Jpro);
        const float Cp = (spro * spro * Qpro) / 1000000.0f;
        Cpro = Cp * 100.0f;
        const float sres = rt_cc_curve_color(chr, Cp, 1.8f);
        Cpro = rt_cc_skinred(Jpro, hpro, sres, Cp, 55.0f, 30.0f, 1,
                             rstprotect, 100.0f, Cpro);
        hpro = hpro + hue;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    } else if (alg == 1) {
        Jpro = rt_lt_lut(camj, RT_CIECAM_TABLE_ENTRIES, Jpro * 327.68f);
        const float Sp = spro / 100.0f;
        const float sres = rt_cc_curve_color(schr, Sp, 1.5f);
        const float dred = 100.0f * sqrt((100.0f * coe) / Qpro);
        const float protect_red = 100.0f * sqrt((80.0f * coe) / Qpro);
        spro = rt_cc_skinred(Jpro, hpro, sres, Sp, dred, protect_red, 0,
                             rstprotect, 100.0f, spro);
        Qpro = qpro_factor * sqrt(Jpro);
        Cpro = (spro * spro * Qpro) / (10000.0f);
        hpro = hpro + hue;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    } else if (alg == 2) {
        Qpro = rt_lt_lut(camq, RT_CIECAM_TABLE_ENTRIES, Qpro * coefq) / coefq;
        const float Mp = Mpro / 100.0f;
        const float sres = rt_cc_curve_color(mchr, Mp, 2.5f);
        Mpro = rt_cc_skinred(Jpro, hpro, sres, Mp, 100.0f * coe, 80.0f * coe,
                             0, rstprotect, 100.0f, Mpro);
        Jpro = (10.0f * Qpro) / wh;
        Jpro *= Jpro;
        Cpro = Mpro / coe;
        Qpro = (Qpro == 0.0f ? epsil : Qpro);
        spro = 100.0f * sqrt(Mpro / Qpro);
        hpro = hpro + hue;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    } else {
        Qpro = rt_lt_lut(camq, RT_CIECAM_TABLE_ENTRIES, Qpro * coefq) / coefq;
        const float Mp = Mpro / 100.0f;
        const float sres = rt_cc_curve_color(mchr, Mp, 2.5f);
        Mpro = rt_cc_skinred(Jpro, hpro, sres, Mp, 100.0f * coe, 80.0f * coe,
                             0, rstprotect, 100.0f, Mpro);
        Jpro = (10.0f * Qpro) / wh;
        Jpro *= Jpro;
        Cpro = Mpro / coe;
        Qpro = (Qpro == 0.0f ? epsil : Qpro);
        spro = 100.0f * sqrt(Mpro / Qpro);
        Jpro = rt_lt_lut(camj, RT_CIECAM_TABLE_ENTRIES, Jpro * 327.68f);
        const float Sp = spro / 100.0f;
        const float sres2 = rt_cc_curve_color(schr, Sp, 1.5f);
        spro = rt_cc_skinred(
            Jpro, hpro, sres2, Sp, 100.0f * sqrt((100.0f * coe) / Q),
            100.0f * sqrt((80.0f * coe) / Q), 0, rstprotect, 100.0f, spro);
        Qpro = qpro_factor * sqrt(Jpro);
        const float Cp = (spro * spro * Qpro) / 1000000.0f;
        Cpro = Cp * 100.0f;
        const float sres3 = rt_cc_curve_color(chr, Cp, 1.8f);
        Cpro = rt_cc_skinred(Jpro, hpro, sres3, Cp, 55.0f, 30.0f, 1,
                             rstprotect, 100.0f, Cpro);
        hpro = hpro + hue;
        if (hpro < 0.0f) {
            hpro += 360.0f;
        }
    }

    float xx, yy, zz;
    rt_cc_jch2xyz(&xx, &yy, &zz, Jpro, Cpro, hpro,
                  rec[RT_CIECAM_REC_XW2], rec[RT_CIECAM_REC_YW2],
                  rec[RT_CIECAM_REC_ZW2], rec[RT_CIECAM_REC_NC2],
                  rec[RT_CIECAM_REC_POW1N], rec[RT_CIECAM_REC_NBBJ],
                  rec[RT_CIECAM_REC_NCBJ], rec[RT_CIECAM_REC_FLJ],
                  rec[RT_CIECAM_REC_DJ], rec[RT_CIECAM_REC_AWJ],
                  rec[RT_CIECAM_REC_RECCMCZ]);

    const float epsilon = 0.00001f;
    const float xb = fmax(epsilon, xx * 655.35f);
    const float yb = fmax(epsilon, yy * 655.35f);
    const float zb = fmax(epsilon, zz * 655.35f);
    float Ll, aa, bb;
    rt_lt_xyz2lab(xb, yb, zb, cachef, cachefy, &Ll, &aa, &bb);

    if (rec[RT_CIECAM_REC_GAMU] != 0.0f) {
        rt_cc_gamut_lch(&Ll, &aa, &bb, wip,
                        (int)rec[RT_CIECAM_REC_HIGHLIGHT]);
    }

    lab[offset] = Ll;
    lab[plane + offset] = aa;
    lab[2 * plane + offset] = bb;
}

)CLC";
