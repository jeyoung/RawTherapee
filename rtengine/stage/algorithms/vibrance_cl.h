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
 * ImProcFunctions::vibrance() (rtengine/ipvibrance.cc) as a device kernel.
 *
 * This fragment is concatenated after lab_tools_cl.h and munsell_lch_cl.h, so
 * it uses their LUT helper, constants and the Munsell correction. It is a
 * port, not an approximation:
 *
 *  - the pastels/saturated transition is the host's double linear
 *    interpolation, including chromaMean and the transition weighting;
 *  - the two-stage chroma ramp and the Luminance/Hue sathue and sathue2
 *    tables are the host's, branch for branch;
 *  - Color::gamutLchonly() is the saturation overload vibrance() calls, with
 *    its lower/higher coefficients of 0.15 and 0.98 and its highlight gate;
 *  - Color::SkinSat() is the host's cascade of skin regions;
 *  - the hue skin curve is a host-built LUTf, uploaded as 65536 floats, and
 *    the LUTf float-index interpolation is rt_lt_lut();
 *  - the avoidcolorshift path calls Color::AllMunsellLch() on the same flat
 *    table set the host reads.
 *
 * xatan2f() and xsincosf() are the sleef implementations the host calls. The
 * host build uses the SSE2 path on x86-64; the scalar and vector paths are the
 * same constants and the same operation order, and both round the quadrant
 * with round-to-nearest-even, which is rint() here.
 */
inline constexpr const char* RT_VIBRANCE_CL = R"CLC(

/* rt_math.h's RT_PI_F/2 and RT_PI_F*.25f, evaluated as the host does. */
#define RT_LT_PI_F 3.1415927f
#define RT_LT_PI_F_2 (RT_LT_PI_F / 2.0f)
#define RT_LT_PI_F_4 (RT_LT_PI_F * 0.25f)
/* RT_2_PI_F: float(2 / pi). */
#define RT_LT_2_PI_F 0.63661977236758134308f

/* sleef.h mulsignf() and signf(). */
inline float rt_lt_mulsignf(const float x, const float y)
{
    return as_float(as_int(x) ^ (as_int(y) & (int)0x80000000));
}

inline float rt_lt_signf(const float d)
{
    return copysign(1.0f, d);
}

/* sleef.h xfabsf(). */
inline float rt_lt_xfabsf(const float x)
{
    return as_float(0x7fffffff & as_int(x));
}

/* sleef.h atan2kf(). */
inline float rt_lt_atan2kf(float y, float x)
{
    float s, t, u;
    float q = 0.0f;

    if (x < 0.0f) {
        x = -x;
        q = -2.0f;
    }
    if (y > x) {
        t = x;
        x = y;
        y = -t;
        q += 1.0f;
    }

    s = y / x;
    t = s * s;

    u = 0.00282363896258175373077393f;
    u = u * t + -0.0159569028764963150024414f;
    u = u * t + 0.0425049886107444763183594f;
    u = u * t + -0.0748900920152664184570312f;
    u = u * t + 0.106347933411598205566406f;
    u = u * t + -0.142027363181114196777344f;
    u = u * t + 0.199926957488059997558594f;
    u = u * t + -0.333331018686294555664062f;

    t = u * t;
    t = t * s + s;
    return q * RT_LT_PI_F_2 + t;
}

/* sleef.h xatan2f(). */
inline float rt_lt_xatan2f(const float y, const float x)
{
    float r = rt_lt_atan2kf(rt_lt_xfabsf(y), x);

    r = rt_lt_mulsignf(r, x);
    if (isinf(x) || x == 0.0f) {
        r = RT_LT_PI_F_2 -
            (isinf(x) ? (rt_lt_signf(x) * RT_LT_PI_F_2) : 0.0f);
    }
    if (isinf(y)) {
        r = RT_LT_PI_F_2 -
            (isinf(x) ? (rt_lt_signf(x) * RT_LT_PI_F_4) : 0.0f);
    }
    if (y == 0.0f) {
        r = (rt_lt_signf(x) == -1.0f) ? RT_LT_PI_F : 0.0f;
    }

    return (isnan(x) || isnan(y)) ? NAN : rt_lt_mulsignf(r, y);
}

