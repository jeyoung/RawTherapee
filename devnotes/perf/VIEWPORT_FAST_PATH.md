# Viewport-aware preview fast path

The viewport fast path is enabled by default for safe post-base parameter
changes. `RT_VIEWPORT_FAST=0` disables it for comparison or troubleshooting.
It rebuilds the parent LUTs that visible `rtengine::Crop` objects consume from
the cached full-frame analysis state, renders the crop ROI immediately, and
accumulates the analysis stages it skipped. The accumulated pass runs once
after the interaction settles so that the navigator, preview image, `workimg`,
histograms, scopes and listeners catch up.

With the kill switch set to `0`, every request takes the existing full path
unchanged. Export is not involved: the fast path lives in
`ImProcCoordinator::updatePreviewImage()` and only affects the interactive
preview.

## Scope

This implementation covers requests that cannot change the base image or the
full-frame analysis inputs:

- `M_AUTOEXP`, `M_RGBCURVE`, `M_LUMACURVE`, `M_LUMINANCE`, `M_COLOR`,
  `M_CROP` and `M_MINUPDATE`;
- `M_HDR` and `M_SPOT` may appear because several GUI events over-approximate
  with them, but only when the corresponding parameters are unchanged. Active
  Fattal, dehaze or spot removal is rejected, because the normal path would
  re-apply those filters.

The transform-stage family (`M_TRANSFORM`) is not promoted. `Crop::update()`
does render ROI transforms, but the full validation could not be completed
with the available headless GUI tooling, so the gate stays closed and the
follow-up is recorded below.

The fast path never calls `ipf.rgbProc` on the full preview, `firstAnalysis()`,
locallab, CIE, Fattal/dehaze, spot removal or film-negative processing.

## Eligibility

`canUseViewportFastPath()` is conservative and compares the current
`ProcParams` with `viewportFullParams`, a copy of the request inputs of the
last completed full-frame pass. It rejects the request if any of the following
holds:

- the kill switch is set to `0`, no full pass completed, or the high-detail
  state changed or is still being computed;
- the request contains `M_PREPROC`, `M_RAW`, `M_WB`, `M_INIT`,
  `M_LINDENOISE`, `M_MONITOR`, `M_TRANSFORM`, `M_CSHARP`, `M_RETINEX`,
  `M_HIGHQUAL` or `M_BLURMAP`, or any bit outside the allowed set above;
- auto exposure, histogram matching, auto B&W or auto colour-toning saturation
  is active;
- CIE colour appearance or Local Lab is enabled;
- the Lab curve contrast is non-zero (no cached `lhist16` yet);
- Fattal, dehaze or spot removal is active;
- `raw`, `wb`, `icm`, `coarse`, `commonTrans`, `rotate`, `distortion`,
  `lensProf`, `perspective`, `gradient`, `pcvignette`, `cacorrection`,
  `vignetting`, `spot`, `filmNegative`, `dehaze`, `fattal`, `retinex`,
  `pdsharpening`, `dirpyrDenoise`, `cg` or `locallab` differ from the
  snapshot;
- the highlight-reconstruction subset of `toneCurve` changes
  (`hrenabled`, `method`, `hlbl`, `hlth`, `clampOOG`);
- an auto flag changes;
- a tone/Lab/wavelet input changed without its corresponding request bit
  (the fast path would otherwise keep stale parent LUTs).

White balance is compared against the pre-pass copy on purpose. The normal
full pass normalises `wb.green` into the rendering parameters; the GUI keeps
the un-normalised value, so comparing post-pass copies would reject every
subsequent request. The base state still corresponds to the pre-pass request,
because white balance is a deterministic function of it.

## Parent LUT preparation

`prepareViewportLuts()` mirrors the parent LUT work of the full path with the
same `scale`/skip arguments:

- `CurveFactory::complexCurve` with the cached `vhist16`, plus `RGBCurve`,
  colour-toning LUTs and opacities, B&W curves, custom tone curves and
  `opautili`;
- `CurveFactory::complexLCurve` with a permanently zero histogram. Lab
  contrast is restricted to zero, so the histogram cannot affect
  `lumacurve`; `histLCurve` is refreshed by the deferred pass;
- `clcurve` and `complexsgnCurve` when `M_LUMACURVE` is requested;
- `wavclCurve` when `M_LUMINANCE`, `M_COLOR` or `M_AUTOEXP` is requested.

The tone/LUT block was factored into the shared helper and the full path calls
the same code.

## Flow

1. A request is accepted if the generation counter is current.
2. Eligibility is checked on the un-augmented request mask, so a transform
   derived from a crop-only change does not force a full render.
3. The crop todo is sanitised to
   `M_CROP|M_MINUPDATE|M_AUTOEXP|M_TRANSFORM|M_RGBCURVE|M_LUMACURVE|M_LUMINANCE|M_COLOR`;
   over-approximated `M_HDR`/`M_SPOT` and all base bits are removed.
