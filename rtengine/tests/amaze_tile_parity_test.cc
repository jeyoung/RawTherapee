/*
 *  This file is part of RawTherapee.
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

#include "amaze_demosaic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct Planes
{
    array2D<float> red;
    array2D<float> green;
    array2D<float> blue;

    Planes(int width, int height) :
        red(width, height), green(width, height), blue(width, height)
    {
        const float sentinel = std::numeric_limits<float>::quiet_NaN();
        for (int y = 0; y < height; ++y) {
            std::fill_n(red[y], width, sentinel);
            std::fill_n(green[y], width, sentinel);
            std::fill_n(blue[y], width, sentinel);
        }
    }
};

void fillRaw(array2D<float>& raw,
             int width,
             int height,
             const unsigned int cfa[2][2],
             std::uint32_t seed)
{
    const float scales[3] = { 0.82f, 1.0f, 0.69f };
    std::uint32_t state = seed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const float noise =
                (static_cast<float>(state & 65535u) / 65535.f - 0.5f) * 0.02f;
            const float ramp = 0.35f + 0.12f * x / width + 0.08f * y / height;
            const bool texture = x >= 128 && (y % 128 < 20 || y % 128 > 104);
            const float checker = ((x / 2 + y / 2) & 1) ? 0.045f : -0.045f;
            const float diagonal = ((x + 2 * y) % 3 - 1) * 0.025f;
            const float value = ramp + noise + (texture ? checker + diagonal : 0.f);
            raw[y][x] = 65535.f * value * scales[cfa[y & 1][x & 1]];
        }
    }
}

bool complete(const Planes& output, int width, int height)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!std::isfinite(output.red[y][x]) || !std::isfinite(output.green[y][x])
                || !std::isfinite(output.blue[y][x]))
            {
                return false;
            }
        }
    }
    return true;
}

std::size_t differences(const Planes& a, const Planes& b, int width, int height)
{
    std::size_t count = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            count += std::memcmp(&a.red[y][x], &b.red[y][x], sizeof(float)) != 0;
            count += std::memcmp(&a.green[y][x], &b.green[y][x], sizeof(float)) != 0;
            count += std::memcmp(&a.blue[y][x], &b.blue[y][x], sizeof(float)) != 0;
        }
    }
    return count;
}

void runCase(int width, int height, const unsigned int cfa[2][2], std::uint32_t seed)
{
    array2D<float> raw(width, height);
    fillRaw(raw, width, height, cfa, seed);
    std::vector<float> original(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        std::copy_n(raw[y], width, original.data() + static_cast<std::size_t>(y) * width);
    }
    Planes reference(width, height);
#ifdef _OPENMP
    omp_set_num_threads(1);
#endif
    rtengine::detail::amazeDemosaic(0, 0, width, height, raw, reference.red,
                                    reference.green, reference.blue, cfa, 1.0, nullptr,
                                    1);
    check(complete(reference, width, height), "all reference samples are written");

    for (int threads : { 1, 2, 8 }) {
#ifdef _OPENMP
        omp_set_num_threads(threads);
        int actual = 0;
#pragma omp parallel
        {
#pragma omp single
            actual = omp_get_num_threads();
        }
        check(actual == threads, "requested OpenMP team is available");
#endif
        for (std::size_t chunk : { std::size_t{ 1 }, std::size_t{ 3 } }) {
            for (int repeat = 0; repeat < 2; ++repeat) {
                Planes output(width, height);
                rtengine::detail::amazeDemosaic(0, 0, width, height, raw, output.red,
                                                output.green, output.blue, cfa, 1.0,
                                                nullptr, chunk);
                check(complete(output, width, height), "all output samples are written");
                const std::size_t changed = differences(reference, output, width, height);
                if (changed != 0) {
                    std::fprintf(stderr,
                                 "AMaZE %dx%d seed=%u threads=%d chunk=%zu "
                                 "repeat=%d differs in %zu float samples\n",
                                 width, height, seed, threads, chunk, repeat, changed);
                }
                check(changed == 0, "tile scheduling preserves every float sample");
                for (int y = 0; y < height; ++y) {
                    check(
                        std::memcmp(raw[y],
                                    original.data() + static_cast<std::size_t>(y) * width,
                                    sizeof(float) * width)
                            == 0,
                        "CFA input remains unchanged");
                }
            }
        }
    }
}

}  // namespace

int main()
{
#ifndef _OPENMP
    std::fprintf(stderr, "SKIP: OpenMP is required for tile scheduling coverage\n");
    return 77;
#else
    omp_set_dynamic(0);
    omp_set_max_active_levels(1);
    const unsigned int patterns[4][2][2] = { { { 0, 1 }, { 1, 2 } },
                                             { { 1, 0 }, { 2, 1 } },
                                             { { 1, 2 }, { 0, 1 } },
                                             { { 2, 1 }, { 1, 0 } } };
    for (const auto& cfa : patterns) {
        for (const auto& size :
             { std::pair<int, int>{ 440, 440 }, { 440, 568 }, { 512, 512 } })
        {
            for (std::uint32_t seed : { 0x1234'5678u, 0x9abc'def1u }) {
                runCase(size.first, size.second, cfa, seed);
                if (failures != 0) {
                    return 1;
                }
            }
        }
    }
    std::puts(
        "amaze-tile-parity-test: PASS (4 Bayer patterns, 3 sizes, "
        "2 seeds, threads 1/2/8, chunks 1/3, 2 repeats)");
    return 0;
#endif
}
