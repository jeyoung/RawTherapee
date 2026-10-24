#include "stage_runtime.h"
#include "stage_lanczos.h"

namespace rtengine::stage
{

void register_builtin_stages(StageCompiler& compiler)
{
    compiler.register_builtin("lanczos3", &lanczos_execute);
}

} // namespace rtengine::stage