/* sleef.h xsincosf(): r.x = sin(d), r.y = cos(d). */
inline float2 rt_lt_xsincosf(const float d)
{
    const int q = (int)rint(d * RT_LT_2_PI_F);

    float s = d;
    s = q * -1.5703125f + s;
    s = q * -0.0004825592041015625f + s;
    s = q * -1.26659870147705078124e-06f + s;
    s = q * -9.9209362947050294678e-10f + s;

    const float t = s;
    s = s * s;

    float u = -0.000195169282960705459117889f;
    u = u * s + 0.00833215750753879547119141f;
    u = u * s + -0.166666537523269653320312f;
    u = u * s * t;

    float rx = t + u;

    u = -2.71811842367242206819355e-07f;
    u = u * s + 2.47990446951007470488548e-05f;
    u = u * s + -0.00138888787478208541870117f;
    u = u * s + 0.0416666641831398010253906f;
    u = u * s + -0.5f;

    float ry = u * s + 1.0f;

    if ((q & 1) != 0) {
        const float swap = rx;
        rx = ry;
        ry = swap;
    }
    if ((q & 2) != 0) {
        rx = -rx;
    }
    if (((q + 1) & 2) != 0) {
        ry = -ry;
    }

    if (isinf(d)) {
        rx = NAN;
        ry = NAN;
    }

    return (float2)(rx, ry);
}

/* Color::f2xyz(float). */
inline float rt_lt_f2xyz(const float f)
{
    return (f > (6.0f / 29.0f)) ? f * f * f
                               : (116.0f * f - 16.0f) * (27.0f / 24389.0f);
}

/* Color::xyz2rgb() with the working-space inverse matrix. */
inline void rt_lt_xyz2rgb_flat(const float x, const float y, const float z,
                               __global const float* m,
                               float* r, float* g, float* b)
{
    *r = m[0] * x + m[1] * y + m[2] * z;
    *g = m[3] * x + m[4] * y + m[5] * z;
    *b = m[6] * x + m[7] * y + m[8] * z;
}

/*
 * Color::gamutLchonly(HH, sincosval, Lprov1, Chprov1, saturation, wip,
 * isHLEnabled, lowerCoef, higherCoef): the overload vibrance() calls, with
 * ClipLevel 1.0. Returns `saturation` and writes the corrected L and C back.
 */
inline float rt_lt_gamut_lchonly(const float HH, const float2 sincosval,
                                 float* Lprov1_out, float* Chprov1_out,
                                 __global const float* wip,
                                 const int isHLEnabled,
                                 const float lowerCoef,
                                 const float higherCoef)
{
    float Lprov1 = *Lprov1_out;
    float Chprov1 = *Chprov1_out;
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    int inGamut;

    do {
        inGamut = 1;

        const float aprov1 = Chprov1 * sincosval.y;
        const float bprov1 = Chprov1 * sincosval.x;

        const float fy = RT_LT_C1BY116 * Lprov1 + RT_LT_C16BY116;
        const float fx = 0.002f * aprov1 + fy;
        const float fz = fy - 0.005f * bprov1;

        const float x_ = rt_lt_f2xyz(fx) * RT_LT_D50X;
        const float z_ = rt_lt_f2xyz(fz) * RT_LT_D50Z;
        const float y_ = (Lprov1 > RT_LT_EPSKAP) ? fy * fy * fy
                                                 : Lprov1 / RT_LT_KAPPA;

        rt_lt_xyz2rgb_flat(x_, y_, z_, wip, &R, &G, &B);

        if (fmin(R, fmin(G, B)) < 0.0f) {
            Lprov1 = fmax(Lprov1, 0.1f);

            /* Gamut for L with ultra blue. */
            if (HH < -0.9f && HH > -1.55f) {
                if (Chprov1 > 160.0f && Lprov1 < 5.0f) {
                    Lprov1 = 5.0f;
                }
                if (Chprov1 > 140.0f && Lprov1 < 3.5f) {
                    Lprov1 = 3.5f;
                }
                if (Chprov1 > 120.0f && Lprov1 < 2.0f) {
                    Lprov1 = 2.0f;
                }
                if (Chprov1 > 105.0f && Lprov1 < 1.0f) {
                    Lprov1 = 1.0f;
                }
                if (Chprov1 > 90.0f && Lprov1 < 0.7f) {
                    Lprov1 = 0.7f;
                }
                if (Chprov1 > 50.0f && Lprov1 < 0.5f) {
                    Lprov1 = 0.5f;
                }
                if (Chprov1 > 20.0f && Lprov1 < 0.4f) {
                    Lprov1 = 0.4f;
                }
            }

            Chprov1 *= higherCoef;

            if (Chprov1 <= 3.0f) {
                Lprov1 += lowerCoef;
            }

            inGamut = 0;
        } else if (!isHLEnabled && fmax(R, fmax(G, B)) > 1.0f &&
                   fmin(R, fmin(G, B)) <= 1.0f) {
            if (Lprov1 > 99.999f) {
                Lprov1 = 99.98f;
            }

            Chprov1 *= higherCoef;

            if (Chprov1 <= 3.0f) {
                Lprov1 -= lowerCoef;
            }

            inGamut = 0;
        }
    } while (!inGamut);

    *Lprov1_out = Lprov1;
    *Chprov1_out = Chprov1;
    return 1.0f - (fmin(R, fmin(G, B)) / fmax(R, fmax(G, B)));
}

