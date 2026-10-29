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
into both kernel sources at run time; only the addressing differs, because the
classic kernel takes one buffer per plane while the device kernel takes one
three-plane buffer with row and plane pitches. The fragment is a
namespace-scope constant, not a macro: GCC 12, the portable baseline, rejects a
multi-line raw string inside a `#define`, which the host compiler accepts.

The device kernel runs a one-dimensional range, one work item per destination
pixel, and derives `y` from the flat id. A two-dimensional `get_global_id(1)`
against a 1-D launch silently writes only the first row, which is worth
remembering when adding nodes: the launch and the kernel must agree.

Evidence: the identity geometry is pixel-exact on the device
(`stage-residency-test`, with a mutation check proving the assertion can fail),
and a rotation export matches the CPU reference within 1 LSB at 16-bit on 3.8%
of pixels; both are in `devnotes/opencl/PARITY.md`.

## Lab-to-RGB family

`output.convert` (float output), `monitor.output` (preview), `viewport.output`
(crop) and `thumbnail.output` share one wrapper. The arguments are the Lab
source buffer, the destination buffer, the 16-bit gamma LUT (65 536 F32
samples) and the nine-entry matrix. The source may be a sub-rectangle view of
the parent Lab image: the pitches stay the parent's, so the kernel reads the
ROI without a host copy.

The arithmetic is shared with the classic kernel through
`rtengine/stage/algorithms/lab_to_rgb_cl.h`. The matrix and the LUT come from
the ICC profile the classic backend already builds, so the node runs inside the
same gated block and takes precedence there; if the classic backend is disabled
the profile is not built and the nodes fall back with it, which is the same
limitation as the other device nodes.

`lab2monitorRgb()` takes the node name, so the preview, crop and thumbnail
paths request `monitor.output`, `viewport.output` and `thumbnail.output`
respectively. Only the export node is reachable from the CLI; the packed 8-bit
output is covered by a reference test in `stage-residency-test`, which shares
no code with the kernel.

`monitor.output` and `viewport.output` have since been exercised on the host
with a 45.7 MP NEF: the GUI records both nodes with `impl=opencl`,
`source=opencl`, and the preview image area is pixel-identical (0 LSB8) to a
CPU-only run of the same state. Three things are required, and two of them cost
an hour to discover:

- run the OpenCL build of the GUI, not the CPU build, whose backend is a stub;
- set an explicit monitor profile (`[Color Management] MonitorProfile`),
  because without one the monitor conversion never reaches a backend at all;
- point `OCL_ICD_VENDORS` at the POCL-only directory by absolute path, or the
  loader finds no platform.

`thumbnail.output` needed a wiring fix: `rtthumbnail` never set
`ipf.setOpenCLAllowed()`, `opencl::setStageAllowed()` or
`stage::set_opencl_stage_allowed()` before building the monitor profile, so the
monitor conversion never reached a backend and a browser thumbnail fell back to
`lab2rgb`. With the policy set from the same preference the editor and the batch
export use, the file browser generates its thumbnails on the device (12 records
for the visible corpus) and the thumbnail cache is byte-identical to a CPU-only
run.

## RGB tone stage

`rgb.proc` runs the RGB tone and Lab conversion on the device. The kernel is
shared with the classic backend through
`rtengine/stage/algorithms/rgb_proc_cl.h`, and it keeps the classic signature:
three source planes, three Lab planes, ten tables (highlight, shadow and tone
LUTs, three optional RGB curves, the two Lab caches, the channel mixer and the
XYZ matrix), then the dimensions and seven scalars. The argument order the
stage node uses is documented at `run_rgb_proc`.

Two details are worth keeping:

- The kernel indexes with `get_global_id(0)`/`get_global_id(1)`, so the launch
  is two-dimensional. A one-dimensional range writes only the first row, which
  is the second time this class of mistake has appeared; the other device
  kernels derive x and y from a flat id and must be launched one-dimensionally.
- The three destination planes are read back after a wait, so the node reports
  `asynchronous=false` while still naming the device. That keeps the event
  honest without inventing a multi-plane submission type.

The stage node runs inside the classic opt-in: `RT_OPENCL_RGBPROC=1` plus the
same tool exclusions (black and white, HSV equalizer, film simulation, colour
toning and the rest). Parity is 1 LSB at 16-bit on 4% of pixels, and the
classic path stays byte-identical to the package before the kernel moved.

## Framing

`framing` draws the frame in `ImProcFunctions::drawFrame()`. The block has no
host stage built-in, so only that call site can select it; the classic engine
path is the fill-and-copy loop it already ran.

The ABI is `[0]` source, `[1]` destination, `[2..4]` doubles border red, green
and blue, `[5..6]` ints row and column offset. Both buffers are three-plane F32
`Imagefloat` views; the framed dimensions come from the destination buffer and
the launcher inserts the source and destination row pitches. The border values
are the `Color::gamma2curve` entries the host computes from the clipped 8-bit
slider values, so the kernel receives the finished colour and does no gamma
work. The offsets are the host's
`int(round((outer - inner) / 2.0))`, which rounds an odd delta .5 away from
zero.

The node rejects a shape that is not three-plane F32, equal source and
destination buffers, a destination smaller than the source, the equal-size
shape that `drawFrame()` returns unchanged, and offsets that do not fit the
source inside the frame. It writes every destination pixel exactly once: the
source sample inside the source rectangle, the constant border colour
elsewhere. That is the host's fill-then-copy result in one pass, and the
explicit pitches keep padded rows correct. The three destination planes are
read back after a wait, so the event is complete with `asynchronous=false`.

Evidence:

| Case | Result |
|---|---|
| device test, five shapes: odd and even deltas, both dimensions smaller, a per-channel border and one shape with a zero offset on one axis | device execution, `device=OPENCL`, PAE 0.000000 LSB16, 0 of 3840, 3840, 2673, 2856 and 1938 samples differ from `ImProcFunctions::drawFrame()` |
| equal-size and source-larger early returns | both return the source pointer unchanged with the node selected, so the node is never entered |
| production call site with the node selected | 0 of the same samples differ from the CPU `drawFrame()` output |
| `verify.sh --stage-device` with `framing.pp3` | PASS, `"stage":"framing".*"impl":"opencl"`, `max_pae=1.5259e-05` on a 1080x725 frame |
| `RT_STAGE_OPENCL` unset, framing profile, `--gpu=off` | byte-identical to the pre-change binary in three concurrent runs; `--gpu=auto` also matched once, and the second run differed only in the wall-clock Exif `DateTime` tag |
| forced refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`, `RT_REQUIRE_GPU=1`) | `verify.sh --stage-device` FAIL, exit 1 |
| targeted framing refusal (large fixed border, budget 150 MB) | the resize and output nodes still run on the device; the framing record is absent and `drawFrame()` completes on the host, so the framing gate predicate fails |

There is no residual for the block: it is a pure pixel copy and a constant
fill, with no arithmetic. The frame geometry stays in `stage_crop()`, as
before.

## Gamut

`gamut` runs the end-of-process gamut block of `stage_gamut()` in
`rtengine/simpleprocess.cc` as one in-place device pass: Lab, the linear
working RGB, an optional gain, the ACES reference gamut compression and back
to Lab. The block keeps its original guard (`workingTRC != NONE`, `trcExp` and
`wgamut != NONE` or `wgamgain != 0`). When the node is selected the call site
tries it first and returns without allocating `provcomp`; a refusal or a
failed launch falls through to the unchanged host block, so the host never
starts from a partly written Lab buffer. The preview, crop and thumbnail paths
keep their own copies of this block.

The ABI is `[0]` the three-plane F32 Lab buffer, read and written in place,
`[1]` `wip`, `[2]` `wp`, `[3]` `to_out` and `[4]` `from_out` (four 9x1 F32
matrices), `[5]` the double gain `pow_F(2, wgamgain)`, `[6]` the double
compression power `wgampower` and `[7]` the int `compress` flag, 1 when a
gamut is selected and 0 for the gain-only case. The single Lab allocation is
addressed through the device row and plane pitches, so padded rows are
correct.

Matrix provenance, in double, is the one `ImProcFunctions::gamutcompr()`
derives for `beginend == 1`: `wpro = workingSpaceMatrix(workingProfile)`,
`inv_out = inverse(out)` for the selected output gamut, `Rprov = inv_out *
wpro`, `to_out = transpose(Rprov)` and `from_out = inverse(to_out)`, while
`wip` is `workingSpaceInverseMatrix(workingProfile)` and `wp` is
`workingSpaceMatrix(workingProfile)`, exactly as the host block uses them. The
four output-gamut matrices are local constants in `gamutcompr()`; they are
repeated, with their source named, in `stage/algorithms/gamut_cl.h`, which the
call site, the launcher and the test share so they cannot drift apart. Only the
upload is single precision.

The kernel reproduces the host arithmetic in the host's order: `Lab2XYZ` with
the same `aa = a / 327.68f` form and CIE constants, `xyz2rgb` and `rgbxyz`,
the gain, `aces_reference_gamut_compression` and `XYZ2Lab`. The compression is
the rolloff branch only, because `gamutcompr()` sets `roll = true` for
`beginend == 1`; the fixed thresholds 0.8/0.75/0.85 and distance limits
1.15/1.25/1.5 are named constants in the kernel because they are the contract,
not settings. `computeXYZ2Lab` and `computeXYZ2LabY` evaluate the same CIE
curve analytically; the host answers the 0..65535 range from its interpolated
`cachef`/`cachefy` tables, and the measured difference between the two is far
below one 16-bit LSB. `#pragma OPENCL FP_CONTRACT OFF` keeps the multiply-add
order.

