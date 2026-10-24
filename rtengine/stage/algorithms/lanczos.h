#pragma once

#include <cstddef>

namespace rtengine::stage::algorithms
{

/*
 * Three-plane floating-point Lanczos resize. The implementation is shared by
 * the host built-in adapter and the loadable module so both execute the same
 * arithmetic in the same order.
 */
void lanczos3(const float* const src[3], int src_width, int src_height,
              size_t src_row_pitch, size_t src_plane_pitch,
              float* const dst[3], int dst_width, int dst_height,
              size_t dst_row_pitch, size_t dst_plane_pitch,
              float scale, int threads);

} // namespace rtengine::stage::algorithms
