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

## Device and buffer state

`rtengine/stage/stage_device.h` adds the device abstraction the graph plan
needs, without changing any production path. The object model is the OpenCL
one: a device creates contexts and queues, a queue executes a compiled
program, and an event carries status and timing.

| Type | Role |
| --- | --- |
| `StageDevice` | type, name, availability, capabilities, context and queue factory |
| `CPUStageDevice` | always available, resolves built-ins and modules through `StageCompiler`, executes synchronously, deterministic and full precision |
| `OpenCLStageDevice` | skeleton; availability maps to `opencl::enabled()` and it adds no OpenCL call |
| `StageContext` | owns a device, a per-context program cache and a buffer allocator |
| `StageQueue` | CPU executes inline; the OpenCL queue returns `RT_STAGE_FALLBACK` |
| `StageEvent` | status and elapsed time; no asynchronous dependency yet |

`StageContext::compile()` calls the existing `StageCompiler`, so the
resolution order and the built-in/module fallback are unchanged. The context
caches the resulting program by stage name and explicit implementation;
`clear_program_cache()` forces the next compile to resolve again. The default
context selects CPU, and `select_device(StageDeviceType::OPENCL)` falls back
to CPU with a reason when the existing OpenCL backend is not enabled.

`rtengine/stage/stage_buffer_state.h` associates an `RtStageBuffer` with a
location, a device generation and an intrusive reference count. The header
contains no OpenCL types:

| Location | Meaning |
| --- | --- |
| `HostValid` | the host copy is current |
| `DeviceValid` | the device copy is current and the host copy is stale |
| `Both` | both copies are current |
| `Dirty` | neither copy may be read |

The CPU `ensure_host()` performs no copy: it reports `RT_STAGE_OK` for a
host-valid or shared buffer, `RT_STAGE_FALLBACK` for a stale device copy and
`RT_STAGE_INVALID` for dirty contents. `ensure_device()` reports
`RT_STAGE_FALLBACK` because the CPU has no device memory and the OpenCL
upload is not wired. `mark_host_write()`, `mark_device_write()`,
`mark_both_valid()` and `mark_dirty()` record the transitions, and the device
generation lets a later implementation invalidate copies from an earlier
device session. The wrapper does not own memory; the descriptor stays a
non-owning view over an engine image or an allocator block.

`StageBufferAllocator` is owned by the context. The CPU allocator returns
zeroed host memory with 64-byte aligned rows and planes and supports `F32`,
`U16` and `U8` with one to three planes; single-plane `U8` is packed RGB,
matching `stage::row_bytes()`. The OpenCL allocator refuses allocation until a
device allocator exists.

Nothing creates a non-CPU context and no call site changed: `ImProcFunctions`,
the export graph and the preview layer still use `StageCompiler` and `run()`
exactly as before. The new types are infrastructure for the next phase.

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

## Parameter fingerprint

`rtengine/stage/stage_param_hash.h` provides `ParamHasher`, the primitive that
will replace the `ProcParams` bitset as the invalidation and cache-key input.
It is utility code only: no pipeline calls it yet, and no `ProcParams`
descriptor is defined in this phase.

A fingerprint is 128 bits, two opaque `uint64_t` words with equality and a
mixed `hash()` for containers. `hexString()` gives exactly 32 lower-case
characters for logs.

The hasher absorbs typed fields in call order:

```cpp
ParamHasher hasher;
hasher.addSectionLabel("denoise");   // domain separation, one per stage
hasher.add(params.denoise.enabled);  // bool
hasher.add(params.denoise.radius);   // int32
hasher.add(params.denoise.strength); // double
hasher.addString(params.outputProfile);
const ParamFingerprint key = hasher.finish();
```

Rules that make a fingerprint a cache key:

- each overload writes a distinct type tag, so `int32_t{1}`, `int64_t{1}`,
  `uint32_t{1}`, `true`, `1.0f` and `1.0` all differ;
- field order is significant, and every 64-bit word is mixed into both lanes,
  so a change propagates to all later bytes;
- numbers are written as fixed-width little-endian bit patterns, never as
  formatted text, so results do not depend on locale, host byte order or
  compiler;
- strings and section labels are tagged and length-prefixed, so prefixes,
  trailing zero bytes and characters moved across a call boundary cannot
  collide; labels are tagged separately from strings, so a label never aliases
  a string field;
- the total byte length is mixed in, so a trailing zero byte stays
  significant;
- `finish()` is `const` and non-destructive.

