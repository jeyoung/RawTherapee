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

`ImageProcessor::normal_pipeline()` has an opt-in graph-driven form. With
`RT_STAGE_GRAPH=1` the canonical export inventory becomes a `StageGraph`:
`stage::buildPipelineSpec(StagePath::Export)` supplies all 20 node ids, the 19
edges and each node's reuse mode, so the graph is not restated in the
processing code. The graph is built per job, so every node is dirty and
`plan()` must let every node run.

Execution is guarded before any node runs:

1. the spec node count must be 20 and every node id must have an executor;
2. the graph must be acyclic and the topological order must cover every node;
3. every decision must be `Recompute` or `ReuseInput` (a reuse-input node
   writes in place). A `ReuseOutput` would skip a block the legacy path runs;
   this path has no stage buffer cache, so that plan is rejected, one line is
   logged (`stage-graph: fallback to legacy: plan would skip the node
   (<id>)`), and the legacy sequence runs instead.

Execution follows `topologicalOrder()`, and each node id selects the same
private method the legacy path calls, so the arithmetic is untouched. An
accepted plan logs `stage-graph: active, 20 nodes executed from the canonical
export inventory` once per process. Unset, empty, `0` and `off`
(case-insensitive) keep the legacy hand-ordered pipeline, which remains the
default. Fast export (`-f`) still uses its own order: it calls the same node
methods, but runs denoise after the early resize, so it is not routed through
the graph in this phase.

`RT_STAGE_PROFILE=1` records one key per node, named by node id
(`source.select` ... `output.icc`), from inside the node method, so both paths
emit comparable figures. The legacy path keeps its coarse `export.init`,
`export.denoise`, `export.transform` and `export.finish` scopes around the same
methods, and the pre-existing `finish.rgbproc`, `finish.sharpening`,
`finish.wavelet`, `finish.resize` and `finish.output` scopes remain inside
their nodes.

All 20 nodes execute real work. Blocks that stay folded inside a neighbour,
because one topological order has to reproduce the implementation's pixel
order, are listed in `devnotes/perf/STAGE_PIPELINE.md`: the framing geometry
is computed in `crop`, the Nearest output resize is in `output.convert`,
`raw.base` is three calls, the histogram allocation is in `analysis`, and the
forced B&W pass is in `output.convert`.

### Export graph evidence

Image: the 45.7 MP NEF (`RT_TEST_IMAGE_45MP`), `-t -b16 -Y`, `--gpu=off`.
Variants: neutral (no profile), sidecar (`-s`), cropped (3001x2001), output
profile (`RTv4_sRGB`), and cropped plus output profile. Host: 4C/8T, load
average 3-8 during the runs.

`RT_STAGE_GRAPH=0` versus `RT_STAGE_GRAPH=1`, all ten pairs `AE=0`:

| Variant | 1 thread | 8 threads |
|---------|----------|-----------|
| neutral | `AE=0`, 136 918 080 samples | `AE=0`, 136 918 080 |
| sidecar | `AE=0`, 6 220 800 | `AE=0`, 6 220 800 |
| cropped | `AE=0`, 18 015 003 | `AE=0`, 18 015 003 |
| output profile | `AE=0`, 136 918 080 | `AE=0`, 136 918 080 |
| cropped + output profile | `AE=0`, 18 015 003 | `AE=0`, 18 015 003 |

Legacy byte-identity: the pre-change binary (same commit, built before the
decomposition) and the decomposed legacy path (`RT_STAGE_GRAPH=0`) produced
identical files for neutral, sidecar, cropped and output-profile at 1 and 8
threads, after normalising the TIFF DateTime tag that records the render time.
Every pair was also `AE=0`.

Per-node profile, `resize.pp3`, 8 threads, one run (ms):

```text
source.select 0.009   raw.preprocess 359.381   raw.demosaic 1095.558
raw.base 902.565      analysis 79.675         transform 0.026
local.adjustments 0.044   rgb.proc 545.215    lab.tools 0.487
lab.sharpening 0.003  lab.wavelet 0.002       working.trc 0.005
ciecam 0.010          gamut 0.000             crop 0.003
resize 1053.657       output.sharpen 0.000    output.convert 1241.654
framing 0.000         output.icc 0.040
```

