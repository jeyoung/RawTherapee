# Viewport-aware preview fast path

`RT_VIEWPORT_FAST=1` enables an experimental, default-off fast path for safe
post-base parameter changes. It rebuilds the parent LUTs that visible
`rtengine::Crop` objects consume from the cached full-frame analysis state,
renders the crop ROI first, and then queues one normal full-frame pass so the
navigator, preview image, `workimg`, histograms, scopes and listeners catch up.

Without the environment variable every request takes the existing full path
unchanged. Export is not involved: the fast path lives in
`ImProcCoordinator::updatePreviewImage()` and only affects the interactive
preview.

## Scope

The first commit covers requests that cannot change the base image or the
full-frame analysis inputs:

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

- `RT_VIEWPORT_FAST` is off, no full pass completed, a deferred pass is
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
- an auto flag changes.

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
   `M_CROP|M_TRANSFORM|M_RGBCURVE|M_LUMACURVE|M_LUMINANCE|M_COLOR`;
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

## Invalidation

The snapshot is invalidated on image switch (`assign()`), `freeAll()`,
destruction, `setScale()` dimension or scale changes, and at the start of every
full-frame pass. It becomes valid again only after a full pass completes.

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

Xvfb is not installed on this host. A nested Xephyr server (`:98`) was used
instead; no host packages were installed. With the 10 MP PEF:

- fast-path hits took 40 ms for the visible crop, followed by a 92 ms deferred
  analysis; the existing path took 113 ms;
- auto levels produced `viewport=fallback reason=auto-exposure`;
- a sidecar with Lab contrast 20 produced `reason=lab-contrast`;
- final main-window screenshots after the same Exposure Compensation sequence
  were pixel-identical with `RT_VIEWPORT_FAST` on and off
  (`compare -metric AE` = 0);
- CLI exports with the switch on and off were pixel-identical
  (`compare -metric AE` = 0; only the TIFF `DateTime` tag differed);
- known user-modified files were left unstaged.

## Follow-ups before enabling by default

- Add a cached `lhist16` so non-zero Lab contrast can use the fast path.
- Handle active Fattal/dehaze/spot correctly rather than falling back.
- Validate crop ROI moves, 100% detail windows and auto B&W/colour-toning on a
  wider image set, including OpenCL.
- Consider suppressing the deferred crop re-render once the fast crop has been
  proven exact for its request class.
- Evaluate whether the deferred pass can skip `M_AUTOEXP` entirely when the
  corresponding auto flags are off.
