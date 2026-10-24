#pragma once

#include "stage_abi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtengine::stage
{

using BuiltinFn = RtStageStatus (*)(const RtStageArg*, uint32_t, RtStageProfiling*);

struct StageRequest {
    std::string name;
    std::vector<RtStageArg> args;
    RtStageParamBlob params{};
    int threads = 0;
    bool deterministic = false;
    RtStageConsumer consumer = RT_STAGE_INTERACTIVE;
    std::string impl; // empty: resolve from the environment
};

struct StageModule;

/* A compiled program: a loaded module or a built-in implementation. */
class StageProgram
{
public:
    StageProgram();
    ~StageProgram();

    StageProgram(const StageProgram&) = delete;
    StageProgram& operator=(const StageProgram&) = delete;

    RtStageStatus execute(const std::vector<RtStageArg>& args,
                          RtStageProfiling& profiling) const;

    bool is_module() const;
    const char* name() const;
    const char* version() const;
    const char* source() const;

private:
    friend class StageCompiler;
    std::shared_ptr<StageModule> module_;
    RtStageKernelInfo info_{};
    BuiltinFn builtin_ = nullptr;
    std::string source_;
};

class StageCompiler
{
public:
    static StageCompiler& instance();

    void register_builtin(const char* name, BuiltinFn fn);
    bool has_builtin(const char* name) const;

    /*
     * Resolve a stage request to a program. The resolution order is:
     * explicit request, per-stage environment override, global environment
     * override, module on RT_STAGE_PATH, built-in implementation.
     */
    std::shared_ptr<StageProgram> compile(const StageRequest& request,
                                          std::string* error = nullptr);

    /* Release cached module handles. Call only when no program is alive. */
    void clear_modules();

private:
    StageCompiler();
    ~StageCompiler();
    StageCompiler(const StageCompiler&) = delete;
    StageCompiler& operator=(const StageCompiler&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Small convenience wrapper used by adapters and tests. */
RtStageStatus run(const char* name, const std::vector<RtStageArg>& args,
                  RtStageProfiling* profiling = nullptr,
                  const char* impl = nullptr);

} // namespace rtengine::stage