Legacy performance: 11 interleaved pairs of the pre-change and decomposed
binaries, both on the legacy path, `resize.pp3`, 8 threads, wall time in ms.
The medians are 4856 (pre-change) and 4781 (decomposed), a 1.5% difference in
favour of the decomposed build, inside the 20% spread caused by host load.
The legacy path runs the same statements; the only added work is one
`ScopedProfile` pair per node and the method call frames.

Run-to-run note: with an unrelated eight-job build saturating the host, one
sidecar pair differed by 4 samples at 1 LSB out of 6 220 800. Under that load
the pre-change binary also differed from itself by 4-8 samples at 1 LSB, so
this is the documented upstream floating-point/thread-order noise, not the
decomposition. On the idle host, five repeated pairs of each kind were exact.

`stage-pipeline-test` and `stage-graph-test` pass with the corrected export
order. `tools/perf/run-suite.sh --graph` runs the neutral comparison as an
optional gate.

The OpenCL configuration compiles with `--stage-modules` and gives the same
result: all ten legacy/graph pairs are `AE=0`, the graph logs 20 executed
nodes, `stage-device-test` reports `opencl-stage-device: available (execution
not wired)`, and `stage-pipeline-test`, `stage-graph-test` and
`stage-device-test` pass. The parity runs use `--gpu=off`; the graph path does
not use the GPU backend in this phase.

`buildTilePlan()` produces a cheap block score from a luminance plane and
splits blocks into full and cheap work. It is the hook for selective
denoise/wavelet work; it is not yet wired into those tools.

## Graph-driven thumbnail

`RT_STAGE_GRAPH=1` also switches the thumbnail path to the canonical
twelve-node specification in `STAGE_PIPELINE.md`. Each entry point builds
`buildPipelineSpec(StagePath::Thumbnail)`, connects every node and edge into a
`StageGraph`, marks the nodes that invocation owns dirty, and executes the
active nodes in `topologicalOrder()` when `plan()` allows it. The entry
points and node counts are:

