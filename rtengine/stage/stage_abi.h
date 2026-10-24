/*
 * Runtime-loadable image stage ABI.
 *
 * The ABI is deliberately C-compatible: modules are shared objects built with
 * a different linkage unit from the host, and a future OpenCL backend must be
 * able to expose the same descriptors without dragging C++ types across the
 * boundary.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_STAGE_ABI_VERSION 1u
#define RT_STAGE_NAME_MAX 64u

#if defined(_WIN32)
#  define RT_STAGE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#  define RT_STAGE_EXPORT __attribute__((visibility("default")))
#else
#  define RT_STAGE_EXPORT
#endif

typedef enum {
    RT_STAGE_OK = 0,
    RT_STAGE_FALLBACK = 1,
    RT_STAGE_INVALID = 2,
    RT_STAGE_ERROR = 3
} RtStageStatus;

typedef enum {
    RT_STAGE_F32 = 0,
    RT_STAGE_U8 = 1,
    RT_STAGE_U16 = 2
} RtStageFormat;

typedef enum {
    RT_STAGE_RECOMPUTE = 0,
    RT_STAGE_REUSE_INPUT = 1,
    RT_STAGE_REUSE_OUTPUT = 2
} RtReuseMode;

typedef enum {
    RT_STAGE_EXACT = 0,
    RT_STAGE_INTERACTIVE = 1
} RtStageConsumer;

typedef enum {
    RT_STAGE_BUFFER_READ = 1u << 0,
    RT_STAGE_BUFFER_WRITE = 1u << 1,
    RT_STAGE_BUFFER_DEVICE = 1u << 2
} RtStageBufferFlags;

typedef enum {
    RT_STAGE_ARG_BUFFER = 1,
    RT_STAGE_ARG_INT = 2,
    RT_STAGE_ARG_DOUBLE = 3,
    RT_STAGE_ARG_PTR = 4
} RtStageArgKind;

typedef struct {
    uint32_t size;
    void* data;
    int32_t width;
    int32_t height;
    size_t row_pitch;
    size_t plane_pitch;
    uint32_t planes;
    RtStageFormat format;
    uint32_t flags;
} RtStageBuffer;

typedef struct {
    uint32_t size;
    const void* data;
    size_t bytes;
} RtStageParamBlob;

typedef struct {
    uint32_t kind;
    union {
        RtStageBuffer buffer;
        const void* ptr;
        int64_t i;
        double f;
    } u;
} RtStageArg;

typedef struct {
    uint64_t elapsed_ns;
    uint32_t cache_hit;
    uint32_t flags;
} RtStageProfiling;

typedef RtStageStatus (*RtStageExecuteFn)(const RtStageArg* args, uint32_t nargs,
                                          RtStageProfiling* profiling);

typedef struct {
    uint32_t size;
    char name[RT_STAGE_NAME_MAX];
    char version[RT_STAGE_NAME_MAX];
    uint32_t flags;
    RtStageExecuteFn execute;
} RtStageKernelInfo;

/*
 * Every module exports exactly this symbol. The host calls it with
 * RT_STAGE_ABI_VERSION and rejects the module if the returned descriptor is
 * smaller than the host's view of RtStageKernelInfo or names another ABI.
 */
typedef const RtStageKernelInfo* (*RtStageQueryFn)(uint32_t abi_version);

#ifdef __cplusplus
}
#endif
