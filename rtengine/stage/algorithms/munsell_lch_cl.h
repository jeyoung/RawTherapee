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
 * The Munsell Lch correction for the lab.tools node, as a device fragment.
 *
 * This is a line-for-line port of Color::MunsellLch() (rtengine/munselllch.cc)
 * and of the single-correction overload of Color::AllMunsellLch()
 * (rtengine/color.cc). The branch cascade, every hue limit, every clamp and
 * every table choice are the host's own; only the table access changed form.
 * The host LUTf tables are exported by Color::munsellLut() into one flat
 * buffer of RT_MUNSELL_STRIDE floats per table, in MUNSELL_TABLE_ID order, and
 * the host LUTf::operator[](int) index clamp is reproduced by
 * rt_lt_mun(): index 0 and index size-1 are the host's clamps.
 *
 * The host comment above Color::initMunsell() says 195 tables; the source
 * declares and sizes 197, and all 197 are referenced by MunsellLch(). The
 * stage exports every declared table, so the full set is what the device
 * reads. RT_MUNSELL_TABLE_COUNT is the source's own count.
 *
 * Keep this fragment in step with the host file: the parity test compares the
 * device result with Color::AllMunsellLch() over the sampled domain.
 */
inline constexpr const char* RT_MUNSELL_LCH_CL = R"CLC(
/* Number of Munsell tables and the flat stride of one table. The host
 * export pads every table to the longest one, 140 entries. */
#define RT_MUNSELL_TABLE_COUNT 197u
#define RT_MUNSELL_STRIDE 140u

#define RT_MUN_4P10 0u
#define RT_MUN_4P20 1u
#define RT_MUN_4P30 2u
#define RT_MUN_4P40 3u
#define RT_MUN_4P50 4u
#define RT_MUN_4P60 5u
#define RT_MUN_1P10 6u
#define RT_MUN_1P20 7u
#define RT_MUN_1P30 8u
#define RT_MUN_1P40 9u
#define RT_MUN_1P50 10u
#define RT_MUN_1P60 11u
#define RT_MUN_5B40 12u
#define RT_MUN_5B50 13u
#define RT_MUN_5B60 14u
#define RT_MUN_5B70 15u
#define RT_MUN_5B80 16u
#define RT_MUN_7B40 17u
#define RT_MUN_7B50 18u
#define RT_MUN_7B60 19u
#define RT_MUN_7B70 20u
#define RT_MUN_7B80 21u
#define RT_MUN_9B40 22u
#define RT_MUN_9B50 23u
#define RT_MUN_9B60 24u
#define RT_MUN_9B70 25u
#define RT_MUN_9B80 26u
#define RT_MUN_10B40 27u
#define RT_MUN_10B50 28u
#define RT_MUN_10B60 29u
#define RT_MUN_10B70 30u
#define RT_MUN_10B80 31u
#define RT_MUN_05PB40 32u
#define RT_MUN_05PB50 33u
#define RT_MUN_05PB60 34u
#define RT_MUN_05PB70 35u
#define RT_MUN_05PB80 36u
#define RT_MUN_10PB10 37u
#define RT_MUN_10PB20 38u
#define RT_MUN_10PB30 39u
#define RT_MUN_10PB40 40u
#define RT_MUN_10PB50 41u
#define RT_MUN_10PB60 42u
#define RT_MUN_9PB10 43u
#define RT_MUN_9PB20 44u
#define RT_MUN_9PB30 45u
#define RT_MUN_9PB40 46u
#define RT_MUN_9PB50 47u
#define RT_MUN_9PB60 48u
#define RT_MUN_9PB70 49u
#define RT_MUN_9PB80 50u
#define RT_MUN_75PB10 51u
#define RT_MUN_75PB20 52u
#define RT_MUN_75PB30 53u
#define RT_MUN_75PB40 54u
#define RT_MUN_75PB50 55u
#define RT_MUN_75PB60 56u
#define RT_MUN_75PB70 57u
#define RT_MUN_75PB80 58u
#define RT_MUN_6PB10 59u
#define RT_MUN_6PB20 60u
#define RT_MUN_6PB30 61u
#define RT_MUN_6PB40 62u
#define RT_MUN_6PB50 63u
#define RT_MUN_6PB60 64u
#define RT_MUN_6PB70 65u
#define RT_MUN_6PB80 66u
#define RT_MUN_45PB10 67u
#define RT_MUN_45PB20 68u
#define RT_MUN_45PB30 69u
#define RT_MUN_45PB40 70u
#define RT_MUN_45PB50 71u
#define RT_MUN_45PB60 72u
#define RT_MUN_45PB70 73u
#define RT_MUN_45PB80 74u
#define RT_MUN_3PB10 75u
#define RT_MUN_3PB20 76u
#define RT_MUN_3PB30 77u
#define RT_MUN_3PB40 78u
#define RT_MUN_3PB50 79u
#define RT_MUN_3PB60 80u
#define RT_MUN_3PB70 81u
#define RT_MUN_3PB80 82u
#define RT_MUN_15PB10 83u
#define RT_MUN_15PB20 84u
#define RT_MUN_15PB30 85u
#define RT_MUN_15PB40 86u
#define RT_MUN_15PB50 87u
#define RT_MUN_15PB60 88u
#define RT_MUN_15PB70 89u
#define RT_MUN_15PB80 90u
#define RT_MUN_10YR20 91u
#define RT_MUN_10YR30 92u
#define RT_MUN_10YR40 93u
#define RT_MUN_10YR50 94u
#define RT_MUN_10YR60 95u
#define RT_MUN_10YR70 96u
#define RT_MUN_10YR80 97u
#define RT_MUN_10YR90 98u
#define RT_MUN_85YR20 99u
#define RT_MUN_85YR30 100u
#define RT_MUN_85YR40 101u
#define RT_MUN_85YR50 102u
#define RT_MUN_85YR60 103u
#define RT_MUN_85YR70 104u
#define RT_MUN_85YR80 105u
#define RT_MUN_85YR90 106u
#define RT_MUN_7YR30 107u
#define RT_MUN_7YR40 108u
#define RT_MUN_7YR50 109u
#define RT_MUN_7YR60 110u
#define RT_MUN_7YR70 111u
#define RT_MUN_7YR80 112u
#define RT_MUN_55YR30 113u
#define RT_MUN_55YR40 114u
#define RT_MUN_55YR50 115u
#define RT_MUN_55YR60 116u
#define RT_MUN_55YR70 117u
#define RT_MUN_55YR80 118u
#define RT_MUN_55YR90 119u
#define RT_MUN_4YR30 120u
#define RT_MUN_4YR40 121u
#define RT_MUN_4YR50 122u
#define RT_MUN_4YR60 123u
#define RT_MUN_4YR70 124u
#define RT_MUN_4YR80 125u
#define RT_MUN_25YR30 126u
#define RT_MUN_25YR40 127u
#define RT_MUN_25YR50 128u
#define RT_MUN_25YR60 129u
#define RT_MUN_25YR70 130u
#define RT_MUN_10R30 131u
#define RT_MUN_10R40 132u
#define RT_MUN_10R50 133u
#define RT_MUN_10R60 134u
#define RT_MUN_10R70 135u
#define RT_MUN_9R30 136u
#define RT_MUN_9R40 137u
#define RT_MUN_9R50 138u
#define RT_MUN_9R60 139u
#define RT_MUN_9R70 140u
#define RT_MUN_7R30 141u
#define RT_MUN_7R40 142u
#define RT_MUN_7R50 143u
#define RT_MUN_7R60 144u
#define RT_MUN_7R70 145u
#define RT_MUN_5R10 146u
#define RT_MUN_5R20 147u
#define RT_MUN_5R30 148u
#define RT_MUN_25R10 149u
#define RT_MUN_25R20 150u
#define RT_MUN_25R30 151u
#define RT_MUN_10RP10 152u
#define RT_MUN_10RP20 153u
#define RT_MUN_10RP30 154u
#define RT_MUN_7G30 155u
#define RT_MUN_7G40 156u
#define RT_MUN_7G50 157u
#define RT_MUN_7G60 158u
#define RT_MUN_7G70 159u
#define RT_MUN_7G80 160u
#define RT_MUN_5G30 161u
#define RT_MUN_5G40 162u
#define RT_MUN_5G50 163u
#define RT_MUN_5G60 164u
#define RT_MUN_5G70 165u
#define RT_MUN_5G80 166u
#define RT_MUN_25G30 167u
#define RT_MUN_25G40 168u
#define RT_MUN_25G50 169u
#define RT_MUN_25G60 170u
#define RT_MUN_25G70 171u
#define RT_MUN_25G80 172u
#define RT_MUN_1G30 173u
#define RT_MUN_1G40 174u
#define RT_MUN_1G50 175u
#define RT_MUN_1G60 176u
#define RT_MUN_1G70 177u
#define RT_MUN_1G80 178u
#define RT_MUN_10GY30 179u
#define RT_MUN_10GY40 180u
#define RT_MUN_10GY50 181u
#define RT_MUN_10GY60 182u
#define RT_MUN_10GY70 183u
#define RT_MUN_10GY80 184u
#define RT_MUN_75GY30 185u
#define RT_MUN_75GY40 186u
#define RT_MUN_75GY50 187u
#define RT_MUN_75GY60 188u
#define RT_MUN_75GY70 189u
#define RT_MUN_75GY80 190u
#define RT_MUN_5GY30 191u
#define RT_MUN_5GY40 192u
#define RT_MUN_5GY50 193u
#define RT_MUN_5GY60 194u
#define RT_MUN_5GY70 195u
#define RT_MUN_5GY80 196u

