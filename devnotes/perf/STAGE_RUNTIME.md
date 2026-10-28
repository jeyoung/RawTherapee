# Runtime stage armature

This record describes the experimental stage runtime added on
`perf-stage-runtime`. The branch is deliberately opt-in: the default build
keeps the existing image pipeline, while the new layer provides a stable
interface for later cache, tile and OpenCL work.

Vocabulary: the typed `EditorRequest` (facts and dirty blocks) replaced the
invalidation bitmask. `M_*` names below appear only in historical evidence and
in quoted code that was removed; none is a current rule.

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

`rtengine/stage/stage_device.h` defines the device abstraction the graph uses.
The object model is the OpenCL one: a device creates contexts and queues, a
queue executes a compiled program or a node request, and an event carries
status, the executing device and timing.

| Type | Role |
| --- | --- |
| `StageDevice` | type, name, availability, capabilities, context and queue factory |
| `CPUStageDevice` | always available, resolves built-ins and modules through `StageCompiler`, executes inline, deterministic and full precision |
| `OpenCLStageDevice` | owns its ICD loader state, device selection, context and in-order queue; a CPU device is used only with `RT_OPENCL_ALLOW_CPU=1` |
| `StageContext` | owns a device, a per-context program cache, the host allocator and the device residency of its queue |
| `StageQueue` | CPU executes inline; the OpenCL queue submits a node kernel asynchronously, refuses a compiled host program, and runs a node without a device implementation on the host |
| `StageEvent` | status, executing device (`NONE` when nothing ran), asynchronous flag, elapsed time, fallback note and completion handle |
| `StageSubmission` | `wait()`, `poll()` and `elapsed_ns()` for an asynchronous submission |
| `StageDeviceResidency` | device allocations keyed by host buffer address, byte budget, LRU eviction, refusals and accounting |

`StageContext::compile()` calls the existing `StageCompiler`, so the
resolution order and the built-in/module fallback are unchanged. The context
caches the resulting program by stage name and explicit implementation;
`clear_program_cache()` forces the next compile to resolve again. The default
context selects CPU. `StageContext::execute()` fills a `StageExecution` with
the device that actually executed and whether the submission was asynchronous,
so a caller never infers device execution from the selection: the device queue
can refuse an over-budget allocation or a launch and run the host program
instead.

`StageBufferAllocator` is host memory only: zeroed memory with 64-byte aligned
rows and planes, `F32`/`U16` with one to three planes, and `U8` only as a
single packed-RGB plane (multi-plane U8 is refused because `stage::row_bytes()`
triples every U8 row). Device memory goes through `StageDeviceResidency`, which
binds an allocation to the host buffer address that is the node output, reuses
it while address and shape are unchanged, evicts the least recently used idle
entries under the byte budget, and refuses a buffer that does not fit so the
node can run on the host. The budget defaults to 1024 MiB; `RT_STAGE_DEVICE_BUDGET_MB` overrides it, out-of-range values clamp to 16-65536 MiB, and a
non-numeric value keeps the default.

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
`RT_STAGE_INVALID` for dirty contents. `ensure_device()` still reports
`RT_STAGE_FALLBACK`: the runtime moves data through `StageDeviceResidency`
rather than through this wrapper, so the wrapper remains a state machine for a
later scheduler. `mark_host_write()`, `mark_device_write()`,
`mark_both_valid()` and `mark_dirty()` record the transitions, and the device
generation lets a later implementation invalidate copies from an earlier
device session. The wrapper does not own memory; the descriptor stays a
non-owning view over an engine image or an allocator block.

### Device selection and events

CPU is the default. `node_device()` is the execution-path resolver: it reads
`RT_STAGE_OPENCL` (opt-in), consults a per-node override only when one exists,
and returns CPU without building a reason string. `select_node_device()` is the
diagnostic API and adds the reason and the implementation flag.

| Control | Effect |
| --- | --- |
| `RT_STAGE_OPENCL=1` | opt in; nodes with a device implementation are dispatched to the device |
| `RT_STAGE_OPENCL=0`, `RT_STAGE_OPENCL_DISABLE=1` | kill switches; even an explicit device request resolves to CPU |
| `set_opencl_stage_allowed()` | per-thread user policy, set by the preview coordinator (`useOpenCL && openCLMode == 1`) and by the batch export path (`useOpenCL`) |
| `select_node_device(node, device)` | explicit per-node request with the resolved device and reason |
| `set_node_device_override()` | per-node override for tests and embedding; the kill switches still win |

Events are honest about what happened. The CPU queue completes before
`submit()` returns (`asynchronous=false`, `device=CPU`). A device submit
returns `Running` with a completion handle (`device=OPENCL`,
`asynchronous=true`); `poll()` queries the driver and can report `Failed`
early, and `wait()` blocks, reads the output back and only then reports
`Complete`. An event that executed nothing, such as a compiled host program
offered to the device queue or a resolution failure, reports `device=NONE`
rather than naming a device that did not run.

The classic backend and the device node cover the same resize. When
`RT_STAGE_OPENCL=1` selects `lanczos3` and a device is available,
`ImProcFunctions::Lanczos` (both the `Imagefloat` and the `LabImage` overload)
runs the stage device first; otherwise it keeps the original order and tries
`opencl::resizeLanczos` before the stage runtime. Without the precedence check
the classic path intercepted the node whenever acceleration was enabled and
the device implementation was unreachable. With the switch unset the resolver
declines without probing the ICD, so the default path is unchanged. The parity
and the end-to-end assertion are in `tools/opencl/verify.sh --stage-device`,
which `--quick` also runs.

## Transform node

`transform` is the second device node. Its argument order is two buffers
(three-plane F32 source and destination), eight ints (`cx`, `cy`, `sx`, `sy`,
`oW`, `oH`, `highQuality`, `useLog`) and four doubles (`hscale`, `vscale`,
`cost`, `sint`); `launch_transform` documents the same order.

The destination-to-source map is built on the host in double precision by
`rtengine/transformmap.h`, which the classic backend now uses as well, so the
two paths cannot disagree about the geometry. The interpolation arithmetic
lives once in `rtengine/stage/algorithms/transform_cl.h` and is concatenated
into both kernel sources; only the addressing differs, because the classic
kernel takes one buffer per plane while the device kernel takes one three-plane
buffer with row and plane pitches.

The device kernel runs a one-dimensional range, one work item per destination
pixel, and derives `y` from the flat id. A two-dimensional `get_global_id(1)`
against a 1-D launch silently writes only the first row, which is worth
remembering when adding nodes: the launch and the kernel must agree.

