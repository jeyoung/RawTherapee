#include "stage_runtime.h"
#include "stage_lanczos.h"
#include "stage/algorithms/sharpen_cl.h"
#include "sharpen_core.h"

#include <string>

namespace rtengine::stage
{

namespace
{

/*
 * Host implementation of the output sharpening node. It reconstructs every
 * field the core reads, including the residual parameter sets, so a request
 * that the device refuses still produces the CPU result and is recorded as a
 * host execution.
 */
RtStageStatus sharpen_execute(const RtStageArg* args, uint32_t nargs,
                              RtStageProfiling* profiling)
{
    (void)profiling;

    SharpenNodeParams node;
    RtStageBuffer buffer{};
    std::string error;
    if (!sharpen_node_decode(args, nargs, node, buffer, error)) {
        return RT_STAGE_INVALID;
    }

    procparams::SharpeningParams params;
    sharpen_apply_params(node, params);

    sharpen_luminance(static_cast<float*>(buffer.data), buffer.width,
                      buffer.height, params, node.scale, true,
                      node.showMask);
    return RT_STAGE_OK;
}

} // namespace

void register_builtin_stages(StageCompiler& compiler)
{
    compiler.register_builtin("lanczos3", &lanczos_execute);
    compiler.register_builtin("output.sharpen", &sharpen_execute);
}

} // namespace rtengine::stage