The launcher rejects a buffer that is not three-plane F32, a table that is not
a 9x1 single-plane F32 matrix (nine flat entries, one row; a 3x3 table with the
same entry count would be read across the device's padded row pitch), a gain
or power that is not positive and
finite, a gain that does not fit the kernel's float multiplier, and a compress
flag that is not 0 or 1. A working gamut value outside the enum is refused by
the host parameter builder rather than mapped to a guessed matrix. Source and
destination are the same buffer on purpose: every pixel is read and written
exactly once, which is the host's `provcomp` result with one allocation. The
three planes are read back after a wait, so the event is complete with
`asynchronous=false`.

Evidence:

| Case | Result |
|---|---|
| device test, 238 and 179 wide (device row pitch 240 and 192), REC2020/ADOBE/SRGB/DCIP3 and the gain-only case, power 1.0 and 1.2, gain +0.5, -0.5 and 0 EV | PAE 0.031250 LSB16 (REC2020, SRGB, DCIP3), 0.045898 (ADOBE), 0.061035 (gain-only); 9976 of 17136 samples differ at 238x24, 5309 of 9129 at 179x17 |
| device test, invalid compress flag and non-positive power | both refused, `RT_STAGE_OK` not returned, and the Lab buffer untouched |
| `verify.sh --stage-device`, POCL CPU, `RT_REQUIRE_GPU=1`, `OMP_NUM_THREADS=1`, `gamut.pp3` | PASS, `max_pae=1.5259e-05`, record `"stage":"gamut","impl":"opencl","source":"opencl"` on a 3888x2608 frame |
| `RT_STAGE_OPENCL` unset, gamut profile, `--gpu=off`, two concurrent runs | byte-identical to the pre-change binary at `3ee70b76f` (`sha256 10cdce4964b714ae93cd4eb843080d96580790d7170ea1130a0598899f678314`, 60 844 902 bytes); a sequential run differs only in the wall-clock Exif date-time string, and `--gpu=auto` is pixel-identical (`PAE 0`) |
| forced refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`) | the gamut record is absent, so the gate predicate fails while the export still succeeds through the host block |

The device test's synthetic Lab content is L in [0, 32768] and a, b in
[-16384, 16384], which saturates the compression. There is no residual for the
block: the whole end-of-process gamut pass is the node.

## Output sharpening

`output.sharpen` runs the unsupported-mask path of
`ImProcFunctions::sharpening()`. The CPU body was extracted unchanged into
`rtengine::sharpen_luminance()` in `rtengine/sharpen_core.h`, so the call site,
the host builtin and the device comparison use one algorithm. The call site in
`ipsharpen.cc` builds the node arguments after the existing early returns and
returns when the node reports `RT_STAGE_OK`; otherwise it runs the extracted
core directly. `simpleprocess.cc` keeps its L clamp before the call.

The ABI is `[0]` a tightly packed one-plane F32 L buffer, read and written in
place, then the scalars `[1]` method (0 USM, 1 RLD), `[2]` amount, `[3]`
contrast, `[4]` radius, `[5]` blur radius, `[6..9]` the four threshold values,
`[10]` the four-value threshold form, `[11]` threshold start-at-one, `[12]`
edges-only, `[13]` edge radius, `[14]` edge tolerance, `[15]` halo control,
`[16]` halo amount, `[17]` deconvolution amount, `[18]` deconvolution radius,
`[19]` deconvolution iterations, `[20]` deconvolution damping, `[21]`
automatic deconvolution radius, `[22]` corner boost, `[23]` corner latitude,
`[24]` scale and `[25]` show-mask. The L buffer must satisfy
`row_pitch == width * sizeof(float)` and
`plane_pitch == width * height * sizeof(float)`: the extracted CPU core is
W-strided and the host builtin passes its raw pointer, so neither can honour a
padded descriptor. A padded row or plane is refused as `RT_STAGE_INVALID`
instead of being misread on the host, and the node leaves the plane untouched.
The host parameter builder rejects a method outside the enum; the decoder
validates the argument kinds, the packed F32 buffer and every 0/1 flag.

The host builtin reconstructs the whole `SharpeningParams` set, including the
residual fields, and calls `sharpen_luminance()`. It therefore covers damped
RLD, separable-IIR RLD, edges-only, halo control and show-mask with the CPU
arithmetic. When the device declines any of those, the device queue runs this
builtin on the host queue and `stage::run()` records `impl=host`,
`source=device-fallback`; a device pass is never mixed with a host pass in one
node call.

The device supports:

- `method=usm` with no edges-only, no halo control, no show-mask,
  `blurradius < 0.25` and `radius / scale < 25`; `radius / scale >= 25`
  follows the CPU `GAUSS_DOUBLE` limit.
- `method=rld` with no edges-only, no halo control, no show-mask,
  `blurradius < 0.25`, `deconvdamping == 0`, `deconviter <= 1000` and
  `deconvradius / scale < 1.15`. A zero `deconvamount` with no blur-radius
  pre-pass is the CPU no-op: the node reports OK with the plane unchanged.
  `deconvdamping != 0`, `deconvradius / scale >= 1.15` (the separable IIR
  range), `deconviter > 1000` and `blurradius >= 0.25` are named host
  residuals in `stage/algorithms/sharpen_cl.h`.

The default post-resize sharpening profile is therefore device-covered: it
selects `rld`, radius 0.45, 100 iterations and no damping. Damped profiles and
radii at or above 1.15 stay on the host and record `impl=host`.

Kernels, in one program:

- `rt_sharpen_mask` reproduces `buildBlendMask()` with `autoContrast` off: the
  four-direction gradient, the float scale `0.0625 / 327.68`, the sigmoid
  blend factor and the CPU border replication. A zero contrast threshold
  writes a flat mask and skips its blur.
- `rt_sharpen_iir_line` is the separable Young-van Vliet recurrence for both
  axes. A line advances by one sample horizontally and by the row pitch
  vertically, so padded rows are handled explicitly.
- `rt_sharpen_fir3` reproduces `gauss3x3()` for distinct source and
  destination, including the horizontal first and last rows, the vertical
  first and last columns and the copied corners.
- `rt_sharpen_apply` evaluates `min(fabs(diff), 2000)`, the threshold mapping
  and `intp(a, b, c) = a * (b - c) + c` in the CPU order, in place.
- `rt_sharpen_rld_init`, `rt_sharpen_div3/5/7`, `rt_sharpen_mult3/5/7` and
  `rt_sharpen_rld_final` implement the undamped Richardson-Lucy iteration:
  clamp L and seed the estimate, divide by the blurred estimate with the CPU
  `GAUSS_DIV` denominator and floor, multiply the estimate by the blurred
  ratio with `GAUSS_MULT`, and finish with
  `intp(blend * amount, max(estimate, 0), L)`. The 3x3 kernels keep the CPU's
  filtered border; the 5x5 and 7x7 division kernels set their 2- and 3-pixel
  borders to 1.0, while their multiplication kernels leave them untouched.
  The 7x7 weighted sum keeps the CPU's double use of `c21`.

The mask blur is always sigma 2. The IIR coefficients are computed on the host
in double from the same formulae as `calculateYvVFactors()` in `gauss.cc`, and
uploaded as floats. The `M` scaling matches the SSE implementation that ships
on x86-64, because that is the production path the device is compared against;
starting the recurrence, the forward pass, the boundary correction and the
backward pass keep the host order. The 3x3 kernel coefficients are the
normalized `gauss3x3()` constants. The 5x5 and 7x7 RLD coefficients reproduce
`compute5x5kernel()` and `compute7x7kernel()` in their own float storage and
float normalization, so the device uses the same values as the CPU. The
Gaussian support windows follow `GAUSS_SKIP` (0.25), 0.6, 0.84 and 1.15:
below 0.25 the RLD loop is the CPU's identity copy, and at or above 1.15 the
node is a host residual. The mask, the blur, the RLD estimate and the RLD
intermediate are device scratch; only the L plane is uploaded and read back.
Each RLD iteration is two blocking kernel submissions (divide, then multiply)
on the in-order device queue. The node is one stage submission with no
intermediate host transfer, not one enqueue.

`#pragma OPENCL FP_CONTRACT OFF` is at the top of the whole kernel source, so
it applies to the mask, apply and USM kernels as well as the RLD kernels. It
keeps the CPU multiply-add order; the USM figures below were re-measured with
the pragma in place.

Evidence:

| Case | Result |
|---|---|
| device test, 25 USM cases: defaults, radius 0.3/0.5/0.59/0.6/1.15/3.0, contrast 0/20/100, amounts 1/100/200/1000, threshold breakpoints and the two-value form, widths 8/63/64/65 and padded 238, sigma 0.25 and a skip case | worst PAE 0.824219 LSB16 (threshold two-value), next 0.664062 (amount 1000); width 8..238 at or below 0.136719; 1953 to 5712 samples per case |
| device test, 15 RLD cases: radius 0.3/0.45/0.75/1.0, iterations 1/5/30/100, deconvolution amounts 1/50/100/1000, padded width 238 and the zero-amount no-op | worst PAE 0.466797 LSB16 (amount 1000), next 0.342773 (radius 0.75); iteration sweep 0.025879 to 0.071289; no-op bit-exact; 1953 to 5712 samples per case |
| RLD content | the RLD fixture keeps smooth gradients, hard edges and a negative band; white noise drives the division into an unrealistic dynamic range and is not deconvolution content |
| identity and flat-mask cases (contrast 0, threshold zeros, sigma below `GAUSS_SKIP`) | bit-exact, 0 differing samples |
| residual request through `stage::run()` with `halocontrol=true`, `deconvdamping=50`, `deconvradius=1.5` and `blurradius=0.3` | `impl=host`, `source=device-fallback`; output bit-identical to both the production CPU path and the extracted core |
| packed-plane contract through `stage::run()` with a padded row or a padded plane, on both the device and the builtin path | `RT_STAGE_INVALID`, 1953 samples untouched in every case |
| `edgesonly=true` and `showMask=true` through `stage::run()` | both are valid host residuals: `impl=host`, no device record; edges-only is bit-identical to the CPU reference, show-mask is bit-identical to the CPU mask and writes all 1953 samples |
| supported request through `stage::run()` | record `impl=opencl`, `source=opencl`; within 1 LSB16 of the CPU path |
| `verify.sh --stage-device`, POCL CPU, `RT_REQUIRE_GPU=1`, `OMP_NUM_THREADS=1`, `sharpen-usm.pp3` | PASS, `max_pae=1.5259e-05` on a 1944x1304 frame; record `"stage":"output.sharpen","impl":"opencl","source":"opencl"` |
| the same gate with the default `sharpen-rld.pp3` (radius 0.45, 100 iterations, no damping) | PASS, `max_pae=1.5259e-05`; record `"stage":"output.sharpen","impl":"opencl","source":"opencl"` |
| the same gate with `sharpen-rld-damped.pp3` | `impl=host`, `source=device-fallback`; printed as a host residual and not counted as device parity |
| pure CPU refactor, USM and RLD profiles, `--gpu=off`, `OMP_NUM_THREADS=1` | normalized 16-bit pixel hashes identical before and after: USM `24ffef79d50c678b4dea72d5e3f047ae32a4a34c0fd4b11c248ba1c500cb093f`, RLD `d0e6555e96fe7ab393cedea13bd4b1d6b45e80faac86af456d8e67b962816b30` |

The threshold mapping uses double when the device advertises `cl_khr_fp64`,
which POCL does, and float otherwise; the fallback only affects the final
rounding of a sample. A device that cannot build the program falls back to the
host builtin, so the node still produces the CPU result.

### Lab sharpening shares the node

`ImProcFunctions::sharpening()` is the entry point for both blocks. The
post-resize call is `stage_output_sharpen()` and the Lab call is
`stage_lab_sharpening()` in `rtengine/simpleprocess.cc`; the latter runs before
resize and is not gated on `labResize`. The node therefore keeps the name
`output.sharpen` in both places. A Lab-only profile with `[Resize]
Enabled=false` reaches the node from `lab.sharpening` and records
`"stage":"output.sharpen","impl":"opencl","source":"opencl"` at the full frame
size, not at a resized size.

The rest of `lab.sharpening` is host-only: impulse denoise, defringe, ML
sharpen edges and ML microcontrast run before or beside the main sharpen call.
They are off by default. When one is enabled the block is only partially
device-covered: the extra pass stays on the host while the shared sharpen node
may still run on the device. This is the atomicity caveat for the block — it
is not one device pass and there is no block-level device record.

Evidence, POCL CPU, `RT_OPENCL_ALLOW_CPU=1 RT_OPENCL_RGBPROC=1
RT_STAGE_OPENCL=1 RT_STAGE_PROFILE=... OMP_NUM_THREADS=1`, export with
`rawtherapee-graph-cli --gpu=auto` against a `--gpu=off` export:

| Case | Result |
|---|---|
| `[Resize] Enabled=false`, `[Sharpening] Enabled=true Method=usm`, default values | record `"stage":"output.sharpen","impl":"opencl","source":"opencl"`, width 3888, height 2608; TIFF PAE `1.5259e-05` (1 LSB16), 392 345 of 30 419 712 samples differ |
| the same profile plus `[SharpenEdge] Enabled=true` | the ML edge pass is host-side, the shared node still records `impl=opencl`; TIFF PAE `1.5259e-05`, 392 387 of 30 419 712 samples differ |

## Thumbnail white balance

`thumbnail.whitebalance` applies the white-balance multipliers that the host
resolves from the colour temperature, with the highlight-average branch for
`toneCurve.hrenabled`. The kernel is small and in place: one three-plane F32
buffer, three multipliers and the flag. The whole image is one device
allocation addressed through its plane pitch; uploading each plane into its
own compact buffer put the plane 1 and 2 writes past the first allocation,
which the device test caught. A 45.7 MP NEF thumbnail cache is byte-identical
to a CPU-only run.

## Remaining thumbnail nodes: measured cost

The plan assumed these were cheaper than step 5's kernels. Inspection says
otherwise, so the next session does not start from that assumption:

- `thumbnail.lab` is a composite: Lab tools, local contrast, working-TRC
  conversion, CIE CAM and gamut compression folded into one block.
- `thumbnail.decode` is a JPEG decode, not a kernel.

`thumbnail.rgb` is also a composite, but its dominant pixel passes are the
existing `transform` and `rgb.proc` device nodes; see *Thumbnail RGB* below.

## Thumbnail coarse transform

`thumbnail.transform` covers the full thumbnail path (three-plane F32) and
`thumbnail.transform.u8` the embedded-JPEG quick path (packed U8). Both express
the rotate-then-flip composition of `IImage::rotate`, `hflip` and `vflip` as one
destination-to-source mapping, so the whole coarse transform is a single pass.
The call sites allocate the destination (the dimensions swap for 90 and 270),
run the node and swap the image pointer.

The kernels are device-tested for all sixteen rotation/flip combinations in
both formats against an independent mapping; disabling the 90-degree branch
makes the float test fail on 2 880 samples. The browser path could not be made
to produce a device record in the harness: a sidecar with `[Coarse] Rotate=90`
changed the thumbnail cache key but no `thumbnail.transform` record appeared,
so the harness did not reach those call sites. The node is therefore verified by
the device test and by compilation, and the GUI path is recorded as uncovered
rather than passed.

## Thumbnail scale

`thumbnail.scale` covers the full thumbnail path (three-plane F32 destination)
and `thumbnail.scale.u8` the embedded-JPEG quick path (packed U8 destination).
The pipeline block id stays `thumbnail.scale`; the second node name exists
because the destination format differs, as with `thumbnail.transform`, and
because the 8-bit reference path is not a float resize at all. The ABI is
`[0]` source, `[1]` destination, `[2]` interpolation (`0` nearest, `1`
bilinear); the source and destination formats come from `RtStageBuffer.format`.

The node reproduces `IImage::resizeImgTo`, which carries three arithmetic
variants:

| Source | Mapping | Conversion |
|---|---|---|
| `Imagefloat` (planar F32) | position accumulated in float (`syf += heightByNh`), `sy = int(syf)`, `dy = syf - float(sy)` | `convertTo(float, float&)`: a NaN becomes 0 |
| `Image16` (planar U16) | the same accumulation, samples promoted to float | the same float conversion |
| `Image8` (packed U8) | `sy = i * height / nh`, `dy = float(i) * float(height) / float(nh) - float(sy)`, not accumulated | the blend is stored in `unsigned char` first, so it is truncated toward zero; a float destination then multiplies by 257 |

A same-size request is a pure type conversion in the reference and takes
priority over the interpolation, so the kernel has a third mode that copies
rather than blends. Without it an infinity would be multiplied by a zero
weight and become a NaN.

The coordinate map is built on the host, not in the kernel. The planar variants
accumulate and the packed variant multiplies and divides, and either one
differs by an ulp if recomputed differently; the host repeats the reference
expressions verbatim and uploads one `{index, next, fraction}` entry per
destination row and column as scratch buffers. The kernel then only indexes and
blends, under `FP_CONTRACT OFF`, so the four products are summed in the
reference's order. One work item owns one destination row: the map is loaded
once per row and the row stays cache friendly, at the cost of a serial inner
loop.

`stage::view(Image16&)` reports the image's own padded row and plane strides,
not the packed `width * sizeof(uint16_t)`: `PlanarRGBData` pads every row to
128 bits, and the residency copies row by row, so the padded host layout stays
valid for widths that are not a multiple of eight.

Both call sites ask `node_device()` first and keep the existing `resizeTo`
line as the fallback. `stageScale` does not return early, because the
film-negative step that follows stays on the host; it selects the device image
and then runs the unchanged inversion. `stageQuickScale` offers the node only a
destination of at least one pixel in each direction, since that path leaves a
collapsed destination to `resizeTo`.

Evidence (POCL CPU device, `RT_OPENCL_ALLOW_CPU=1 RT_REQUIRE_GPU=1`,
`OMP_NUM_THREADS=1`):

- `stage-residency-test` compares the device output with the production
  `resizeImgTo` template for all four reachable format pairs (U8→U8, U8→F32,
  U16→F32, F32→F32), nearest and bilinear, at 40x24 → 23x14 (downscale),
  40x24 → 61x37 (upscale) and 40x24 → 40x24: 24 cases, all bit-exact
  (`PAE 0.000000 LSB16`; 966 samples on the downscale, 6 771 on the upscale
  and 2 880 at the same size, none differing). The fixture covers 0, the top
  of the range, the midpoint and its neighbour, one NaN for a float source and
  one infinity, which separates the same-size conversion from the resampling
  modes.
- The test has teeth: storing the U8 blend rounded instead of truncated fails
  the U8 bilinear cases (459 of 966 samples on the downscale, 3 395 of 6 771
  on the upscale), and dropping the same-size copy fails the float same-size
  case on the infinity sample.
- `thumbnail-stage-test` on the public 10.2 MP PEF with its sidecar at 160 px
  produced `thumbnail.scale` and `thumbnail.scale.u8` records with
  `impl=opencl`, twice each because the harness renders legacy and graph in one
  process. The PPM files are byte-identical to the same binary with
  `RT_STAGE_OPENCL` unset and to a binary built from the pre-change commit:
  embedded `239x160`, sha256
  `d961f0b52aaa8db2c7acd3c0a5924ad3ef22942156617bdf8fc204d52bd65514`; raw
  `238x160`, sha256
  `254e4451dfd4dede1ab0199f46d8eecad1d0c1574e704c96979a6d5fc9173824`. With the
  switch unset no stage record is written at all, because the call sites never
  enter the runtime.
- The node is harness-covered, not CLI-covered: thumbnail generation is not on
  the export path, so `VALIDATION.md` gains no row for it, and the GUI
  thumbnail run recorded for `thumbnail.output` was not repeated with the
  scale node on the device. That interactive path is uncovered rather than
  passed.

## Thumbnail RGB

`thumbnail.rgb` is the composite `Thumbnail::stageRgb()`
(`rtengine/rtthumbnail.cc:2121`). It owns no kernel: its two dominant pixel
passes are sub-nodes that already have device implementations, and the rest of
the block is host-side residual. The catalogue therefore marks it `partial`.

### Passes and coverage

In execution order, with the call site in `stageRgb()`:

| Pass | Site | Coverage |
|------|------|----------|
| film negative, input colour space | `rtthumbnail.cc:2158` | host |
| colour-space conversion, raw | `rtthumbnail.cc:2165` | host |
| colour-space conversion, non-raw | `rtthumbnail.cc:2167` | host |
| film negative, working space | `rtthumbnail.cc:2172` | host |
| first analysis | `rtthumbnail.cc:2179` | host |
| dehaze | `rtthumbnail.cc:2181` | host |
| Fattal tone mapping | `rtthumbnail.cc:2182` | host |
| geometry transform | `rtthumbnail.cc:2191` | device node `transform` |
| `rgb2lab` for local adjustments | `rtthumbnail.cc:2224` | host |
| `calc_ref` | `rtthumbnail.cc:2459` and `:2461` | host |
| `Lab_Local` | `rtthumbnail.cc:2505` | host |
| `lab2rgb` after local adjustments | `rtthumbnail.cc:2577` | host |
| `moyeqt` colour-toning scan | `rtthumbnail.cc:2639` | host |
| RGB tone and Lab conversion | `rtthumbnail.cc:2672` | device node `rgb.proc` |

The auto-exposure derivation at `:2213` reads the analysis histogram and
produces parameters; it is not a pixel pass. The two local-adjustment passes
and the colour-toning scan only run when their tools are enabled. The LUT
construction around the block (`CurveFactory::complexCurve` `:2596`, `RGBCurve`
`:2605-2607`, `colorToning.getCurves` `:2618`, `curveBW` `:2628`) is host-side
table work whose output `rgb.proc` consumes.

Both device nodes also have a classic OpenCL backend:
`opencl::transformRotateScale` (`iptransform.cc:1245`) and `opencl::rgbProc`
(`improcfun.cc:2600`). They are tried only when the stage resolver declines the
node. No other pass in this block has one: the remaining passes have no
`opencl::` call site in their implementations.

The device nodes are reached through their existing gates, not through a
thumbnail-specific one. `transform` needs acceleration allowed, an enabled
device and no lens, CA, distortion, vignette, gradient or post-crop-vignette
work (`iptransform.cc:1225-1244`). `rgb.proc` sits inside the classic
`RT_OPENCL_RGBPROC=1` opt-in and its tool exclusions (`improcfun.cc:2528-2543`),
so the `rgb.proc` record needs that variable in addition to
`RT_STAGE_OPENCL=1`. That is the documented classic gate, not a thumbnail
wiring gap: with the variable unset the node is not offered to the runtime and
no record is written.

### Evidence

Sidecar: `devnotes/opencl/pp3/rgbproc-exposure.pp3` plus
`[Rotation] Degree=90`, so `needsTransform()` is true and both sub-nodes run.
It is kept untracked at `build/test-scratch/thumb-rgb/thumb-rgb.pp3`.

```bash
PEF=${RT_TESTDATA:-$PWD/build/rt-testdata}/colorspace_flowers.pef
SCRATCH=$PWD/build/test-scratch/thumb-rgb
OCL_ICD_VENDORS=$PWD/build/test-scratch/pocl-icd RT_OPENCL_ALLOW_CPU=1 \
OMP_NUM_THREADS=1 RT_SETTINGS=$SCRATCH/rt-settings RT_CACHE=$SCRATCH/rt-cache \
RT_STAGE_OPENCL=1 RT_STAGE_PROFILE=$SCRATCH/device.jsonl \
RT_OPENCL_RGBPROC=1 \
build/opencl-perf-graph/rtengine/thumbnail-stage-test "$PEF" \
    $SCRATCH/thumb-rgb.pp3 160 $SCRATCH/device
```

The host twin runs the same binary and arguments with `RT_STAGE_OPENCL`,
`RT_STAGE_PROFILE` and `RT_OPENCL_RGBPROC` unset and with `OCL_ICD_VENDORS`
pointing at an empty directory. The empty directory is required: with
`RT_STAGE_OPENCL` unset alone, an ambient ICD (`rusticl.icd`, `mesa.icd`) is
still found, `opencl::enabled()` is true because the user preference is on, and
the classic backend handles `transform` and the Lab-to-RGB sinks. The "host"
run is then a classic-backend run and agrees with the device by construction.
With no ICD the backend is off and the reference is the CPU code; a run of the
CPU build reproduces that hash exactly.

```bash
mkdir -p $SCRATCH/empty-icd
OCL_ICD_VENDORS=$SCRATCH/empty-icd OMP_NUM_THREADS=1 \
RT_SETTINGS=$SCRATCH/rt-settings RT_CACHE=$SCRATCH/rt-cache \
build/opencl-perf-graph/rtengine/thumbnail-stage-test "$PEF" \
    $SCRATCH/thumb-rgb.pp3 160 $SCRATCH/host
```

A third run keeps `RT_STAGE_OPENCL=1` but leaves `RT_OPENCL_RGBPROC` unset, so
`transform` and the other thumbnail nodes run on the device while `rgb.proc`
stays on the host. POCL's CPU device is selected, so these are CPU-device runs,
never GPU parity.

Both sub-node records appear, chained by their hashes: `transform` reads
`a677044a3725d807` and writes `142265ab331fb8d6`, which is exactly `rgb.proc`'s
input:

```text
{"stage":"transform","impl":"opencl","source":"opencl","consumer":"interactive",
 "ns":828188,"width":238,"height":160,"threads":0,"cache_hit":0,
 "input_hash":"a677044a3725d807","output_hash":"142265ab331fb8d6"}
{"stage":"rgb.proc","impl":"opencl","source":"opencl","consumer":"interactive",
 "ns":0,"width":238,"height":160,"threads":0,"cache_hit":0,
 "input_hash":"142265ab331fb8d6","output_hash":"b5564018d6e26f03"}
```

(The records above are re-wrapped for width; the file is one JSON object per
line. The `rgb.proc` `output_hash` is stable across passes after the row-pitch
fix.)

PPM results against the pure CPU host:

| Run | `raw-graph.ppm` sha256 | Samples differing from the host |
|-----|------------------------|---------------------------------|
| device, both nodes, 238x160 | `27a9c5f4fd5ae6cf5d05444faf7f0980af37336e95693750defbb45845696f42` | 1 of 114 240, 1 LSB8 (row 29) |
| device, both nodes, 179x120 | `c621bad329efc65b4c6cc10f9e6d744769cbf7235797f4cb8a3ff3729b299ffc` | 1 of 64 440, 1 LSB8 |
| device, both nodes, 267x179 | `4dfbc3dc487337f7583343b9948dcd81e4365a5f94fb10409787dd18a885f418` | 4 of 143 379, 1 LSB8 |
| classic rgbProc, 238x160 | `27a9c5f4fd5ae6cf5d05444faf7f0980af37336e95693750defbb45845696f42` | byte-identical to the device |
| pure host, 238x160 | `c712e74faf30ff5bd826112d52804e46a3857c1ef2d7214317d662abfb6ccf84` | reference |
| pure host, 179x120 | `a2572fc61344b57313861764174b562fca957963f59031e0de658f12ee49fbf5` | reference |
| pure host, 267x179 | `36b1a4e5f708777e2049fa205ff172db70673c5bdf02672f1b228e8f6259ed6a` | reference |

The device and classic runs are stable across repeats: two device runs at each
height give the same hash, and the `rgb.proc` `output_hash` is
`b5564018d6e26f03` in both passes of a run.

The device path still differs from the CPU host by 1-4 samples at 1 LSB8, the
8-bit face of the documented 1 LSB16 kernel tolerance, so byte identity is not
the applicable gate for this composite. The embedded-JPEG branch is
byte-identical in every run, 239x160, sha256
`d961f0b52aaa8db2c7acd3c0a5924ad3ef22942156617bdf8fc204d52bd65514`.

### `rgb.proc` row pitch

`rt_rgb_proc` used to index source and destination as `y * width + x`, which is
only correct for a compact plane. The stage device pads every plane row to
`STAGE_BUFFER_ALIGNMENT` = 64 bytes (`stage_device.cc`, `stage_buffer_layout`;
`stage_device.h:172`), so on any width whose row is not already 64-byte
aligned the kernel addressed the wrong sample: rows sheared, the tail of the
allocation was never written, and at width 179 the thumbnail run segfaulted.
The classic backend allocates compact planes (`planeBytes` per plane, zero
buffer row pitch), which is why the defect was latent there.

The kernel now takes `srcPitch` and `dstPitch` in float elements and indexes
`y * srcPitch + x` and `y * dstPitch + x`; the arithmetic is unchanged. The
stage ABI is unchanged: `run_rgb_proc` inserts the device row pitches after
`height`, rejects a set of planes whose row pitches disagree, and falls back to
the host if a pitch does not cover the width. The classic launcher passes
`width` for both pitches, because its planes are compact.

Before and after, 238x160 thumbnail:

| | before | after |
|---|---|---|
| samples differing from the pure host | 943-948, rows 158-159, max 245 | 1 of 114 240, row 29, max 1 LSB8 |
| `rgb.proc` `output_hash` | unstable between passes | `b5564018d6e26f03` twice |
| `raw-graph.ppm` | `42f3f55a…` or `1afeaf31…`, not stable | `27a9c5f4…`, stable |
| height 120 (width 179) | segfault, exit 139 | PASS, 1 of 64 440 at 1 LSB8 |

The regression case is in `stage_residency_test.cc`: the node runs at width 238
and 179 against a compact-pitch reference at width 240 and 192 with the same
fixture, and the shared columns must agree bit for bit, the last row must be
finite, and a repeat must be identical. Against the pre-fix indexing (the
mutation restores `y * width + x` while keeping the pitch arguments) it fails
with 128 of 17 136 and 785 of 12 888 shared samples differing, plus non-finite
tail rows; after the fix both cases report `PAE 0.000000 LSB16, 0 differing`.

The classic export control is a fast export resized to 238 px, `--gpu=auto`
with `RT_OPENCL_RGBPROC=1` against the same command on the pre-fix build:
`AE=0`, `PAE=0` on 238x160. `tools/opencl/verify.sh --stage-device` also passes
the rgbproc pair at the 1 LSB16 limit.

### Kernel row-pitch audit

Every stage-device kernel in `stage_opencl_device.cc` was checked for the
compact-index assumption. `rt_rgb_proc` was the only one; the rest already take
and use pitches, and each is reachable at a padded width on a normal path:

| Kernel | Row addressing | Padded-width test |
|--------|----------------|-------------------|
| `lanczos3` | `src_pitch`, `src_plane_pitch`, `dst_pitch`, `dst_plane_pitch` | node parity 238x160 → 119x80 and → 357x240 against the CPU |
| `rt_transform` | `srcPitch`, `srcPlanePitch`, `dstPitch`, `dstPlanePitch` | identity geometry at 238x160, exact; 90° rotation within 1 LSB16 |
| `rt_lab_to_float` | `srcPitch`, `srcPlanePitch`, `dstPitch`, `dstPlanePitch` | `output.convert` parity at 238x160 |
| `rt_lab_to_u8` | `srcPitch`, `srcPlanePitch`, `dstRowBytes` | `thumbnail.output` at 238 (covered by the thumbnail harness) |
| `rt_thumbnail_wb` | `srcPitch`, `srcPlanePitch`, `dstPitch`, `dstPlanePitch` | existing test at width 24, which is already padded; add 238 |
| `rt_thumbnail_permute` | `srcPitch`, `srcPlanePitch`, `dstPitch`, `dstPlanePitch` | existing test at width 20, already padded |
| `rt_thumbnail_permute_u8` | `srcPitch`, `dstPitch` (bytes) | same test, packed U8 |
| `rt_thumbnail_scale_f32` | `src_pitch`, `src_plane_pitch`, `dst_pitch`, `dst_plane_pitch` | existing padded Image16 case (41x23); add a 238-wide F32 case |
| `rt_thumbnail_scale_u8` | `src_pitch`, `dst_pitch` (bytes) | same test, packed U8 |

No other kernel needs the pitch fix. The proposed rows are additional coverage,
not known defects, and are not part of this change.

### Host-side residual

Confirmed against the code, not copied from the block description:

- colour-space conversion, raw and non-raw: `RawImageSource::colorSpaceConversion`
  (`rawimagesource.cc:1139`) and `StdImageSource::colorSpaceConversion`
  (`stdimagesource.cc:227`). The raw form is an lcms transform,
  `cmsDoTransform` at `rawimagesource.cc:3568`, with a DCP branch.
- film negative, before and after the conversion:
  `ImProcFunctions::filmNegativeProcess` (`filmnegativeproc.cc:387`), called at
  `rtthumbnail.cc:2158` and `:2172`.
- first analysis: `improcfun.cc:602`, produces the histogram that feeds the
  auto-exposure parameters, the tone curve and `rgbProc`.
- dehaze: `ipdehaze.cc:308`.
- Fattal tone mapping: `tmo_fattal02.cc:1067`.
- local adjustments: `rgb2lab` (`improcfun.cc:6047`), `calc_ref`, `Lab_Local`
  and `lab2rgb` (`improcfun.cc:6206`); enabled only with `locallab` spots.
- colour-toning automatic saturation: `moyeqt` (`improcfun.cc:2149`), a
  whole-image scan enabled only by `colorToning.autosat`.

The dehaze and Fattal calls sit in this block rather than in `transform` on the
thumbnail path, although the export and preview catalogues fold them into
`transform`; the thumbnail block boundary is as implemented, and both are
host-only.

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

## Working TRC

`ImProcFunctions::workingtrc()` keeps the primaries, illuminant, CAT and TRC
setup for the working-TRC conversion. Its trailing parameters expose that setup
instead of duplicating it in a second implementation:

- `probeMeanX`/`probeMeanY` receive the dominant-colour probe means on the
  `mul == 5` pass; every other pass leaves them untouched.
- `outProfile` receives the RGB profile the call builds, with ownership
  transferred to the caller (`cmsCloseProfile`). The handle must outlive any
  transform made from it.
- `outPrimMatrix` receives the probe's RGB to XYZ primaries matrix in double,
  row-major.
- `setupOnly` stops once the profile and its TRC tags exist. No transform is
  created and no pixel is transformed; only the unconditional epsilon clamp at
  the start of the function runs. It needs `outProfile` and a call that builds
  its own profile, so passing an existing `transform` makes it a no-op.

All four parameters default to null/false, so the existing call sites are
source-compatible and produce the same pixels. A full call still creates and
applies the lcms transform exactly as before.

### Node

`working.trc` runs the default configuration as a **host prefix plus one
device second pass**. The prefix is part of the node, not a device kernel:

1. `lab2rgb()` into the linear working RGB image, with a copy of that image as
   the `apsatur()` reference;
2. the fixed first pass, `workingtrc(mul = -5, gampos = 2.4,
   slpos = 12.92310)`, including its epsilon clamp and the
   `Color::igammatab_srgb` inverse;
3. the setup-only probe on that host first pass, then `buildGpuProfile()` for
   the XYZ to linear matrix and the linear to encoded table;
4. `toxyz` followed, when `icm.gamut` is set, by `Color::gamutmap()` with the
   probe's double primaries matrix. The result is the second-pass input.

The device work starts at the second pass: the matrix times the host XYZ, the
inverse profile TRC, `apsatur()` and RGB to Lab. The prefix exists to make the
probe, the profile and the second-pass input byte-identical to the CPU block on
every OpenCL compiler. The device first pass was removed: a one-ulp difference
in `lab2rgb()` and the sRGB inverse shifted the probe means and the profile,
and no OpenCL compiler reproduced the host's SSE/scalar split exactly. The
first pass is therefore not claimed as device work.

The ABI is `[0]` the three-plane F32 Lab buffer, read and written in place,
`[1]` the host-only `RtWorkingTrcContext` pointer (`RT_STAGE_ARG_PTR`), `[2]`
the int support flag, mirrored from the context descriptor; the launcher
requires both to be 1, so a configuration or setup refusal reaches the
builtin as `impl=host`, `[3]` the double `icm.wapsat`, `[4]` the three-plane
F32 second-pass XYZ image from the host prefix and `[5]` the three-plane F32
linear pre-TRC reference for `apsatur()`. The context is host-only: its
`build` callback runs the setup-only probe, `buildGpuProfile()`, `toxyz` and
`Color::gamutmap()` and returns the matrices, the gamma table and the
parametric TRC. Nothing in the context is passed to a kernel.

The profile provenance is the block's own: the whitelist name selects the
primaries and illuminant, `setupOnly` returns the profile, and
`buildGpuProfile()` turns its colorants into the float XYZ to linear matrix and
its red TRC into the 65536-entry linear to encoded table. The kernel uses the
table for `0 <= linear <= 1`, the exact `linear / c` inverse below the TRC
breakpoint (where the table cannot resolve the first encoded codes) and the
analytic inverse of lcms type 5 outside the table. A profile TRC of type -5
is not admitted. `Color::cachef` and
`Color::cachefy` are uploaded so RGB to Lab matches `Color::XYZ2Lab()` rather
than re-evaluating the cube root. `#pragma OPENCL FP_CONTRACT OFF`,
`#pragma STDC FP_CONTRACT OFF` and
`-cl-fp32-correctly-rounded-divide-sqrt` keep the float division and
multiply-add order. The host prefix applies the epsilon clamp before the probe
and before toxyz, exactly as the block does; the device second pass consumes
the already-clamped first-pass values and has no clamp of its own.

Residuals run the `working.trc` host builtin and record `impl=host`: a
`WorkingTRC` of `NONE`, `trcExp` off, a profile outside the twelve-name list,
a non-relative intent, non-default primaries or illuminant, soft light on,
local contrast on, either tone equaliser on (`wmidtcie` or
`wsmoothciesli`), black-and-white output (`fbw`), a first-pass sample above
`WORKING_TRC_FIRST_PASS_MAX` (1.001, so a working-gamut overflow stays on the
host), a profile that is not a matrix shaper, a TRC that is not lcms type 5, or
a `buildGpuProfile()` failure. The inverse type -5 curve is the named case: a
CUSTOM `WorkingTRC` with gamma below 1 builds it (`gampos` 2.5, TRC parameters
`0.868743, 0.131257, 0.222222, 0.100725` at gamma 0.4, slope 4.5). Its steep
inverse amplifies the remaining float-versus-lcms second-pass residual through
the Lab a-channel cancellation, so it stays on the host. The builtin disables
device recursion and runs the original host block on the Lab buffer.

Evidence, with `OMP_NUM_THREADS=1` and a 3888x2608 PEF:

| Case | Result |
|---|---|
| `stage-residency-test` on POCL, twelve cases at 238x24 and 179x17, gamut on/off, `wapsat` 0/0.5/1, five TRC values | supported PAE 0.015625 to 0.376953 LSB16 (linear worst), all finite; probe means `device == host` bit for bit (`delta 0/0`); out-of-gamut, midtone-equaliser and soft-light residuals `impl=host`, PAE 0, 0 differing samples |
| `verify.sh --stage-device`, POCL CPU, `working-trc.pp3` | PASS, `max_pae=1.5259e-05`, record `"stage":"working.trc","impl":"opencl","source":"opencl"` |
| `verify.sh --stage-device`, AMD RX 550 (rusticl), `working-trc.pp3` | PASS, `max_pae=1.5259e-05`, record `"stage":"working.trc","impl":"opencl","source":"opencl"` |
| AMD RX 550, `Gamut=false` | PASS, `max_pae=1.5259e-05` |
| `working-trc-residual.pp3` (soft light on) | PASS as a host residual: `"stage":"working.trc","impl":"host"`, `max_pae=1.5259e-05` informational |
| CUSTOM `WorkingTRC`, gamma 0.4, slope 4.5 (type -5) | host residual: `impl=host`, no `impl=opencl`, PAE 0 against the CPU block; observed TRC type -5, `params 2.5, 0.868743, 0.131257, 0.222222, 0.100725` |
| `--gpu=off` CPU path, two runs | identical (`PAE 0`) |

## Lab tools

`lab.tools` runs the Lab block's Lab-grid colour-toning pass,
shadows/highlights in both of its modes, local contrast, the Lab curves,
vibrance and the bounded Lab colour-correction region set as one device node.
Everything else in the block stays on the host.

### Supported set

`labToolsSupported()` admits a profile where

- colour toning is off, or on with `method == "LabGrid"` or with a
  `LabRegions` profile inside the region set below; any other method is a
  residual;
- a `LabRegions` profile has no mask display (`labregionsShowMask < 0`), one
  to four regions, every mask curve empty, `FCT_Linear` or
  `FCT_MinMaxCPoints`, and a/b inside +-20, saturation inside +-100, slope and
  power inside 0.1..4, offset inside +-0.1, mask blur inside -10..100 and a
  channel of -1, 0, 1 or 2. The host samples each mask curve to a
  piecewise-linear table and refuses one the table cannot follow within the
  error budget;
- `labCurve.contrast` is 0, so the `hist16` refill a later node reads stays on
  the host. With the curves enabled the pass is admitted when the chromaticity
  slider is 0, the C=f(H), L=f(H) and H=f(H) flat curves are the identity, the
  Munsell method is `NONE` or `MUN` (and `Color::init()` filled the tables for
  `MUN`), the red/skin protection is inside 0..100 and directional denoise is
  off (`chromiLuminanceCurve()`'s `factnoise` reads it);
- local contrast, when enabled, has a radius inside 1..200, an amount inside
  0..2, darkness and lightness inside 0..3, and finite values; the ABI is
  wider than the UI (radius 20..200, amount 0..1) because the parity matrix
  exercises sigma 1 and 10 and amount 2. A value outside those bounds is a
  residual;
- EPD is off, and black-and-white output is off
  (`chromiLuminanceCurve()` zeroes a and b for it even with the Lab curves
  off);
- the shadows/highlights sliders are inside 0..100 for the tonal widths and
  the radius, and 0..10000 for the amounts (the amounts have no geometric
  limit; the tone table is built on the host);
- vibrance, when enabled, has pastels and saturated inside -100..100 and both
  threshold values inside 0..100, would actually change a pixel (a slider is
  set or the skin curve is not the identity), and, when `avoidcolorshift` is
  on, `Color::init()` has filled the Munsell tables;
- vibrance, local contrast, the Lab curves and Lab regions additionally
  require a CPU OpenCL device. A GPU may round the division inside `xatan2f()`
  or the `sqrt` differently, which moves a pixel whose hue or chroma sits on a
  Munsell limit across the gate, and its compiler may lower the
  local-contrast IIR recurrence or the region mask arithmetic differently; the
  launcher refuses all four on a GPU and the builtin records `impl=host`; the
  gate prints a specific `SKIP ... CPU-device-only` for those rows (see "GPU
  isolation" below);
- local contrast, the Lab-curve pass and Lab regions also require an fp64
  device, because their programs repeat the host's double-precision
  `sqrt`/divide, IIR recurrence or mask product and working-matrix products;
  without fp64 the launcher refuses them and the block stays on the host;
- local contrast below sigma 25 also requires a host build with `__SSE2__` or
  `RT_SIMDE`. The exported coefficients describe the SSE float path; a host
  built without either runs `gaussianBlur()`'s double path for the same sigma,
  so the launcher refuses that sigma and records `impl=host`. At sigma 25 and
  above both sides use the double path and the pass stays available;

and at least one of the five tools would run. A profile where none would is a
residual: the node would be a no-op and the host block records it as such. An
enabled Lab curve counts as a running tool even when all six tables are the
identity: the pass still rewrites L, a and b through the block's divides.

The setup refines that in one place. RGB mode closes each pixel through
`lab2rgb()` and `rgb2lab()`, and the host evaluates those two colour-matrix
products in double. The device keeps single precision, so the channel that
indexes the tone table can differ by an ulp of a 16-bit value; a table whose
largest adjacent entry-to-entry step times that ulp can exceed one LSB16 is
refused, and the builtin runs the block. With the block's own `sh_contrast`
this bound is crossed only by a hand-edited amount above the 0..100 slider
range. Lab mode applies the table to the exact L value and has no such bound.

### Arithmetic

The block's sequence is preserved exactly:

1. the mask and guiding image from the L plane
   (`pow4(l * scale)` / `pow4(scale / l)`, threshold `tonalwidth * 327.68`);
2. `guidedFilter(L, mask, mask, radius, 0.075, multiThread, 4)`;
3. the host-built tone table and `intp(blend, f[l], l)`, with the shadow
   round's chromaticity scaling; or in RGB mode `lab2rgb()`, the per-channel
   table where the channel is not out of gamut, and `rgb2lab()`.

The guided filter is a port, not an approximation. The subsampled grid is
`W/4` by `H/4`, `r1` is `float(r)/subsampling` truncated, and `f_mean()`
clamps it the way `guidedfilter.cc` does. `boxblur()` is reproduced by one
work item per row for the horizontal pass and one per column for the vertical
pass, in `boxblur()`'s float order: the growing head, the running sum with the
departing sample, and the shrinking tail, with division in the head and tail
and the reciprocal multiply of the SSE and remaining-column paths in the
interior. The departing samples that the host keeps in a ring buffer are read
from the read-only source instead, which holds the same values because the
two passes always write a different buffer. The launcher keeps the subsampled
grid as five separate single-plane buffers - `I1`, `p1`, `meanI`, `meanp` and
one temp - and routes every box pass through the temp buffer, so a source that
is also the destination is safe. `rescaleBilinear()` and `getBilinearValue()`
are ported operation for operation, with `intp(a, b, c) = a * (b - c) + c`.

`Color::computeXYZ2Lab()`'s cube root is the host's own `xcbrtf()`, the sleef
polynomial, not the libm root: above the 16-bit range the two differ by a few
ulps, and the RGB-mode blend reads the result.

The lab.tools program builds with `-cl-fp32-correctly-rounded-divide-sqrt` and
does not use `-cl-opt-disable`: the flag was removed after the POCL vibrance
parity held without it, so LabGrid and shadows/highlights keep their optimised
kernels and no global optimisation cost is retained. The fp64
`lab.tools.labcurve` and `lab.tools.localcontrast` programs build with the
same flag.

### Local contrast

Local contrast is a port of `ImProcFunctions::localContrast()` for the
block's call site: `lab->L` is Gaussian-blurred, the difference from L is
scaled by `amount`, the sign selects `lightness` or `darkness` when either is
not 1, and the result is `LIM(L + bufval, 0.0001, 32767)` written back to L.
The sliders narrow to float exactly as the host narrows them, and `sigma` is
the host's `float radius / scale`, which is the radius at the export's scale 1.

The Gaussian is `gaussianBlur()`'s separable Young-van Vliet IIR recurrence.
The host has two implementations and selects between them at sigma 25, so the
device reproduces both:

- below 25 the host runs `gaussHorizontalSse()`/`gaussVerticalSse()`. Rows in
  groups of four and columns in groups of eight recur in single precision with
  float coefficients; the remaining tail lines recur in double with the same
  coefficients and narrow every store. Their boundary correction keeps the
  source and the forward state in `float`, so each deviation is a float
  subtraction before the double matrix multiplies it; the kernel narrows the
  deviation the same way. The kernel uses one work item per line and takes the
  float branch for the vector group and the double branch for the tail lines,
  so both halves of the host path are reproduced.
- at 25 and above the host runs `gaussHorizontal<double>()` and
  `gaussVertical<double>()`. The forward state, the boundary correction and
  the backward recurrence stay double, and only the finished line narrows to
  float. The kernel keeps the forward state in a device double buffer - the
  stage ABI has no fp64 buffer format, so the launcher creates it directly
  and refuses the pass when it does not fit the residency's remaining budget -
  then applies the boundary correction and the backward recurrence before
  writing the float line.

The coefficients are the host's rather than re-derived in the kernel.
`gaussianIirCoefficients()` in `gauss.cc` calls the same
`calculateYvVFactors()` the blur uses and applies the two paths'
boundary-matrix normalisations in their own operation order. The context
exports B, b1..b3 and the nine M entries in three forms: the float SSE lane,
the double SSE tail and the double path. The launcher refuses a coefficient
block whose `double_path` disagrees with the radius. The program is
`lab.tools.localcontrast`, built with `cl_khr_fp64`; like the Lab-curve
program it is refused on a GPU or a device without fp64. The pass order is
LabGrid, shadows/highlights, local contrast, Lab curves, vibrance, regions.

### Lab colour-correction regions

The region pass is `ImProcFunctions::labColorCorrectionRegions()` for one to
four regions, run after vibrance as the host block orders it.

Each region's hue, chromaticity and lightness mask is a `FlatCurve`. Those
curves are evaluated on the host into a piecewise-linear table: the host
refines the table where its chord misses `FlatCurve::getVal()` and refuses a
curve whose stored float table misses it by more than 5e-7 in mask units. The
table is a node count followed by x and y nodes; the kernel interpolates it
linearly and extrapolates at both ends with the first/last segment slope,
which is what the host validates. A negative lightness (raw `L/32768`) wraps
once, because `FlatCurve::getVal()` adds 1.0 below its first node; the
lightness and chroma tables run to 4.0 so an L above 32768 or a large chroma
follows the same final segment the host reads.

The mask kernel reproduces the block's first loop: `guide = LIM01(L/32768)`,
`Color::Lab2Lch()`'s scalar `sqrt(a*a+b*b)/327.68f`, the sleef `xatan2f()`,
`fastlin2log()` with `327.68f/48000.f`, `Color::huelab_to_huehsv2()` in
double, the `+1/6` wrap and `xlin2log(h, 3)`, then the product of the active
tables clamped to 0..1. The product is a double expression narrowed to float,
as in the block.

Both masks are built from a snapshot of the Lab as it stands when
`labColorCorrectionRegions()` is entered, after LabGrid, the shadows/highlights
rounds, local contrast, the Lab curves and vibrance, so a later region does not
see an earlier region's result. The launcher copies the working Lab into the
snapshot immediately before the region loop, not at setup. Each is filtered by the existing
guided-filter kernels with the block's implicit `calculate_subsampling()`
choice and the two radii `max(int(4/scale*blur+0.5),1)` and
`max(int(25/scale*blur+0.5),1)` with epsilons 0.001 and 0.0001. The host
computes the radii and the initial mask stays in the scratch planes, so the
second radius filters the same mask. The region applications then run in
region order, one kernel launch each, against the previous region's result.

The apply kernel repeats the block's per-region sequence: the
`LIM(a + l*abca, -42000, 42000)` and b form, the CDL round trip
(`Lab2XYZ`, `xyz2rgb`, per-channel `pow_F(max(rgb/65535*slope+offset,0),
power)*65535`, the optional saturation about `rgbLuminance(r,g,b,ws)`, then
`rgbxyz` and `XYZ2Lab`), `max(l_new,0)`, the optional channel copy from the
original pixel, and the two `intp()` blends. `abca`/`abcb` are the host's
`abcoord()` and `rs` is the host's saturation scale. The program is
`lab.tools.regions`, built with `cl_khr_fp64`; it reads the working matrices
as doubles because the host's CDL, channel and luminance products are double
expressions, while `Lab2XYZ()` itself is float.

The kernels use the host's `xcbrtf()` and `cachef`/`cachefy` lookups and the
same double negative branches of `computeXYZ2Lab()`; that distinction is what
keeps the CDL round trip inside one LSB16.

### Vibrance and Munsell

Vibrance is a port of `ImProcFunctions::vibrance()`. The kernel keeps the
host's sequence: `xatan2f()`/`xsincosf()` (the sleef polynomials),
`Color::gamutLchonly()` with lower/higher coefficients 0.15/0.98 and the
highlight gate, `Color::SkinSat()`, the pastels/saturated double linear
transition, the Luminance/Hue `sathue` and `sathue2` cascade, the two-stage
chroma ramp with its `-0.93..2.0`/`-0.93..1.8` clamps, the skin-hue curve,
and the `avoidcolorshift` loop with its second gamut clamp.

The Munsell tables are the host's own. `Color::initMunsell()` declares and
sizes 197 `LUTf`s (the historical comment says 195; all 197 are referenced by
`MunsellLch()`), and the stage exports them through `Color::munsellLut()` into
one flat set of 197 x 140 floats plus one length per table. The table-read
helper reproduces `LUTf::operator[](int)`'s clamp, so the device reads a
padded table exactly as the host reads the unpadded one. The skin curve is
built by the setup with `fillCurveArrayVib()`'s expression and the host's
`/ask`; the identity curve is not uploaded. `rt_lt_munsell_lch()` and
`rt_lt_all_munsell_lch()` are a line-for-line port of `Color::MunsellLch()`
and the single-correction `AllMunsellLch()` overload, including every hue
limit, luminance band and table choice.

#### GPU isolation

The PEF export was run on the RX 550 twice, once with the host block and once
with the device node, and the pre-vibrance Lab buffer was captured inside
each block. The two inputs are bit-identical (30,419,712 samples, max delta
0), so no upstream node is involved: `rgb.proc`, `transform`, `working.trc`
and `gamut` are not implicated. The two pixels that reach the full export,
with their block values, are:

| row, col | L | a | b | C | H | host (a, b) | device (a, b) | correction |
|---|---|---|---|---|---|---|---|---|
| 1392, 1648 | 12361.283203 | 14259.261719 | -8742.428711 | 51.043476 | -0.550000072 | 21007.525391, -11792.148438 | 20538.070312, -12592.000000 | host +0.038500 rad, device 0 |
| 78, 1456 | 18340.724609 | 13182.235352 | -11103.244141 | 52.597767 | -0.700000048 | 21525.298828, -16749.046875 | 20744.187500, -16266.509766 | host +0.038750 rad, device +0.035000 rad |

The input delta is (0, 0, 0) at both coordinates. The L output delta is 0;
the a/b deltas are (+469.455079, +799.851562) at 1392,1648 and
(+781.111328, -482.537109) at 78,1456.

Counts, with their scope:

- the committed full export differs at **2 pixels** (max 9 LSB8, 2,329 LSB16);
- the committed block, compared on the identical input, differs at **29
  pixels** above 1 LSB16 (max 799.851562 LSB16). The other 27 are masked by
  later stages in the full export;
- an experimental FMA-residual correctly rounded division, applied at
  **block level only**, fixed the two named pixels (both within 0.005 LSB16
  of the host) and left **27 pixels** above 1 LSB16 (max 470.764656 LSB16,
  down from 799.851562). The experiment was not kept: it did not make the
  whole image exact and was not run through the full export.

The first pixel sits on zone 1's upper limit (`huelimit[1] = -0.55`, the
input hue is -0.550000072, one ulp inside): the host enters zone 1 and
corrects the hue, while the device output keeps the input hue, so its
`xatan2f()` result was at or above -0.55 and the zone gate excluded the pixel.
The second pixel is inside zone 1; the two implementations take a different
table branch (corrections 0.03875 and 0.03500 rad). The isolation points at
the quotient in `xatan2f()`: rusticl does not appear to honour
`-cl-fp32-correctly-rounded-divide-sqrt`, while the host divides with IEEE
rounding. The same block is bit-exact under POCL (the knife-edge regression
case below), which is why the node supports vibrance only on a CPU OpenCL
device. On the RX 550 the launcher refuses it and the builtin runs the host
block; the export records `impl=host` and is not counted as device parity.

### Lab curves

The Lab-curve kernel is a port of the supported part of
`ImProcFunctions::chromiLuminanceCurve()`. The host builds the six tables with
the block's own `CurveFactory::complexLCurve()`, `complexsgnCurve()` and
`diagonalCurve2Lut()` from the export LUT instances (`lumacurve`, `curve1`,
`curve2`, `satcurve`, `lhskcurve`, `clcurve`), so the kernel applies exactly
the host's LUTs with `LUTf::operator[](float)`'s clip semantics: the a and b
tables clip at both ends, and the L, C=f(C), L=f(C) and C=f(L) tables keep the
`LUT_CLIP_OFF` extrapolation the call site gives them. The kernel follows the
block order: the L table, the signed a and b tables, the first red/skin gate,
C=f(L), C=f(C), L=f(C), the closing chroma, and the Munsell correction with
`Color::AllMunsellLch(lumaMuns = true)`'s luminance correction. It reads the
exported Munsell set, `settings->protectred` and `protectredh`, `100 -
rstprotection`, `lcredsk` and the working-profile chroma scale (1.2 for
ProPhoto and WideGamut, 1.8 for Adobe RGB and BruceRGB, 2.0 for sRGB, 1.4 for
Beta RGB and BestRGB, 1.0 otherwise). `settings->amchroma` is exported and
validated (ABI `[46]`) but not read: it only feeds the residual L=f(H) path.
The residual families are the chromaticity slider, the C=f(H), L=f(H) and
H=f(H) flat curves, the LAB, XYZ and XYZREL Munsell methods, a denoise chroma
factor and black-and-white output.

The chroma arithmetic is where a naive float port fails. On the host,
`sqrt(float) / 327.68f` resolves to the libm double square root divided by the
promoted float constant and rounded to float, while the SSE2 precompute that
feeds `sincosval` when the a and b tables are inactive is single precision.
That precompute only covers whole four-column blocks:
`for (k = 0; k < W - 3; k += 4)` covers `4 * (floor((W-4)/4) + 1)` columns,
and the host's tail loop uses the scalar double `sqrt` for the last up to
three columns (`improcfun.cc:4693-4708`). The kernel reproduces both: the
precompute columns take the single-precision `vsqrtf`/divide, the tail
columns and the active-a/b case take the double value, and the closing chroma
and the Munsell index always take the double `sqrt` divided by
`(double)327.68f`. `Color::MunsellLch()` truncates the chroma and the memory
chroma to `int` table indices, so a one-ulp chroma difference can select the
next table entry and rotate a pixel's hue by tens of LSB16; this is why the
tail rounding matters. That requires fp64, so
the kernel lives in its own program (`lab.tools.labcurve`) with
`#pragma OPENCL EXTENSION cl_khr_fp64 : enable`; the fp32 `lab.tools` program
and every GPU node are unaffected. The launcher refuses the pass when
`CL_DEVICE_DOUBLE_FP_CONFIG` is unavailable or zero, and also when the host
itself was built without `__SSE2__`/`RT_SIMDE`, because the block's first
chroma would then be the scalar double value rather than the precompute.

`rt_lt_all_munsell_lch_lum()` is a port of the seven-argument
`Color::AllMunsellLch()` overload, not the five-argument vibrance one: it
clamps the current chroma to 6..140 rather than 139, and computes
`correctlum = lbe(Loldd) - lbe(Lprov1)` when the first `MunsellLch()` call set
`correctL`. It shares `rt_lt_munsell_lch()` and the exported tables with
vibrance.

### ABI

`[0]` the three-plane F32 Lab buffer, read and written in place, `[1]` the
host-only `RtLabToolsContext` pointer (`RT_STAGE_ARG_PTR`), `[2]` the int
support flag, mirrored from the context descriptor; the launcher requires both
to be 1, so a configuration or setup refusal reaches the builtin as
`impl=host`, `[3]` the int LabGrid flag, `[4..7]` the doubles `a_scale`,
`a_base`, `b_scale`, `b_base` exactly as `colorToningLabGrid()` derives them,
`[8]` shadows/highlights enabled, `[9]` highlights, `[10]` shadows, `[11]`
highlight tonal width, `[12]` shadow tonal width, `[13]` radius, `[14]` the Lab
mode flag, `[15]` the block's `scal` (always 1 at the export call site; the
launcher requires it), `[16]` and `[17]` the highlight and shadow tone tables
(32768 F32 in Lab mode, 65536 in RGB mode), `[18]` and `[19]` the working
RGB-to-XYZ and XYZ-to-RGB matrices, nine floats each, `[20]` and `[21]`
`Color::cachef` and `Color::cachefy`, `[22]` the int vibrance flag (1 only
when the block would touch a pixel), `[23]` and `[24]` pastels and saturated,
`[25]` and `[26]` the pastels/saturated threshold top and bottom, `[27]`
protect-skins, `[28]` avoid-colour-shift, `[29]` the skin-curve flag, `[30]`
`toneCurve.hrenabled`, `[31]` the flat Munsell set (197 x 140 floats), `[32]`
one float per table with its own length, `[33]` the 65536-entry skin curve
(one float when the curve is the identity), `[34]` the int Lab-curve flag (1
only when the enabled curve has `contrast == 0`), `[35..40]` the int L, a, b,
C=f(C), L=f(C) and C=f(L) activity flags, `[41]` the Munsell method (0 or 4),
`[42]` `lcredsk`, `[43]` `100 - rstprotection`, `[44]` and `[45]` the raw
`settings->protectred` and `protectredh`, `[46]` `settings->amchroma`
(validated but not read by the supported path; it only feeds the residual
L=f(H) curve), `[47]`
the working-profile chroma scale, `[48..53]` the six Lab-curve tables (the L
table is 32770 floats, the other five 65536), `[54]` and `[55]` those two
lengths, `[56]` the int local-contrast flag (1 when the enabled tool would
run), `[57]` the int radius, `[58]` to `[60]` the doubles `amount`,
`darkness` and `lightness` after the block's float narrowing, `[61]` the int
scale (always 1 at the export call site; the launcher requires it), `[62]`
the host-only `RtLabToolsIir` pointer with the three coefficient forms for
each direction, `[63]` the region count (0 or 1..4), `[64]` the region mask
tables (count x 3 tables of 65537 floats, each a node count followed by x and
y nodes), `[65]` that table stride, `[66]` the per-region parameter table
(count x 14 floats: abca, abcb, rs, slope, offset, power, channel, maskBlur,
the two radii, the three mask-active flags and a reserved slot), `[67]` and
`[68]` the working and inverse matrices as nine doubles each. A submission
with a wrong kind, length or non-finite value,
a local-contrast setting outside the ABI range, a coefficient block of the
wrong size or version, a region parameter outside the region ranges, or with a
method other than 0 and 4 is refused.

The context is host-only. `build()` derives the four Lab-grid floats, builds
the tone tables through the block's own `shadowsHighlightsLut()` (extracted
from `ImProcFunctions::shadowsHighlights()` so the host path and the setup
cannot drift), copies the working matrices and the two Lab caches, packs the
197 Munsell tables with one length per table, builds the vibrance skin curve
with `fillCurveArrayVib()`'s expression and the host's `/ask`, applies the
RGB-mode table check, builds and exports the six Lab-curve tables with the
block's own `CurveFactory` calls, and derives the local-contrast IIR
coefficients from the block's sigma through `gaussianIirCoefficients()`. The
Munsell set is exported when vibrance
needs it or when the Lab-curve method is `MUN`. `runHost()` runs the extracted
host block on the Lab buffer, honouring the buffer's pitches. The launcher
validates every kind, every table length and the matrix and cache shapes
before it uploads; the Lab buffer is read back only after every pass
succeeded, so a refusal leaves the host image untouched.

