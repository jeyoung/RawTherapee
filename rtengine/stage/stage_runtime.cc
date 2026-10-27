#include "stage_runtime.h"

#include "stage_device.h"
#include "stage_opencl_device.h"
#include "stage_profile.h"

#include <glib.h>
#include <glibmm/module.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <vector>

#ifndef RT_STAGE_BUILD_DIR
#define RT_STAGE_BUILD_DIR ""
#endif

namespace rtengine::stage
{

void register_builtin_stages(StageCompiler& compiler);

namespace
{

std::string upper_stage_name(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        } else {
            result.push_back('_');
        }
    }
    return result;
}

std::vector<std::string> search_paths()
{
    std::vector<std::string> paths;
    const char* env = std::getenv("RT_STAGE_PATH");
    if (env != nullptr) {
        std::string value(env);
        size_t begin = 0;
        while (begin <= value.size()) {
            const size_t end = value.find(':', begin);
            const std::string item = value.substr(begin, end == std::string::npos
                                                             ? std::string::npos
                                                             : end - begin);
            if (!item.empty()) {
                paths.push_back(item);
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    if (RT_STAGE_BUILD_DIR[0] != '\0') {
        paths.emplace_back(RT_STAGE_BUILD_DIR);
    }
    return paths;
}

bool file_stamp(const std::string& path, int64_t& mtime, int64_t& size)
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    mtime = static_cast<int64_t>(st.st_mtime);
    size = static_cast<int64_t>(st.st_size);
    return true;
}

/*
 * Device selection. CPU is the default; RT_STAGE_OPENCL=1 opts in and the
 * kill switches force CPU. The user preference is wired in through
 * set_opencl_stage_allowed(), so a disabled backend never runs a node on the
 * device even when the environment asks for it.
 */
std::atomic<bool> opencl_allowed{true};
std::mutex selection_mutex;
std::map<std::string, StageNodeDevice> node_overrides;

bool env_flag(const char* name, bool& is_set)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        is_set = false;
        return false;
    }
    is_set = true;
    return g_ascii_strcasecmp(value, "0") != 0 &&
           g_ascii_strcasecmp(value, "off") != 0 &&
           g_ascii_strcasecmp(value, "false") != 0 &&
           g_ascii_strcasecmp(value, "no") != 0;
}

bool has_override(const std::string& node, StageNodeDevice& device)
{
    std::lock_guard<std::mutex> lock(selection_mutex);
    const auto found = node_overrides.find(node);
    if (found == node_overrides.end()) {
        return false;
    }
    device = found->second;
    return true;
}

StageNodeSelection select_impl(const char* node, StageNodeDevice requested)
{
    const std::string name = node != nullptr ? node : "";
    StageNodeSelection selection;
    selection.implemented = node_has_device_implementation(node);

    if (requested == StageNodeDevice::CPU) {
        selection.device = StageNodeDevice::CPU;
        selection.reason = selection.implemented
                               ? "CPU selected"
                               : "no OpenCL implementation for node '" + name + "'";
        return selection;
    }

    /* Kill switches win over every request. */
    if (!opencl_allowed.load(std::memory_order_acquire)) {
        selection.device = StageNodeDevice::CPU;
        selection.reason = "OpenCL stage execution is disabled by the user preference";
        return selection;
    }
    bool set = false;
    const bool opt_in = env_flag("RT_STAGE_OPENCL", set);
    if (set && !opt_in) {
        /* An explicit RT_STAGE_OPENCL=0 is a kill switch. */
        selection.device = StageNodeDevice::CPU;
        selection.reason = "RT_STAGE_OPENCL is off";
        return selection;
    }
    if (env_flag("RT_STAGE_OPENCL_DISABLE", set) && set) {
        selection.device = StageNodeDevice::CPU;
        selection.reason = "RT_STAGE_OPENCL_DISABLE is set";
        return selection;
    }

    if (!selection.implemented) {
        selection.device = StageNodeDevice::CPU;
        selection.reason = "no OpenCL implementation for node '" + name + "'";
        return selection;
    }

    std::string device_reason;
    if (!opencl_stage_available(&device_reason)) {
        selection.device = StageNodeDevice::CPU;
        selection.reason = device_reason;
        return selection;
    }

    selection.device = StageNodeDevice::OpenCL;
    selection.reason.clear();
    return selection;
}

/* Lazily created device context shared by stage::run(). */
std::shared_ptr<StageContext> opencl_context()
{
    static std::mutex mutex;
    static std::shared_ptr<StageContext> context;
    static bool tried = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (!tried) {
        tried = true;
        if (opencl_stage_available(nullptr)) {
            auto device = opencl_device();
            if (device != nullptr) {
                context = device->create_context();
            }
        }
    }
    return context;
}