4. Crops with a listener are updated immediately. This is the only work the
   interaction pays for.
5. The analysis stages the hit skipped are accumulated in
   `pendingDeferredMask` under `paramsUpdateMutex`, with a deadline. The hit
   does not queue a full-frame pass and does not block further fast hits.
6. When the deadline expires, `flushDeferredAnalysis()` merges the accumulated
   mask into `changeSinceLast`, sets `forceFullPreview` and starts the normal
   `process()` loop. It updates `previmg`, `workimg`, histograms/scopes and
   listeners, re-renders the crops, and only then marks the viewport state
   valid again.

`forceFullPreview` is set when the analysis is queued, not when the hit is
served, and it no longer blocks eligibility. `absorbPendingDeferred()` folds a
pending mask into any request that takes the normal path, so a base-changing
edit cannot skip the stages a fast hit already applied to the crop.
`markViewportStateValid()` publishes the snapshot only after the full-frame
block ran, and it keeps the per-hit bookkeeping when a newer burst was queued
while the pass was running.

## Deferred analysis

`rtengine/viewportdefer.h` holds the pure decision logic:

- `viewportDeferredMask()` forces `M_MONITOR|M_RGBCURVE|M_LUMACURVE
  |M_LUMINANCE|M_COLOR` and strips every base bit;
- `viewportDeferredDeadline()` re-arms the settle window on each hit and caps
  the wait from the first hit of the burst;
- `viewportCropRenderReusable()` allows the deferred pass to skip the crop
  render only while the burst contains a single hit.

Both the settle window and the max-wait cap follow
`Settings::previewRefreshMs`, the Preview Refresh preference (100-200 ms,
default 125 ms). The value is taken once per request, so a live change applies
to the next hit, and the two values always match: the deadline is one period
after the first hit of a burst, so a continuous drag produces one analysis per
period. This intentionally gives up the earlier burst coalescing for cadence
alignment; `RT_VIEWPORT_DEFER_MS` restores a longer settle when coalescing
matters more.

| Value | Source | Unit | Range | Purpose |
|---|---|---|---|---|
| Settle window | `Settings::previewRefreshMs` | ms | 100–200 | Quiet period before the analysis runs |
| Max-wait cap | `Settings::previewRefreshMs` | ms | 100–200 | Cap from the first hit |
| `VIEWPORT_DEFERRED_POLL_MS` | 10 | ms | — | Watcher sleep while an analysis is pending |
| `VIEWPORT_DEFERRED_IDLE_POLL_MS` | 100 | ms | — | Watcher sleep while idle |

`RT_VIEWPORT_DEFER_MS` and `RT_VIEWPORT_DEFER_MAX_MS` override the two values
independently and keep the wider 0 to 1000 ms range; `RT_VIEWPORT_DEFER_MS=0`
queues the analysis immediately and reproduces the pre-deferral behaviour.
The old fixed 250 ms settle and 500 ms cap are gone; they added a 250 to
500 ms timer mismatch between the debounce and the catch-up. The residual
histogram lag is now the analysis time minus the crop time: about 23 ms at
10 MP and 65 to 70 ms at 45.7 MP at fit and 100 %. See
`devnotes/perf/PREVIEW_REFRESH.md`.
`RT_VIEWPORT_FAST=0` still disables the whole path.

The watcher is one low-priority `Glib::Thread` per coordinator, created on the
first deferred hit and joined in the destructor. `stopProcessing()` sets a
pause flag and drops the pending mask before joining the render thread, so an
image switch or the rough/final loading handoff cannot resurrect work against
buffers that are about to be rebuilt. `startProcessing(int)` clears the pause.
`invalidateViewportState()`, `assign()`, `freeAll()`, `setScale()` and
`setTweakOperator()` drop the pending mask as well.

## State validity and invalidation

The fast path has two snapshots:

- `viewportFullParams`, the pre-pass base/global inputs of the last completed
  full-frame pass;
- `viewportLutParams`, per-family snapshots of the last parameters whose tone,
  Lab and wavelet parent LUTs were actually rebuilt.

State is invalidated on image switch (`assign()`), `freeAll()`, destruction,
`stopProcessing()`, and `setScale()` dimension or scale changes. It also
becomes invalid whenever a request bypasses the fast path with a missing or
mismatched base snapshot. In that case the request is augmented with
`M_INIT | M_HDR | M_MONITOR` (and `M_SPOT` when spot parameters differ),
forcing a real base/analysis rebuild rather than marking stale state valid.