### Residuals and the builtin

A residual runs the `lab.tools` host builtin, which disables device recursion
for the duration and runs the original block, and the runtime records
`impl=host`: a non-zero Lab contrast, the chromaticity family and the LAB, XYZ
and XYZREL Munsell methods, a local contrast outside the ABI range or combined
with any other residual tool, EPD, black-and-white output, a
colour-toning method other than `LabGrid` and the bounded `LabRegions` set (a
mask display, more than four regions, an unsupported mask curve, or a region
slider outside its range), a profile with no active tool, an
out-of-range slider or red/skin setting, a vibrance no-op, a device without
fp64 for local contrast, the Lab curves and regions, a host built without SSE2
(or RT_SIMDE) for local contrast below sigma 25, a GPU for vibrance, local
contrast, the Lab curves and regions, an image too small for the guided filter
(fewer than three
samples on the subsampled grid) or for the IIR recurrence (fewer than four
samples per line),
an RGB-mode tone table above the slope bound, a process without the Munsell
tables, a local-contrast double state above the residency budget, and any
allocation or launch failure. The call site runs the same
block when the device is not selected, so with `RT_STAGE_OPENCL` unset the
node is the pre-node code path and its export is byte-identical to the
previous binary.

### Evidence

With `OMP_NUM_THREADS=1`, a 3888x2608 PEF and the POCL CPU device:

| Case | Result |
|---|---|
| `stage-residency-test` lab.tools cases at 238x64, 179x48 and 8192x64 | LabGrid-only, shadows/highlights Lab mode, both tools and every Lab-mode slider case are bit-exact (PAE 0, 0 differing samples). RGB mode is measured, not proved: 0.062500 LSB16 for the amount-50 pair, 0.046875 for both tools at 179x48, 0.076172 for the padded 238 case, 0.044922 and 0.045898 for the single highlight and shadow rounds, and a worst of 0.216797 at amount 100. The steep-step refusal is a worst-case bound, not a proof of those measurements: at the 0..100 slider maximum the measured error sits about four times inside the bound it enforces |
| Slider coverage | amounts 1/50/200/1000, tonal widths 0/100 and a split pair, radii 1/10/100, padded widths 179 and 238, and an 8192-wide image; all finite, no NaN or Inf |
| Vibrance, 21 cases at 125x160, 179x48, 238x64, 8192x64 and 64x16 | pastels/saturated combinations, negative saturated, thresholds 0/75/100 and split, protect-skins, avoidcolorshift on/off, a skin curve, highlight reconstruction, neutral and saturated images, padded widths, the 8192-wide case and the two-pixel knife-edge case: 2,570,352 samples, measured PAE 0 and asserted within 1 LSB16 (`over == 0`), no NaN/Inf, `impl=opencl` on every case (POCL CPU) |
| Munsell tables | all 197 tables and 27,580 exported entries equal `Color::MunsellLch()`'s LUTs; the avoidcolorshift path changes 11,696 of 60,000 domain samples, so the comparison is not vacuous |
| Lab curves, 21 cases at 238x64, 179x48 and 8192x64 | L only, brightness, an enabled pass with all six tables the identity, a only, a/b, C=f(C), L=f(C), C=f(L), all six combined, combined with MUN and with NONE, three red/skin protection settings, `lcredsk` off, neutral and saturated images, padded 179/238 and 8192 wide: 2,347,344 samples, `impl=opencl` on every case, no NaN/Inf, every sample bit-exact (PAE 0, 0 differing samples). The earlier 169 one-ulp samples came from the host's ≤3-column double-sqrt tail, which the kernel now reproduces |
| Lab-curve non-vacuity | on a 179x48 saturated fixture the MUN correction changes 288 of 25,776 samples and the red/skin protection changes 2,610, so the device comparison exercises both paths |
| Lab-curve residuals | contrast 15, chromaticity 25, a non-identity C=f(H), L=f(H) or H=f(H) curve, the XYZ Munsell method and black-and-white output are refused, record `impl=host` with no `impl=opencl`, and are byte-identical to the block (PAE 0) |
| Local contrast, 23 cases at 238x64, 179x48, 179x50, 238x63 and 8192x64 | radii 1/10/40/80/100 (both blur paths), amounts 0/0.1/1/2, darkness and lightness 0.5/1/2 and the two-way split, padded 179 and 238, an odd height of 50 (the horizontal tail rows), the 8192-wide case, and combinations with the Lab curves, shadows/highlights and vibrance: 2,461,164 samples, `impl=opencl` on every case, no NaN/Inf, every sample bit-exact (PAE 0, 0 differing samples) |
| Local-contrast tail, sigma 24 | the integer fixture and three wide non-integer fixtures (L alternates around 30000 and 0.4 so the float deviation subtraction rounds) cover the last `H mod 4` rows and `W mod 8` columns at 179x50 and 238x63. With the kernel's deviation kept in double - the pre-fix code - the amount-2 179x50 case moves 7 of 26,850 samples, worst 0.164062 LSB16 (26,843 exact); the fixed kernel is bit-exact on all four cases |
| Local-contrast non-vacuity | on a 179x48 fixture radius 10, amount 1 changes 8,592 of 25,776 samples (PAE 11,071.999900 LSB16), so the device comparison is not a copy |
| Local-contrast residuals, nine cases | radius 0 and 300, amount 3 and -0.1, darkness 4, lightness -0.5, a NaN amount, and the tool combined with EPD or a non-zero Lab contrast are refused, record `impl=host` with no `impl=opencl`, and are byte-identical to the block (PAE 0, 231,984 samples) |
| Lab regions, 27 cases at 238x64, 179x48 and 8192x64 | one/two/four regions; all masks open, no masks, hue-only, chroma-only, lightness-only, two- and three-point curved masks; CDL slope, offset, power and all combined; saturation +80 and -60; channels all/red/green/blue; mask blur 0, -2 and 5; after shadows/highlights, after the Lab curves and after vibrance; padded 179/238 and 8192 wide. 2,741,040 samples, `impl=opencl` on every case, no NaN/Inf, worst 0.500000 LSB16 and 0 samples over 1 LSB16. The single-region and open-mask cases are bit-exact; the residue is the mask table's sub-ulp interpolation and the CDL round trip |
| Lab-region combinations after the preceding tools | shadows/highlights + regions: PAE 0.003906 LSB16, 0 over 1; Lab curves + regions: PAE 0.500000, 0 over 1; vibrance + regions: PAE 0.003906, 0 over 1. Before the start-of-regions snapshot fix the same fixtures failed at 128.453125 (42,410 of 45,696 samples over 1), 464.082031 (44,510 over) and 415.078125 (44,447 over) respectively, because the masks were built from the image before those tools ran |
| Lab-region non-vacuity | the single-region case changes the block's own output on the fixture, so the comparison is not a copy |
| Lab-region residuals | a mask display, five regions, an `FCT_Unchanged` hue curve, slope 0.05, channel 3, saturation 200 and mask blur -20 are refused, record `impl=host` with no `impl=opencl`, and are byte-identical to the block (PAE 0, 7 x 25,776 samples) |
| Lab-region unresolvable table | a 2000-node zigzag `FCT_MinMaxCPoints` chromaticity curve is inside the predicate but the host table cannot follow it within 5e-7, so the setup refuses it: `impl=host`, no `impl=opencl`, PAE 0 against the block |
| Shared Lab-cache helper change | `rt_lt_cache_lookup` now clips above at `LUTf`'s `size-2` bound and `rt_lt_compute_f/_fy` use `Color::computeXYZ2Lab()`'s double negative branches. The affected paths are the RGB-mode shadows/highlights blend (`rt_lab_tools_apply_rgb`) and the region apply kernel. With the helpers reverted the RGB SH cases were 24,437/13,637/23,643/16,950/14,188/24,613 differing samples at PAEs 0.062500/0.046875/0.076172/0.044922/0.045898/0.228516 LSB16; with the fix 24,237/13,522/23,485/16,648/14,056/24,508 at the same PAEs except 0.216797 for amount 100. The real-PEF `labtools-sh-rgb.pp3` export is pixel-identical old vs new (AE 0, 425,762 samples differ from the host by 1 LSB in both). The change is a fidelity improvement, never a regression |
| Block on the real PEF, POCL | the device block matches `stage::labToolsHostBlock` bit for bit (30,419,712 samples, max float delta 0) on the full pre-vibrance Lab buffer |
| Residuals | vibrance + Lab contrast, vibrance + local contrast with amount 30 (outside the ABI range), vibrance + EPD, vibrance + a Lab region mask display, vibrance + black-and-white, a vibrance no-op (sliders at zero, identity curve) and pastels 200 (out of range) are refused, record `impl=host` with no `impl=opencl`, and are byte-identical to the block |
| RGB-mode steep table | amounts 200 and 1000 are inside the predicate but refused by the setup: `impl=host`, PAE 0 |
| Forced launcher refusal (a tone table of half the required length) | `impl=host`, no device pass, byte-identical to the block |
| `verify.sh --stage-device`, POCL CPU, `labtools-sh.pp3`, `labtools-sh-rgb.pp3`, `labtools-labgrid.pp3` | PASS, `max_pae=1.5259e-05` (1 LSB16); records `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608 |
| `verify.sh --stage-device`, POCL CPU, `labtools-vibrance.pp3` on the PEF chart | PASS, `max_pae=1.5259e-05` (1 LSB16); records `"stage":"lab.tools","impl":"opencl","source":"opencl"` |
| `verify.sh --stage-device`, POCL CPU, `labtools-labcurve.pp3` on the PEF chart | PASS, `max_pae=1.5259e-05` (1 LSB16); records `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608. The profile enables the L, a, b, C=f(C), L=f(C) and C=f(L) tables with `Gamutmunse=MUN`, so the gate row is the full supported set |
| `verify.sh --stage-device`, POCL CPU, `labtools-localcontrast.pp3` on the PEF chart | PASS, `max_pae=1.5259e-05` (1 LSB16); records `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608. Radius 60 takes the double IIR path, so the gate exercises the double forward state buffer |
| `verify.sh --stage-device`, POCL CPU, `labtools-regions.pp3` on the PEF chart | PASS, `max_pae=1.5259e-05` (1 LSB16); records `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608. Two regions: curved hue/chroma/lightness masks, CDL slope 1.1/offset 0.02/power 0.9, saturation 40, and a second region with a red channel copy, so the row exercises both mask radii and both CDL and channel paths |
| GPU, `labtools-localcontrast.pp3` | not run on a GPU in this change. The gate row is declared `cpu`, so a non-CPU device takes the same `SKIP ... CPU-device-only` branch as the vibrance and Lab-curve rows, and the launcher's device-type check refuses the pass before any upload |
| `verify.sh --stage-device`, RX 550 (rusticl), `labtools-vibrance.pp3` | prints `SKIP` for `pef/stage-device-lab-tools-vibrance` with `the row is CPU-device-only; the selected device is not a CPU OpenCL device`. The row stops before `compare_images` and prints no PAE. A manual `--gpu=auto` export records `"stage":"lab.tools","impl":"host","source":"device-fallback"`; it is not counted as device parity (the other GPU nodes move 372,307 samples by 1 LSB16). A manual `--gpu=off RT_STAGE_OPENCL=1` export is pixel-identical to `--gpu=off` (AE 0); only the Software tag differs |
| `verify.sh --stage-device` with `RT_REQUIRE_GPU=1`, RX 550 | the vibrance row FAILs intentionally with `the row is CPU-device-only; the selected device is not a CPU OpenCL device`; the POCL run is the authoritative gate |
| `labtools-residual.pp3` (shadows/highlights + vibrance + local contrast) | PASS as a host residual: `"stage":"lab.tools","impl":"host","source":"device-fallback"` |
| `--gpu=off`, `RT_STAGE_OPENCL` unset, five lab.tools profiles (including `labtools-labcurve.pp3`) | pixel data identical to the pre-change binary (`tiffcmp`); only the Software tag differs |
| `--gpu=off`, `RT_STAGE_OPENCL` unset, `labtools-localcontrast.pp3` and a Lab-curve + local-contrast profile | pixel data identical to the binary built at `e2e7011e5` (ImageMagick AE 0 and `tiffcmp` identical), so the CPU path is the pre-node code path |
| `--gpu=off`, `RT_STAGE_OPENCL` unset, `labtools-regions.pp3`, `OMP_NUM_THREADS=1` | pixel data identical to the binary built at `e41b62b16` (ImageMagick AE 0 and PAE 0), so the region change does not touch the CPU path. Without `OMP_NUM_THREADS=1` the pre-existing parallel export varies by about 2,800 of 30.4M samples between two runs of either binary; the serialised comparison removes that noise |
| OpenCL-disabled build (`build/cpu-perf-graph`, no OpenCL backend) | `rtengine` and `stage-residency-test` compile; the test prints `OpenCL unavailable (built without OpenCL)` and passes with 21 skipped. The `lab.tools` rows are host-only, so the CPU tree stays buildable and green |
| Forced refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`, `RT_REQUIRE_GPU=1`) | FAIL, exit 1, so the gate is not vacuous |

## Lab wavelet

`lab.wavelet` runs the directional pyramid equaliser and, as a second
increment, the wavelet tool's contrast-only path as one device node. Either
pass may be the only one enabled: the node no longer requires
`dirpyrequalizer.enabled` when only the wavelet runs. The wavelet
decomposition, statistics, contrast multiply and reconstruction are device
kernels; the other wavelet panels stay on the host and are residuals.

### Supported set

The equaliser part is admitted when

- `dirpyrequalizer.enabled` is true and `cbdlMethod == "aft"`;
- the call site's CIE-CAM gate is open
  (`(colorappearance.enabled && !settings->autocielab) || !cam02`);
- every multiplier is finite and in 0..4, the threshold in 0..1, skin
  protection in -100..100 and each hue point in -10..10;
- at least one multiplier is more than 0.001 away from 1, so the block would
  touch the image.

The wavelet part is admitted when `wavelet.enabled` is true and

- `Expcontrast` is set and every other export panel is clear: chroma, edge,
  blur, residual, final, toning, noise and clarity;
- `ChoiceLevMethod == "all"` and `TilesMethod == "full"`;
- `Softrad` and `Softradend` are zero, skin protection is zero, and
  `AvoidColorShift` and `Median` are off;
- `ContExtra` is zero and `DaubMethod == "6_"` (the default 8-tap
  Daubechies);
- `HSMethod` is `"with"` or `"without"`, and the chroma-by-hue curve is the
  identity;
- sigma is in 0.05..2.5, offset in 0.33..1.66, lowthr in 20..100, the
  highlight and shadow level gates are 1..9 and 3..9, strength is 0..100,
  Thrend is 0..100 and every contrast multiplier is -100..350;
- the finest contrast multiplier is non-zero and the image is large enough
  for the decomposition the block would build.

The transform level count is the block's own: the `MaxLev` slider, capped by
the image-size limits, the extra level, the trailing-zero trim that applies
when `Contrast` is zero and nothing else needs a level, and the unconditional
`Thrend` restore that follows it. A positive `Thrend` restores the untrimmed
capped count, so the device and the block agree on `maxlvl` even when the
last contrast multipliers are zero; `Thrend` is host-side only and is not in
the parameter table. The launcher
additionally requires a CPU OpenCL device (POCL), an 8x8 image for the
equaliser, a width of at least twice the largest half-window for its active
levels, fp64 for the wavelet statistics, and half-size dimensions of at least
`2^(levels-1)` for the undecimated Haar bands. A GPU device or an image that
narrow reaches the builtin and records `impl=host`.

### ABI

The ABI is `[0]` the three-plane F32 Lab buffer, read and written in place;
`[1]` the host-only `RtLabWaveletContext` pointer (`RT_STAGE_ARG_PTR`), version
2; `[2]` the int support flag, mirrored from the context descriptor; `[3..8]`
the six double multipliers, level 0 finest; `[9]` the double threshold; `[10]`
the double skin protection; `[11..13]` the doubles `b_l`, `t_l` and `t_r` (the
block's `hueskin` points divided by 100); `[14]` int `gamutlab`; `[15]` int
the block's scale, which the launcher requires to be 1; `[16]` int the number
of active equaliser levels; `[17]` the host-built range tables, `levels *
0x20000` floats; `[18]` the host-built hue mask and `[19]` the chroma mask,
each `width * height` floats and present only when skin protection is
non-zero; `[20]` int the wavelet device flag; `[21]` the host-built wavelet
parameter table, `64 + levels * 10` floats.

The context carries a `RtLabWaveletWavelet` sub-structure with the wavelet
support flag, level count, tap count, the parameter-table pointer and a
`compute_mea()` callback. The table's first 64 floats are the scalars
(levels, HS method, the two level gates, `c[0..9]`, thresholds, sigma, offset,
lowthr, strength, the four highlight and four shadow corners, the tap count)
and the 8-tap analysis/synthesis filter rows; the tail is one 10-entry `mea`
block per level. `compute_mea()` is called with the device statistics and
fills the tail on the host through the block's own `calceffect()`, so the
device never re-derives a threshold band. Nothing in the context is uploaded:
the pointers are host memory the launcher reads.

### Arithmetic and provenance

The block's own code supplies both host inputs, through two helpers extracted
from `dirpyr_equalizer.cc` so the block and the setup cannot drift:

- `dirpyrFillLut()` is the block's `fillLut()`, run once per active level. The
  kernel never evaluates the threshold band, the `exp(level * log(3))` scale
  or the 0.65/0.45 multiplier taper.
- `dirpyrHueChroma()` is the block's precompute, including its SSE
  `xatan2f()`/`sqrt` path and its scalar tail. The kernel reads the resulting
  masks, so it never re-derives an angle.

The kernel is a port of `dirpyr_channel()`,
`idirpyr_eq_channel()` and `Color::SkinSatCbdl()`:

- levels 0 and 1 use the kernel-free neighbour set; levels 2 to 5 use the
  5x5 domain kernel (`1000/2000`) at `scale = 2^level`;
- every level reads the same neighbour order, boundary clamps and
  `rangeFn()` floor of 1000 as the host;
- the host's SSE loop processes four output columns at a time and loads four
  consecutive samples per neighbour, so for `scale > 1` lanes 1..3 are not the
  scalar neighbour set. The device reconstructs the group start and lane and
  reproduces that load; without it the multi-level cases diverged by up to
  4.16 LSB16 on the test fixtures;
- the range lookup reproduces `LUTf::operator[](float)` with
  `LUT_CLIP_BELOW | LUT_CLIP_ABOVE`: truncation, the `size-2` upper clip and
  linear interpolation between the two neighbouring entries;
- the reconstruction starts from the coarsest pyramid level, accumulates the
  weighted high-pass into that level exactly as the block reuses it, and level
  0 writes the source L plane, which is the block's final copy;
- `Color::SkinSatCbdl()` is ported verbatim, with the same float constants,
  comparison order, `scale == 1` hard/soft branch and the `skinprot > 85`
  hair/beard extension.

`#pragma OPENCL FP_CONTRACT OFF`, `#pragma STDC FP_CONTRACT OFF` and
`-cl-fp32-correctly-rounded-divide-sqrt` keep the float operations in the
block's order. The program is fp32 only.