`add(float)` and `add(double)` hash the bit pattern exactly as stored, so
`0.0` and `-0.0` differ. A caller that wants them to agree must normalise the
value before adding it. The hash is not cryptographic; it claims only the
128-bit birthday bound.

The test pins one published fingerprint
(`553f1f9e677666f509db98fafda02b42`), so changing a tag, constant or framing
byte fails the test instead of silently invalidating stored keys.

`addBytes()` is the raw escape hatch for arrays and packed data. It is
untagged and byte-order agnostic, so the caller is responsible for framing and
for passing the same platform-independent byte order everywhere.

`stage-param-hash-test` covers identical inputs, one-field changes, order,
type, shared string prefixes, float and double bit patterns, labelled
sections, raw-byte framing, repeated `finish()` and 100k repeated calls.

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

## Preview decision layer and dump

`RT_PREVIEW_GRAPH=1` adds an opt-in decision layer to
`ImProcCoordinator::updatePreviewImage()`. Unset, empty, `0` and `off`
(case-insensitive) keep the legacy path, which is the default. The layer
models the full-frame preview as six `StageGraph` nodes connected in
execution order:

```text
preview.base -> preview.analysis -> preview.local
             -> preview.rgb -> preview.lab -> preview.output
```

Dirty state comes from the event mask and the base-cache result:

- `base` is dirty only when the pass does not restore the base from the
  preview base cache;
- `analysis` is dirty when any preprocessing, demosaic, white-balance,
  retinex, spot or first-analysis bit is set;
- `local`, `rgb` and `lab` are dirty when the pass carries any bit that
  starts work in that region (`ALL` and the tool bit sets);
- `output` is dirty for a monitor conversion or a panning-related change.

`plan()` then resolves the actions. Every block the legacy code executes
must be allowed to run: if the plan returns `ReuseOutput` for such a block,
the layer logs `preview-graph: fallback to legacy: plan would skip ...` and
rejects the plan. Rejection changes no pixels, because this version is a
decision and parity layer, not a rewrite: the legacy block sequence is the
only implementation, the graph never reorders stages and no arithmetic or
thread count changes. Once per process, the accepted plan is logged as
`preview-graph: active, 6 blocks in execution order: ...`.
`RT_STAGE_PROFILE=1` also records `preview.graph.decide`; measured on the
45.7 MP image it costs 0.008-0.010 ms per full-frame pass.

`RT_PREVIEW_DUMP=<dir>` writes the completed full-frame pass and the visible
crop for parity work. It is off when unset, empty, `0` or `off`.

```text
preview.tif        previmg, uncompressed 16-bit TIFF
work.tif           workimg, uncompressed 16-bit TIFF
preview.json       pass, scale, todo, dimensions and generated file names
crop-main.tif      visible main-preview crop, same format
crop-main.json     tag, dimensions, visible rectangle and scale
crop-detail.tif    detail-window crop when one exists
crop-detail.json
```

The 8-bit `Image8` samples are widened exactly to 16-bit (`v * 257`), so the
dump is byte-comparable between runs. The JSON descriptors contain only
generated file names, image geometry and the event mask: never the source
path, the dump directory, a user name or a host name. The TIFF writer adds
only the RawTherapee version tag because the preview images carry no
metadata. The dump path holds the existing preview mutex and is disabled by
default; writing a dump adds disk I/O to the pass and is not part of the
latency gate.

Reproduce under Xvfb with a fresh settings directory, a fresh cache and
`OMP_NUM_THREADS=1` for every run, loading the same image and waiting for
`preview imageReady`:

```bash
RT_SETTINGS=... RT_CACHE=... RT_PREVIEW_PROFILE=1 \
RT_PREVIEW_GRAPH=0 RT_PREVIEW_DUMP=/tmp/dump-legacy rawtherapee "$RT_TEST_IMAGE_45MP"
RT_SETTINGS=... RT_CACHE=... RT_PREVIEW_PROFILE=1 \
RT_PREVIEW_GRAPH=1 RT_PREVIEW_DUMP=/tmp/dump-graph rawtherapee "$RT_TEST_IMAGE_45MP"
cmp /tmp/dump-legacy/preview.tif /tmp/dump-graph/preview.tif
```

Evidence on the 45.7 MP NEF under Xvfb, single-threaded, fresh settings and
cache for every run:

- `RT_PREVIEW_GRAPH=0` versus `1`: `preview.tif`, `work.tif` and
  `crop-main.tif` are byte-identical (`cmp`, `AE=0`) over five independent
  pairs.