Evidence: the identity geometry is pixel-exact on the device
(`stage-residency-test`, with a mutation check proving the assertion can fail),
and a rotation export matches the CPU reference within 1 LSB at 16-bit on 3.8%
of pixels; both are in `devnotes/opencl/PARITY.md`.

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

`ImageProcessor::normal_pipeline()` runs the canonical export inventory as a
`StageGraph`, and that is the only export sequence: the hand-ordered version
and its `RT_STAGE_GRAPH` kill switch have been removed.
`stage::buildPipelineSpec(StagePath::Export)` supplies all 20 node ids, the 19
edges and each node's reuse mode, so the graph is not restated in the
processing code. The graph is built per job, so every node is dirty and
`plan()` must let every node run.

Execution is guarded before any node runs:

1. the spec node count must be 20 and every node id must have an executor;
2. the graph must be acyclic and the topological order must cover every node;
3. every decision must be `Recompute` or `ReuseInput` (a reuse-input node
   writes in place). A `ReuseOutput` would skip a block; this path has no
   stage buffer cache, so that plan is refused. There is no second sequence to
   run, so the refusal is fatal: one line is logged
   (`stage-graph: fatal: plan would skip the node (<id>)`) and the process
   aborts.

Execution follows `topologicalOrder()`, and each node id selects the private
method that implements the block, so the arithmetic is untouched. An accepted
plan logs `stage-graph: active, 20 nodes executed from the canonical export
inventory` once per process. Fast export (`-f`) still uses its own order: it
calls the same node methods, but runs denoise after the early resize, so it is
not routed through the graph.

`RT_STAGE_PROFILE=1` records one key per node, named by node id
(`source.select` ... `output.icc`), from inside the node method.
`stage_finish()` keeps the coarse `export.init`, `export.denoise`,
`export.transform` and `export.finish` scopes around the same methods for the
fast path, and the pre-existing `finish.rgbproc`, `finish.sharpening`,
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

While the kill switch existed, `RT_STAGE_GRAPH=0` versus `RT_STAGE_GRAPH=1`
gave all ten pairs `AE=0`:

| Variant | 1 thread | 8 threads |
|---------|----------|-----------|
| neutral | `AE=0`, 136 918 080 samples | `AE=0`, 136 918 080 |
| sidecar | `AE=0`, 6 220 800 | `AE=0`, 6 220 800 |
| cropped | `AE=0`, 18 015 003 | `AE=0`, 18 015 003 |
| output profile | `AE=0`, 136 918 080 | `AE=0`, 136 918 080 |
| cropped + output profile | `AE=0`, 18 015 003 | `AE=0`, 18 015 003 |

Legacy byte-identity: the pre-change binary (same commit, built before the
decomposition) and the decomposed legacy path produced identical files for
neutral, sidecar, cropped and output-profile at 1 and 8 threads, after
normalising the TIFF DateTime tag that records the render time. Every pair was
also `AE=0`.

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
nodes, and `stage-pipeline-test`, `stage-graph-test` and `stage-device-test`
pass. The parity runs use `--gpu=off`. A POCL CPU run of the device tests is
recorded in `PARITY.md`; the device path is opt-in and does not affect these
export results.

`buildTilePlan()` produces a cheap block score from a luminance plane and
splits blocks into full and cheap work. It is the hook for selective
denoise/wavelet work; it is not yet wired into those tools.

## Graph-driven thumbnail

The thumbnail path is graph-driven and that is its only sequence: the
`RT_STAGE_GRAPH` switch and the hand-ordered fallback have been removed. Each
entry point builds `buildPipelineSpec(StagePath::Thumbnail)`, connects every
node and edge into a `StageGraph`, marks the nodes that invocation owns dirty,
and executes the active nodes in `topologicalOrder()`. The entry points and
node counts are:

| Entry point | Nodes | Use |
|-------------|-------|-----|
| `loadQuickFromRaw()` | `source.embedded`, `thumbnail.decode` | embedded-JPEG fast path |
| `loadFromRaw()` | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base` | raw fallback |
| `quickProcessImage()` | `thumbnail.scale`, `thumbnail.transform` | embedded continuation |
| `processImage()` | `thumbnail.scale` … `thumbnail.output` (6) | profile continuation |

The union is all twelve nodes. The nodes an invocation does not own are
marked clean: their data is already held by the `Thumbnail` (`thumbImg`) or
belongs to the branch that invocation does not take, so skipping them is
correct. If `plan()` would skip a node the invocation owns, the plan is
refused: `thumbnail-graph: <entry>: fatal: plan rejected: <node>` is logged and
the process aborts, because there is no legacy sequence left to run.

The extraction is a move, not a rewrite: `loadQuickFromRaw()`,
`loadFromRaw()`, `quickProcessImage()` and `processImage()` were split into
the private `Thumbnail::stage*` methods, and the graph calls those same
methods. No arithmetic, buffer type, thread count or ordering changed, and
`thumbnail.scale` keeps one implementation per continuation (8-bit nearest for
the embedded branch, float bilinear for the profile path).

Reduced domain only: the graph covers the normal embedded-JPEG and raw
fallback thumbnails. Inspector mode (`ThumbnailInspectorMode::RAW` and its
`RAW_IF_NOT_JPEG_FULLSIZE` fallback) loads a full-resolution image through its
own code path; it executes no graph node. The graph does not touch the
file-browser thumbnail cache, its `.rtti` files or the cache invalidation
keys.

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
  no faked node.
- The harness is `tools/perf/thumbnail-stage-test.cc`; build it with
  `cmake --build <build-dir> --target thumbnail-stage-test` and run
  `build/<dir>/rtengine/thumbnail-stage-test <raw> [pp3] [height] [out-dir]`.
  `tools/perf/run-suite.sh --thumbnail-graph` wraps it as an optional gate.
- The same harness passes on the OpenCL build and produces byte-identical
  PPM files to the CPU build (embedded and raw fallback), so the thumbnail
  path is not variant-dependent.
- Fail-closed check: a temporary, uncommitted change that marked the owned
  nodes clean made all four entry points log the plan rejection and abort;
  with the change reverted the harness reproduced the reference PPM files. The
  refusal used to fall back to the legacy sequence; the graph-only build aborts
  instead.
- No `rtgui/` file changed, so the thumbnail cache keys, `.rtti` contents and
  invalidation rules are untouched.
- Builds: `tools/build-rt.sh cpu -j 6 --ccache --stage-modules` and
  `tools/build-rt.sh opencl -j 6 --ccache --stage-modules` both complete;
  `stage-pipeline-test` and `stage-graph-test` pass.

## Preview stage graph and dump

The canonical full-frame preview inventory is the only preview sequence: it
executes through `StageGraph`, and the `RT_PREVIEW_GRAPH`/`RT_STAGE_GRAPH`
switches and the hand-ordered fallback have been removed.

### Runtime mapping

Every block of the full-frame pass is now one private
`ImProcCoordinator` method. The graph executes the plan's order through those
methods, so the inventory and the executor cannot drift apart.

| Node | Method | Event-mask gate |
|------|--------|-----------------|
| `source.select` | `stageSourceSelect()` | preprocess bits or high detail newly needed |
| `raw.preprocess` | `stageRawPreprocess()` | same |
| `raw.demosaic` | `stageRawDemosaic()` | `rawIntent`, `captureSharpenIntent`, `colorHlrWbRequest`, modified RGB source, high detail |
| `raw.base` | `stageRawBase()` | work-order items `rawBaseRebuild`, `retinexPrepare`, `spotApply`, `spotRestore`, `analysisRun` |
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

The Lab chain is the typed fact `labChainIntent`, derived once in the adapter
from `M_LUMINANCE`, `M_COLOR` and `M_AUTOEXP`.

A node keeps the sub-step gate the hand-written block used, because a preview
block decides how much of its output a request allows, not only whether it
runs: `raw.base` rebuilds the base for one work item, re-prepares retinex for
another and re-applies spots for a third. The plan decides whether the node is
executed; the node decides what the request permits inside it. The raw body is
now typed: `stageRawDemosaic()` reads `rawIntent`, `captureSharpenIntent` and
the local `demosaicRebuilt` fact, and `stageRawBase()` reads only its work
items. The remaining nodes keep their legacy predicate until step 5; none of
them decides node entry. The gates used for dirty marking are the same
predicates the nodes use, and are documented supersets of the block's internal
guards, so a plan can only skip a node that would have done nothing.

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

The Color/Coloropp white-balance augmentation (`markColorHlrDemosaic()`) also
runs in the caller: it is a pure function of the request facts, and the plan
must see the request the pass will actually use.

### Plan and validation

Each full-frame pass builds `buildPipelineSpec(StagePath::Preview)`, connects
its 14 nodes and 13 edges into a `StageGraph`, marks each node dirty from the
parameter fingerprints and render state (see *Preview dirty blocks* below; the
event mask was the authority before that change), and asks `plan()` for the
actions before anything executes. A node the pass must run may
not come back `ReuseOutput`, and every scheduled node must have an executor;
either failure refuses the plan: `preview-graph: fatal: plan rejected:
<reason>` is logged and the process aborts, because there is no second
sequence to run. Once per process the accepted plan is logged as
`preview-graph: active, N of 14 canonical nodes executed from the preview
inventory (analysis folded into raw.base)`.

Execution walks the plan's topological order. `monitor.output` runs at its
legacy position, after the crop render, because the monitor profile cache
refresh it follows feeds the crop viewport. `RT_STAGE_PROFILE=1` records
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

- CPU build: all five scenarios were byte-identical between
  `RT_PREVIEW_GRAPH=0` and `1` for both `preview.tif` and `work.tif`
  (`cmp`, `AE=0`) while the switch still existed.
- OpenCL build: the same five scenarios were byte-identical between the two
  paths, and the legacy dumps were `AE=0` against the CPU build, so the change
  is variant-independent.
- The scenarios are not vacuous: the output-profile pass changes `work.tif`
  (`AE=456992`) while leaving the monitor image untouched, and the exposure,
  tone-curve and monitor-transform passes change both dumps by about
  457 000 samples.
- `preview-graph: active, 13 of 14 canonical nodes executed` appeared once per
  run and no plan rejection was logged.
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

## Preview dirty blocks

The first graph tranche scheduled a preview node from the legacy `M_*` event
mask: `planPreviewPass()` evaluated the same gate the hand-written block used
and marked the node dirty from it. That made the mask the execution authority
of the graph as well as the notification, coalescing and crop-render input.

This tranche separates the two. The full-frame preview graph now derives its
dirty set from the preview parameter fingerprints and `RenderStateKey`;
`rtengine/stage/stage_dirty_blocks.h` holds the derivation, shared with the
crop viewport. The `M_*` mask keeps every other job it had: it still
drives the viewport fast path, the deferred-analysis queue, the crop-render
trigger, the notifications and the compatibility payload. The downstream node
gates and the converted producers are described under "Typed editor requests
and downstream gates" below.

### Derivation

The unit is `stage_fingerprints()` / `derive_dirty_blocks()` /
`close_dirty_blocks()`, documented in `devnotes/perf/STAGE_PIPELINE.md`. The
coordinator calls the compatibility adapters over those primitives, which keep
the call shape of the first draft of this tranche:

| API | Input | Result |
| --- | --- | --- |
| `dirtyBlocksFromParams(previous, current, path)` | two `ProcParams` | the execution set behind a parameter change |
| `snapshotParams(params, path)` | one `ProcParams` | one fingerprint per descriptor, for the next pass |
| `dirtyBlocksFromSnapshot(snapshot, current, path)` | snapshot and `ProcParams` | the same result without copying the previous parameters |
| `dirtyBlocksFromRenderState(previous, current, path)` | two `RenderStateKey` | the execution set behind an explicit state change |
| `closeOverGraph(blocks, path)` | any set | the set plus every downstream consumer |
| `allDirtyBlocks(path)` | — | every canonical node, used when no previous state exists |
| `mergeDirtyBlocks(a, b)` | two sets | union, valid only when both inputs are valid |

Every adapter returns the execution set of `derive_dirty_blocks()`: direct
blocks, coupled group, residents and forward closure, with `ids`, `valid` and
`reason` filled. The preview plan marks those ids dirty and lets
`StageGraph::plan()` propagate the rest, so the two closures agree.

The parameter comparison uses the same fingerprint a stage cache key uses, so
a block is dirty exactly when one of the `ProcParams` members it consumes
changes. Whole sub-structs are hashed, so the result is an over-approximation:
a member the block ignores still dirties it, which costs work but never shows a
stale frame.

The render-state comparison covers what `ProcParams` cannot carry. The shared
table is in `stage_dirty_blocks.cc` and is listed per path in
`STAGE_PIPELINE.md`; the preview rows are:

| Field group | Preview blocks | Why |
| --- | --- | --- |
| preview geometry, including the crop or ROI rectangle | all | a new scale, allocation or rectangle re-renders the pass |
| high-detail decision and completed work | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base`, `analysis`, `transform` | the demosaic mode and the base change |
| source generation | `source.select`, `raw.preprocess`, `raw.demosaic`, `raw.base` | a replaced source invalidates the raw core; the closure reaches the rest |
| modified RGB source | the same raw core | the demosaic output changed |
| monitor profile, intent, soft proof, gamut check | `monitor.output` | the display sink |
| sharpening mask, FAST toggle, full-resolution record, rough flag | the raw core | one aspect covers four switches, so its answer is their union |
| request kind | none, sink or all | a parameter-only request schedules nothing; a repaint the sink; a high-quality or full request the path, because the request may stand for work no field delta describes |
| previous preview base | `source.select` … `transform` | a different base was built; read by the state adapter, because the aspect table groups it with the request |

