#include "stage_buffer.h"
#include "stage_parity.h"
#include "stage_runtime.h"

#include "rtengine/imagefloat.h"

#include <cstdint>
#include <cstdio>
#include <limits>
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
    check(!rtengine::stage::passes_exact(fractional),
          "the strict form rejects a fractional difference");
    check(rtengine::stage::passes_within(fractional, 1.0),
          "the tolerance form accepts a difference inside the bound");

    /*
     * Packed U8 RGB: one plane, three samples per pixel. The last byte of the
     * row must be compared, not only the first `width` bytes.
     */
    std::uint8_t packed_a[6] = {1, 2, 3, 4, 5, 6};
    std::uint8_t packed_b[6] = {1, 2, 3, 4, 5, 7};
    RtStageBuffer u8{};
    u8.size = sizeof(u8);
    u8.data = packed_a;
    u8.width = 2;
    u8.height = 1;
    u8.row_pitch = sizeof(packed_a);
    u8.plane_pitch = sizeof(packed_a);
    u8.planes = 1;
    u8.format = RT_STAGE_U8;
    RtStageBuffer u8_other = u8;
    u8_other.data = packed_b;
    const auto packed = rtengine::stage::compare(u8, u8_other);
    check(packed.differing == 1, "the last packed U8 channel is compared");
    check(packed.total == 6, "packed U8 samples the whole row");

    /*
     * Non-finite values: matching NaNs are equal, a single NaN differs, and
     * infinities compare by sign rather than by abs(inf - inf).
     */
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    const float inf_value = std::numeric_limits<float>::infinity();
    float nan_a = nan_value;
    float nan_b = nan_value;
    RtStageBuffer fa = a;
    fa.data = &nan_a;
    RtStageBuffer fb = fa;
    fb.data = &nan_b;
    const auto matching_nan = rtengine::stage::compare(fa, fb);
    check(matching_nan.differing == 0, "matching NaNs compare equal");
    check(rtengine::stage::passes_exact(matching_nan),
          "matching NaNs pass the strict form");

    float finite = 1.0f;
    RtStageBuffer ff = fa;
    ff.data = &finite;
    const auto one_nan = rtengine::stage::compare(fa, ff);
    check(one_nan.differing == 1, "a single NaN differs");

    float pos_inf = inf_value;
    float neg_inf = -inf_value;
    RtStageBuffer fi = fa;
    fi.data = &pos_inf;
    RtStageBuffer fj = fi;
    fj.data = &pos_inf;
    check(rtengine::stage::compare(fi, fj).differing == 0,
          "equal infinities compare equal");
    RtStageBuffer fk = fi;
    fk.data = &neg_inf;
    check(rtengine::stage::compare(fi, fk).differing == 1,
          "infinities of opposite sign differ");

    if (failures == 0) {
        std::puts("stage-parity-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
