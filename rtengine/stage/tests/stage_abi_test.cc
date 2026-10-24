#include "stage_runtime.h"

#include <cstdio>
#include <string>

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

} // namespace

int main()
{
    using namespace rtengine::stage;

    static_assert(RT_STAGE_ABI_VERSION == 1, "unexpected ABI version");
    static_assert(sizeof(RtStageBuffer) >= 48, "buffer descriptor too small");

    StageCompiler& compiler = StageCompiler::instance();
    check(compiler.has_builtin("lanczos3"), "lanczos3 built-in is registered");

    StageRequest missing;
    missing.name = "does-not-exist";
    std::string error;
    check(compiler.compile(missing, &error) == nullptr, "unknown stage is rejected");
    check(!error.empty(), "unknown stage reports an error");

    StageRequest request;
    request.name = "lanczos3";
    request.impl = "builtin";
    auto program = compiler.compile(request, &error);
    check(program != nullptr, "built-in stage compiles");
    if (program) {
        check(!program->is_module(), "built-in source is reported");
        check(std::string(program->name()) == "lanczos3", "stage name is preserved");
    }

    if (failures == 0) {
        std::puts("stage-abi-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