| Entry point | Nodes | Use |
|-------------|-------|-----|
| `loadQuickFromRaw()` | `source.embedded`, `thumbnail.decode` | embedded-JPEG fast path |
| `loadFromRaw()` | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base` | raw fallback |
| `quickProcessImage()` | `thumbnail.scale`, `thumbnail.transform` | embedded continuation |
| `processImage()` | `thumbnail.scale` … `thumbnail.output` (6) | profile continuation |

The union is all twelve nodes. The nodes an invocation does not own are
marked clean: their data is already held by the `Thumbnail` (`thumbImg`) or
belongs to the branch that invocation does not take, so skipping them is
correct and matches the legacy code. If `plan()` would skip a node the legacy
code executes, the plan is rejected, `thumbnail-graph: <entry>: fallback to
legacy: plan would skip <node>` is logged once, and the unchanged legacy
sequence runs.

The extraction is a move, not a rewrite: `loadQuickFromRaw()`,
`loadFromRaw()`, `quickProcessImage()` and `processImage()` were split into
the private `Thumbnail::stage*` methods, and both the legacy sequence and the
graph sequence call those same methods in the same order. No arithmetic,
buffer type, thread count or ordering changed, and `thumbnail.scale` keeps
one implementation per continuation (8-bit nearest for the embedded branch,
float bilinear for the profile path).

Reduced domain only: the graph covers the normal embedded-JPEG and raw
fallback thumbnails. Inspector mode (`ThumbnailInspectorMode::RAW` and its
`RAW_IF_NOT_JPEG_FULLSIZE` fallback) loads a full-resolution image and stays
on the legacy path; it executes no graph node. The graph does not touch the
file-browser thumbnail cache, its `.rtti` files or the cache invalidation
keys.

Unset, empty, `0` and `off` (case-insensitive) keep the legacy thumbnail
sequence, which remains the default.

Evidence (CPU build with OpenCL disabled, single-threaded host run,
`OMP_NUM_THREADS=1`, representative NEF sample):

- `thumbnail-stage-test` reports `PASS embedded` and `PASS raw` and the saved
  PPM files are byte-identical between the two passes. Neutral parameters at
  160 px: embedded `240x160`, sha256
  `f433fb4d519f1af8b51f3f1cada112a02df39824277a1837702c50d4e5540b3f` for
  both passes; raw fallback `240x160`, sha256
  `a60c8d6612fd979ca1e694108bf8b193442dca7bb497c8614c8b2567a1185165` for
  both passes.
- The sample sidecar at 200 px (`[Exposure]`, `[Vibrance]`,
  `[White Balance]`, `[Shadows & Highlights]`, `[Wavelet]`, `[RGB Curves]`)
  gives embedded `300x200`, sha256
  `6a41827293458268929be4fa5857547c4b43cc7e83c3a1a7f00f593ede402159` for
  both passes, and raw fallback `300x200`, sha256
  `3861307ec90fb952af876f1a2c5bfcbbc2fc73dc5cec09f45d113a41b6628a20` for
  both passes.
- The four accepted plans are logged as 2 (`source.embedded`,
  `thumbnail.decode`), 2 (`thumbnail.scale`, `thumbnail.transform`), 4
  (`source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base`) and 6
  (`thumbnail.scale` … `thumbnail.output`) canonical nodes: 12 distinct nodes,
  no fallback and no faked node.
- The harness is `tools/perf/thumbnail-stage-test.cc`; build it with
  `cmake --build <build-dir> --target thumbnail-stage-test` and run
  `build/<dir>/rtengine/thumbnail-stage-test <raw> [pp3] [height] [out-dir]`.
  `tools/perf/run-suite.sh --thumbnail-graph` wraps it as an optional gate.
- The same harness passes on the OpenCL build and produces byte-identical
  PPM files to the CPU build (embedded and raw fallback), so the thumbnail
  path is not variant-dependent.
- Fail-closed check: a temporary, uncommitted change that marked the owned
  nodes clean made all four entry points log
  `thumbnail-graph: <entry>: fallback to legacy: plan would skip <node>` and
  run the legacy sequence; the harness still reported `PASS embedded` and
  `PASS raw` with unchanged pixels. The change was reverted before commit.
- No `rtgui/` file changed, so the thumbnail cache keys, `.rtti` contents and
  invalidation rules are untouched.
- Builds: `tools/build-rt.sh cpu -j 6 --ccache --stage-modules` and
  `tools/build-rt.sh opencl -j 6 --ccache --stage-modules` both complete;
  `stage-pipeline-test` and `stage-graph-test` pass.

## Preview stage graph and dump

`RT_PREVIEW_GRAPH=1` executes the canonical full-frame preview inventory
through `StageGraph`; `RT_STAGE_GRAPH=1` selects the same path when
`RT_PREVIEW_GRAPH` is unset. Unset, empty, `0` and `off` (case-insensitive)
keep the legacy sequence, which remains the default and stays byte-identical.

### Runtime mapping

Every block of the full-frame pass is now one private
`ImProcCoordinator` method. The legacy sequence and the graph sequence call
the same methods in the same order, so the two paths cannot drift apart.

| Node | Method | Event-mask gate |
|------|--------|-----------------|
| `source.select` | `stageSourceSelect()` | preprocess bits or high detail newly needed |
| `raw.preprocess` | `stageRawPreprocess()` | same |
| `raw.demosaic` | `stageRawDemosaic()` | `M_RAW`, `M_CSHARP`, `M_WB`, modified RGB source, high detail |
| `raw.base` | `stageRawBase()` | `M_INIT`, `M_LINDENOISE`, `M_HDR`, `M_RETINEX`, `M_SPOT`, pending spot copy |
| `analysis` | folded into `raw.base` | the base rebuild |
| `transform` | `stageTransform()` | `M_HDR`, `M_TRANSFORM`, `M_RGBCURVE`, base rebuild, `needsTransform()` |
| `local.adjustments` | `stageLocalAdjustments()` | `M_AUTOEXP`, or `M_RGBCURVE`/`M_CROP` with local spots |
| `rgb.proc` | `stageRgbProc()` | `M_RGBCURVE`, `M_CROP` |
| `lab.tools` | `stageLabTools()` | `M_LUMACURVE`, `M_CROP`, the Lab chain |
| `lab.wavelet` | `stageLabWavelet()` | the Lab chain |
| `working.trc` | `stageWorkingTrc()` | the Lab chain |
| `ciecam` | `stageCieCam()` | the Lab chain |
| `gamut` | `stageGamut()` | the Lab chain |
| `monitor.output` | `stageMonitorOutput()` | monitor conversion or a panning-related change |

The Lab chain is the hand-written mask `(todo & (M_LUMINANCE + M_COLOR)) ||
(todo & M_AUTOEXP)`.

A node keeps the event-mask gate the hand-written block used, because a
preview block decides how much of its output a mask allows, not only whether
it runs: `raw.base` rebuilds the base for one class of bits, re-prepares
retinex for another and re-applies spots for a third. The plan decides whether
the node is executed; the node decides what the mask permits inside it. The
gates used for dirty marking are the same predicates the nodes use, and are
documented supersets of the block's internal guards, so a plan can only skip a
node that would have done nothing.

`analysis` is folded into `raw.base`. `firstAnalysis()` is the final statement
of the base-rebuild guard, and the spot pass that closes `raw.base` mutates
`orig_prev` immediately afterwards; extracting it would reorder the pass and
change the analysis input. The inventory keeps the node and the plan tracks its
dirty state, but the body runs inside `raw.base`. Folded nodes: one
(`analysis`), for that reason.

Three statements that were unconditional in the hand-written pass stay in the
caller rather than in a node, because a node can be skipped:

- the working-pointer reset `oprevi = orig_prev` (the previous pass may have
  left `oprevi` on a deleted fork);
- the local-adjustment damping floor for selective-editing capture sharpening;
- the frame-count and image-type notices.

The Color/Coloropp white-balance augmentation (`todo |= DEMOSAIC`) also runs in
the caller: it is a pure function of the mask, and the plan must see the mask
the pass will actually use.

### Plan and validation

Each full-frame pass builds `buildPipelineSpec(StagePath::Preview)`, connects
its 14 nodes and 13 edges into a `StageGraph`, marks each node dirty from the
event mask, the high-detail state and the pending spot copy, and asks
`plan()` for the actions before anything executes. A node the pass must run may
not come back `ReuseOutput`, and every scheduled node must have an executor;
either failure rejects the plan, logs `preview-graph: fallback to legacy:
<reason>` once, and runs the unchanged legacy sequence. Once per process the
accepted plan is logged as `preview-graph: active, N of 14 canonical nodes
executed from the preview inventory (analysis folded into raw.base)`.

Execution walks the plan's topological order. `monitor.output` runs at its
legacy site, after the crop render, because the monitor profile cache refresh
it follows feeds the crop viewport. `RT_STAGE_PROFILE=1` records
`preview.graph.plan` and one key per node id.

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

### Evidence

Harness: one Xvfb instance per configuration, a fresh settings directory and
cache, a copy of the 45.7 MP NEF, `OMP_NUM_THREADS=1`, and a fixed sequence of
editor actions. After each action the harness waits for `preview.json` to stop
changing and copies `preview.tif`, `work.tif` and `preview.json`; the event
mask recorded in `preview.json` proves the scenario produced the intended
pass. The image, sidecar and dumps stay outside the repository.

| Scenario | Editor action | Recorded mask |
|----------|---------------|---------------|
| initial fit | none | `0x4000`-`0x4003` |
| output-profile change | Color Management output profile | `0x4280` |
| exposure change | Exposure compensation stepper | `0x1009f` |
| tone-curve change | tone curve control point | `0x401f` |
| monitor-profile change | soft-proof profile selection | `0x4000` |

- CPU build: all five scenarios are byte-identical between
  `RT_PREVIEW_GRAPH=0` and `1` for both `preview.tif` and `work.tif`
  (`cmp`, `AE=0`).
- OpenCL build: the same five scenarios are byte-identical between the two
  paths, and the legacy dumps are `AE=0` against the CPU build, so the change
  is variant-independent.
- The scenarios are not vacuous: the output-profile pass changes `work.tif`
  (`AE=456992`) while leaving the monitor image untouched, and the exposure,
  tone-curve and monitor-transform passes change both dumps by about
  457 000 samples.
- `preview-graph: active, 13 of 14 canonical nodes executed` appears once per
  run and `preview-graph: fallback` never appears.
- Preview latency on the same build and image, driven through the Exposure
  stepper without a dump: five paired GUI runs. Legacy p50
  391/376/481/348/350 ms and p95 435/481/859/408/393 ms; graph p50
  367/371/367/356/347 ms and p95 445/448/481/407/372 ms. Median p50 is
  376 ms legacy to 367 ms graph (-2.4%) and median p95 435 ms to 445 ms
  (+2.3%), both inside the 5% tolerance: the graph skips nodes whose gates
  are false and the plan itself costs microseconds. One legacy p95 sample of
  859 ms is host noise and is kept in the set rather than dropped.
- Builds: `tools/build-rt.sh cpu -j 6 --ccache --stage-modules` and
  `tools/build-rt.sh opencl -j 6 --ccache --stage-modules` both complete, and
  the stage tests pass on both.

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

`RT_STAGE_PROFILE=1` prints one line per export node plus the legacy coarse
scopes and the finer scopes that were already there:

```text
stage source.select: ...
stage raw.preprocess: ...
stage raw.demosaic: ...
stage raw.base: ...
stage analysis: ...
stage transform: ...
stage local.adjustments: ...
stage rgb.proc: ...
stage finish.rgbproc: ...          (inside rgb.proc)
stage lab.tools: ...
stage lab.sharpening: ...
stage finish.sharpening: ...       (inside lab.sharpening)
stage lab.wavelet: ...
stage finish.wavelet: ...          (inside lab.wavelet)
stage working.trc: ...
stage ciecam: ...
stage gamut: ...
stage crop: ...
stage resize: ...
stage finish.resize: ...           (inside resize)
stage output.sharpen: ...
stage output.convert: ...
stage finish.output: ...           (inside output.convert)
stage framing: ...
stage output.icc: ...
stage export.init: ...             (legacy only, around the first nodes)
stage export.denoise: ...          (legacy only)
stage export.transform: ...        (legacy only)
stage export.finish: ...           (legacy only, around the finish nodes)
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