`gamutlab` with non-zero skin protection is not a kernel: `BadpixelsLab()`
runs on the host, on a private copy of the image, before the masks are
computed. Its separable Gaussian analysis and its L/a/b repairs therefore
match the block bit for bit, and a refusal after the pre-pass leaves the
caller's Lab buffer untouched. The device launcher then uploads only L and
copies the repaired a/b planes from the prepared copy after readback.

### Wavelet transform, statistics and contrast

The wavelet pass is a direct port of `ip_wavelet()`'s contrast-only path. The
launcher runs it after the equaliser (if any) on the same L plane, in the
block's order:

1. The level-0 analysis runs the decimated 8-tap Daubechies filter
   (`Daub4_anal8`, offset 2) at half resolution: one vertical kernel writes
   the two half-height rows with the host's clamped border and ascending tap
   order, and one horizontal kernel writes the low pass and the three detail
   bands, subsampling every other column.
2. Levels 1..L-1 run the undecimated Haar analysis at stride `2^(level-1)` on
   the same half-size grid. The border rule is the host's asymmetric one: the
   band `[size - skip, skip)` is refused by the predicate rather than read
   uninitialised.
3. One work item per (level, direction) band scans the band in ascending
   row-major order and reproduces `Aver()` and `Sigma()` back to back. Every
   sum and variance accumulates in double, the threshold is `32.7f`, the
   positive and negative counts are separate, and the mean, sigma, max and min
   are converted to float exactly as the host does at `OMP_NUM_THREADS=1`.
   A parallel tree reduction would flip near-threshold `lbeta` values; it is
   not used.