`StageGraph::plan()` propagates dirtiness through the pipeline edges, so a
caller marks only the entry block. `closeOverGraph()` produces the same closure
explicitly for diagnostics and tests.

### Coordinator integration

`ImProcCoordinator` keeps two snapshots of the last completed full-frame pass:

- the parameter fingerprints, taken **before** the nodes run, because the pass
  normalises white balance and similar deterministic values in place, while the
  GUI keeps the un-normalised values;
- the `RenderStateKey`, rebuilt **after** the nodes run, because its high-detail
  fields record the work the pass completed.

`previewDirtyBlocks()` merges the two differences, adds the folded `analysis`
node and records one residual input that `RenderStateKey` cannot carry yet: a
pending spot buffer that the next `raw.base` restores. The request kind is the
second such input: `M_HIGHQUAL`, `M_INIT`, `M_LINDENOISE` and `M_HDR` are
request-level bits the render state has no field for, so the adapter maps them
once to `HighQuality` and `FullPreview` in the typed `EditorRequest` and the
state adapter treats an absolute request as a whole-path rebuild. An ordinary
pass is `ParamsOnly`. The plan never reads the mask.
The snapshots are dropped on `assign()`, `freeAll()`, `stopProcessing()` and the
input-ICC reference path, so the next pass is all-dirty whenever the buffers
they describe are gone. A pass that does not take the full-frame path (the
viewport fast path or an aborted request) does not update them.

### Base-cache and deferred scheduling typing

`baseCacheInvalidationFor()` replaces the mask mapper with a typed table:
`raw.preprocess` maps to `preprocess`, `raw.demosaic` to `raw`, `raw.base` (the
white-balance request fact) to `white-balance`, and the high-detail request
fact to `high-detail-request`, in that precedence. The coordinator derives a
narrow base-cache input set and never passes the pass execution set, so a spot,
film-negative, Retinex or tone-curve change still leaves the snapshot reusable.

The deferred burst is a `DeferredRecord`: a `DirtyBlocks` set, an explicit
`forcedAnalysis` flag, the hit count and the first-hit/deadline timestamps.
Absorbing it merges only the typed downstream stages into the pass dirty set.
The render path reads a packed 64-bit `DeferredAnalysisSnapshot` with one
relaxed atomic load; writers update the authoritative record under
`paramsUpdateMutex` and then publish the snapshot with release ordering.

`RenderStateKey::deferredAnalysisMask` was removed, so the persisted
render-state fingerprint format changed. The published golden value in
`stage-render-state-test` moved from `7ad04e9ccb3fb3abd888a235cc2f2b25` to
`e607b13944b921db473300421f0da1b7`; stored render states and node-cache keys
written by an older build must be discarded, not compared.

The plan is refused, and the process aborts, when the derivation is invalid,
names a block outside the preview inventory, or the graph would skip a dirty
node.

The preview plan has one authority: the parameter fingerprints and the render
state. There is no mask-authority switch and no shadow plan. The typed request
still drives the viewport fast path, the crop render, GUI notification and the
per-node work inside a scheduled node, but it no longer decides which
full-frame nodes are scheduled.

### Dirty-block evidence

`stage-dirty-blocks-test` is the specification test: identical parameters and
identical render state produce no dirty block; every render-state group dirties
exactly the blocks of the table above on each path; a crop rectangle change of
the parameters dirties only `transform`, which is why the render state carries
the rectangle; the closure adds every downstream consumer; the snapshot form
equals the ProcParams form; every adapter returns the same execution set as
`derive_dirty_blocks()`, including the resident sink; and every id returned is
a canonical node of the path. The other stage tests (`stage-pipeline-test`,
`stage-render-state-test`, `stage-param-map-test`,
`viewport-admission-test`) still pass.

The latency and screenshot evidence of the first preview draft was measured
against the standalone unit and is not carried over: the shared derivation is
wider in three table rows, so those figures do not describe this code. This
tranche measures the unified unit through the exact dump the pass produces.

While the preview kill switch existed, `RT_PREVIEW_GRAPH=0` (legacy
sequence) and `RT_PREVIEW_GRAPH=1` (graph) ran the
same scripted session in an isolated settings and cache directory with
`OMP_NUM_THREADS=1` and `RT_PREVIEW_DUMP=<dir>`. The session loads the 45.7 MP
NEF in place (the raw file and the dumps stay outside the commit; the dumps
live under the gitignored `build/test-scratch/`), applies one edit through the
real `StagedImageProcessor` API, and the final `preview.tif`/`work.tif` pair of
the two runs is compared with `magick compare -metric AE`.

| Scenario | Edit | Second-pass todo | preview AE | work AE | Transition AE |
| --- | --- | --- | --- | --- | --- |
| initial fit | open the image | — (1 pass) | 0 | 0 | n/a |
| exposure | `expcomp += 0.7` | 16415 (`AUTOEXP`) | 0 | 0 | 2 852 450 / 2 852 450 |
| output profile | `icm.outputProfile` `RTv4_sRGB` -> `RTv4_Large` | 17024 (`M_MONITOR`) | 0 | 0 | 0 / 2 847 720 |
| monitor profile | monitor profile `""` -> `RTv4_Large` | 16384 (`M_MONITOR`) | 0 | 0 | 2 847 760 / 0 |

The transition column is the difference between the scenario dump and the
initial-fit dump of the same authority (preview / work), so a non-zero value
proves the edit exercised a real pass. The two profile rows also show the
expected separation: an output profile reaches only `work.tif`, a monitor
profile only `preview.tif`.