- Export graph (`RT_STAGE_GRAPH=1`): the graph executes the 20 canonical
  nodes in topological order and logs `stage-graph: active, 20 nodes executed
  from the canonical export inventory`. Ten legacy/graph pairs (neutral,
  sidecar, cropped, output-profile, cropped plus output-profile; 1 and 8
  threads) are `AE=0`, and the legacy path is byte-identical to the pre-change
  binary. A plan that would skip a node is rejected before execution and
  falls back to the legacy sequence with a logged reason.

- Preview graph (`RT_PREVIEW_GRAPH=1`): the graph executes the 13 executable
  canonical nodes in topological order and logs `preview-graph: active, 13 of
  14 canonical nodes executed`; `analysis` is folded into `raw.base`. On the
  CPU and OpenCL builds, five legacy/graph dump pairs (initial fit,
  output-profile, exposure, tone-curve and monitor-profile changes) are
  byte-identical for `preview.tif` and `work.tif`, and no fallback is logged.
  Median p50/p95 are 376/435 ms legacy and 367/445 ms graph (-2.4%/+2.3%),
  inside the 5% tolerance, so the plan-driven skip of idle nodes is not a
  regression.
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

- The preview is graph-driven when `RT_PREVIEW_GRAPH=1`, but the plan has no
  buffer cache behind it: a dirty node recomputes and a clean node is not
  executed at all, so a skipped node still relies on the state the previous
  pass left in the coordinator. `analysis` is folded into `raw.base` for the
  ordering reason above, so the preview executes 13 of its 14 canonical
  nodes. The crop and viewport pipelines are still hand-ordered; the crop
  belongs to the next phase. Export is graph-driven only when
  `RT_STAGE_GRAPH=1`; the legacy path is the default for all three.
- Five export blocks are folded inside a neighbouring node rather than
  having an execution boundary of their own: the framing geometry (in
  `crop`), the Nearest output resize (in `output.convert`), the three
  `raw.base` sub-blocks, the histogram allocation (in `analysis`) and the
  forced B&W pass (in `output.convert`). See `devnotes/perf/STAGE_PIPELINE.md`.
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