/* Profile record shared by the host and device executions. */
void record_stage(const char* name, const char* impl, const char* source,
                  const std::vector<RtStageArg>& args,
                  const RtStageProfiling& local)
{
    if (!profiling_enabled()) {
        return;
    }
    StageRecord record;
    record.stage = name != nullptr ? name : "";
    record.impl = impl;
    record.source = source;
    record.consumer = "interactive";
    record.elapsed_ns = local.elapsed_ns;
    record.width = !args.empty() && args[0].kind == RT_STAGE_ARG_BUFFER
                       ? args[0].u.buffer.width
                       : 0;
    record.height = !args.empty() && args[0].kind == RT_STAGE_ARG_BUFFER
                        ? args[0].u.buffer.height
                        : 0;
    record.cache_hit = local.cache_hit;
    if (!args.empty() && args[0].kind == RT_STAGE_ARG_BUFFER) {
        record.input_hash = hash_buffer(args[0].u.buffer);
    }
    if (args.size() > 1 && args[1].kind == RT_STAGE_ARG_BUFFER) {
        record.output_hash = hash_buffer(args[1].u.buffer);
    }
    profile(record);
}

} // namespace

bool node_has_device_implementation(const char* node)
{
    return node != nullptr && has_opencl_stage_node(node);
}

StageNodeSelection select_node_device(const char* node,
                                      StageNodeDevice requested)
{
    return select_impl(node, requested);
}

StageNodeSelection select_node_device(const char* node)
{
    const std::string name = node != nullptr ? node : "";
    StageNodeDevice requested = StageNodeDevice::CPU;

    StageNodeDevice overridden;
    if (has_override(name, overridden)) {
        requested = overridden;
    } else {
        bool set = false;
        if (env_flag("RT_STAGE_OPENCL", set) && set) {
            requested = StageNodeDevice::OpenCL;
        }
    }
    return select_impl(node, requested);
}

void set_opencl_stage_allowed(bool allowed)
{
    opencl_allowed.store(allowed, std::memory_order_release);
}

bool opencl_stage_allowed()
{
    return opencl_allowed.load(std::memory_order_acquire);
}

