#pragma once

#include "stage_abi.h"

namespace rtengine::stage
{

RtStageStatus lanczos_execute(const RtStageArg* args, uint32_t nargs,
                              RtStageProfiling* profiling);

} // namespace rtengine::stage