4. The statistics read back to the host, `RtLabWaveletContext::compute_mea()`
   averages the three directions, calls the block's `calceffect()` for each
   level and fills the `mea` tail of the parameter table. The table is
   uploaded again before the contrast pass.
5. One kernel per level applies `ContAllL()` to the three bands: the `lbeta`
   piecewise factor, `alpha = (1024 + 15 * c[level] * scale * scale2 * lbeta *
   diagacc) / 1024` with `scale = scale2 = diagacc = 1`, and, with
   `HSMethod=with`, the per-pixel `kH` ramp, the four highlight and four
   shadow corners and the two level gates. `cpMul == 0` skips the band, as the
   block does. `LL100` is the original L at `(2y, 2x)` and `LL100res` is the
   level-0 residual coefficient.
6. The inverse runs the Haar levels from the coarsest down, using the two
   half-size scratch rows, then the level-0 Daubechies synthesis with
   `blend = strength / 100` and the `L <= 0 -> 1` clamp that is the block's
   last operation on L.

`wavelet.contrast`, the residual contrast curve, `strength` only through the
level-0 blend, and the clamps are all reproduced. The transform and the
contrast multiply are bit-exact against the block on POCL.

Device memory is the bands (`3 * levels` half-size planes), the two
half-size low-pass ping-pong planes, two full-width half-height scratch rows
(`W * ceil(H/2)` floats each), and the fixed statistics and parameter tables
(`levels * 18` and `64 + levels * 10` floats). That is
`(3 * levels + 2) * ceil(W/2) * ceil(H/2) + 2 * W * ceil(H/2) + levels * 28 +
64` floats; the commonly quoted `(3 * levels + 7) * ceil(W/2) * ceil(H/2)` is
an upper bound that folds the scratch rows into half-size planes. Every
buffer is acquired before the equaliser runs, so an exhausted
`RT_STAGE_DEVICE_BUDGET_MB` falls back with the caller's image untouched.