Historical evidence from the retired shadow tranche (the mask vocabulary in
this paragraph is the vocabulary of that era): across the four graph runs the
shadow log reported zero mismatches and two over-approximations. On the
exposure pass (`mask=0x401f`) the derivation adds
`raw.base`: the event mask leaves the base to the retinex sub-path, the block
runs and its own gate declines the work. On the output-profile pass
(`mask=0x4280`, which carries the injected `M_INIT | M_HDR` rebuild bits) it
adds `source.select`, `raw.preprocess` and `raw.demosaic`: the request is
absolute, so the derivation opens the raw core, and the raw nodes return
immediately because their own gates see no source change. Both are the
documented direction (`extra` is inert, `missing` would fail closed) and the
pixels are identical.

The crop harness found the one unsafe case during reconciliation. An earlier
build labelled every full pass `FullPreview` and rebuilt only when the kind
changed; its 100% preview passes then logged `missing=[raw.base]` with mask
`0x1429f`, because the viewport fallback injects `M_INIT | M_HDR` without any
ProcParams or render-state field moving. That is why the request kind is now
built from the request-level mask bits and an absolute request rebuilds the
path unconditionally. The final crop run logs no mismatch.

The preview pass plan was not timed on this branch. The dump-parity runs show
one extra inert block call on a single pass and identical plans elsewhere, so
no latency change is claimed rather than assumed.

Builds: `tools/build-rt.sh cpu -j 6 --ccache` and `tools/build-rt.sh opencl
-j 6 --ccache` both complete with no warning in the changed translation units.
`stage-dirty-blocks-test`, `stage-pipeline-test`,
`stage-render-state-test` and `stage-param-map-test` pass in both build
directories.

## Crop dirty blocks

The crop graph no longer takes its dirty set from the `M_*` mask. `Crop`
derives it from the previous and current per-block parameter fingerprints and
from the crop `RenderStateKey`, and uses it to mark the canonical nodes dirty.
The derivation itself lives in `rtengine/stage/stage_dirty_blocks.h/.cc` and
is the same unit the preview coordinator uses through the adapters above; the
rules and tables are in `devnotes/perf/STAGE_PIPELINE.md`.

The crop plan has one authority: the derived dirty set. There is no
`RT_CROP_DIRTY` mode and no mask comparison. The typed request keeps the raw
reload decision: the init, line-denoise and HDR families ask for the
destructive raw stage to be reloaded; the crop carries that as
`RequestKind::FullCrop`, so the derivation rebuilds the path instead of
skipping a reload no fingerprint describes.

`crop_graph` exposes `runs()`, `fallbacks()` and `dirtyDerivations()` as
process-wide counters. `RT_CROP_DIRTY_TRACE=1` prints one line per derivation
with the request kind and identity, the forced flag, the key parameter values
and the directly dirty blocks.

`crop-stage-test --sequences [--only=<scenario>]` drives one edit per scenario
inside a live session and compares the incremental crop with a fresh render of
the final state. `--bench=<n> [--bench-edit=crop]` times repeated updates.
There is no legacy execution path or runtime mask baseline.

Re-run on 2026-10-25 against the unified unit: same image,
`OMP_NUM_THREADS=1`, `--sequences`. The harness reports `33 plans executed, 0
plan fallbacks, 33 dirty derivations` and `crop-stage-test: PASS`; every static
and incremental scenario is `AE=0` for display and output-space images. The
full preview/crop/thumbnail gate also passes with the mask switches removed.

## Typed editor requests and downstream gates

This phase finishes the request migration for the full-frame preview and the
crop viewport. `EditorRequest` is the only planning and execution input.

### Downstream node entry

Preview node entry is now the plan: a node is executed only when the dirty set
names it. The five mask helpers (`transformMayRun()`,
`localAdjustmentsMayRun()`, `rgbProcRuns()`, `labPipelineRuns()` and
`labToolsRun()`) are gone. `previewDirtyBlocks()` seeds the nodes named by the
typed intent facts (`transformFamilyIntent`, `hdrIntent`, `rgbProcIntent`,
`labChainIntent`, `autoExposureIntent`, `localAdjustmentsIntent`) and closes
the set once, after every coordinator-side insertion. The plan therefore
covers the full envelope: a node cannot be skipped because a request family
happened to be narrower than the parameter delta.

The body still needs to know *how much* of a scheduled block the request
allows. Those decisions read the typed facts, never the mask:

| Body decision | Typed fact |
| --- | --- |
| transform applies the pre-RGB directional-pyramid equaliser | `transformFamilyIntent` |
| transform applies dehaze/Fattal | `hdrIntent` |
| local adjustments recompute auto exposure | `autoExposureIntent` |
| local adjustments rerun the spots | `localAdjustmentsIntent` |
| rgb.proc applies the tone/colour chain | `rgbProcIntent` |
| Lab tools, sharpening, wavelet, working TRC, CIE CAM and gamut touch pixels | `labChainIntent` |
| monitor.output renders | `monitorOutputRequest` |

Crop entry already came from its own plan. The crop bodies that used to test
`todo` now read the same facts, because the crop plan over-approximates: the
resident `transform` block closes over every consumer on each update, so plan
membership alone cannot tell a real downstream change from a resident-only
pass. `Crop::update()` also takes the typed request: its render state now maps
`request.kind` to the crop kind, `request.imperativeRepaint` to
`viewportMoved`, and `request.spotPending` to the spot fact. No crop planning
decision reads a mask.

#### Forced crop passes

The crop performs every step when the buffers were reallocated or the request
is high quality, because the new or accurate base needs a full refill. The
typed request describes only what the editor asked for, so every body gate
whose legacy predicate was `todo & M_*` also consults the force; otherwise a
reallocating pass or a bare `M_HIGHQUAL` request (opening a detail window,
`imagearea.cc:809`) skips work that `ALL` used to admit.
`CropStageContext::forcedFull` carries that force, and the gates are:

| Body | Legacy predicate | Gate |
| --- | --- | --- |
| dehaze/Fattal | `todo & M_HDR` | `hdrIntent \|\| forcedFull` |
| transform pre-RGB equaliser | `todo & (M_TRANSFORM \| M_RGBCURVE)` | `transformFamilyIntent \|\| forcedFull` |
| local adjustments | `todo & (M_AUTOEXP \| M_RGBCURVE)` | `localAdjustmentsIntent \|\| forcedFull` |
| rgb.proc | `todo & M_RGBCURVE` | `rgbProcIntent \|\| forcedFull` |
| Lab tools, sharpening, wavelet, working TRC, CIE CAM, gamut | `todo & (M_LUMINANCE + M_COLOR)` | `labChainIntent \|\| forcedFull` |