void set_node_device_override(const char* node, StageNodeDevice device)
{
    if (node == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(selection_mutex);
    node_overrides[node] = device;
}

void clear_node_device_overrides()
{
    std::lock_guard<std::mutex> lock(selection_mutex);
    node_overrides.clear();
}

struct StageModule {
    std::string path;
    int64_t mtime = 0;
    int64_t size = 0;
    std::unique_ptr<Glib::Module> module;
    const RtStageKernelInfo* info = nullptr;
};

StageProgram::StageProgram() = default;
StageProgram::~StageProgram() = default;

RtStageStatus StageProgram::execute(const std::vector<RtStageArg>& args,
                                   RtStageProfiling& profiling) const
{
    if (info_.execute == nullptr) {
        return RT_STAGE_INVALID;
    }
    return info_.execute(args.data(), static_cast<uint32_t>(args.size()), &profiling);
}

bool StageProgram::is_module() const
{
    return module_ != nullptr;
}

const char* StageProgram::name() const
{
    return info_.name;
}

const char* StageProgram::version() const
{
    return info_.version;
}

const char* StageProgram::source() const
{
    return source_.c_str();
}

struct StageCompiler::Impl {
    std::map<std::string, BuiltinFn> builtins;
    std::map<std::string, std::shared_ptr<StageModule>> modules;
    std::mutex mutex;
};

StageCompiler& StageCompiler::instance()
{
    static StageCompiler compiler;
    return compiler;
}

StageCompiler::StageCompiler() : impl_(std::make_unique<Impl>())
{
    register_builtin_stages(*this);
}

StageCompiler::~StageCompiler() = default;

void StageCompiler::register_builtin(const char* name, BuiltinFn fn)
{
    if (name == nullptr || fn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->builtins[name] = fn;
}

bool StageCompiler::has_builtin(const char* name) const
{
    if (name == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->builtins.find(name) != impl_->builtins.end();
}

void StageCompiler::clear_modules()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->modules.clear();
}

std::shared_ptr<StageProgram> StageCompiler::compile(const StageRequest& request,
                                                     std::string* error)
{
    if (request.name.empty()) {
        if (error != nullptr) {
            *error = "empty stage name";
        }
        return nullptr;
    }

    std::string impl = request.impl;
    if (impl.empty()) {
        const std::string per_stage = "RT_STAGE_" + upper_stage_name(request.name);
        if (const char* env = std::getenv(per_stage.c_str()); env != nullptr) {
            impl = env;
        } else if (const char* env = std::getenv("RT_STAGE_IMPL"); env != nullptr) {
            impl = env;
        }
    }
    if (impl.empty()) {
        impl = "auto";
    }

    std::shared_ptr<StageModule> module;
    if (impl == "auto" || impl == "module") {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const std::string& dir : search_paths()) {
            const std::string candidates[] = {
                dir + "/" + request.name + ".so",
                dir + "/libstage_" + request.name + ".so",
                request.name
            };
            for (const std::string& candidate : candidates) {
                int64_t mtime = 0;
                int64_t size = 0;
                if (!file_stamp(candidate, mtime, size)) {
                    continue;
                }
                auto found = impl_->modules.find(candidate);
                if (found != impl_->modules.end() && found->second->mtime == mtime &&
                    found->second->size == size) {
                    module = found->second;
                    break;
                }
                try {
                    auto loaded = std::make_shared<StageModule>();
                    loaded->path = candidate;
                    loaded->mtime = mtime;
                    loaded->size = size;
                    loaded->module = std::make_unique<Glib::Module>(candidate);
                    void* symbol = nullptr;
                    if (!loaded->module->get_symbol("rt_stage_query", symbol) ||
                        symbol == nullptr) {
                        continue;
                    }
                    auto query = reinterpret_cast<RtStageQueryFn>(symbol);
                    const RtStageKernelInfo* info = query(RT_STAGE_ABI_VERSION);
                    if (info == nullptr || info->size < sizeof(RtStageKernelInfo) ||
                        info->execute == nullptr ||
                        info->name[RT_STAGE_NAME_MAX - 1] != '\0') {
                        continue;
                    }
                    loaded->info = info;
                    impl_->modules[candidate] = loaded;
                    module = loaded;
                    break;
                } catch (...) {
                    continue;
                }
            }
            if (module != nullptr) {
                break;
            }
        }
    }

    if (module != nullptr) {
        auto program = std::make_shared<StageProgram>();
        program->module_ = module;
        program->info_ = *module->info;
        program->source_ = module->path;
        return program;
    }

    if (impl != "module") {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->builtins.find(request.name);
        if (found != impl_->builtins.end()) {
            auto program = std::make_shared<StageProgram>();
            program->builtin_ = found->second;
            std::snprintf(program->info_.name, sizeof(program->info_.name), "%s",
                          request.name.c_str());
            std::snprintf(program->info_.version, sizeof(program->info_.version), "%s",
                          "builtin");
            program->info_.size = sizeof(RtStageKernelInfo);
            program->info_.execute = found->second;
            program->source_ = "builtin";
            return program;
        }
    }

    if (error != nullptr) {
        *error = "no implementation for stage '" + request.name + "' (" + impl + ")";
    }
    return nullptr;
}

RtStageStatus run(const char* name, const std::vector<RtStageArg>& args,
                  RtStageProfiling* profiling, const char* impl)
{
    StageRequest request;
    request.name = name != nullptr ? name : "";
    request.args = args;
    request.impl = impl != nullptr ? impl : "";

    /*
     * The device path is opt-in and is only entered when the node has an
     * implementation and the device is usable. With the switch unset the
     * function takes exactly the host path below, so the default behaviour,
     * timing and memory are unchanged.
     */
    if (request.impl.empty()) {
        const StageNodeSelection selection = select_node_device(name);
        if (selection.device == StageNodeDevice::OpenCL) {
            auto context = opencl_context();
            if (context != nullptr) {
                RtStageProfiling local{};
                const RtStageStatus status = context->execute(request, &local);
                if (status == RT_STAGE_OK) {
                    if (profiling != nullptr) {
                        *profiling = local;
                    }
                    record_stage(name, "opencl", "opencl", args, local);
                    return status;
                }
            }
        }
    }

    std::string error;
    auto program = StageCompiler::instance().compile(request, &error);
    if (!program) {
        return RT_STAGE_FALLBACK;
    }
    RtStageProfiling local{};
    const auto begin = std::chrono::steady_clock::now();
    const RtStageStatus status = program->execute(args, local);
    const auto end = std::chrono::steady_clock::now();
    local.elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    if (profiling != nullptr) {
        *profiling = local;
    }

    record_stage(name, program->is_module() ? "module" : "builtin",
                 program->source(), args, local);

    return status;
}

} // namespace rtengine::stage
