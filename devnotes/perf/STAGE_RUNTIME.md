# Runtime stage armature

This record describes the experimental stage runtime added on
`perf-stage-runtime`. The branch is deliberately opt-in: the default build
keeps the existing image pipeline, while the new layer provides a stable
interface for later cache, tile and OpenCL work.

## Purpose

The engine has four hand-ordered pipelines (`ImProcCoordinator`, `Crop`,
`ImageProcessor` and `Thumbnail`) and a bitmask for invalidation. The stage
runtime gives those pipelines a common execution vocabulary:

- a C ABI for a stage, with buffer descriptors and a parameter blob;
- a CPU command queue and a compiler that resolves a stage to a built-in
  implementation or a loadable shared object;
- a small dependency graph with explicit reuse decisions;
- a bounded cache, a block scorer and parity/profiling helpers.

The object model deliberately mirrors OpenCL: platform, device, context,
queue, program, kernel, buffer and event. A future OpenCL backend can replace
the CPU device without changing the graph or the cache.

## ABI

`rtengine/stage/stage_abi.h` defines `RtStageBuffer`, `RtStageParamBlob`,
`RtStageArg`, `RtStageKernelInfo` and `RtStageQueryFn`. Rules:

- no C++ objects or exceptions cross the module boundary;
- every struct starts with `size`; the loader rejects a mismatched ABI;
- buffers carry byte row and plane pitches;
- modules export `rt_stage_query` with default visibility;
- a missing, corrupt or incompatible module falls back to the built-in
  implementation and never fails a render.

## Compiler and module loading

`StageCompiler::compile()` resolves a stage in this order:

1. the explicit request implementation;
2. `RT_STAGE_<NAME>`;
3. `RT_STAGE_IMPL`;
4. a shared object on `RT_STAGE_PATH` or the build stage directory;
5. the built-in implementation.

`RT_STAGE_RELOAD=1` rechecks the module mtime and size, so a developer can
rebuild one shared object and have the next request pick it up without
relinking the GUI or CLI.

## Pilot stage

`lanczos3` is the pilot. The arithmetic lives in
`rtengine/stage/algorithms/lanczos.cc` and is compiled into both the host and
the `lanczos3.so` module. This gives exact built-in/module parity by
construction. `ImProcFunctions::Lanczos` calls the runtime and falls back to
the existing loop if the stage cannot be resolved. The horizontal taps are
computed once per resize and shared by all worker threads instead of being
recomputed independently by each thread.

Controls:

```bash
RT_STAGE_IMPL=builtin      # force the host copy
RT_STAGE_IMPL=module       # force the loadable copy
RT_STAGE_IMPL=auto         # module first, then built-in
RT_STAGE_PATH=/path        # extra module directories
RT_STAGE_RELOAD=1          # pick up a rebuilt module
RT_STAGE_PROFILE=1         # stderr timing/JSONL
RT_STAGE_TRACE=1           # reserved for cache/graph tracing
```

## Cache and graph

`StageCache` is a byte-budgeted LRU keyed by stage name, parameter
fingerprint, input fingerprints, level and consumer. `StageGraph` records
nodes, edges and one of three reuse modes:

- `RT_STAGE_RECOMPUTE` — recompute when dirty;
- `RT_STAGE_REUSE_INPUT` — reuse the upstream buffer, recompute this stage;
- `RT_STAGE_REUSE_OUTPUT` — reuse the stage output when inputs are clean.

The graph returns a plan; it does not yet drive the four production
pipelines. That migration is the next step, and is intentionally separate
from the armature so the existing pipelines remain unchanged by default.

## Graph-driven export

`ImageProcessor::normal_pipeline()` now has an opt-in graph-driven form. With
`RT_STAGE_GRAPH=1` the four export stages become `StageGraph` nodes
(`export.init`, `export.denoise`, `export.transform`, `export.finish`), each
declared with an explicit `RT_STAGE_RECOMPUTE` reuse mode and connected in
execution order. The graph is built per job, so every node is dirty and
`plan()` returns `Recompute` for all four. Execution follows
`topologicalOrder()`; a reuse decision fails closed with `nullptr` because
this path has no stage buffer cache yet. The node id selects the same
`stage_*` member the legacy path calls, so the arithmetic is untouched.

