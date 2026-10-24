#include "stage_abi.h"
#include "stage_lanczos.h"

extern "C" RT_STAGE_EXPORT const RtStageKernelInfo* rt_stage_query(uint32_t abi_version)
{
    static const RtStageKernelInfo info = {
        sizeof(RtStageKernelInfo),
        "lanczos3",
        "1",
        0,
        &rtengine::stage::lanczos_execute
    };
    return abi_version == RT_STAGE_ABI_VERSION ? &info : nullptr;
}
