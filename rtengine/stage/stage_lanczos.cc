#include "stage_lanczos.h"

#include "algorithms/lanczos.h"

#include <cstdint>

namespace rtengine::stage
{

namespace
{

bool buffer_ok(const RtStageBuffer& buffer)
{
    return buffer.size == sizeof(RtStageBuffer) && buffer.data != nullptr &&
           buffer.width > 0 && buffer.height > 0 && buffer.planes > 0 &&
           buffer.row_pitch >= static_cast<size_t>(buffer.width) * sizeof(float);
}

} // namespace

RtStageStatus lanczos_execute(const RtStageArg* args, uint32_t nargs,
                              RtStageProfiling* profiling)
{
    if (args == nullptr || nargs < 4 || args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_BUFFER || args[2].kind != RT_STAGE_ARG_DOUBLE ||
        args[3].kind != RT_STAGE_ARG_INT) {
        return RT_STAGE_INVALID;
    }

    const RtStageBuffer& src = args[0].u.buffer;
    const RtStageBuffer& dst = args[1].u.buffer;
    if (!buffer_ok(src) || !buffer_ok(dst) || src.format != RT_STAGE_F32 ||
        dst.format != RT_STAGE_F32 || src.planes != 3 || dst.planes != 3) {
        return RT_STAGE_INVALID;
    }

    const float* src_planes[3] = {
        static_cast<const float*>(src.data),
        reinterpret_cast<const float*>(static_cast<const unsigned char*>(src.data) +
                                       src.plane_pitch),
        reinterpret_cast<const float*>(static_cast<const unsigned char*>(src.data) +
                                       2 * src.plane_pitch)
    };
    float* dst_planes[3] = {
        static_cast<float*>(dst.data),
        reinterpret_cast<float*>(static_cast<unsigned char*>(dst.data) + dst.plane_pitch),
        reinterpret_cast<float*>(static_cast<unsigned char*>(dst.data) +
                                 2 * dst.plane_pitch)
    };

    algorithms::lanczos3(src_planes, src.width, src.height, src.row_pitch,
                         src.plane_pitch, dst_planes, dst.width, dst.height,
                         dst.row_pitch, dst.plane_pitch,
                         static_cast<float>(args[2].u.f),
                         static_cast<int>(args[3].u.i));

    if (profiling != nullptr) {
        profiling->flags = 0;
    }
    return RT_STAGE_OK;
}

} // namespace rtengine::stage
