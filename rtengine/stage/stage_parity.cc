#include "stage_parity.h"

#include "stage_profile.h"

#include <cmath>
#include <cstring>
#include <type_traits>

namespace rtengine::stage
{

namespace
{

template<typename T>
void compare_typed(const RtStageBuffer& a, const RtStageBuffer& b, ParityResult& result)
{
    for (uint32_t plane = 0; plane < a.planes; ++plane) {
        const T* pa = reinterpret_cast<const T*>(
            static_cast<const unsigned char*>(a.data) +
            static_cast<size_t>(plane) * a.plane_pitch);
        const T* pb = reinterpret_cast<const T*>(
            static_cast<const unsigned char*>(b.data) +
            static_cast<size_t>(plane) * b.plane_pitch);
        for (int y = 0; y < a.height; ++y) {
            const T* ra = reinterpret_cast<const T*>(
                reinterpret_cast<const unsigned char*>(pa) +
                static_cast<size_t>(y) * a.row_pitch);
            const T* rb = reinterpret_cast<const T*>(
                reinterpret_cast<const unsigned char*>(pb) +
                static_cast<size_t>(y) * b.row_pitch);
            for (int x = 0; x < a.width; ++x) {
                ++result.total;
                if constexpr (std::is_floating_point_v<T>) {
                    if (std::isnan(static_cast<double>(ra[x])) ||
                        std::isnan(static_cast<double>(rb[x]))) {
                        if (!(std::isnan(static_cast<double>(ra[x])) &&
                              std::isnan(static_cast<double>(rb[x])))) {
                            ++result.differing;
                        }
                        continue;
                    }
                }
                const int64_t diff = static_cast<int64_t>(ra[x]) -
                                     static_cast<int64_t>(rb[x]);
                const int64_t magnitude = diff < 0 ? -diff : diff;
                if (magnitude != 0) {
                    ++result.differing;
                }
                if (magnitude > result.max_abs) {
                    result.max_abs = magnitude;
                }
            }
        }
    }
}

} // namespace

ParityResult compare(const RtStageBuffer& a, const RtStageBuffer& b)
{
    ParityResult result;
    result.hash_a = hash_buffer(a);
    result.hash_b = hash_buffer(b);
    result.same_geometry = a.width == b.width && a.height == b.height &&
                           a.planes == b.planes && a.format == b.format;
    if (!result.same_geometry) {
        return result;
    }

    switch (a.format) {
    case RT_STAGE_F32:
        compare_typed<float>(a, b, result);
        break;
    case RT_STAGE_U16:
        compare_typed<uint16_t>(a, b, result);
        break;
    case RT_STAGE_U8:
        compare_typed<uint8_t>(a, b, result);
        break;
    }
    return result;
}

bool passes(const ParityResult& result, int64_t tolerance)
{
    return result.same_geometry && result.differing == 0 && result.max_abs <= tolerance;
}

} // namespace rtengine::stage
