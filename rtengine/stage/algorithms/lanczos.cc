#include "lanczos.h"

#include "rt_math.h"
#include "sleef.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace rtengine::stage::algorithms
{

namespace
{

inline float lanc(float x, float a)
{
    if (x * x < 1e-6f) {
        return 1.0f;
    }
    if (x * x > a * a) {
        return 0.0f;
    }
    x = static_cast<float>(rtengine::RT_PI) * x;
    return a * xsinf(x) * xsinf(x / a) / (x * x);
}

} // namespace

void lanczos3(const float* const src[3], int src_width, int src_height,
              size_t src_row_pitch, size_t src_plane_pitch,
              float* const dst[3], int dst_width, int dst_height,
              size_t dst_row_pitch, size_t dst_plane_pitch,
              float scale, int threads)
{
    const float delta = 1.0f / scale;
    const float a = 3.0f;
    const float sc = std::min(scale, 1.0f);
    const int support = static_cast<int>(2.0f * a / sc) + 1;

    const size_t src_pitch = src_row_pitch / sizeof(float);
    const size_t dst_pitch = dst_row_pitch / sizeof(float);
    const float* const src0 = src[0];
    const float* const src1 = src[1];
    const float* const src2 = src[2];
    float* const dst0 = dst[0];
    float* const dst1 = dst[1];
    float* const dst2 = dst[2];

    /*
     * The horizontal taps are identical for every output row. Computing them
     * once outside the row loop avoids repeating the same transcendental
     * work in every worker thread.
     */
    std::vector<float> wwh(static_cast<size_t>(support) * dst_width);
    std::vector<int> jj0(dst_width);
    std::vector<int> jj1(dst_width);

#ifdef _OPENMP
#pragma omp parallel for num_threads(threads > 0 ? threads : omp_get_max_threads())
#endif
    for (int j = 0; j < dst_width; ++j) {
        const float x0 = (static_cast<float>(j) + 0.5f) * delta - 0.5f;
        float* const w = wwh.data() + static_cast<size_t>(j) * support;
        float ws = 0.0f;

        jj0[j] = std::max(0, static_cast<int>(floorf(x0 - a / sc)) + 1);
        jj1[j] = std::min(src_width,
                          static_cast<int>(floorf(x0 + a / sc)) + 1);

        for (int jj = jj0[j]; jj < jj1[j]; ++jj) {
            const int k = jj - jj0[j];
            const float z = sc * (x0 - static_cast<float>(jj));
            w[k] = lanc(z, a);
            ws += w[k];
        }
        for (int k = 0; k < support; ++k) {
            w[k] /= ws;
        }
    }

#ifdef _OPENMP
#pragma omp parallel num_threads(threads > 0 ? threads : omp_get_max_threads())
#endif
    {
        float* const lr = new float[src_width];
        float* const lg = new float[src_width];
        float* const lb = new float[src_width];

#ifdef _OPENMP
#pragma omp for
#endif
        for (int i = 0; i < dst_height; ++i) {
            const float y0 = (static_cast<float>(i) + 0.5f) * delta - 0.5f;
            float w[support];
            for (float& value : w) {
                value = 0.0f;
            }
            float ws = 0.0f;

            const int ii0 = std::max(0, static_cast<int>(floorf(y0 - a / sc)) + 1);
            const int ii1 = std::min(src_height,
                                     static_cast<int>(floorf(y0 + a / sc)) + 1);

            for (int ii = ii0; ii < ii1; ++ii) {
                const int k = ii - ii0;
                const float z = sc * (y0 - static_cast<float>(ii));
                w[k] = lanc(z, a);
                ws += w[k];
            }
            for (int k = 0; k < support; ++k) {
                w[k] /= ws;
            }

            std::fill(lr, lr + src_width, 0.0f);
            std::fill(lg, lg + src_width, 0.0f);
            std::fill(lb, lb + src_width, 0.0f);

            for (int ii = ii0; ii < ii1; ++ii) {
                const int k = ii - ii0;
                const float* const r0 = src0 + static_cast<size_t>(ii) * src_pitch;
                const float* const r1 = src1 + static_cast<size_t>(ii) * src_pitch;
                const float* const r2 = src2 + static_cast<size_t>(ii) * src_pitch;
                for (int j = 0; j < src_width; ++j) {
                    lr[j] += w[k] * r0[j];
                    lg[j] += w[k] * r1[j];
                    lb[j] += w[k] * r2[j];
                }
            }

            float* const out0 = dst0 + static_cast<size_t>(i) * dst_pitch;
            float* const out1 = dst1 + static_cast<size_t>(i) * dst_pitch;
            float* const out2 = dst2 + static_cast<size_t>(i) * dst_pitch;
            for (int j = 0; j < dst_width; ++j) {
                const float* const wh =
                    wwh.data() + static_cast<size_t>(support) * j;
                float r = 0.0f;
                float g = 0.0f;
                float b = 0.0f;
                for (int jj = jj0[j]; jj < jj1[j]; ++jj) {
                    const int k = jj - jj0[j];
                    r += wh[k] * lr[jj];
                    g += wh[k] * lg[jj];
                    b += wh[k] * lb[jj];
                }
                out0[j] = r;
                out1[j] = g;
                out2[j] = b;
            }
        }

        delete[] lr;
        delete[] lg;
        delete[] lb;
    }

    (void)src_plane_pitch;
    (void)dst_plane_pitch;
}

} // namespace rtengine::stage::algorithms