`viewportMoved` is read before the force and stays request-only, because
`M_MINUPDATE` is not part of `ALL`. `Crop::fullUpdate()` already builds its
request from `ALL | M_MONITOR`, so it needs no extra fact. The same force is
folded into `crop_request_kind()`, so the work order names `rawBaseRebuild`,
`analysisRun` and, at 100%, `denoiseRun`, and the bodies start from fresh
source pixels, as the `ALL` reload bits did. `M_SPOT` belongs to the same
class even though spot apply is work-order driven: `ALL` carries it, and a
refilled base loses the removal unless the spot work runs again, so
`spotPending` is true for a forced pass when spots are configured. The
regression is covered by two `crop-stage-test --sequences` states: a bare
`M_HIGHQUAL` update that enables dehaze (`highqual-dehaze`) and an
`M_HIGHQUAL` exposure edit over a base with local adjustments and a visible
spot (`highqual-refill`), both compared with a fresh full render and required
to be byte-identical.

`stageTransform()` keeps `pass.transformNeeded` as a cross-check on the plan;
it is a parameter/metadata predicate, not a mask test. The raw/core bodies keep
their block-1 work-order authority and their internal compatibility reads.

Every execution-payload augmentation in `updatePreviewImage()` is a named
intent method on `EditorRequest`, and each sets the typed facts before any
consumer runs. The post-crop-vignetting augmentation applies the transform
family through `markCropTransform()`, which recomputes the kind,
`fullFrameIntent`, `spotPending` and the remaining facts. The viewport fast
path adds the luminance and colour families for a luminance-curve or
auto-exposure crop through `markLabPixels()`, so the crop bodies read the same
intents the request implies.

A forced crop pass forces `origCrop` to be refilled even when the editor
request is a plain parameter update. `crop_request_kind()` maps the render
state to `FullCrop` for that pass, so `derive_crop_work()` names
`rawBaseRebuild` and `analysisRun`. The force is the same
`needsinitupdate || HighQuality` that drove the legacy `ALL` widening; like the
legacy `M_INIT | M_LINDENOISE | M_HDR` bits that `ALL` carried, it must refill the
base because the HDR and local-adjustment bodies overwrite `origCrop`. The GUI
normally pairs a reallocation with a `FullCrop` request, but the force no
longer depends on that.

A forced pass reruns every body, but the parameters it renders from are
prepared by the coordinator. The histogram-matched tone curve is a derived
parameter: `toneCurve.histmatching` without `toneCurve.fromHistMatching` asks
the engine to complete the curve from the raw histogram, and every render
consumes it. Completion was reached only through the auto-exposure request,
so a narrow incremental pass rendered from the identity curve while a fresh
pass matched it first. `ImProcCoordinator::matchToneCurve()` owns the
completion; `stageLocalAdjustments()` calls it for any pass whose parameters
need it and `previewDirtyBlocks()` schedules the node for that reason, so the
preview and the crop that follows consume the same parameters whichever
request triggered the pass. The auto-exposure path is otherwise unchanged, and
the viewport fast path still rejects histogram matching.

### Typed producer API

`rtengine/editorrequest.h` defines `RequestIntent`, `make_request()` and the
typed `EditorRequest`. A tool names the operation it asks for (`Transform`,
`WhiteBalance`, `Hdr`, `AllNoRaw`, `Demosaic`, `SpotAdjust`, `Compr`,
`CaptureSharpen`, `DarkFrame`, `First`, `RepaintOnly`, `ControlOnly`,
`AutoExposure`, `LuminanceCurve`, `DirPyrEqualizer`, `RgbCurve`,
`Sharpening`); `make_request()` builds the typed kind and facts in one place.
A producer that knows its blocks passes them as a seed list;
`make_request()` closes over the pipeline edges and adds the path residents,
so the result is an execution set. An empty list keeps the
parameter-fingerprint derivation.

`ProcEventMapper::newEvent(RequestIntent, message)` builds the request and
registers it with `rtengine::EditorRequestRegistry`. The integer
`newEvent(action, message)` overload is deleted. `ImProcCoordinator::
endUpdateParams(ProcEvent)` resolves the registered typed request; a static
`ProcEventCode` resolves through the registry's recorded intent ranges.
`endUpdateParams(int)` has no request meaning and fails fast.

The profile filter is the only boundary that replaces the parameter set
without a raw reload. It registers an `AllNoRaw` request and passes its event
identity, so the dirty set comes from the parameter fingerprints and the raw
families only run when a fingerprint moved.

### The event registry

`rtengine/editorrequest_registry.h/.cc` owns `ProcEvent` identity,
deduplication and history. It allocates a fresh identity for a typed producer
event and stores the typed request. Static `ProcEventCode` identities resolve
through a compact range table of recorded intents; the table is data about the
events the GUI already emits, not a request mask. `ProcEventCode` and the
`ProcEvent` values are unchanged.

### Retired mask surface

The legacy request mask is gone: no `refreshmap.*`, no `stage_mask_map.*`, no
`M_*` mask constant, no `legacyGateMask`, no `from_legacy_mask()`, no
`executionPayload()` and no `viewportCropTodo()`/`viewportCropPayload()`.
Every execution-payload widening is a named method on `EditorRequest`
(`markBaseRefresh()`, `markViewportMoved()`, `markCropTransform()`,
`markColorHlrDemosaic()`, `markDemosaicRebuilt()`, `markLabPixels()`,
`absorbDeferredExecution()`), and each sets the typed facts directly.

Diagnostics print typed data: the preview timer prints the request kind and
identity, the preview dump records the kind and identity, and the crop dirty
trace prints the kind and identity. No diagnostic reconstructs a mask.

`editorrequest-test` scans every engine, GUI and tool source file, ignoring
comments, and fails if `refreshmap`, `stage_mask_map`, a mask constant (apart
from the standard maths macros), a retired helper or an integer `newEvent`
action reappears.

### Evidence

- Both configurations build: `tools/build-rt.sh cpu -j 6` and
  `tools/build-rt.sh opencl -j 6`.
- `editorrequest-test` covers the recorded decision table for every intent,
  the producer-provided execution set, coalescing, the registry lookup for
  dynamic and static events, the producer event-key uniqueness, the
  augmentations, the crop reallocation work order, the tool-to-intent wiring
  and the producer source scan (358 files, 443 typed calls). It also runs the
  mask-reintroduction guard over 663 engine, GUI and tool sources: no retired
  unit, constant, helper or integer `newEvent` action is present.
- Viewport admission: `viewport-admission-test` checks the recorded eligibility,
  LUT and crop decision for every intent, the producer block envelope, the
  coalescing rule and the parameter-veto matrix (42 parameter states x 32
  admission states = 1 344 comparisons).
- All 16 stage test executables pass in the CPU build; the OpenCL build
  compiles the engine, CLI and GUI.
