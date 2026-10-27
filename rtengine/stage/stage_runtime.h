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

/* Device a node is allowed to run on. CPU is the default. */
enum class StageNodeDevice {
    CPU,
    OpenCL
};

struct StageNodeSelection {
    StageNodeDevice device = StageNodeDevice::CPU;
    /* Whether the node has a device implementation at all. */
    bool implemented = false;
    /* Why the selection is CPU, or why the requested device was refused. */
    std::string reason;
};

/* True when the node has an OpenCL implementation in this build. */
bool node_has_device_implementation(const char* node);

/*
 * Explicit selection for one node: the caller asks for a device and gets the
 * resolved choice. A node without a device implementation, a disabled device
 * or an active kill switch resolves to CPU with a reason.
 */
StageNodeSelection select_node_device(const char* node,
                                      StageNodeDevice requested);

/*
 * Environment selection. The default is CPU; RT_STAGE_OPENCL=1 opts in and
 * RT_STAGE_OPENCL_DISABLE=1 (or RT_STAGE_OPENCL=0) is the kill switch.
 */
StageNodeSelection select_node_device(const char* node);

/*
 * Process-wide kill switch, wired from the user's OpenCL preference. A false
 * value forces every node to CPU whatever the environment requests; the
 * default is true so an embedding without a preference still honours the
 * opt-in switch.
 */
void set_opencl_stage_allowed(bool allowed);
bool opencl_stage_allowed();

/* Explicit per-node override, for tests and embedding. The kill switch wins. */
void set_node_device_override(const char* node, StageNodeDevice device);
void clear_node_device_overrides();

} // namespace rtengine::stage