/*
 * Color::SkinSat(): the host's skin-region cascade. `satreduc` starts at 1
 * and keeps the host's value when no region matches.
 */
inline float rt_lt_skin_sat(const float lum, const float hue,
                            const float chrom)
{
    const float reduction = 0.3f;
    const float extendedreduction = 0.4f;
    const float extendedreduction2 = 0.6f;

    const float C9 = 8.f, C8 = 15.f, C7 = 12.f, C4 = 7.f, C3 = 5.f,
                C2 = 5.f, C1 = 5.f;
    const float H9 = 0.05f, H8 = 0.25f, H7 = 0.1f, H4 = 0.02f, H3 = 0.02f,
                H2 = 0.1f, H1 = 0.1f, H10 = -0.2f, H11 = -0.2f;

    float satreduc = 1.0f;

    if (lum >= 85.f) {
        if ((hue > (0.78f - H9) && hue < (1.18f + H9)) &&
            (chrom > 8.f && chrom < (14.f + C9))) {
            satreduc = reduction;
        } else if (lum >= 92.f) {
            if ((hue > 0.8f && hue < 1.65f) && (chrom > 7.f && chrom < 15.f)) {
                satreduc = extendedreduction;
            } else if ((hue > -0.1f && hue < 1.65f) &&
                       (chrom > 7.f && chrom < 18.f)) {
                satreduc = extendedreduction2;
            }
        } else if ((hue > 0.7f && hue < 1.4f) &&
                   (chrom > 7.f && chrom < (26.f + C9))) {
            satreduc = extendedreduction;
        } else if (lum < 92.f && (hue > 0.f && hue < 1.65f) &&
                   (chrom > 7.f && chrom < (35.f + C9))) {
            satreduc = extendedreduction2;
        }
    } else if (lum >= 70.f) {
        if ((hue > 0.4f && hue < (1.04f + H8)) &&
            (chrom > 8.f && chrom < (35.f + C8))) {
            satreduc = reduction;
        } else if ((hue > (0.02f + H11) && hue < 1.5f) &&
                   (chrom > 7.0f && chrom < (48.f + C9))) {
            satreduc = extendedreduction;
        } else if ((hue > (0.02f + H11) && hue < 1.65f) &&
                   (chrom > 7.f && chrom < (55.f + C9))) {
            satreduc = extendedreduction2;
        }
    } else if (lum >= 52.f) {
        if ((hue > 0.3f && hue < (1.27f + H7)) &&
            (chrom > 11.f && chrom < (35.f + C7))) {
            satreduc = reduction;
        } else if ((hue > (0.02f + H11) && hue < 1.5f) &&
                   (chrom > 7.0f && chrom < (48.f + C9))) {
            satreduc = extendedreduction;
        } else if ((hue > (0.02f + H11) && hue < 1.65f) &&
                   (chrom > 7.f && chrom < (55.f + C9))) {
            satreduc = extendedreduction2;
        }
    } else if (lum >= 35.f) {
        if ((hue > 0.3f && hue < (1.25f + H4)) &&
            (chrom > 13.f && chrom < (37.f + C4))) {
            satreduc = reduction;
        } else if ((hue > (0.02f + H11) && hue < 1.5f) &&
                   (chrom > 7.0f && chrom < (48.f + C9))) {
            satreduc = extendedreduction;
        } else if ((hue > (0.02f + H11) && hue < 1.65f) &&
                   (chrom > 7.f && chrom < (55.f + C9))) {
            satreduc = extendedreduction2;
        }
    } else if (lum >= 20.f) {
        if ((hue > 0.3f && hue < (1.2f + H3)) &&
            (chrom > 7.f && chrom < (35.f + C3))) {
            satreduc = reduction;
        } else if ((hue > (0.02f + H11) && hue < 1.5f) &&
                   (chrom > 7.0f && chrom < (48.f + C9))) {
            satreduc = extendedreduction;
        } else if ((hue > (0.02f + H11) && hue < 1.65f) &&
                   (chrom > 7.f && chrom < (55.f + C9))) {
            satreduc = extendedreduction2;
        }
    } else if (lum > 10.f) {
        if ((hue > (0.f + H10) && hue < (0.95f + H2)) &&
            (chrom > 8.f && chrom < (23.f + C2))) {
            satreduc = reduction;
        } else if ((hue > (0.02f + H11) && hue < 1.f) &&
                   (chrom > 7.f && chrom < (35.f + C1))) {
            satreduc = extendedreduction;
        } else if ((hue > (0.02f + H11) && hue < 1.6f) &&
                   (chrom > 7.f && chrom < (45.f + C1))) {
            satreduc = extendedreduction2;
        }
    } else {
        if ((hue > (0.02f + H10) && hue < (0.9f + H1)) &&
            (chrom > 8.f && chrom < (23.f + C1))) {
            satreduc = reduction;
        } else if ((hue > (0.02f + H11) && hue < 1.f) &&
                   (chrom > 7.f && chrom < (35.f + C1))) {
            satreduc = extendedreduction;
        } else if ((hue > (0.02f + H11) && hue < 1.6f) &&
                   (chrom > 7.f && chrom < (45.f + C1))) {
            satreduc = extendedreduction2;
        }
    }

    return satreduc;
}