Unset, empty, `0` and `off` (case-insensitive) keep the legacy hand-ordered
pipeline, which remains the default. `RT_STAGE_PROFILE=1` prints the same
`export.*` keys from the graph nodes, timed with `stage::ScopedProfile`.

Evidence: the 45.7 MP sidecar export with `devnotes/opencl/pp3/resize.pp3`
at 8 threads is pixel-identical (`AE=0`, 0 differing of 34 229 520 samples)
between `RT_STAGE_GRAPH=0` and `RT_STAGE_GRAPH=1`, and also against unset and
`off`. `tools/perf/run-suite.sh --graph` runs that comparison as an optional
gate.

`buildTilePlan()` produces a cheap block score from a luminance plane and
splits blocks into full and cheap work. It is the hook for selective
denoise/wavelet work; it is not yet wired into those tools.

## Export profiling

`RT_STAGE_PROFILE=1` now prints the main export phases:

```text
stage export.init: ...
stage export.denoise: ...
stage export.transform: ...
stage finish.rgbproc: ...
stage finish.sharpening: ...
stage finish.wavelet: ...
stage finish.resize: ...
stage finish.output: ...
stage export.finish: ...
```

The numbers are wall time on the host and are not a substitute for a full
benchmark; they exist to locate the next target.

## Preview and output changes

Two small changes are in the default path:

- `PreviewBaseCache::tryRestore()` swaps the cached image with the
  destination instead of copying it, so a hit does not move a full-frame
  buffer;
- output and monitor lcms transforms are cached by profile, intent, BPC and
  pixel type, so repeated preview requests do not rebuild them.

Both are covered by the existing preview base-cache test and by export
parity.

## Evidence

- `stage-abi-test`, `stage-parity-test`, `stage-cache-test`,
  `stage-graph-test` and `stage-indexer-test` pass on the CPU build.
- `stage-parity-test` compares the host and `lanczos3.so` output over a
  64×48 synthetic image: zero differing pixels and zero maximum difference.
- A 45.7 MP sidecar export with the resize profile is pixel-identical
  (`AE=0`) to the unmodified baseline.
- The OpenCL build compiles; `tools/opencl/verify.sh --quick` passes with
  `max_pae=1.5259e-05` (1 LSB at 16-bit), using the AMD Radeon RX 550 device.
- Fair interleaved 45.7 MP export with the resize profile, 8 threads:
  baseline 5.14–5.42 s, candidate 4.94–5.04 s, with `AE=0` on every pair.
  The gain is about 5% on this workload.
- Thread matrix on the same profile: `AE=0` at 1, 2, 4 and 8 threads. With a
  denoise profile the existing engine varies by 1 LSB between thread counts
  (about 7–9 k pixels of 6.2 M); that is a pre-existing denoise reduction
  order issue, not introduced here, and is recorded as a limitation.
- Build KPI on the 4C/8T host: no-op `rth-cli` build 0.56–0.80 s, one module
  rebuild 0.25 s, one translation-unit rebuild 1.48–2.28 s. The project
  ccache is `build/ccache`; the first builds after the wrapper change are
  cold, so the hit rate is not yet representative.
- Identical preview runs on the same image, display and settings:
  baseline p50 148 ms / p95 224 ms; candidate p50 142 ms / p95 197 ms over
  12 samples. The p95 gain is about 12%; the p50 gain is about 4%. The GUI
  harness is timing-sensitive, so these are indicative rather than a
  controlled microbenchmark.
- Whole-suite export timing on the same run: baseline 7.82–8.24 s,
  candidate 7.74–8.04 s at 8 threads. The fair interleaved run above is the
  cleaner comparison because it removes the suite's disk and cache noise.

## Limitations

- Preview, crop and thumbnail pipelines are not graph-driven. Export is
  graph-driven only when `RT_STAGE_GRAPH=1`; the legacy path is the default
  and the graph path does not yet cache or reuse buffers.
- Only Lanczos is exposed as a loadable stage. Denoise, wavelet, local
  adjustments and the output conversion still use the existing code paths.
- The block scorer is not connected to denoise or wavelet; it has no runtime
  effect yet.
- The preview parity screenshot harness is present but needs a stable Xvfb
  display and the first-run notice dismissed; use
  `RT_PREVIEW_DISPLAY` and the existing GUI procedure.