`viewportStateValid`, `viewportHighDetail`, `forceFullPreview`,
`viewportDeferredRequest` and `deferredCropRendered` are atomic. The state is
marked valid only after the full-frame block actually ran; a request that
skipped that block cannot publish a snapshot. High-detail preprocess/demosaic
flags are reset on a new image source (`assign()`) and after
`saveInputICCReference()`; buffer reallocations do not reset them because the
source planes remain valid.

## Profiling

With `RT_PREVIEW_PROFILE=1` each completed request reports one of:

```text
preview update: 40 ms todo=0x1009f cache=0 viewport=fast crop_ms=39 deferred_todo=0x1005f deferred_hits=3
preview update: 92 ms todo=0x1005f cache=0 viewport=deferred-analysis
preview update: 113 ms todo=0x1009f cache=0 viewport=fallback reason=switch-off
```

`deferred_hits` is the size of the burst the hit belongs to; `1` means the
deferred pass may reuse the crop render, greater values mean it renders the
crops again. Fallback reasons include `no-complete-full-state`, `base-request`,
`auto-exposure`, `lab-contrast`, `highlight-recovery`, `cie`, `local-lab`,
`hdr-processing`, `spot-removal` and the individual base-parameter names.

## Validation

Built both trees:

```bash
cmake --build build/cpu-opencl -j
cmake --build build/opencl-opencl -j
RT_BUILD_DIR=build/opencl-opencl tools/opencl/verify.sh --quick
```

`verify.sh --quick` passed `pef/neutral` (max PAE 1.5259e-05) and the monitor
self-test.

Xvfb is not installed on this host. Nested Xephyr servers were used instead;
no host packages were installed. With the 10 MP PEF, neutral exposure profile
and a fresh settings directory:

- at fit zoom, fast-path hits took about 40 ms for the visible crop, followed
  by a 92 ms deferred analysis; the existing path took about 113 ms;
- at 100 % zoom, the visible crop took 29 ms and the deferred analysis 30 ms;
- the first edit after opening can fall back once while the startup/default
  profile settles the base snapshot; subsequent edits take the fast path;
- auto levels produced `viewport=fallback reason=auto-exposure`;
- a sidecar with Lab contrast 20 produced `reason=lab-contrast`;
- final main-window screenshots after the same two-step Exposure Compensation
  sequence were pixel-identical with `RT_VIEWPORT_FAST` on and off at fit and
  100 % zoom (`compare -metric AE` = 0);
- with the feature enabled by default (no environment variable), the same
  100 % sequence took 26 ms for the visible crop and 30 ms for the deferred
  pass, and matched the `RT_VIEWPORT_FAST=0` screenshot with AE = 0;
- CLI exports with the switch on and off were pixel-identical
  (`compare -metric AE` = 0; only the TIFF `DateTime` tag differed);
- known user-modified files were left unstaged.

### Deferred analysis (issue #23)

Unit test: `tools/perf/test-fast-preview.sh build/cpu-issue-23-viewport-defer`
PASS, covering the deferred mask, the deadline cap, and the single-hit crop
reuse rule. The OpenCL build passed `tools/opencl/verify.sh --quick` on the
AMD Radeon RX 550: `pef/neutral max_pae=1.5259e-05` and the monitor self-test
(1 byte, 14 of 196608 samples).

Method: CPU build, 4C/8T host, GUI under Xvfb 1600x1000 with
`GDK_BACKEND=x11`, default threading. The edit is 30 Up presses on the
Exposure compensation spin box, which produces the same debounced burst as a
slider drag without depending on the panel layout. The counts come from
`RT_PREVIEW_PROFILE=1`; one run per configuration, so the timings are context
rather than a benchmark.

- 10.2 MP PEF, neutral profile (`HistogramMatching=false`), final value
  +1.50 EV.
- 45.7 MP NEF (private sample, 8288x5520) with its sidecar, final value
  +1.86 EV. Sidecars and raw files stay under `build/test-scratch/` and are
  never tracked.

`GDK_BACKEND=x11` is required on a Wayland host: without it GDK connects to
the host compositor and the Xvfb window tree stays empty.

These runs predate the single Preview Refresh preference and used a fixed
250 ms settle with a 500 ms cap. The current default is 125 ms for both; the
tables are historical and record the previous coalescing behaviour. The
current model produces one analysis per period, as described above.

### 10.2 MP, fit