- Two legacy runs are byte-identical to each other, and two graph runs are
  byte-identical to each other, so the comparison is not measuring
  run-to-run noise.
- `RT_PREVIEW_GRAPH=off` matches the unset default byte for byte, and
  `RT_PREVIEW_GRAPH=1` logs the active plan without a fallback on those
  passes.
- A deliberate dirty-rule mutation that under-reported the base block
  produced `preview-graph: fallback to legacy: plan would skip preview.base
  but the legacy path runs it (todo=118783)` exactly once; the pixels were
  unchanged.
- Preview latency on the same build and image, driven through the Exposure
  slider: four paired legacy/graph GUI runs gave p50 372-487 ms and
  415-493 ms respectively. The direction changes between pairs and the
  spread is about 25%, while the decision costs 0.008-0.010 ms, so the
  difference is host and GUI noise rather than a regression. The standard
  `bench-preview.sh` gate exits zero in both modes; its drag coordinates
  only produce two samples on this host, so the paired harness above is the
  useful measurement.

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

## Persistent preview cache

`RT_STAGE_STORE=1` enables a checksummed copy of the preview base image in
`$RT_CACHE/stages` (or the configured settings cache directory). The key is a
SHA-256 of the stable image identity, white balance, demosaic and high-detail
inputs, transform, scale and dimensions. The process-local `ImageSource`
pointer and the transient source-generation counter are excluded; the image
identity is the path plus file size and mtime.

`RT_STAGE_STORE_MAX_MB` sets the budget (default 512 MiB). Before a write that
would exceed it, the store removes the oldest files until the new entry fits.
The store's tests cover `prune(0)` and the preview cache exercises the budget
path.

The store is opt-in and does not change the default path. A warm reopen
produced `preview-base-cache: persistent-hit` and the normal second preview
request fell from about 235 ms to about 198 ms on the 45.7 MP image. Initial
load is unchanged because the rough preview deliberately bypasses the base
cache. `preview-base-cache-test` covers the persistent key derivation:
changing the image identity, white balance or scale changes the key, while a
change to the transient generation counter does not.

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
  `stage-graph-test`, `stage-indexer-test`, `stage-param-hash-test` and
  `stage-store-test` pass on the CPU build.
- `stage-device-test` passes in both the CPU and OpenCL configurations. It
  checks that the CPU device is available, deterministic and full precision;
  that Lanczos compiles, caches, executes through the context and the inline
  CPU queue; that the default context and explicit selection pick CPU; that
  the OpenCL queue reports `RT_STAGE_FALLBACK`; that selection falls back to
  CPU with a reason when OpenCL is unavailable; that the buffer state machine,
  generation and reference count behave as documented; and that the context
  output is pixel-identical to a direct `StageCompiler` execution (`AE=0`,
  0 differing of 2304 samples). The OpenCL build on this host reports
  `opencl-stage-device: available (execution not wired)` and still passes;
  the CPU-only build exercises the unavailable/fallback branch.
- `stage-param-hash-test` exercises the fingerprint contract in 41 checks:
  identical descriptors agree, a published golden value pins the format, every
  field type and order changes the fingerprint, shared string prefixes and
  trailing zero bytes differ, adjacent floats and doubles differ, the same
  value in two labelled sections differs, and 100k repeated calls give one
  value. It also passes in the OpenCL configuration.
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
- Thread matrix on the same profile: `AE=0` at 1, 2, 4 and 8 threads. The
  denoise block merge is now ordered; on a deterministic full-frame TIFF input
  the residual falls from 215/414 differing pixels to 6/12 at 1 LSB. The raw
  path still has upstream thread-order variation, so the full NEF denoise
  export is not yet bit-identical; see `devnotes/perf/DETERMINISM.md`.
- Build KPI on the 4C/8T host: no-op `rth-cli` build 0.56–0.80 s, one module
  rebuild 0.25 s, one translation-unit rebuild 1.48–2.28 s. The project
  ccache is `build/ccache`; the first builds after the wrapper change are
  cold, so the hit rate is not yet representative.
- Identical preview runs on the same image, display and settings:
  baseline p50 148 ms / p95 224 ms; the final merged candidate reports
  p50 135 ms / p95 184 ms over 12 samples. The p95 gain is about 18%; the
  p50 gain is about 9%. The GUI harness is timing-sensitive, so these are
  indicative rather than a controlled microbenchmark.