/*
 * ImProcFunctions::vibrance(). One work item per pixel; `lab` is read and
 * written in place. `wip` is the working-space inverse matrix, `skin_curve`
 * the host-built 65536-entry hue curve (read only when skin_curve_set), and
 * `munsell` / `munsell_sizes` the flat Munsell table set.
 */
__kernel void rt_lab_tools_vibrance(__global float* lab,
                                    const int width, const int height,
                                    const int lab_pitch, const int lab_plane,
                                    __global const float* wip,
                                    __global const float* skin_curve,
                                    const int skin_curve_set,
                                    __global const float* munsell,
                                    __global const float* munsell_sizes,
                                    const int pastels, const int saturated,
                                    const int threshold_top,
                                    const int threshold_bottom,
                                    const int protectskins,
                                    const int avoidcolorshift, const int highlight)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x >= width || y >= height) {
        return;
    }

    const int offset = y * lab_pitch + x;

    /* ipvibrance.cc's constants and derived thresholds. */
    const float chromaPastel = (float)pastels / 100.f;
    const float chromaSatur = (float)saturated / 100.f;
    const float p00 = 0.07f;
    const float limitpastelsatur =
        ((float)threshold_top / 100.f) * (1.f - p00) + p00;
    const float maxdp = (limitpastelsatur - p00) / 4.f;
    const float maxds = (1.f - limitpastelsatur) / 4.f;
    const float p0 = p00 + maxdp;
    const float p1 = p00 + 2.f * maxdp;
    const float p2 = p00 + 3.f * maxdp;
    const float s0 = limitpastelsatur + maxds;
    const float s1 = limitpastelsatur + 2.f * maxds;
    const float s2 = limitpastelsatur + 3.f * maxds;
    const float transitionweighting = (float)threshold_bottom / 100.f;

    float chromamean = 0.f;

    if (chromaPastel != chromaSatur) {
        chromamean = maxdp * (chromaSatur - chromaPastel) / (s0 - p2) +
                     chromaPastel;

        if (transitionweighting > 0.f) {
            chromamean =
                (chromaSatur - chromamean) * transitionweighting + chromamean;
        } else if (transitionweighting < 0.f) {
            chromamean =
                (chromamean - chromaPastel) * transitionweighting + chromamean;
        }
    }

    const float chromaPastel_a =
        (chromaPastel - chromamean) / (p2 - limitpastelsatur);
    const float chromaPastel_b = chromaPastel - chromaPastel_a * p2;

    const float chromaSatur_a =
        (chromaSatur - chromamean) / (s0 - limitpastelsatur);
    const float chromaSatur_b = chromaSatur - chromaSatur_a * s0;

    const float dhue = 0.15f;
    const float dchr = 20.f;
    const float skbeg = -0.05f;
    const float skend = 1.60f;
    const float xx = 0.5f;
    const float ask = 65535.f / (skend - skbeg);
    const float bsk0 = -skbeg;
    const float bsk = -skbeg * ask;

    const float LL = lab[offset] / 327.68f;
    const float a_lab = lab[lab_plane + offset];
    const float b_lab = lab[2 * lab_plane + offset];
    float HH = rt_lt_xatan2f(b_lab, a_lab);
    const float CC = sqrt(a_lab * a_lab + b_lab * b_lab) / 327.68f;

    float Lprov = LL;
    float Chprov = CC;
    float2 sincosval;

    if (CC == 0.f) {
        sincosval.y = 1.f;
        sincosval.x = 0.f;
    } else {
        sincosval.y = a_lab / (CC * 327.68f);
        sincosval.x = b_lab / (CC * 327.68f);
    }

    const float saturation = rt_lt_gamut_lchonly(
        HH, sincosval, &Lprov, &Chprov, wip, highlight, 0.15f, 0.98f);

    if (Chprov > 6.f) {
        float satredu = 1.f;

        if (protectskins != 0) {
            satredu = rt_lt_skin_sat(LL, HH, CC);
        }

        if (saturation > 0.f) {
            /*
             * All four tables are fully written by every branch of the host
             * cascade; the initialisers only keep a host path that reads an
             * unwritten entry from becoming device-defined.
             */
            float sathue[5] = {1.f, 1.f, 1.f, 1.f, 1.f};
            float sathue2[4] = {1.f, 1.f, 1.f, 1.f};

            if (satredu != 1.f) {
                /* For skin, no differentiation. */
                sathue[0] = sathue[1] = sathue[2] = sathue[3] = sathue[4] =
                    1.f;
                sathue2[0] = sathue2[1] = sathue2[2] = sathue2[3] = 1.f;
            } else {
                /* The host's double pyramid: LL and HH. */
                if (LL < 20.0f) {
                    sathue[4] = 0.4f;
                    sathue2[3] = 1.f;
                    if (HH < -1.5f) {
                        sathue[0] = 1.3f;
                        sathue[1] = 1.2f;
                        sathue[2] = 1.1f;
                        sathue[3] = 1.05f;
                        sathue2[0] = 1.05f;
                        sathue2[1] = 1.1f;
                        sathue2[2] = 1.05f;
                    } else if (HH < -0.7f) {
                        sathue[0] = 1.6f;
                        sathue[1] = 1.4f;
                        sathue[2] = 1.3f;
                        sathue[3] = 1.2f;
                        sathue2[0] = 1.2f;
                        sathue2[1] = 1.15f;
                        sathue2[2] = 1.1f;
                    } else if (HH < 0.0f) {
                        sathue[0] = 1.2f;
                        sathue[1] = 1.0f;
                        sathue[2] = 1.0f;
                        sathue[3] = 1.0f;
                        sathue2[0] = 1.0f;
                        sathue2[1] = 1.0f;
                        sathue2[2] = 1.0f;
                    } else if (HH <= 1.4f) {
                        sathue[0] = 1.3f;
                        sathue[1] = 1.2f;
                        sathue[2] = 1.1f;
                        sathue[3] = 1.0f;
                        sathue2[0] = 1.0f;
                        sathue2[1] = 1.0f;
                        sathue2[2] = 1.0f;
                    } else if (HH <= 2.1f) {
                        sathue[0] = 1.0f;
                        sathue[1] = 1.0f;
                        sathue[2] = 1.0f;
                        sathue[3] = 1.0f;
                        sathue2[0] = 1.0f;
                        sathue2[1] = 1.0f;
                        sathue2[2] = 1.0f;
                    } else {
                        sathue[0] = 1.4f;
                        sathue[1] = 1.3f;
                        sathue[2] = 1.2f;
                        sathue[3] = 1.15f;
                        sathue2[0] = 1.15f;
                        sathue2[1] = 1.1f;
                        sathue2[2] = 1.05f;
                    }
                } else if (LL < 50.0f) {
                    sathue[4] = 0.4f;
                    if (HH < -1.5f) {
                        sathue[0] = 1.5f;
                        sathue[1] = 1.4f;
                        sathue[2] = 1.3f;
                        sathue[3] = 1.2f;
                        sathue2[0] = 1.2f;
                        sathue2[1] = 1.1f;
                        sathue2[2] = 1.05f;
                        sathue2[3] = 1.0f;
                    } else if (HH < -0.7f) {
                        sathue[0] = 1.3f;
                        sathue[1] = 1.2f;
                        sathue[2] = 1.1f;
                        sathue[3] = 1.05f;
                        sathue2[0] = 1.05f;
                        sathue2[1] = 1.05f;
                        sathue2[2] = 1.0f;
                        sathue2[3] = 1.0f;
                    } else if (HH < 0.0f) {
                        sathue[0] = 1.2f;
                        sathue[1] = 1.0f;
                        sathue[2] = 1.0f;
                        sathue[3] = 1.0f;
                        sathue2[0] = 1.0f;
                        sathue2[1] = 1.0f;
                        sathue2[2] = 1.0f;
                        sathue2[3] = 1.0f;
                    } else if (HH <= 1.4f) {
                        sathue[0] = 1.1f;
                        sathue[1] = 1.0f;
                        sathue[2] = 0.9f;
                        sathue[3] = 0.8f;
                        sathue2[0] = 0.8f;
                        sathue2[1] = 0.8f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.8f;
                    } else if (HH <= 2.1f) {
                        sathue[0] = 1.1f;
                        sathue[1] = 1.1f;
                        sathue[2] = 1.1f;
                        sathue[3] = 1.05f;
                        sathue2[0] = 0.9f;
                        sathue2[1] = 0.8f;
                        sathue2[2] = 0.7f;
                        sathue2[3] = 0.6f;
                    } else {
                        sathue[0] = 1.5f;
                        sathue[1] = 1.4f;
                        sathue[2] = 1.3f;
                        sathue[3] = 1.2f;
                        sathue2[0] = 1.2f;
                        sathue2[1] = 1.1f;
                        sathue2[2] = 1.05f;
                        sathue2[3] = 1.0f;
                    }
                } else if (LL < 80.0f) {
                    sathue[4] = 0.3f;
                    if (HH < -1.5f) {
                        sathue[0] = 1.3f;
                        sathue[1] = 1.2f;
                        sathue[2] = 1.15f;
                        sathue[3] = 1.1f;
                        sathue2[0] = 1.1f;
                        sathue2[1] = 1.1f;
                        sathue2[2] = 1.05f;
                        sathue2[3] = 1.0f;
                    } else if (HH < -0.7f) {
                        sathue[0] = 1.3f;
                        sathue[1] = 1.2f;
                        sathue[2] = 1.15f;
                        sathue[3] = 1.1f;
                        sathue2[0] = 1.1f;
                        sathue2[1] = 1.05f;
                        sathue2[2] = 1.0f;
                        sathue2[3] = 1.0f;
                    } else if (HH < 0.0f) {
                        sathue[0] = 1.2f;
                        sathue[1] = 1.0f;
                        sathue[2] = 1.0f;
                        sathue[3] = 1.0f;
                        sathue2[0] = 1.0f;
                        sathue2[1] = 1.0f;
                        sathue2[2] = 1.0f;
                        sathue2[3] = 1.0f;
                    } else if (HH <= 1.4f) {
                        sathue[0] = 1.1f;
                        sathue[1] = 1.0f;
                        sathue[2] = 0.9f;
                        sathue[3] = 0.8f;
                        sathue2[0] = 0.8f;
                        sathue2[1] = 0.8f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.8f;
                    } else if (HH <= 2.1f) {
                        sathue[0] = 1.3f;
                        sathue[1] = 1.2f;
                        sathue[2] = 1.1f;
                        sathue[3] = 1.05f;
                        sathue2[0] = 1.0f;
                        sathue2[1] = 0.9f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.7f;
                    } else {
                        sathue[0] = 1.6f;
                        sathue[1] = 1.4f;
                        sathue[2] = 1.3f;
                        sathue[3] = 1.25f;
                        sathue2[0] = 1.25f;
                        sathue2[1] = 1.2f;
                        sathue2[2] = 1.15f;
                        sathue2[3] = 1.05f;
                    }
                } else {
                    sathue[4] = 0.2f;
                    if (HH < -1.5f) {
                        sathue[0] = 1.0f;
                        sathue[1] = 1.0f;
                        sathue[2] = 0.9f;
                        sathue[3] = 0.8f;
                        sathue2[0] = 0.8f;
                        sathue2[1] = 0.8f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.8f;
                    } else if (HH < -0.7f) {
                        sathue[0] = 1.0f;
                        sathue[1] = 1.0f;
                        sathue[2] = 0.9f;
                        sathue[3] = 0.8f;
                        sathue2[0] = 0.8f;
                        sathue2[1] = 0.8f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.8f;
                    } else if (HH < 0.0f) {
                        sathue[0] = 1.2f;
                        sathue[1] = 1.0f;
                        sathue[2] = 1.0f;
                        sathue[3] = 0.9f;
                        sathue2[0] = 0.9f;
                        sathue2[1] = 0.9f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.8f;
                    } else if (HH <= 1.4f) {
                        sathue[0] = 1.1f;
                        sathue[1] = 1.0f;
                        sathue[2] = 0.9f;
                        sathue[3] = 0.8f;
                        sathue2[0] = 0.8f;
                        sathue2[1] = 0.8f;
                        sathue2[2] = 0.8f;
                        sathue2[3] = 0.8f;
                    } else if (HH <= 2.1f) {
                        sathue[0] = 1.6f;
                        sathue[1] = 1.5f;
                        sathue[2] = 1.4f;
                        sathue[3] = 1.2f;
                        sathue2[0] = 1.1f;
                        sathue2[1] = 1.05f;
                        sathue2[2] = 1.0f;
                        sathue2[3] = 1.0f;
                    } else {
                        sathue[0] = 1.4f;
                        sathue[1] = 1.3f;
                        sathue[2] = 1.2f;
                        sathue[3] = 1.1f;
                        sathue2[0] = 1.1f;
                        sathue2[1] = 1.05f;
                        sathue2[2] = 1.05f;
                        sathue2[3] = 1.0f;
                    }
                }
            }

            float chmod = 0.f;
            float pa, pb;

            if (saturation < p00) {
                pa = 0.f;
                pb = chromaPastel * sathue[4];
            } else if (saturation < p0) {
                const float chl00 = chromaPastel * sathue[4];
                const float chl0 = chromaPastel * sathue[0];
                pa = (chl00 - chl0) / (p00 - p0);
                pb = chl00 - pa * p00;
            } else if (saturation < p1) {
                const float chl0 = chromaPastel * sathue[0];
                const float chl1 = chromaPastel * sathue[1];
                pa = (chl0 - chl1) / (p0 - p1);
                pb = chl0 - pa * p0;
            } else if (saturation < p2) {
                const float chl1 = chromaPastel * sathue[1];
                const float chl2 = chromaPastel * sathue[2];
                pa = (chl1 - chl2) / (p1 - p2);
                pb = chl1 - pa * p1;
            } else if (saturation < limitpastelsatur) {
                const float chl2 = chromaPastel * sathue[2];
                const float chl3 = chromaPastel * sathue[3];
                pa = (chl2 - chl3) / (p2 - limitpastelsatur);
                pb = chl2 - pa * p2;
            } else if (saturation < s0) {
                const float chl3 = chromaPastel * sathue[3];
                const float chs0 = chromaSatur * sathue2[0];
                pa = (chl3 - chs0) / (limitpastelsatur - s0);
                pb = chl3 - pa * limitpastelsatur;
            } else if (saturation < s1) {
                const float chs0 = chromaSatur * sathue2[0];
                const float chs1 = chromaSatur * sathue2[1];
                pa = (chs0 - chs1) / (s0 - s1);
                pb = chs0 - pa * s0;
            } else if (saturation < s2) {
                const float chs1 = chromaSatur * sathue2[1];
                const float chs2 = chromaSatur * sathue2[2];
                pa = (chs1 - chs2) / (s1 - s2);
                pb = chs1 - pa * s1;
            } else {
                const float chs2 = chromaSatur * sathue2[2];
                const float chs3 = chromaSatur * sathue2[3];
                pa = (chs2 - chs3) / (s2 - 1.f);
                pb = chs2 - pa * s2;
            }
            chmod = pa * saturation + pb;
            chmod *= satredu;

            if (chromaPastel != chromaSatur) {
                /* Pastels. */
                if (saturation > p2 && saturation < limitpastelsatur) {
                    const float newchromaPastel =
                        chromaPastel_a * saturation + chromaPastel_b;
                    chmod = newchromaPastel * satredu * sathue[3];
                }

                /* Saturated. */
                if (saturation < s0 && saturation >= limitpastelsatur) {
                    const float newchromaSatur =
                        chromaSatur_a * saturation + chromaSatur_b;
                    chmod = newchromaSatur * satredu * sathue2[0];
                }
            }

            if (saturation <= limitpastelsatur) {
                chmod = fmax(-0.93f, fmin(chmod, 2.f));
                Chprov *= 1.0f + chmod;
            } else {
                chmod = fmax(-0.93f, fmin(chmod, 1.8f));
                Chprov *= 1.0f + chmod;
            }
            Chprov = fmax(Chprov, 6.f);
        }
    }

    int hhModified = 0;

    /* Vibrance's skin curve. */
    if (skin_curve_set != 0) {
        if (HH > skbeg && HH < skend) {
            if (Chprov < 60.f) {
                const float HHsk = ask * HH + bsk;
                const float Hn = rt_lt_lut(skin_curve, 65536, HHsk) - bsk0;
                const float Hc = Hn * xx + HH * (1.f - xx);
                HH = Hc;
                hhModified = 1;
            } else if (Chprov < (60.f + dchr)) {
                const float HHsk = ask * HH + bsk;
                const float Hn = rt_lt_lut(skin_curve, 65536, HHsk) - bsk0;
                const float Hc = Hn * xx + HH * (1.f - xx);
                const float aa = (HH - Hc) / dchr;
                const float bb = HH - (60.f + dchr) * aa;
                HH = aa * Chprov + bb;
                hhModified = 1;
            }
        } else if (HH > (skbeg - dhue) && HH <= skbeg &&
                   Chprov < (60.f + dchr * 0.5f)) {
            const float HHsk = ask * skbeg + bsk;
            const float Hn = rt_lt_lut(skin_curve, 65536, HHsk) - bsk0;
            const float Hcc = Hn * xx + skbeg * (1.f - xx);
            const float adh = (Hcc - (skbeg - dhue)) / dhue;
            const float bdh = Hcc - adh * skbeg;
            HH = adh * HH + bdh;
            hhModified = 1;
        } else if (HH >= skend && HH < (skend + dhue) &&
                   Chprov < (60.f + dchr * 0.5f)) {
            const float HHsk = ask * skend + bsk;
            const float Hn = rt_lt_lut(skin_curve, 65536, HHsk) - bsk0;
            const float Hcc = Hn * xx + skend * (1.f - xx);
            const float adh = (skend + dhue - Hcc) / dhue;
            const float bdh = Hcc - adh * skend;
            HH = adh * HH + bdh;
            hhModified = 1;
        }
    }

    /* Munsell correction. */
    if (avoidcolorshift == 0 && hhModified != 0) {
        sincosval = rt_lt_xsincosf(HH);
    }

    float aprovn = 0.f;
    float bprovn = 0.f;

    const float fyy = RT_LT_C1BY116 * Lprov + RT_LT_C16BY116;
    const float yy_ = (Lprov > RT_LT_EPSKAP) ? fyy * fyy * fyy
                                             : Lprov / RT_LT_KAPPA;
    /* std::numeric_limits<float>::min(): the smallest normal float. */
    float ChprovOld = 0x1p-126f;
    int inGamut;

    do {
        inGamut = 1;

        if (avoidcolorshift != 0) {
            const float correctionHue =
                rt_lt_all_munsell_lch(Lprov, HH, Chprov, CC, munsell,
                                      munsell_sizes);

            if (correctionHue != 0.f || hhModified != 0) {
                sincosval = rt_lt_xsincosf(HH + correctionHue);
                hhModified = 0;
            }
        }
        aprovn = Chprov * sincosval.y;
        bprovn = Chprov * sincosval.x;

        if (Chprov == ChprovOld) {
            break;
        }
        ChprovOld = Chprov;

        const float fxx = 0.002f * aprovn + fyy;
        const float fzz = fyy - 0.005f * bprovn;
        const float xx_ = rt_lt_f2xyz(fxx) * RT_LT_D50X;
        const float zz_ = rt_lt_f2xyz(fzz) * RT_LT_D50Z;

        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        rt_lt_xyz2rgb_flat(xx_, yy_, zz_, wip, &R, &G, &B);

        if (fmin(R, fmin(G, B)) < 0.0f) {
            Chprov *= 0.98f;
            inGamut = 0;
        }

        /* With highlight reconstruction, highlights keep their gamut. */
        if (highlight == 0 && fmax(R, fmax(G, B)) > 1.f &&
            fmin(R, fmin(G, B)) <= 1.f) {
            Chprov *= 0.98f;
            inGamut = 0;
        }
    } while (!inGamut);

    lab[offset] = Lprov * 327.68f;
    lab[lab_plane + offset] = aprovn * 327.68f;
    lab[2 * lab_plane + offset] = bprovn * 327.68f;
}
)CLC";