| Configuration | fast hits | analyses | fallbacks | samples | p50 | p95 |
|---|---:|---:|---:|---:|---:|---:|
| `RT_VIEWPORT_FAST=0` | 0 | 0 | 9 | 9 | 54 ms | 64 ms |
| `RT_VIEWPORT_DEFER_MS=0` | 5 | 5 | 4 | 14 | 24 ms | 93 ms |
| fixed settle 250 / cap 500 ms (pre-#24) | 5 | 2 | 4 | 11 | 20 ms | 69 ms |

### 45.7 MP, fit

| Configuration | fast hits | analyses | fallbacks | samples | p50 | p95 |
|---|---:|---:|---:|---:|---:|---:|
| `RT_VIEWPORT_FAST=0` | 0 | 0 | 10 | 10 | 151 ms | 206 ms |
| `RT_VIEWPORT_DEFER_MS=0` | 7 | 7 | 4 | 18 | 110 ms | 299 ms |
| fixed settle 250 / cap 500 ms (pre-#24) | 6 | 2 | 4 | 12 | 42 ms | 203 ms |

### 45.7 MP, 100 %

| Configuration | fast hits | analyses | fallbacks | samples | p50 | p95 |
|---|---:|---:|---:|---:|---:|---:|
| `RT_VIEWPORT_FAST=0` | 0 | 0 | 11 | 11 | 144 ms | 196 ms |
| `RT_VIEWPORT_DEFER_MS=0` | 7 | 7 | 4 | 18 | 74 ms | 219 ms |
| fixed settle 250 / cap 500 ms (pre-#24) | 6 | 2 | 4 | 12 | 92 ms | 204 ms |

A second default run at 100 % gave p50 63 ms / p95 291 ms; the p50 spread is
run noise, the 6 hits / 2 analyses ratio is stable.

- Served hits cost 12-19 ms at 10 MP, 37-40 ms at 45.7 MP fit and 57-61 ms at
  45.7 MP 100 %. The deferred analysis costs 41-43 ms at 10 MP, 104-105 ms at
  45.7 MP fit and 129-133 ms at 45.7 MP 100 %.
- The settle coalesces the burst into one analysis per window as designed; at
  45.7 MP fit it removes five of seven analysis passes.
- p95 includes the one-time first-edit `no-complete-full-state` pass and the
  zoom transition, so it is not the interaction latency; the hit counts and
  the per-kind figures are the evidence.
- AE = 0 for the full 1600x1000 frame at 10 MP (fit) between every pair of
  configurations, and for the 45.7 MP viewport at fit and 100 %.
- At 45.7 MP 100 % the navigator and histogram are not bit-identical between
  the fast and fallback paths: navigator MAE 0.0005, histogram panel MAE
  0.012. The viewport itself is AE = 0. The difference tracks the base cache
  (`cache=1` on the fallback, `cache=0` on the fast path's deferred pass) and
  is within the engine's OpenMP rounding variation seen on CLI exports; it is
  not a stale pass, because two runs of either configuration are identical.
- Fallbacks beyond the initial load are the first-edit
  `no-complete-full-state` pass and a monitor-only update.

### Debian package

`tools/build-rt.sh portable` built
`rawtherapee-5.13-157-gfbe080bdf_5.13+git13178.fbe080bdf-1_amd64.deb`,
SHA-256 `163ad2a19b7b736de04ced488ff3246733f7dfb26e5b03dd64c52863be661dc0`.

Debian 12 and Debian 13 containers: `apt-get install ./...deb` PASS,
`--list-formats` includes JPEG XL PASS, single-threaded 16-bit TIFF export
compared with the host single-threaded export AE = 0, GUI startup under Xvfb
PASS. The containers have no OpenCL ICD, so `--gpu-test` and
`--gpu-test-monitor` were not run there; GPU parity was checked on the host
with `verify.sh --quick` as above.

## Known limitations and follow-ups

- Promote `M_TRANSFORM` once ROI-transform parity is validated on the GUI.
  The crop path already accepts the bit, but the headless checks could not
  drive the Lens/Geometry sliders reliably, so the gate remains closed rather
  than accepted unmeasured.
- Add a cached `lhist16` so non-zero Lab contrast can use the fast path. The
  histogram is built from `oprevl` after `rgbProc()`, so it includes the tone
  curve; a cached copy is only exact for Lab-only edits and needs a
  `rgbProc()` refresh when the request also carries `M_RGBCURVE`.
- Handle active Fattal/dehaze/spot correctly rather than falling back.
- Validate crop ROI moves, panning/zooming cache reuse and detail windows on a
  wider image set, including OpenCL.
- Validate the spot-reset and base-refresh path and add an automated
  fast-on/off regression for it.
- Evaluate whether the deferred pass can skip `M_AUTOEXP` entirely when the
  corresponding auto flags are off.
- Progressive crop resolution during a drag is not implemented. It would
  multiply `CropHandler::getWindow()`'s skip while a gesture is active and
  must keep the colour picker and the edit-subscriber object map at full
  resolution.

## FAST preview toggle interaction

The per-tab FAST preview toggle and a 100% zoom crossing change the effective
`highDetailNeeded`. The coordinator invalidates the viewport state and queues
`ALL | M_MONITOR` when the effective full/fast result changes, so a cache entry
from the other mode is never reused. A no-op transition does not queue a
render. See `devnotes/perf/FAST_PREVIEW_TOGGLE.md`.