- Whole-suite export timing on the same run: baseline 7.82–8.24 s,
  candidate 7.74–8.04 s at 8 threads. The fair interleaved run above is the
  cleaner comparison because it removes the suite's disk and cache noise.
- Cropped sidecar export at 8 threads: baseline 17.14 s, candidate 15.31 s
  (about 11% faster), with `AE=0` over 6.2 M pixels. The gain includes the
  row-wise Lab crop copy, the cached output transform and the shared Lanczos
  taps; it is not attributed to any one change.
- Persistent preview cache: first run stores one entry; a second run reports
  `preview-base-cache: persistent-hit`, and the second preview request falls
  from about 235 ms to about 198 ms. The initial rough preview remains
  bypassed and unchanged.

## Merged phase results

- Preview graph (`RT_PREVIEW_GRAPH=1`): five independent legacy/graph
  dump pairs were byte-identical for `preview.tif`, `work.tif` and the main
  crop. The decision costs 0.008–0.010 ms per pass. The layer is advisory in
  this version: it certifies blocks and fails closed, while execution keeps
  the legacy arithmetic order.
- Tile merge and ordering: the tiled denoise and wavelet overlap merge now
  accumulates per-tile contributions in ascending tile-row/column order.
  `RT_TILE_ORDER=0` and `1` are `AE=0` at 1 and 8 threads, and the default
  output is unchanged. Peak RSS rises by about 200 MB for the band buffers.
  Ordering gives about 8% of the wavelet stage and is neutral for denoise.
- Fast export: converting only the crop region to Lab removes about 1.1 GB of
  buffer traffic. The `export.early_resize` stage fell from 3056 ms to
  1148 ms (−62%), whole fast export from 46.82 s to 43.81 s in the isolated
  eight-pair set, and peak RSS fell by 124 MB. CPU fast-export parity is
  `AE=0`.
- Final interleaved medians under a loaded host: default export 7.16 s →
  6.81 s (5.2% faster); fast export 39.11 s → 36.01 s (8.6% faster, high
  variance). The isolated stage figures above are the stronger evidence.
- Final OpenCL `verify.sh --quick` and `--formats` pass at 1 LSB; monitor
  parity is 1 byte.

## Limitations

- The preview has a graph-backed decision layer, not a graph-driven
  execution path: with `RT_PREVIEW_GRAPH=1` the plan certifies which blocks
  may run but the legacy block sequence still performs every computation.
  Crop and thumbnail pipelines are still hand-ordered. Export is graph-driven
  only when `RT_STAGE_GRAPH=1`; the legacy path is the default and neither
  graph path caches or reuses buffers yet.
- The preview dump writes at most one file per crop tag, so two detail
  windows overwrite each other's `crop-detail.*` pair.
- The persistent preview cache is opt-in and covers only the preview base
  image. The store prunes oldest files to the configured budget, but the store
  itself has no key derivation; `PreviewBaseCache` owns the key. A store
  shared by two processes is atomic per entry but is not range-locked, so two
  writers of the same key race by rename order.
- The persistent cache does not speed up the first rough preview, because that
  pass deliberately bypasses the base cache. Its measured benefit is later
  preview requests and reopen after a base has been stored.
- Only Lanczos is exposed as a loadable stage. Denoise, wavelet, local
  adjustments and the output conversion still use the existing code paths.
- The OpenCL device is a skeleton: it mirrors the existing backend's enabled
  state, advertises no execution capability, and its queue always reports
  `RT_STAGE_FALLBACK`. Its allocator refuses allocation. No OpenCL execution
  or device-side buffer is implemented in this phase.
- The context program cache is keyed by stage name and explicit
  implementation, so it reads `RT_STAGE_*` and `RT_STAGE_PATH` on the first
  compile of a key. Call `clear_program_cache()` to re-resolve after changing
  the environment or rebuilding a module.
- `StageBufferState` is non-owning and has no readback path: `ensure_host()`
  reports `RT_STAGE_FALLBACK` while the device copy is authoritative, and
  `ensure_device()` reports `RT_STAGE_FALLBACK` on CPU.
- `ParamHasher` is a standalone primitive. No `ProcParams` descriptor is
  defined yet, so no cache key or invalidation decision uses a fingerprint;
  the existing bitset remains the production invalidation input.
- Tile ordering is connected to denoise and wavelet behind `RT_TILE_ORDER=1`,
  but it is opt-in and has no reduced-work mode. The deterministic merge is
  always on.
- Preview parity is measured through `RT_PREVIEW_DUMP`, not screenshots; the
  screenshot harness was not stable enough to commit.
