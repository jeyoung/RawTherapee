# Viewport-aware preview fast path

The viewport fast path is enabled by default for safe post-base parameter
changes. `RT_VIEWPORT_FAST=0` disables it for comparison or troubleshooting.
It rebuilds the parent LUTs that visible `rtengine::Crop` objects consume from
the cached full-frame analysis state, renders the crop ROI first, and then
queues one normal full-frame pass so the navigator, preview image, `workimg`,
histograms, scopes and listeners catch up.

With the kill switch set to `0`, every request takes the existing full path
unchanged. Export is not involved: the fast path lives in
`ImProcCoordinator::updatePreviewImage()` and only affects the interactive
preview.

## Scope

This first implementation covers requests that cannot change the base image or
the full-frame analysis inputs:

- `M_AUTOEXP`, `M_RGBCURVE`, `M_LUMACURVE`, `M_LUMINANCE`, `M_COLOR`,
  `M_CROP` and `M_MINUPDATE`;
- `M_HDR` and `M_SPOT` may appear because several GUI events over-approximate
  with them, but only when the corresponding parameters are unchanged. Active
  Fattal, dehaze or spot removal is rejected, because the normal path would
  re-apply those filters.

The fast path never calls `ipf.rgbProc` on the full preview, `firstAnalysis()`,
locallab, CIE, Fattal/dehaze, spot removal or film-negative processing.

## Eligibility

`canUseViewportFastPath()` is conservative and compares the current
`ProcParams` with `viewportFullParams`, a copy of the request inputs of the
last completed full-frame pass. It rejects the request if any of the following
holds:

- the kill switch is set to `0`, no full pass completed, a deferred pass is
  pending, or the high-detail state changed or is still being computed;
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
4. Crops with a listener are updated immediately.
5. A deferred analysis todo of
   `M_MONITOR|M_RGBCURVE|M_LUMACURVE|M_LUMINANCE|M_COLOR` (plus the original
   downstream bits) is merged into `changeSinceLast` under
   `paramsUpdateMutex`, and `forceFullPreview` is set.
6. The normal `process()` loop runs that request through the full path. It
   updates `previmg`, `workimg`, histograms/scopes and listeners, re-renders
   the crops for correctness, and only then marks the viewport state valid
   again.

`forceFullPreview` is not cleared by stale-request bailouts. If a newer request
arrives, the normal bailout merges the old todo into `changeSinceLast`, so the
deferred analysis is either kept queued or coalesced with the newer request.
The deferred pass always includes `M_MONITOR` so it cannot be skipped by the
outer full-frame block condition. When the deferred request still matches the
request generation and the full parameter snapshot of the fast path, the full
pass suppresses the duplicate crop render; a newer request gets a normal crop
render.

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
preview update: 40 ms todo=0x1009f cache=0 viewport=fast crop_ms=39 deferred_todo=0x1009f
preview update: 92 ms todo=0x401f cache=0 viewport=deferred-analysis
preview update: 113 ms todo=0x1009f cache=0 viewport=fallback reason=switch-off
```

Fallback reasons include `no-complete-full-state`, `base-request`,
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

## Known limitations and follow-ups

- Add a cached `lhist16` so non-zero Lab contrast can use the fast path.
- Handle active Fattal/dehaze/spot correctly rather than falling back.
- Validate crop ROI moves, panning/zooming cache reuse and detail windows on a
  wider image set, including OpenCL.
- Validate the spot-reset and base-refresh path and add an automated
  fast-on/off regression for it.
- Evaluate whether the deferred pass can skip `M_AUTOEXP` entirely when the
  corresponding auto flags are off.

## FAST preview toggle interaction

The per-tab FAST preview toggle and a 100% zoom crossing change the effective
`highDetailNeeded`. The coordinator invalidates the viewport state and queues
`ALL | M_MONITOR` when the effective full/fast result changes, so a cache entry
from the other mode is never reused. A no-op transition does not queue a
render. See `devnotes/perf/FAST_PREVIEW_TOGGLE.md`.
