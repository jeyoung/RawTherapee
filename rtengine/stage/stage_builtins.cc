#include "stage_runtime.h"
#include "stage_buffer.h"
#include "lab_tools_context.h"
#include "stage_lanczos.h"
#include "working_trc_context.h"
#include "stage/algorithms/sharpen_cl.h"
#include "sharpen_core.h"

#include <cmath>
#include <cstdint>
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

/*
 * Host implementation of the working.trc node.
 *
 * The device launcher refuses a residual configuration before it touches the
 * image, and a refused or unsupported configuration arrives here because the
 * device queue falls back to the compiled program. The builtin then runs the
 * original host block through the call-site context, so the result is the CPU
 * result and the runtime records impl=host. Device recursion is disabled for
 * the duration: the host block must not re-enter this node.
 */
RtStageStatus working_trc_execute(const RtStageArg* args, uint32_t nargs,
                                  RtStageProfiling* profiling)
{
    (void)profiling;

    constexpr uint32_t ARG_COUNT = 6;
    if (nargs != ARG_COUNT || args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_PTR ||
        args[2].kind != RT_STAGE_ARG_INT ||
        args[3].kind != RT_STAGE_ARG_DOUBLE) {
        return RT_STAGE_INVALID;
    }

    RtStageBuffer lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        return RT_STAGE_INVALID;
    }

    RtWorkingTrcContext* context =
        const_cast<RtWorkingTrcContext*>(
            static_cast<const RtWorkingTrcContext*>(args[1].u.ptr));
    if (context == nullptr || context->size != sizeof(RtWorkingTrcContext) ||
        context->version != RT_WORKING_TRC_CONTEXT_VERSION ||
        context->magic != RT_WORKING_TRC_CONTEXT_MAGIC ||
        context->run_host == nullptr) {
        return RT_STAGE_INVALID;
    }

    const std::int64_t supported = args[2].u.i;
    if (supported != 0 && supported != 1) {
        return RT_STAGE_INVALID;
    }
    const double saturation = args[3].u.f;
    if (!std::isfinite(saturation) || saturation < 0.0 || saturation > 1.0) {
        return RT_STAGE_INVALID;
    }

    const bool allowed = opencl_stage_allowed();
    set_opencl_stage_allowed(false);
    const int status = context->run_host(context->self, &lab);
    set_opencl_stage_allowed(allowed);
    return status == 0 ? RT_STAGE_OK : RT_STAGE_ERROR;
}

/*
 * Host implementation of the lab.tools node.
 *
 * The device launcher refuses a residual configuration before it touches the
 * image, and a refused or unsupported configuration arrives here because the
 * device queue falls back to the compiled program. The builtin then runs the
 * original host block through the call-site context, so the result is the CPU
 * result and the runtime records impl=host. Device recursion is disabled for
 * the duration: the host block must not re-enter this node.
 */
RtStageStatus lab_tools_execute(const RtStageArg* args, uint32_t nargs,
                                RtStageProfiling* profiling)
{
    (void)profiling;

    constexpr uint32_t ARG_COUNT = 22;
    if (nargs != ARG_COUNT || args[0].kind != RT_STAGE_ARG_BUFFER ||
        args[1].kind != RT_STAGE_ARG_PTR ||
        args[2].kind != RT_STAGE_ARG_INT || args[3].kind != RT_STAGE_ARG_INT) {
        return RT_STAGE_INVALID;
    }
    for (uint32_t index = 4; index <= 7; ++index) {
        if (args[index].kind != RT_STAGE_ARG_DOUBLE) {
            return RT_STAGE_INVALID;
        }
    }
    for (uint32_t index = 8; index <= 15; ++index) {
        if (args[index].kind != RT_STAGE_ARG_INT) {
            return RT_STAGE_INVALID;
        }
    }
    for (uint32_t index = 16; index <= 21; ++index) {
        if (args[index].kind != RT_STAGE_ARG_BUFFER) {
            return RT_STAGE_INVALID;
        }
    }

    RtStageBuffer lab = args[0].u.buffer;
    if (!valid(lab) || lab.format != RT_STAGE_F32 || lab.planes != 3) {
        return RT_STAGE_INVALID;
    }

    const std::int64_t supported = args[2].u.i;
    if (supported != 0 && supported != 1) {
        return RT_STAGE_INVALID;
    }

    RtLabToolsContext* context =
        const_cast<RtLabToolsContext*>(
            static_cast<const RtLabToolsContext*>(args[1].u.ptr));
    if (context == nullptr || context->size != sizeof(RtLabToolsContext) ||
        context->version != RT_LAB_TOOLS_CONTEXT_VERSION ||
        context->magic != RT_LAB_TOOLS_CONTEXT_MAGIC ||
        context->run_host == nullptr) {
        return RT_STAGE_INVALID;
    }

    const bool allowed = opencl_stage_allowed();
    set_opencl_stage_allowed(false);
    const int status = context->run_host(context->self, &lab);
    set_opencl_stage_allowed(allowed);
    return status == 0 ? RT_STAGE_OK : RT_STAGE_ERROR;
}

} // namespace

void register_builtin_stages(StageCompiler& compiler)
{
    compiler.register_builtin("lanczos3", &lanczos_execute);
    compiler.register_builtin("output.sharpen", &sharpen_execute);
    compiler.register_builtin("working.trc", &working_trc_execute);
    compiler.register_builtin("lab.tools", &lab_tools_execute);
}

} // namespace rtengine::stage