/* Color::MunsellLch()'s table read: LUTf::operator[](int), which clamps the
 * index to [0, size - 1]. Entries past a table's own size are padding and are
 * never read. */
inline float rt_lt_mun(__global const float* tables, __global const float* sizes,
                       const int table, const int index)
{
    const int last = (int)sizes[table] - 1;
    const int clamped = min(max(index, 0), last);
    return tables[table * (int)RT_MUNSELL_STRIDE + clamped];
}

/*
 * Color::MunsellLch(), with the two out-parameters made explicit. `correction`
 * receives the hue correction and `lbe` the table's own hue at the clamped
 * chroma; `correctL` is the host's flag, kept so the branch logic is unchanged.
 */
inline void rt_lt_munsell_lch(const float lum, const float hue, const float chrom,
                              const float memChprov, float* correction,
                              const int zone, float* lbe, int* correctL,
                              __global const float* tables,
                              __global const float* sizes)
{
    float correction_v = *correction;
    float lbe_v = *lbe;
    int correctL_v = *correctL;
    const int x = memChprov;
    int y = chrom;

    //begin PB correction + sky
    if (zone == 1) {
        if (lum > 5.f) {
            if (lum < 15.f) {
                if (x <= 45 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB10, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB10, x) + 0.052f))) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB10, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= ( rt_lt_mun(tables, sizes, RT_MUN_3PB10, x) - 0.052f))  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB10, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_3PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_3PB10, x);
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_3PB10, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB10, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB10, x) + 0.052f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB10, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB10, x) - 0.052f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB10, x)) * 0.5f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB10, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB10, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB10, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB10, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB10, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB10, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB10, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_9PB10, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P10, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB10, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10PB10, y) - rt_lt_mun(tables, sizes, RT_MUN_10PB10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10PB10, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB10, x) + rt_lt_mun(tables, sizes, RT_MUN_1P10, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P10, x) + rt_lt_mun(tables, sizes, RT_MUN_4P10, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1P10, y) - rt_lt_mun(tables, sizes, RT_MUN_1P10, x);
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1P10, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_1P10, x) + rt_lt_mun(tables, sizes, RT_MUN_4P10, x)) * 0.5f)  && (hue < (0.035f + rt_lt_mun(tables, sizes, RT_MUN_4P10, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4P10, y) - rt_lt_mun(tables, sizes, RT_MUN_4P10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4P10, y);
                    correctL_v = true;
                }
            } else if (lum < 25.f) {
                if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB20, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB20, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB20, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB20, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB20, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_3PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_3PB20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_3PB20, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB20, x)) * 0.5f)  && (hue < ( rt_lt_mun(tables, sizes, RT_MUN_45PB20, x) + 0.052f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB20, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB20, x) + 0.052f))  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB20, x);
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB20, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB20, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB20, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB20, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB20, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_9PB20, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P20, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10PB20, y) - rt_lt_mun(tables, sizes, RT_MUN_10PB20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10PB20, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB20, x) + rt_lt_mun(tables, sizes, RT_MUN_1P20, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P20, x) + rt_lt_mun(tables, sizes, RT_MUN_4P20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1P20, y) - rt_lt_mun(tables, sizes, RT_MUN_1P20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1P20, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_1P20, x) + rt_lt_mun(tables, sizes, RT_MUN_4P20, x)) * 0.5f)  && (hue < (0.035f + rt_lt_mun(tables, sizes, RT_MUN_4P20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4P20, y) - rt_lt_mun(tables, sizes, RT_MUN_4P20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4P20, y);
                    correctL_v = true;
                }
            } else if (lum < 35.f) {
                if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB30, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB30, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB30, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_3PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_3PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_3PB30, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB30, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB30, x) + 0.052f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB30, y);
                    correctL_v = true;
                } else if ((hue >= ( rt_lt_mun(tables, sizes, RT_MUN_45PB30, x) + 0.052f))  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB30, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB30, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB30, y) ;
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB30, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB30, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_9PB30, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P30, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10PB30, y) - rt_lt_mun(tables, sizes, RT_MUN_10PB30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10PB30, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB30, x) + rt_lt_mun(tables, sizes, RT_MUN_1P30, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P30, x) + rt_lt_mun(tables, sizes, RT_MUN_4P30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1P30, y) - rt_lt_mun(tables, sizes, RT_MUN_1P30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1P30, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_1P30, x) + rt_lt_mun(tables, sizes, RT_MUN_4P30, x)) * 0.5f)  && (hue < (0.035f + rt_lt_mun(tables, sizes, RT_MUN_4P30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4P30, y) - rt_lt_mun(tables, sizes, RT_MUN_4P30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4P30, y);
                    correctL_v = true;
                }
            } else if (lum < 45.f) {
                if (x < 75 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_15PB40, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_05PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_10B40, x)) * 0.5f)) {
                    if (y > 75) {
                        y = 75;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_05PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_05PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_05PB40, y);
                    correctL_v = true;
                } else if (x < 70 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_10B40, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10B40, x) + rt_lt_mun(tables, sizes, RT_MUN_9B40, x)) * 0.5f)) {
                    if (y > 70) {
                        y = 70;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10B40, y) - rt_lt_mun(tables, sizes, RT_MUN_10B40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10B40, y);
                    correctL_v = true;
                } else if (x < 70 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10B40, x) + rt_lt_mun(tables, sizes, RT_MUN_9B40, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9B40, x) + rt_lt_mun(tables, sizes, RT_MUN_7B40, x)) * 0.5f)) {
                    if (y > 70) {
                        y = 70;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9B40, y) - rt_lt_mun(tables, sizes, RT_MUN_9B40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9B40, y);
                    correctL_v = true;
                } else if (x < 70 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9B40, x) + rt_lt_mun(tables, sizes, RT_MUN_7B40, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B40, x) + rt_lt_mun(tables, sizes, RT_MUN_7B40, x)) * 0.5f)) {
                    if (y > 70) {
                        y = 70;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7B40, y) - rt_lt_mun(tables, sizes, RT_MUN_7B40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7B40, y);
                    correctL_v = true;
                } else if (x < 70 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5B40, x) + rt_lt_mun(tables, sizes, RT_MUN_7B40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B40, x) - 0.035f))) {
                    if (y > 70) {
                        y = 70;    //
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5B40, y) - rt_lt_mun(tables, sizes, RT_MUN_5B40, x) ;
                    lbe_v =  rt_lt_mun(tables, sizes, RT_MUN_5B40, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB40, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB40, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB40, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_3PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_3PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_3PB40, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB40, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB40, x) + 0.052f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB40, y) ;
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB40, x) + 0.052f))  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB40, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB40, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB40, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB40, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB40, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_9PB40, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P40, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10PB40, y) - rt_lt_mun(tables, sizes, RT_MUN_10PB40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10PB40, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB40, x) + rt_lt_mun(tables, sizes, RT_MUN_1P40, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P40, x) + rt_lt_mun(tables, sizes, RT_MUN_4P40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1P40, y) - rt_lt_mun(tables, sizes, RT_MUN_1P40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1P40, y);
                    correctL_v = true;
                } else if ((hue >= (rt_lt_mun(tables, sizes, RT_MUN_1P40, x) + rt_lt_mun(tables, sizes, RT_MUN_4P40, x)) * 0.5f)  && (hue < (0.035f + rt_lt_mun(tables, sizes, RT_MUN_4P40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4P40, y) - rt_lt_mun(tables, sizes, RT_MUN_4P40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4P40, y);
                    correctL_v = true;
                }
            } else if (lum < 55.f) {
                if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_15PB50, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_05PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_10B50, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_05PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_05PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_05PB50, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_10B50, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10B50, x) + rt_lt_mun(tables, sizes, RT_MUN_9B50, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10B50, y) - rt_lt_mun(tables, sizes, RT_MUN_10B50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10B50, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10B50, x) + rt_lt_mun(tables, sizes, RT_MUN_9B50, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9B50, x) + rt_lt_mun(tables, sizes, RT_MUN_7B50, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9B50, y) - rt_lt_mun(tables, sizes, RT_MUN_9B50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9B50, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9B50, x) + rt_lt_mun(tables, sizes, RT_MUN_7B50, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B50, x) + rt_lt_mun(tables, sizes, RT_MUN_7B50, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7B50, y) - rt_lt_mun(tables, sizes, RT_MUN_7B50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7B50, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5B50, x) + rt_lt_mun(tables, sizes, RT_MUN_7B50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B50, x) - 0.035f))) {
                    if (y > 79) {
                        y = 79;    //
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5B50, y) - rt_lt_mun(tables, sizes, RT_MUN_5B50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5B50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB50, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_3PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_3PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_3PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_9PB50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P50, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10PB50, y) - rt_lt_mun(tables, sizes, RT_MUN_10PB50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10PB50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB50, x) + rt_lt_mun(tables, sizes, RT_MUN_1P50, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P50, x) + rt_lt_mun(tables, sizes, RT_MUN_4P50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1P50, y) - rt_lt_mun(tables, sizes, RT_MUN_1P50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1P50, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_1P50, x) + rt_lt_mun(tables, sizes, RT_MUN_4P50, x)) * 0.5f)  && (hue < (0.035f + rt_lt_mun(tables, sizes, RT_MUN_4P50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4P50, y) - rt_lt_mun(tables, sizes, RT_MUN_4P50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4P50, y);
                    correctL_v = true;
                }
            } else if (lum < 65.f) {
                if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_15PB60, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_05PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_10B60, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_05PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_05PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_05PB60, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_10B60, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10B60, x) + rt_lt_mun(tables, sizes, RT_MUN_9B60, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10B60, y) - rt_lt_mun(tables, sizes, RT_MUN_10B60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10B60, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10B60, x) + rt_lt_mun(tables, sizes, RT_MUN_9B60, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9B60, x) + rt_lt_mun(tables, sizes, RT_MUN_7B60, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9B60, y) - rt_lt_mun(tables, sizes, RT_MUN_9B60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9B60, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9B60, x) + rt_lt_mun(tables, sizes, RT_MUN_7B60, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B60, x) + rt_lt_mun(tables, sizes, RT_MUN_7B60, x)) * 0.5f)) {
                    if (y > 79) {
                        y = 79;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7B60, y) - rt_lt_mun(tables, sizes, RT_MUN_7B60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7B60, y);
                    correctL_v = true;
                } else if (x < 79 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5B60, x) + rt_lt_mun(tables, sizes, RT_MUN_7B60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B60, x) - 0.035f))) {
                    if (y > 79) {
                        y = 79;    //
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5B60, y) - rt_lt_mun(tables, sizes, RT_MUN_5B60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5B60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB60, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_45PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_3PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_3PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_3PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_9PB60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P60, x) + rt_lt_mun(tables, sizes, RT_MUN_10PB60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10PB60, y) - rt_lt_mun(tables, sizes, RT_MUN_10PB60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10PB60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_10PB60, x) + rt_lt_mun(tables, sizes, RT_MUN_1P60, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_1P60, x) + rt_lt_mun(tables, sizes, RT_MUN_4P60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1P60, y) - rt_lt_mun(tables, sizes, RT_MUN_1P60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1P60, y);
                    correctL_v = true;
                } else if (x <= 85 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_1P60, x) + rt_lt_mun(tables, sizes, RT_MUN_4P60, x)) * 0.5f)  && (hue < (0.035f + rt_lt_mun(tables, sizes, RT_MUN_4P60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4P60, y) - rt_lt_mun(tables, sizes, RT_MUN_4P60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4P60, y);
                    correctL_v = true;
                }
            } else if (lum < 75.f) {
                if (x < 50 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_15PB70, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_05PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_10B70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_05PB70, y) - rt_lt_mun(tables, sizes, RT_MUN_05PB70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_05PB70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_10B70, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10B70, x) + rt_lt_mun(tables, sizes, RT_MUN_9B70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10B70, y) - rt_lt_mun(tables, sizes, RT_MUN_10B70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10B70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10B70, x) + rt_lt_mun(tables, sizes, RT_MUN_9B70, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9B70, x) + rt_lt_mun(tables, sizes, RT_MUN_7B70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9B70, y) - rt_lt_mun(tables, sizes, RT_MUN_9B70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9B70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9B70, x) + rt_lt_mun(tables, sizes, RT_MUN_7B70, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B70, x) + rt_lt_mun(tables, sizes, RT_MUN_7B70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7B70, y) - rt_lt_mun(tables, sizes, RT_MUN_7B70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7B70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5B70, x) + rt_lt_mun(tables, sizes, RT_MUN_7B70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B70, x) - 0.035f))) {
                    if (y > 49) {
                        y = 49;    //
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5B70, y) - rt_lt_mun(tables, sizes, RT_MUN_5B70, x) ;
                    lbe_v =  rt_lt_mun(tables, sizes, RT_MUN_5B70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB70, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB70, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB70, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB70, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB70, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB70, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB70, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB70, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB70, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB70, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB70, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB70, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB70, x) + 0.035f))) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB70, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB70, y);
                    correctL_v = true;
                }
            } else if (lum < 85.f) {
                if (x < 40 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_15PB80, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_05PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_10B80, x)) * 0.5f)) {
                    if (y > 39) {
                        y = 39;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_05PB80, y) - rt_lt_mun(tables, sizes, RT_MUN_05PB80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_05PB80, y) ;
                    correctL_v = true;
                } else if (x < 40 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_05PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_10B80, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10B80, x) + rt_lt_mun(tables, sizes, RT_MUN_9B80, x)) * 0.5f)) {
                    if (y > 39) {
                        y = 39;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10B80, y) - rt_lt_mun(tables, sizes, RT_MUN_10B80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10B80, y);
                    correctL_v = true;
                } else if (x < 40 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10B80, x) + rt_lt_mun(tables, sizes, RT_MUN_9B80, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9B80, x) + rt_lt_mun(tables, sizes, RT_MUN_7B80, x)) * 0.5f)) {
                    if (y > 39) {
                        y = 39;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9B80, y) - rt_lt_mun(tables, sizes, RT_MUN_9B80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9B80, y);
                    correctL_v = true;
                } else if (x < 50 &&(hue <= (rt_lt_mun(tables, sizes, RT_MUN_9B80, x) + rt_lt_mun(tables, sizes, RT_MUN_7B80, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B80, x) + rt_lt_mun(tables, sizes, RT_MUN_7B80, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7B80, y) - rt_lt_mun(tables, sizes, RT_MUN_7B80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7B80, y);
                    correctL_v = true;
                } else if (x < 50 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5B80, x) + rt_lt_mun(tables, sizes, RT_MUN_7B80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5B80, x) - 0.035f))) {
                    if (y > 49) {
                        y = 49;    //
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5B80, y) - rt_lt_mun(tables, sizes, RT_MUN_5B80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5B80, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_15PB80, x) - 0.035f)) && (hue < (rt_lt_mun(tables, sizes, RT_MUN_15PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB80, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_15PB80, y) - rt_lt_mun(tables, sizes, RT_MUN_15PB80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_15PB80, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_45PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_3PB80, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB80, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_45PB80, y) - rt_lt_mun(tables, sizes, RT_MUN_45PB80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_45PB80, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_45PB80, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_6PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB80, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_6PB80, y) - rt_lt_mun(tables, sizes, RT_MUN_6PB80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_6PB80, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_6PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB80, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB80, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75PB80, y) - rt_lt_mun(tables, sizes, RT_MUN_75PB80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75PB80, y);
                    correctL_v = true;
                } else if (x < 50 && (hue >= (rt_lt_mun(tables, sizes, RT_MUN_9PB80, x) + rt_lt_mun(tables, sizes, RT_MUN_75PB80, x)) * 0.5f)  && (hue < (rt_lt_mun(tables, sizes, RT_MUN_9PB80, x) + 0.035f))) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9PB80, y) - rt_lt_mun(tables, sizes, RT_MUN_9PB80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9PB80, y);
                    correctL_v = true;
                }
            }
        }
    } else if (zone == 2) { //red yellow correction
        if (lum > 15.f) {
            if (lum < 25.f) {
                if (x <= 45 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR20, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR20, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR20, x)) * 0.5f)) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR20, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR20, y);
                    correctL_v = true;
                } else if (x <= 45 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR20, x) + rt_lt_mun(tables, sizes, RT_MUN_10YR20, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR20, x) + 0.035f))) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR20, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR20, y);
                    correctL_v = true;
                }
            } else if (lum < 35.f) {
                if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR30, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR30, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR30, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR30, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7YR30, y) - rt_lt_mun(tables, sizes, RT_MUN_7YR30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7YR30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_7YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR30, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_4YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4YR30, y) - rt_lt_mun(tables, sizes, RT_MUN_4YR30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4YR30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_4YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_10R30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25YR30, y) - rt_lt_mun(tables, sizes, RT_MUN_25YR30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25YR30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_25YR30, x) + rt_lt_mun(tables, sizes, RT_MUN_10R30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10R30, x) + rt_lt_mun(tables, sizes, RT_MUN_9R30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10R30, y) - rt_lt_mun(tables, sizes, RT_MUN_10R30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10R30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10R30, x) + rt_lt_mun(tables, sizes, RT_MUN_9R30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9R30, x) + rt_lt_mun(tables, sizes, RT_MUN_7R30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9R30, y) - rt_lt_mun(tables, sizes, RT_MUN_9R30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9R30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9R30, x) + rt_lt_mun(tables, sizes, RT_MUN_7R30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7R30, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }

                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7R30, y) - rt_lt_mun(tables, sizes, RT_MUN_7R30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7R30, y) ;
                    correctL_v = true;
                }
            } else if (lum < 45.f) {
                if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR40, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR40, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR40, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR40, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7YR40, y) - rt_lt_mun(tables, sizes, RT_MUN_7YR40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7YR40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_7YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR40, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_4YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4YR40, y) - rt_lt_mun(tables, sizes, RT_MUN_4YR40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4YR40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_4YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_10R40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25YR40, y) - rt_lt_mun(tables, sizes, RT_MUN_25YR40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25YR40, y) ;
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_25YR40, x) + rt_lt_mun(tables, sizes, RT_MUN_10R40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10R40, x) + rt_lt_mun(tables, sizes, RT_MUN_9R40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10R40, y) - rt_lt_mun(tables, sizes, RT_MUN_10R40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10R40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10R40, x) + rt_lt_mun(tables, sizes, RT_MUN_9R40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9R40, x) + rt_lt_mun(tables, sizes, RT_MUN_7R40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9R40, y) - rt_lt_mun(tables, sizes, RT_MUN_9R40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9R40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9R40, x) + rt_lt_mun(tables, sizes, RT_MUN_7R40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7R40, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7R40, y) - rt_lt_mun(tables, sizes, RT_MUN_7R40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7R40, y);
                    correctL_v = true;
                }
            } else if (lum < 55.f) {
                if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR50, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR50, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR50, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR50, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7YR50, y) - rt_lt_mun(tables, sizes, RT_MUN_7YR50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7YR50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_7YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR50, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_4YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4YR50, y) - rt_lt_mun(tables, sizes, RT_MUN_4YR50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4YR50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_4YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_10R50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25YR50, y) - rt_lt_mun(tables, sizes, RT_MUN_25YR50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25YR50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_25YR50, x) + rt_lt_mun(tables, sizes, RT_MUN_10R50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10R50, x) + rt_lt_mun(tables, sizes, RT_MUN_9R50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10R50, y) - rt_lt_mun(tables, sizes, RT_MUN_10R50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10R50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10R50, x) + rt_lt_mun(tables, sizes, RT_MUN_9R50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9R50, x) + rt_lt_mun(tables, sizes, RT_MUN_7R50, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9R50, y) - rt_lt_mun(tables, sizes, RT_MUN_9R50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9R50, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9R50, x) + rt_lt_mun(tables, sizes, RT_MUN_7R50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7R50, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7R50, y) - rt_lt_mun(tables, sizes, RT_MUN_7R50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7R50, y);
                    correctL_v = true;
                }
            } else if (lum < 65.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR60, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR60, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR60, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR60, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7YR60, y) - rt_lt_mun(tables, sizes, RT_MUN_7YR60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7YR60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR60, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_4YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4YR60, y) - rt_lt_mun(tables, sizes, RT_MUN_4YR60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4YR60, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_4YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_10R60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25YR60, y) - rt_lt_mun(tables, sizes, RT_MUN_25YR60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25YR60, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_25YR60, x) + rt_lt_mun(tables, sizes, RT_MUN_10R60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10R60, x) + rt_lt_mun(tables, sizes, RT_MUN_9R60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10R60, y) - rt_lt_mun(tables, sizes, RT_MUN_10R60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10R60, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10R60, x) + rt_lt_mun(tables, sizes, RT_MUN_9R60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9R60, x) + rt_lt_mun(tables, sizes, RT_MUN_7R60, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9R60, y) - rt_lt_mun(tables, sizes, RT_MUN_9R60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9R60, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9R60, x) + rt_lt_mun(tables, sizes, RT_MUN_7R60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7R60, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7R60, y) - rt_lt_mun(tables, sizes, RT_MUN_7R60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7R60, y);
                    correctL_v = true;
                }
            } else if (lum < 75.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR70, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR70, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR70, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR70, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7YR70, y) - rt_lt_mun(tables, sizes, RT_MUN_7YR70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7YR70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR70, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_4YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4YR70, y) - rt_lt_mun(tables, sizes, RT_MUN_4YR70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4YR70, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_4YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_25YR70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_10R70, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25YR70, y) - rt_lt_mun(tables, sizes, RT_MUN_25YR70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25YR70, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_25YR70, x) + rt_lt_mun(tables, sizes, RT_MUN_10R70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10R70, x) + rt_lt_mun(tables, sizes, RT_MUN_9R70, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10R70, y) - rt_lt_mun(tables, sizes, RT_MUN_10R70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10R70, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10R70, x) + rt_lt_mun(tables, sizes, RT_MUN_9R70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_9R70, x) + rt_lt_mun(tables, sizes, RT_MUN_7R70, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_9R70, y) - rt_lt_mun(tables, sizes, RT_MUN_9R70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_9R70, y) ;
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_9R70, x) + rt_lt_mun(tables, sizes, RT_MUN_7R70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7R70, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7R70, y) - rt_lt_mun(tables, sizes, RT_MUN_7R70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7R70, y);
                    correctL_v = true;
                }
            } else if (lum < 85.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR80, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR80, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_85YR80, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR80, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR80, y);
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_7YR80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR80, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7YR80, y) - rt_lt_mun(tables, sizes, RT_MUN_7YR80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7YR80, y);
                    correctL_v = true;
                } else if (x < 45 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_7YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_55YR80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR80, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR80, y);
                    correctL_v = true;
                } else if (x < 45 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR80, x) + rt_lt_mun(tables, sizes, RT_MUN_4YR80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_4YR80, x) - 0.035f))) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_4YR80, y) - rt_lt_mun(tables, sizes, RT_MUN_4YR80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_4YR80, y) ;
                    correctL_v = true;
                }
            } else if (lum < 95.f) {
                if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10YR90, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10YR90, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10YR90, y) - rt_lt_mun(tables, sizes, RT_MUN_10YR90, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10YR90, y);
                    correctL_v = true;
                } else if (x < 85 && hue <= (rt_lt_mun(tables, sizes, RT_MUN_85YR90, x) + 0.035f)  && hue > (rt_lt_mun(tables, sizes, RT_MUN_85YR90, x) - 0.035f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_85YR90, y) - rt_lt_mun(tables, sizes, RT_MUN_85YR90, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_85YR90, y);
                    correctL_v = true;
                } else if (x < 45 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_55YR90, x) + 0.035f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_55YR90, x) - 0.035f)))) {
                    if (y > 49) {
                        y = 49;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_55YR90, y) - rt_lt_mun(tables, sizes, RT_MUN_55YR90, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_55YR90, y);
                    correctL_v = true;
                }
            }
        }
    } else if (zone == 3) { // //Green yellow correction
        if (lum >= 25.f) {
            if (lum < 35.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G30, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7G30, x) + rt_lt_mun(tables, sizes, RT_MUN_5G30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7G30, y) - rt_lt_mun(tables, sizes, RT_MUN_7G30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7G30, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G30, x) + rt_lt_mun(tables, sizes, RT_MUN_5G30, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5G30, x) + rt_lt_mun(tables, sizes, RT_MUN_25G30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5G30, y) - rt_lt_mun(tables, sizes, RT_MUN_5G30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5G30, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25G30, x) + rt_lt_mun(tables, sizes, RT_MUN_5G30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25G30, x) + rt_lt_mun(tables, sizes, RT_MUN_1G30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25G30, y) - rt_lt_mun(tables, sizes, RT_MUN_25G30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25G30, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G30, x) + rt_lt_mun(tables, sizes, RT_MUN_25G30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_1G30, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1G30, y) - rt_lt_mun(tables, sizes, RT_MUN_1G30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1G30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G30, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10GY30, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10GY30, y) - rt_lt_mun(tables, sizes, RT_MUN_10GY30, x) ;
                    lbe_v =  rt_lt_mun(tables, sizes, RT_MUN_10GY30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10GY30, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_75GY30, x) + rt_lt_mun(tables, sizes, RT_MUN_5GY30, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75GY30, y) - rt_lt_mun(tables, sizes, RT_MUN_75GY30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75GY30, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5GY30, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5GY30, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5GY30, y) - rt_lt_mun(tables, sizes, RT_MUN_5GY30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5GY30, y) ;
                    correctL_v = true;
                }
            } else if (lum < 45.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G40, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7G40, x) + rt_lt_mun(tables, sizes, RT_MUN_5G40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7G40, y) - rt_lt_mun(tables, sizes, RT_MUN_7G40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7G40, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G40, x) + rt_lt_mun(tables, sizes, RT_MUN_5G40, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5G40, x) + rt_lt_mun(tables, sizes, RT_MUN_25G40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5G40, y) - rt_lt_mun(tables, sizes, RT_MUN_5G40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5G40, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25G40, x) + rt_lt_mun(tables, sizes, RT_MUN_5G40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25G40, x) + rt_lt_mun(tables, sizes, RT_MUN_1G40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25G40, y) - rt_lt_mun(tables, sizes, RT_MUN_25G40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25G40, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G40, x) + rt_lt_mun(tables, sizes, RT_MUN_25G40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_1G40, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY40, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1G40, y) - rt_lt_mun(tables, sizes, RT_MUN_1G40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1G40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G40, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10GY40, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10GY40, y) - rt_lt_mun(tables, sizes, RT_MUN_10GY40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10GY40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10GY40, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_75GY40, x) + rt_lt_mun(tables, sizes, RT_MUN_5GY40, x)) * 0.5f)) {
                    if (y > 89) {
                        y = 89;
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75GY40, y) - rt_lt_mun(tables, sizes, RT_MUN_75GY40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75GY40, y);
                    correctL_v = true;
                } else if (x < 85 && (hue <= (rt_lt_mun(tables, sizes, RT_MUN_5GY40, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY40, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5GY40, x) - 0.035f))) {
                    if (y > 89) {
                        y = 89;    //
                    }
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5GY40, y) - rt_lt_mun(tables, sizes, RT_MUN_5GY40, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5GY40, y);
                    correctL_v = true;
                }
            } else if (lum < 55.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G50, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7G50, x) + rt_lt_mun(tables, sizes, RT_MUN_5G50, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7G50, y) - rt_lt_mun(tables, sizes, RT_MUN_7G50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7G50, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G50, x) + rt_lt_mun(tables, sizes, RT_MUN_5G50, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5G50, x) + rt_lt_mun(tables, sizes, RT_MUN_25G50, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5G50, y) - rt_lt_mun(tables, sizes, RT_MUN_5G50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5G50, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25G50, x) + rt_lt_mun(tables, sizes, RT_MUN_5G50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25G50, x) + rt_lt_mun(tables, sizes, RT_MUN_1G50, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25G50, y) - rt_lt_mun(tables, sizes, RT_MUN_25G50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25G50, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G50, x) + rt_lt_mun(tables, sizes, RT_MUN_25G50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_1G50, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY50, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1G50, y) - rt_lt_mun(tables, sizes, RT_MUN_1G50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1G50, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G50, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10GY50, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY50, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10GY50, y) - rt_lt_mun(tables, sizes, RT_MUN_10GY50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10GY50, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10GY50, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_75GY50, x) + rt_lt_mun(tables, sizes, RT_MUN_5GY50, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75GY50, y) - rt_lt_mun(tables, sizes, RT_MUN_75GY50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75GY50, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5GY50, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY50, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5GY50, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5GY50, y) - rt_lt_mun(tables, sizes, RT_MUN_5GY50, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5GY50, y);
                    correctL_v = true;
                }
            } else if (lum < 65.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G60, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7G60, x) + rt_lt_mun(tables, sizes, RT_MUN_5G60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7G60, y) - rt_lt_mun(tables, sizes, RT_MUN_7G60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7G60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G60, x) + rt_lt_mun(tables, sizes, RT_MUN_5G60, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5G60, x) + rt_lt_mun(tables, sizes, RT_MUN_25G60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5G60, y) - rt_lt_mun(tables, sizes, RT_MUN_5G60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5G60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25G60, x) + rt_lt_mun(tables, sizes, RT_MUN_5G60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25G60, x) + rt_lt_mun(tables, sizes, RT_MUN_1G60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25G60, y) - rt_lt_mun(tables, sizes, RT_MUN_25G60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25G60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G60, x) + rt_lt_mun(tables, sizes, RT_MUN_25G60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_1G60, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1G60, y) - rt_lt_mun(tables, sizes, RT_MUN_1G60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1G60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G60, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10GY60, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10GY60, y) - rt_lt_mun(tables, sizes, RT_MUN_10GY60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10GY60, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10GY60, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_75GY60, x) + rt_lt_mun(tables, sizes, RT_MUN_5GY60, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75GY60, y) - rt_lt_mun(tables, sizes, RT_MUN_75GY60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75GY60, y) ;
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5GY60, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY60, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5GY60, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5GY60, y) - rt_lt_mun(tables, sizes, RT_MUN_5GY60, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5GY60, y);
                    correctL_v = true;
                }
            } else if (lum < 75.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G70, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7G70, x) + rt_lt_mun(tables, sizes, RT_MUN_5G70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7G70, y) - rt_lt_mun(tables, sizes, RT_MUN_7G70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7G70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G70, x) + rt_lt_mun(tables, sizes, RT_MUN_5G70, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5G70, x) + rt_lt_mun(tables, sizes, RT_MUN_25G70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5G70, y) - rt_lt_mun(tables, sizes, RT_MUN_5G70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5G70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25G70, x) + rt_lt_mun(tables, sizes, RT_MUN_5G70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25G70, x) + rt_lt_mun(tables, sizes, RT_MUN_1G70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25G70, y) - rt_lt_mun(tables, sizes, RT_MUN_25G70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25G70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G70, x) + rt_lt_mun(tables, sizes, RT_MUN_25G70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_1G70, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1G70, y) - rt_lt_mun(tables, sizes, RT_MUN_1G70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1G70, y) ;
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G70, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10GY70, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10GY70, y) - rt_lt_mun(tables, sizes, RT_MUN_10GY70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10GY70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10GY70, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_75GY70, x) + rt_lt_mun(tables, sizes, RT_MUN_5GY70, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75GY70, y) - rt_lt_mun(tables, sizes, RT_MUN_75GY70, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75GY70, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5GY70, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY70, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5GY70, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5GY70, y) - rt_lt_mun(tables, sizes, RT_MUN_5GY70, x) ;
                    lbe_v =  rt_lt_mun(tables, sizes, RT_MUN_5GY70, y);
                    correctL_v = true;
                }
            } else if (lum < 85.f) {
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G80, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_7G80, x) + rt_lt_mun(tables, sizes, RT_MUN_5G80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_7G80, y) - rt_lt_mun(tables, sizes, RT_MUN_7G80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_7G80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_7G80, x) + rt_lt_mun(tables, sizes, RT_MUN_5G80, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5G80, x) + rt_lt_mun(tables, sizes, RT_MUN_25G80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5G80, y) - rt_lt_mun(tables, sizes, RT_MUN_5G80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5G80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25G80, x) + rt_lt_mun(tables, sizes, RT_MUN_5G80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25G80, x) + rt_lt_mun(tables, sizes, RT_MUN_1G80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25G80, y) - rt_lt_mun(tables, sizes, RT_MUN_25G80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25G80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G80, x) + rt_lt_mun(tables, sizes, RT_MUN_25G80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_1G80, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_1G80, y) - rt_lt_mun(tables, sizes, RT_MUN_1G80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_1G80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_1G80, x) + rt_lt_mun(tables, sizes, RT_MUN_10GY80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10GY80, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10GY80, y) - rt_lt_mun(tables, sizes, RT_MUN_10GY80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10GY80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10GY80, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_75GY80, x) + rt_lt_mun(tables, sizes, RT_MUN_5GY80, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_75GY80, y) - rt_lt_mun(tables, sizes, RT_MUN_75GY80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_75GY80, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5GY80, x) + rt_lt_mun(tables, sizes, RT_MUN_75GY80, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5GY80, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5GY80, y) - rt_lt_mun(tables, sizes, RT_MUN_5GY80, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5GY80, y);
                    correctL_v = true;
                }
            }
        }
    } else if (zone == 4) { //Red purple correction : only for L < 30
        if (lum > 5.f) {
            if (lum < 15.f && x < 45) {
                y = min(y, 44);
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5R10, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5R10, x) - 0.043f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5R10, y) - rt_lt_mun(tables, sizes, RT_MUN_5R10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5R10, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25R10, x) + 0.043f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_25R10, x) + rt_lt_mun(tables, sizes, RT_MUN_10RP10, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25R10, y) - rt_lt_mun(tables, sizes, RT_MUN_25R10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25R10, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_25R10, x) + rt_lt_mun(tables, sizes, RT_MUN_10RP10, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10RP10, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10RP10, y) - rt_lt_mun(tables, sizes, RT_MUN_10RP10, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10RP10, y);
                    correctL_v = true;
                }
            } else if (lum < 25.f && x < 70) {
                y = min(y, 70);
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5R20, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5R20, x) + rt_lt_mun(tables, sizes, RT_MUN_25R20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5R20, y) - rt_lt_mun(tables, sizes, RT_MUN_5R20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5R20, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5R20, x) + rt_lt_mun(tables, sizes, RT_MUN_25R20, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10RP20, x) + rt_lt_mun(tables, sizes, RT_MUN_25R20, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25R20, y) - rt_lt_mun(tables, sizes, RT_MUN_25R20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25R20, y);
                    correctL_v = true;
                } else if ( (hue <= (rt_lt_mun(tables, sizes, RT_MUN_10RP20, x) + rt_lt_mun(tables, sizes, RT_MUN_25R20, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10RP20, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10RP20, y) - rt_lt_mun(tables, sizes, RT_MUN_10RP20, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10RP20, y);
                    correctL_v = true;
                }
            } else if (lum < 35.f && x < 85) {
                y = min(y, 85);
                if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5R30, x) + 0.035f)) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_5R30, x) + rt_lt_mun(tables, sizes, RT_MUN_25R30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_5R30, y) - rt_lt_mun(tables, sizes, RT_MUN_5R30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_5R30, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_5R30, x) + rt_lt_mun(tables, sizes, RT_MUN_25R30, x)) * 0.5f) && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10RP30, x) + rt_lt_mun(tables, sizes, RT_MUN_25R30, x)) * 0.5f)) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_25R30, y) - rt_lt_mun(tables, sizes, RT_MUN_25R30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_25R30, y);
                    correctL_v = true;
                } else if ((hue <= (rt_lt_mun(tables, sizes, RT_MUN_10RP30, x) + rt_lt_mun(tables, sizes, RT_MUN_25R30, x)) * 0.5f)  && (hue > (rt_lt_mun(tables, sizes, RT_MUN_10RP30, x) - 0.035f))) {
                    correction_v = rt_lt_mun(tables, sizes, RT_MUN_10RP30, y) - rt_lt_mun(tables, sizes, RT_MUN_10RP30, x) ;
                    lbe_v = rt_lt_mun(tables, sizes, RT_MUN_10RP30, y);
                    correctL_v = true;
                }
            }
        }
    }
    //end red purple
    *correction = correction_v;
    *lbe = lbe_v;
    *correctL = correctL_v;
}

