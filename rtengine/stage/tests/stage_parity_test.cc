#include "stage_buffer.h"
#include "stage_parity.h"
#include "stage_runtime.h"

#include "rtengine/imagefloat.h"

#include <cstdio>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

RtStageArg buffer_arg(const RtStageBuffer& buffer)
{
    RtStageArg arg{};
    arg.kind = RT_STAGE_ARG_BUFFER;
    arg.u.buffer = buffer;
    return arg;
}

RtStageArg double_arg(double value)
{
    RtStageArg arg{};
    arg.kind = RT_STAGE_ARG_DOUBLE;
    arg.u.f = value;
    return arg;
}

RtStageArg int_arg(int64_t value)
{
    RtStageArg arg{};
    arg.kind = RT_STAGE_ARG_INT;
    arg.u.i = value;
    return arg;
}

bool execute(const char* impl, rtengine::Imagefloat& src, rtengine::Imagefloat& dst,
             std::string& error)
{
    using namespace rtengine::stage;
    StageRequest request;
    request.name = "lanczos3";
    request.impl = impl;
    request.args = {buffer_arg(view(src)), buffer_arg(view(dst)), double_arg(0.5),
                    int_arg(2)};
    auto program = StageCompiler::instance().compile(request, &error);
    if (!program) {
        return false;
    }
    RtStageProfiling profiling{};
    return program->execute(request.args, profiling) == RT_STAGE_OK;
}

} // namespace

int main()
{
    using namespace rtengine;

    Imagefloat src(64, 48);
    Imagefloat builtin(32, 24);
    Imagefloat module(32, 24);

    for (int y = 0; y < src.getHeight(); ++y) {
        for (int x = 0; x < src.getWidth(); ++x) {
            src.r(y)[x] = static_cast<float>((x * 17 + y * 31) % 65536);
            src.g(y)[x] = static_cast<float>((x * 7 + y * 13) % 65536);
            src.b(y)[x] = static_cast<float>((x * 3 + y * 5) % 65536);
        }
    }

    std::string error;
    check(execute("builtin", src, builtin, error), "built-in Lanczos executes");
    check(execute("module", src, module, error), "module Lanczos executes");

    const auto result = rtengine::stage::compare(
        rtengine::stage::view(builtin), rtengine::stage::view(module));
    check(result.same_geometry, "built-in and module geometry match");
    check(result.differing == 0, "built-in and module pixels match exactly");
    check(result.max_abs == 0, "built-in and module maximum difference is zero");

    float one = 1.0f;
    float one_and_half = 1.5f;
    RtStageBuffer a{};
    a.size = sizeof(a);
    a.data = &one;
    a.width = 1;
    a.height = 1;
    a.row_pitch = sizeof(float);
    a.plane_pitch = sizeof(float);
    a.planes = 1;
    a.format = RT_STAGE_F32;
    RtStageBuffer b = a;
    b.data = &one_and_half;
    const auto fractional = rtengine::stage::compare(a, b);
    check(fractional.differing == 1, "fractional difference is detected");
    check(fractional.max_abs == 0.5, "fractional maximum difference is exact");

    if (failures == 0) {
        std::puts("stage-parity-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