- Preview sweep: `preview-work-order-sweep` runs its state sequence with
  `AE=0` for `preview.tif` and `work.tif`.
- Fast path on: `preview-work-order-sweep --fast-path` reports 5 fast hits and
  5 deferred analyses, one initial fallback, and `AE=0` for `preview.tif`,
  `work.tif` and the visible `crop-main.tif` in all 6 states.
- Crop: `crop-stage-test --sequences --extra-states` reports `48 plans
  executed, 0 plan fallbacks, 48 dirty derivations`, and every state is
  `AE=0` with `OMP_NUM_THREADS=1`. `--stages` reports `6 plans executed, 0
  plan fallbacks, 6 dirty derivations` with `AE=0`.
- Export: the neutral and sidecar exports are `AE=0` against the pre-change
  binary built from the same commit, and the graph export is `AE=0` against
  the neutral export. The `baseline-dev` comparison and the 8-thread result
  are not parity evidence here: `baseline-dev` is 158 commits older, and the
  8-thread delta is the known host float-order variation.
- Thumbnail: `thumbnail-stage-test` reports `PASS` for the embedded and raw
  paths.
- Node cache: `stage-node-cache-test` passes in both builds. Base cache:
  `preview-base-cache-test` passes.
- Control edit timing on the 10.2 MP PEF with its sidecar,
  `OMP_NUM_THREADS=1`, 10 samples per side: the exact typed order p50
  374.4 ms (min 369.3, max 379.4) against the conservative fallback p50
  475.4 ms (min 469.3, max 487.0).
- The cross-branch `baseline-dev` export differs because it is an older
  commit (5.13-158-g15390b0f), not because of this migration; the pre-change
  binary from this commit is byte-identical.



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
  the OpenCL queue refuses a compiled host program and reports `device=NONE`
  for it; that selection falls back to CPU with a reason when OpenCL is
  unavailable; that the buffer state machine, generation and reference count
  behave as documented; and that the context output is pixel-identical to a
  direct `StageCompiler` execution (`AE=0`, 0 differing of 2304 samples). The
  OpenCL build reports `opencl-stage-device: available` when a device is
  present; the CPU-only build exercises the unavailable branch.
- `stage-residency-test` covers device allocation, upload and readback, reuse,
  true LRU eviction order, budget clamping and refusals, the event state
  machine including a driver failure (`poll()` to `Failed`), selection and
  kill switches, and the `lanczos3` device node's parity with an explicit
  executing-device assertion. `RT_REQUIRE_GPU=1` turns a missing device into a
  failure instead of a skip. The suite runner builds it with the other stage
  tests.
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

- Export graph: the graph executes the 20 canonical
  nodes in topological order and logs `stage-graph: active, 20 nodes executed
  from the canonical export inventory`. Ten legacy/graph pairs (neutral,
  sidecar, cropped, output-profile, cropped plus output-profile; 1 and 8
  threads) were `AE=0` while the switch existed, and the legacy path was
  byte-identical to the pre-change binary. A plan that would skip a node is
  refused before execution; the refusal now aborts.

- Preview graph: the graph executes the 13 executable
  canonical nodes in topological order and logs `preview-graph: active, 13 of
  14 canonical nodes executed`; `analysis` is folded into `raw.base`. On the
  CPU and OpenCL builds, five legacy/graph dump pairs (initial fit,
  output-profile, exposure, tone-curve and monitor-profile changes) were
  byte-identical for `preview.tif` and `work.tif`, and no plan rejection was
  logged.
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

## Final default gate

The merged branch runs the graph paths by default. `run-suite.sh --graph
--thumbnail-graph --crop-graph` passes every stage test plus neutral, sidecar
and graph export `AE=0`, thumbnail parity, crop parity and the crop dirty
derivation. Preview dumps remain `AE=0`. The final preview run reported p50
147 ms and p95 183 ms over 12 samples. OpenCL `--quick` and `--formats` both
pass at 1 LSB.

## Graph-only execution

The legacy sequences and the `RT_STAGE_GRAPH`/`RT_PREVIEW_GRAPH` kill switches
have been removed. Export, preview, crop and thumbnail each build the canonical
inventory, validate the plan and execute only the planned nodes; there is no
second sequence. The extracted `stage*` methods are unchanged: the graph still
calls them, and the node order and arithmetic are untouched.

The plan validation is kept and is now fail-closed rather than fall-back:
a plan that would skip a node the path must run, names an unknown node, or
cannot produce a complete topological order logs one diagnostic and aborts
(`stage-graph: fatal: ...`, `preview-graph: fatal: plan rejected: ...`,
`crop-graph: fatal: plan rejected: ...`, `thumbnail-graph: <entry>: fatal:
plan rejected: ...`). A partly processed image is never produced.

The request mask, `refreshmap.*` and the `todo` payload are deleted. GUI
notification and request coalescing use the typed request facts, and the
diagnostics print the request kind and identity. No body gate, crop-render
trigger or node-entry gate reads a mask: `RT_PREVIEW_DIRTY` and
`RT_CROP_DIRTY` are gone.

The gate is unchanged in shape. `run-suite.sh --graph` now requires the export
log to report the 20-node inventory and the export to be byte-identical to the
suite reference, because there is no legacy run to difference against;
`--thumbnail-graph` and `--crop-graph` still require their harnesses to pass
with every plan accepted.

### Graph-only evidence

Host 4C/8T, GCC, generic target; image is the 45.7 MP NEF, single-threaded
where the harness fixes `OMP_NUM_THREADS=1`.

- Gate: `run-suite.sh --graph --thumbnail-graph --crop-graph` on the CPU build
  passes with `failures: 0`. Neutral, sidecar and graph exports are `AE=0`
  against the pre-change `baseline-dev` binary (the sidecar export was also
  `AE=0` with a non-neutral `Pop 2 Lab` profile in an earlier run of the same
  gate); the export log reports
  `stage-graph: active, 20 nodes executed from the canonical export
  inventory`; the thumbnail harness logs all four plans active (2, 2, 4 and 6
  nodes); the crop harness logs `6 plans executed, 0 plan fallbacks` for the
  viewport states and for the stage-coverage run.
- Crop dirty derivation: `33 plans executed, 0 plan fallbacks, 33 dirty
  derivations`, with `AE=0` for every static and incremental scenario.
- Threads: neutral export `AE=0` at 1, 2, 4 and 8 threads.
- Preview latency over 12 samples: p50 144 ms, p95 232 ms.
- OpenCL: the OpenCL build compiles with no new warnings in the four changed
  translation units. `verify.sh --quick` selects the GPU and passes
  (`pef/neutral max_pae=1.5259e-05`, one LSB at 16-bit; monitor self-test
  1 byte), and `verify.sh --formats` passes with 0 failures.