### Residuals

For the equaliser: `cbdlMethod == "bef"`, a multiplier, threshold, skin or hue
point outside the ABI bounds, an all-neutral pyramid, a closed CIE-CAM gate, a
GPU device, a too-narrow image, and a failed host setup are residuals.

For the wavelet: an enabled section without `Expcontrast`, any other export
panel, another `ChoiceLevMethod` or `TilesMethod`, a non-zero soft radius,
skin protection, avoid-colour-shift, the blue-sky median, `ContExtra`, a
non-default Daubechies length, an unknown `HSMethod`, a non-identity
chroma-by-hue curve, any scalar outside its bounds, a zero finest multiplier,
a zero level count, a too-small image, and a GPU or non-fp64 device are
residuals. Nothing enabled at all is also a residual.

Each runs the `lab.wavelet` host builtin, which executes the extracted host
block with device recursion disabled, and records `impl=host`, never a device
pass. When both passes are supported they run in the block's order in one
submission.

### Evidence

`stage-residency-test`, POCL CPU, `OMP_NUM_THREADS=1`:

| Case | Result |
|---|---|
| Equaliser parity, 14 cases at 179x48, 238x64 and 8192x64 | six levels, finest only, three levels, threshold 0 and 1, skin protection +60 and -60, gamutlab with and without skin, padded 179/238, a saturated hue circle and an 8192-wide plain and skin case; 3,574,560 samples, `impl=opencl` on every case, no NaN/Inf, worst 0.115234 LSB16 and 0 samples over 1 LSB16 |
| Equaliser non-vacuity | skin protection changes 574 of 45,696 samples (PAE 108.908203 LSB16); the gamutlab pre-pass changes 29,542 of 45,696 (PAE 20,453.597931) |
| Wavelet parity, 14 cases | transform only (all contrast multipliers zero), contrast with and without HS, negative multipliers, sigma 0.5 / offset 1.4, strength 60, padded 179x49, levels 1/2/3/7, 8192x64, the equaliser plus wavelet in one submission, and two trailing zero multipliers with `Thrend=5`: 2,068,233 samples, measured PAE 0 and asserted bit-exact (0 differing samples), finite, `impl=opencl` on every case. Before `Thrend` was honoured the last case diverged by PAE 70.428223 LSB16 and 10,061 samples over 1 LSB16 |
| Wavelet non-vacuity | the contrast path changes 14,651 of 45,696 fixture samples against the identity, PAE 3,933.345703 LSB16 |
| Wavelet residuals, 30 cases | each unsupported flag, method, curve, range (including `Thrend=101`), zero multiplier, zero level count and the all-off profile are refused, record `impl=host` with no `impl=opencl`, and are byte-identical to the block (PAE 0, 30 x 45,696 samples) |
| Default path, equaliser and wavelet | with `RT_STAGE_OPENCL` unset and no override, `run()` resolves to the builtin, records `impl=builtin`, and is byte-identical to `labWaveletHostBlock` |
| GPU | not run on a GPU in this change. The launcher's `CL_DEVICE_TYPE` check refuses before any upload, so a GPU submission records `impl=host`; the test's GPU residual is skipped on POCL and runs when a non-CPU device is selected |
| `verify.sh --stage-device`, POCL CPU, `labwavelet.pp3` | PASS, `max_pae=1.5259e-05` (1 LSB16), record `"stage":"lab.wavelet","impl":"opencl","source":"opencl"` at 3888x2608 |
| `verify.sh --stage-device`, POCL CPU, `labwavelet-contrast.pp3` | PASS, `max_pae=1.5259e-05` (1 LSB16), record `"stage":"lab.wavelet","impl":"opencl","source":"opencl"` at 3888x2608; the profile leaves the equaliser off |
| Builds | `build/opencl-perf-graph` and `build/cpu-perf-graph` compile `rth-cli` and `stage-residency-test` from the final sources with GCC and the generic target, no warnings. The CPU-only test prints `OpenCL unavailable (built without OpenCL)` and reports `stage-residency-test: PASS (24 skipped)` |
| `--gpu=off`, `RT_STAGE_OPENCL` unset | the call site takes the extracted host block; the node is not submitted, and the output is the pre-node CPU result |

