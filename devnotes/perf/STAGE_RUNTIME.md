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
the existing loop if the stage cannot be resolved.

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

`buildTilePlan()` produces a cheap block score from a luminance plane and
splits blocks into full and cheap work. It is the hook for selective
denoise/wavelet work; it is not yet wired into those tools.

## Persistent store

`StageStore` is a standalone on-disk store for stage buffers, intended for a
future preview-cache tier. It is not wired into any pipeline yet: nothing in
the engine creates or reads a store.

- The root is supplied by the caller, for example `RT_CACHE/stages`. The store
  owns that tree; `clear()` empties it.
- A key is exactly 64 hexadecimal digits computed by the caller. The key is
  canonicalised to lower case and sharded two levels deep
  (`<root>/ab/cd/<key>.stage`), so no directory grows without bound and no
  caller text ever reaches the file system.
- The entry is little-endian and self-describing: magic `RTSTG001`, format
  version, fixed header size, CRCs of the header and the payload, stage name
  and version, width, height, planes, pixel format, packed row bytes and
  payload length. The payload follows the name and version text.
- `put()` writes a temporary file in the destination shard, flushes and syncs
  it, then renames it over the final name. A reader never sees a partial
  entry, and a failed write leaves the previous entry intact. The directory is
  synced after the rename so the entry survives a power loss.
- `get()` validates magic, version, header size, descriptor, row geometry,
  total length, header CRC and payload CRC before it writes anything. The
  payload is verified in a first pass and copied in a second, so a corrupt or
  truncated entry leaves the destination untouched. Only the packed row bytes
  are copied, row by row, so destination padding outside its row and plane
  pitches is never written.
- `F32`, `U16` and `U8` are supported with one to three planes and any source
  or destination pitch that is at least the packed row. A single-plane `U8`
  buffer is packed RGB with three bytes per pixel, matching
  `stage::row_bytes()` and the engine's `Image8`; a three-plane `U8` buffer is
  one byte per plane.
- `erase()`, `contains()`, `clear()` and `bytes()` complete the API.
  `bytes()` sums every regular file below the root so a caller can enforce a
  size budget; it includes a temporary file left by a crashed writer.
- All operations are serialised by one mutex. The store holds no engine state
  and can be used from any thread.

`stage-store-test` covers the round trip for all three formats with one and
three planes and mismatched source/destination pitches, metadata, sharding,
case-insensitive keys, invalid and traversal keys, a corrupted payload byte,
two truncations, wrong geometry, short destination pitches, atomic overwrite,
erase, contains, clear, a small byte budget and concurrent writers.

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
  `stage-graph-test`, `stage-indexer-test` and `stage-store-test` pass on the
  CPU build.
- `stage-store-test` also covers the store's failure paths: a flipped payload
  byte, two truncations, wrong geometry, short destination pitches, invalid
  and traversal keys and concurrent writers are all rejected or kept intact,
  and the temporary directory is removed afterwards. The test builds and
  passes in both the CPU and OpenCL configurations.
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

- The four production pipelines are not yet graph-driven. The graph and cache
  are armature plus tests, not a live replacement.
- The persistent store has no pipeline integration, eviction policy or key
  derivation of its own: the caller owns the key, the budget check and the
  decision to delete. A store shared by two processes is atomic per entry but
  is not range-locked, so two writers of the same key race by rename order.
- Only Lanczos is exposed as a loadable stage. Denoise, wavelet, local
  adjustments and the output conversion still use the existing code paths.
- The block scorer is not connected to denoise or wavelet; it has no runtime
  effect yet.
- The preview parity screenshot harness is present but needs a stable Xvfb
  display and the first-run notice dismissed; use
  `RT_PREVIEW_DISPLAY` and the existing GUI procedure.