/*
 * Color::AllMunsellLch(Lprov1, HH, Chprov1, CC, correctionHueChroma): the
 * single-correction overload vibrance() calls. The chroma clamps are the
 * host's: 139 for the LUT limit, then 6 for the lowest table entry.
 */
inline float rt_lt_all_munsell_lch(const float Lprov1, const float HH,
                                   const float Chprov1_in, const float CC,
                                   __global const float* tables,
                                   __global const float* sizes)
{
    float correctionHue = 0.f;
    if (CC >= 6.f && CC < 140.f) {
        const float huelimit[8] = {-2.48f, -0.55f, 0.44f, 1.52f,
                                   1.87f, 3.09f, -0.27f, 0.44f};
        float Chprov1 = Chprov1_in;
        if (Chprov1 > 140.f) {
            Chprov1 = 139.f;
        }
        Chprov1 = max(Chprov1, 6.f);
        for (int zo = 1; zo <= 4; ++zo) {
            if (HH > huelimit[2 * zo - 2] && HH < huelimit[2 * zo - 1]) {
                float correctionHueLum = 0.f;
                int correctL = 0;
                rt_lt_munsell_lch(Lprov1, HH, Chprov1, CC, &correctionHue, zo,
                                  &correctionHueLum, &correctL, tables, sizes);
                break;
            }
        }
    }
    return correctionHue;
}
)CLC";