## Local adjustments

`local.adjustments` is the export block at `simpleprocess.cc:1468`. The block
applies the spot parameter fixups, the optional pre-CBDL directional pyramid
equaliser, `rgb2lab()` into `labView`, one `ImProcFunctions::Lab_Local()` call
per spot and the final `lab2rgb()` back into `baseImg`. `Lab_Local()` takes
270 parameters and the colour-only path is a small island inside it: the spot
loop, the colour sliders and curves, the deltaE blend and the two full-image
round trips.

### Host block

The body of `stage_local_adjustments()` moved unchanged into
`rtengine::stage::localAdjustmentsHostBlock()` in
`rtengine/stage/local_adjustments_context.cc`. The call site, the host builtin
and the tests now share one CPU implementation. The block still owns the
pre-CBDL pass and the final `lab2rgb()`; the caller creates `labView` and
passes it in, so the extracted function no longer allocates it. The `nocrash`
prefilter at the start of every `Lab_Local()` call is the per-spot negative
value round trip (`lab2rgb`, every channel `max(epsi0)`, `rgb2lab`); the tail
clamp is the same round trip with `epsi` (`1e-5`) and, when the Laplacian arm
is selected, `clipR()`. The tone curve mode selects `notzero` in every profile,
so a supported spot always takes the tail arm; the record carries the arm
rather than assuming it.

### ABI and context

The node takes three arguments: a three-plane `F32` Lab buffer, a pointer to
`RtLocalAdjustmentsContext` and the block support flag. The context is
host-only and versioned (`RT_LOCAL_ADJUSTMENTS_CONTEXT_VERSION 1`,
`RT_LOCAL_ADJUSTMENTS_CONTEXT_MAGIC 0x4C414A43`, size checked). It holds a
`build` callback and a `run_host` callback. The call site passes only
`RT_STAGE_ARG_BUFFER`, `RT_STAGE_ARG_PTR` and `RT_STAGE_ARG_INT`, so a stale
descriptor is rejected before any callback runs.

`build(spot, lab, out)` prepares one spot at a time. It calls
`ImProcFunctions::localAdjustmentsSpotParams()`, the new narrow interface to
the block's own `calcLocalParams()`, so geometry, transition, deltaE weights
and the activity flags are the values `Lab_Local()` will use rather than a
second derivation. It then calls `calc_ref()` on the image *as it stands
before that spot* (each spot's references depend on the previous result),
`complexCurvelocal()` for the lightness/contrast table and the curve factories
for the four diagonal and three hue curves. The flat record has
`RT_LOCAL_ADJUSTMENTS_SPOT_STRIDE` (66) floats: geometry, transition and
deltaE weights, the slider values, the references and area average, the
activity and method flags, the clamp arm, the curve offsets and the
working-profile chroma scale. Curve offsets index one float table
(`RT_LOCAL_ADJUSTMENTS_MAX_LUT_ENTRIES`, 2^22 floats); `-1` means absent. The
setup also exports the nine working-space matrix floats, `Color::cachef[]` and
`cachefy[]`, and the 197x140 Munsell set.

The reference build is deliberately per spot. A single `build` for the whole
block cannot be faithful: `calc_ref()` for spot N reads the Lab result of spot
N-1, and a future launcher must read the device buffer back between spots
before calling `build` again. The per-spot record shape and callback make that
ordering explicit; the current launcher does not use it.

### Predicate and refusal

`localAdjustmentsSupported()` is block-level. It admits a normal elliptical or
rectangular spot with no recursion, inversion, merge, explicit mask, soft
radius, gradient, LabGrid, RGB curve, exposure curve or tool other than
Colour & Light, `avoidgamutMethod` NONE or MUNS, `avoidrad == 0` and
`gamc == 1`; an inactive spot is admitted as the clamp-only case. It refuses
the pre-CBDL and dehaze passes, the exposure/soft-light/dirpyrequalizer
Laplacian arm, every other tool, LAB/XYZ/XYZREL gamut methods and any value
outside the record's ranges. One refused spot makes the whole block a
residual, so device and host are never mixed per spot.

The OpenCL queue registers the node, validates the three arguments, buffer
format and pitch, the context magic/version/size and the support flag, then
refuses with `local.adjustments colour-only kernel is not implemented` (or
`local.adjustments residual` for a zero flag). The refusal happens before the
upload, the host queue runs the builtin, and the record is
`"impl":"host","source":"device-fallback"`. The builtin runs
`localAdjustmentsHostBlock()` with device recursion disabled.

### Measured blocker

The colour kernel itself is not in this change. A faithful port needs, per
spot, the ROI Gaussian for `origblur` (radius `(2 + 0.2 * blurcolde) / sk`),
the colour core (the chroma NURBS pair, C=f(C), C=f(L), L=f(C), H=f(H),
L=f(H), C=f(H), the lightness/contrast `calclight()` table and the LabGrid
residual gate), `transit_shapedetect2()`'s transition/deltaE blend, the
per-spot `nocrash` and tail round trips, and `avoidcolshi()`'s Munsell pass.
`calc_ref()` and `localAdjustmentsSpotParams()` stay host-side, which forces
one submission per spot with a read-back between them; the block's
`reservedView` is fixed and the `lastorigView`/`shbuffer` state is only needed
by merge and inverse sharpening, both residuals. The host also uses Sleef
`xatan2f`/`xcosf`/`xsincosf`/`pow_F` in the transition and Munsell products;
the 1 LSB16 target has to be measured against those, not against OpenCL's
libm. The host block, predicate, record builder, matrices, caches, Munsell
export and refusal are therefore landed first; no subset that skips the clamp
or the Munsell pass is shipped.

### Evidence

`stage-residency-test` on the CPU and POCL builds:

| Case | Result |
|---|---|
| Host extraction, four profiles at 179x48, 179x49 and 238x64 | one inactive spot, two overlapping spots, three overlapping spots and the three reversed: `impl=host` on the POCL run, no `impl=opencl`, PAE 0.000000 LSB16, 0 differing samples, finite (25,776 / 26,313 / 45,696 / 45,696 samples) |
| Predicate | the default profile is refused; a default inactive spot is admitted; 25 residual mutations (exposure, SH, vibrance, local contrast, blur, tone map, soft light, sharpening, log, mask, explicit mask, soft radius, recursion, inversion, merge, `exc`, LAB, XYZ, `avoidrad`, RGB curve, LabGrid, structure, gamma, pre-CBDL, dehaze) are refused |
| Record builder | one spot record, finite slots, L curve active and L=f(H) inactive for a non-identity L curve and an identity hue curve, MUNS recorded, the negative-value prefilter recorded, `CLAMP_NOTZERO` recorded, absent tables marked `-1` |
| CLI refusal record | `localadjustments.pp3`, 3888x2608 PEF, POCL CPU, `--gpu=auto`, `RT_STAGE_OPENCL=1`: the node records `"stage":"local.adjustments","impl":"host","source":"device-fallback"` before the classic backend's pre-existing `opencl::initLocked()` P2 crashes the process. The block itself completes in 3.3-3.5 s. No CLI TIFF parity is claimed from this run |
| Host block parity | the four residency cases above are the TIFF-equivalent Lab comparison: `impl=host`, PAE 0, 0 differing samples, finite |
| Default path | with `RT_STAGE_OPENCL` unset the call site runs the host block directly; the four residency cases are unchanged and the CPU-only test passes |
| Builds | `build/cpu-perf-graph` and `build/opencl-perf-graph` compile `rth-cli` and `stage-residency-test`; CPU-only test reports `PASS (24 skipped)` |
| Gate | `verify.sh --stage-device` rows added for both profiles; with the current refusal they print `SKIP ... stage device declined`, and `RT_REQUIRE_GPU=1` turns that into a failure. The device row is not marked PASS. |

## Limitations

- GPU vibrance and GPU Lab curves are refused by design. The RX 550
  isolation above shows the vibrance inputs are identical and the block output
  differs at two Munsell-limit pixels because rusticl's `xatan2f()` division is
  not correctly rounded. The Lab-curve pass has the same Munsell-limit
  sensitivity, and its fp64 program has no reason to be admitted on a GPU
  either. The region pass has the same CPU-only requirement because its mask
  product, hue mapping and double working-matrix products are contracted
  differently by a GPU compiler. The launcher therefore requires a CPU OpenCL
  device for vibrance, local contrast, the Lab curves and regions; a GPU
  export runs the host block and records `impl=host`. POCL is the deterministic
  device for this node, and its PEF gate rows measure 1 LSB16. A CPU device
  without fp64 runs the Lab-curve, local-contrast and region blocks on the host
  and records `impl=host`.
- The directional pyramid equaliser and the wavelet contrast path are
  CPU-device only; the wavelet statistics also need fp64. The equaliser uses
  the host's float division and its SSE group load, and the wavelet path
  depends on the host's exact double scan and float multiply order, so the
  launcher refuses a GPU (or a non-fp64 device for the wavelet) before any
  upload and the block records `impl=host`. The equaliser alone, the wavelet
  contrast tool alone and both in the block's order are supported; the other
  wavelet panels are residuals.
- P2 residual, not a lab.tools defect: with `RT_STAGE_OPENCL=1` and
  `--gpu=auto` but without `RT_OPENCL_RGBPROC=1`, a CLI export segfaults in
  `opencl::initLocked()` through `ImProcFunctions::lab2rgbOut()` - a null call
  in the classic backend's lazy OpenCL initialisation - for every profile, the
  neutral default included. It reproduces at `03c94581c`, before the lab.tools
  node existed. `RT_OPENCL_RGBPROC=1` avoids it, and `tools/opencl/verify.sh`
  sets it, so the lab.tools gate evidence was produced with that environment;
  a bare CLI export of a lab.tools profile needs the same variable.
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
- The device nodes are the ones `opencl_stage_nodes()` lists; the catalogue in
  `STAGE_PIPELINE.md` records which blocks are `yes`, `partial` or `no`. Every
  other node resolves to the host program with a reason. The device path is
  opt-in (`RT_STAGE_OPENCL=1`) and nothing in the default configuration runs on
  a device.
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