- Preview dumps: the parent-commit binary with `RT_PREVIEW_GRAPH=0` (legacy
  sequence) and the graph-only build run the same scripted session with
  `RT_PREVIEW_DUMP`. For the initial fit (`scale=10`, `todo=0x4003`) and an
  Exposure edit (`scale=10`, `todo=0x1429f`) the final `preview.tif` and
  `work.tif` are `AE=0` (`max_abs=0`, 0 differing of 1 371 168 samples). The
  legacy run logs no `preview-graph` line; the graph-only run logs
  `preview-graph: active`.

### Harness limitation found during the gate

`crop-stage-test --sequences` waits for `PreviewImageListener::imageReady()`,
which the coordinator emits only when `monitor.output` is scheduled. A preview
delta that dirties no preview node (a sharpening-only edit under a profile that
already has the Lab chain warm) produces an empty plan, so no `imageReady`
arrives and the harness waits out its 600 s timeout. This was reproduced
unchanged on the parent-commit binary (`--only=sharpening` with a non-neutral
`Pop 2 Lab` profile stalls; the same run with a neutral profile passes in
24 s), so it is a pre-existing harness/profile interaction, not a regression
from removing the switches. The gate therefore runs with a neutral profile. A
follow-up should give the harness a completion signal that does not depend on
`monitor.output`, or make the preview pass notify once it has executed an
(empty) plan.

## Limitations

- Preview, crop, thumbnail and export run only the graph. No
  graph path caches or reuses buffers yet. The preview executes 13 of its 14
  canonical nodes (`analysis` folded into `raw.base`), the crop 13 of 15
  (`raw.preprocess`/`raw.demosaic` folded into `source.select`), the export
  all 20 and the thumbnail all 12.
- Inspector-mode thumbnails and the thumbnail raw fallback keep their own
  non-graph loading code; they are outside the reduced-domain thumbnail
  inventory and were never part of the legacy graph switch.
- The preview plan has no buffer cache behind it: a dirty node recomputes and
  a clean node is not executed at all, so a skipped node still relies on the
  state the previous pass left in the coordinator.
- The preview dirty set has two residual inputs that `RenderStateKey` does not
  carry yet. A pending spot buffer is added to `raw.base` explicitly, and a
  Preview Demosaic Method preference change while the main preview is at 100%
  can still require a base rebuild: the effective demosaic decision may not
  change, but the next quality crossing handles it.
- The crop dirty-block derivation is the execution-set authority, but the
  stage bodies still use the typed request facts for the work inside a
  scheduled block, because the crop plan over-approximates (the resident
  `transform` closes over every consumer). The derivation cannot express the
  raw reload as a parameter delta; the crop carries the request kind as a
  rebuild request. Removing the body facts and making the raw stage
  incremental are still the next steps.
- The preview adapters return the same execution set as the crop derivation,
  which is wider than the first preview draft in three places: the preview
  geometry row includes the crop rectangle, the high-detail row starts at
  `source.select`, and the editor row covers the sharpening-mask toggle and the
  FAST switches together. Each is an over-approximation; the graph plan
  remains the authority and the body's own gate decides how much work runs.
- The GUI scenario coverage for the dirty-block change is partial. Initial fit,
  exposure, monitor profile, output profile and image reload have dump pairs;
  tone curve and crop geometry do not, because the xdotool harness could not
  drive those panels reproducibly in both runs. Those two are covered by
  `stage-dirty-blocks-test` and by the dump pairs of the transitions that did
  run, and are recorded as a harness gap rather than a PASS.
- Five export blocks are folded inside a neighbouring node rather than
  having an execution boundary of their own: the framing geometry (in
  `crop`), the Nearest output resize (in `output.convert`), the three
  `raw.base` sub-blocks, the histogram allocation (in `analysis`) and the
  forced B&W pass (in `output.convert`). See `devnotes/perf/STAGE_PIPELINE.md`.
- The crop path folds the pre-RGB directional-pyramid equaliser into
  `stageTransform()` and `softLight()` into `stageWorkingTrc()` because both
  are position-sensitive in the legacy order.
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
- The OpenCL device executes two nodes, `lanczos3` (resize) and `transform`
  (rotation/scale). Every other node has no device implementation and resolves
  to the host program with a reason. The device path is opt-in
  (`RT_STAGE_OPENCL=1`) and nothing in the default configuration runs on a
  device.
- The classic backend and the device node cover the same resize and transform.
  With `RT_STAGE_OPENCL=1` and a device available the stage device runs first, so
  `--gpu=auto` exports reach it; with the switch unset the classic backend
  handles the node and no stage record is emitted. The batch policy still
  follows `useOpenCL`, so `--gpu=off` disables the graph device as well.
  `verify.sh --stage-device` (also in `--quick`) asserts the `impl=opencl`
  record and the 1 LSB parity against the CPU reference. A forced refusal
  (`RT_STAGE_DEVICE_BUDGET_MB=16`, `RT_REQUIRE_GPU=1`) makes it FAIL, so the
  assertion is not vacuous.
- The context program cache is keyed by stage name and explicit
  implementation, so it reads `RT_STAGE_*` and `RT_STAGE_PATH` on the first
  compile of a key. Call `clear_program_cache()` to re-resolve after changing
  the environment or rebuilding a module.
- `StageBufferState` is non-owning and has no readback path: `ensure_host()`
  reports `RT_STAGE_FALLBACK` while the device copy is authoritative, and
  `ensure_device()` reports `RT_STAGE_FALLBACK` on CPU. The runtime uses
  `StageDeviceResidency` for device transfers, not this wrapper.
- The kill-switch policy is per-thread, matching `opencl::setStageAllowed()`;
  a thread that never sets it defaults to allowed and still needs the
  environment opt-in.
- Parameter fingerprints now drive the preview and crop dirty-block sets, and
  the graph paths are default. GUI notification, coalescing and diagnostics
  use the typed request facts; every producer is typed, including the five
  parameter families, and no body gate or crop-render trigger reads a mask.
  The event registry keeps the ProcEvent identity and history only.
- Tile ordering is connected to denoise and wavelet behind `RT_TILE_ORDER=1`,
  but it is opt-in and has no reduced-work mode. The deterministic merge is
  always on.
- Preview parity is measured through `RT_PREVIEW_DUMP`, not screenshots; the
  screenshot harness was not stable enough to commit.
