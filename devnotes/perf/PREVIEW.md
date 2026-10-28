# Preview latency baseline

Preview timings are emitted when `RT_PREVIEW_PROFILE=1` is set. Each completed
render prints:

```text
preview update: <wall-ms> ms kind=<request-kind> request=<request-identity> cache=<0|1> [viewport=<shape> ...]
```

The timer starts after a stale queued request has been rejected, so dropped
requests are not included. Queued requests are coalesced by the preview
generation counter.

## Harness

```bash
tools/perf/bench-preview.sh /usr/local/bin/rawtherapee \
    /path/to/image.NEF 5
```

It starts a private Xvfb display, opens the Exposure tool, drags Exposure
Compensation, and prints warm p50/p95. See `tools/perf/README.md`. On a
Wayland host, export `GDK_BACKEND=x11` and unset `WAYLAND_DISPLAY`; otherwise
GDK connects to the host compositor and the Xvfb window tree stays empty.

## Manual procedure

1. Build the normal GUI with GCC and OpenCL enabled.
2. Start RawTherapee with `RT_PREVIEW_PROFILE=1` and open a 45 MP NEF.
3. Record the first fit-zoom render, then five consecutive edits of each kind:
   - a Tone Curve point;
   - Exposure;
   - a Lab luminance-curve point;
   - the output profile.
4. Repeat at 100% zoom.
5. Capture stderr and report p50/p95 and the `todo` masks.

Use the same image and profile for every run. Do not commit images or sidecars.

## Current status

45.7 MP NEF under Xvfb, Exposure Compensation slider, no OpenCL device:

| Run | Fit zoom | 100% zoom |
|---|---:|---:|
| Cold first render | 5323 ms | 5324 ms |
| Warm 1 | 152 ms | 152 ms |
| Warm 2 | 141 ms | 141 ms |
| Warm 3 | 53 ms | 51 ms |
| Warm 4 | 53 ms | 53 ms |
| Warm 5 | 54 ms | 54 ms |

Automated harness, cold Xvfb start: warm fit-zoom p50 = 54 ms; p95 = 152 ms.

## Interactive host baseline

45.7 MP NEF, real interactive session on the host, Exposure and
Shadows/Highlights edits:

| Sequence | Samples | p50 | p95 | Max |
|---|---:|---:|---:|---:|
| Cold initial render | 1 | 7392 ms | — | 7392 ms |
| Fit zoom, Exposure (`0x1009f`) | 17 | 252 ms | 274 ms | 274 ms |
| High-detail transition (`0x40000`) | 1 | 9001 ms | — | 9001 ms |
| 100% zoom, Shadows/Highlights (`0x7`) | 11 | 243 ms | 255 ms | 255 ms |
| 100% zoom, Exposure (`0x1009f`) | 24 | 335 ms | 373 ms | 3196 ms |
| Monitor-only update (`0x1000`) | 2 | 86.5 ms | 89 ms | 89 ms |

The 100% Exposure sequence contains one 3196 ms outlier; excluding it would
lower the p95, but the raw sequence is reported. The current interactive
target p95 <100 ms at fit and <250 ms at 100% is **not met**.

The timer starts after stale-request rejection; cold renders include initial
image loading and are reported separately.

The analysis pass is now deferred to the end of the interaction burst, so a
drag no longer pays one whole-frame pass per update (see
`devnotes/perf/VIEWPORT_FAST_PATH.md`, "Deferred analysis"). The table above
still counts each completed request, so its p50 includes both the viewport
crop and the analysis. The remaining interactive lever is progressive
lower-resolution crop rendering during the gesture, not another cache.

On a private 45.7 MP NEF the debounced burst went from seven analysis passes
to two at fit and at 100 %, and the viewport was pixel-identical to the
fallback; the served hit cost 37-40 ms at fit and 57-61 ms at 100 %. See
`VIEWPORT_FAST_PATH.md`, "Deferred analysis", for the full tables and the
navigator/histogram MAE note.


## Known defect: second image stuck at the rough stage

Single-tab, load an image, open a second from the file browser, then
double-click the image area to zoom. The second image stays at the first
progressive stage: a small pixelated block with the grid overlay and a `0%`
zoom indicator.

Attribution: **pre-existing upstream behaviour**, not from the typed-request
work. `build/baseline-dev/rtgui/rawtherapee` (dev, 2026-10-24, before this
branch) reproduces the same state from the same steps.

Trace from the branch build, with `RT_STAGE_PROFILE` counting real passes:

| Step | Device records | Trace |
| --- | --- | --- |
| first image loaded and zoomed | 20 | `setZoom old=200 new=200 enabled=1`, crop `0,0 8280x5512` |
| second image loaded | 21 | one `monitor.output` at `552x828`; no transform, no final pass |
| double-click on the second image | 21 | `setZoom old=200 new=500 enabled=0`, crop `0,0 5512x8280`, no `update()` |

Two findings to start from when it is fixed:

- the crop handler is `enabled=0` after the image switch, so
  `CropHandler::setZoom()` never calls `update()` and no render is requested;
- the fit zoom for the portrait image resolves to `500` (factor 0.02) where a
  fit in a 1200x680 window needs about `200`, which is why the image appears as
  a small block.

Do not patch from the symptom alone: the disabled flag and the wrong fit value
may share one stale input. Instrument `CropHandler::setEnabled`,
`CropWindow::zoomFit()`/`getFitZoom()` and the editor load path first.

## Initial-load reveal

Single-editor mode replaces the previous image immediately when a new raw
image is selected. The image area shows a neutral canvas using the current
preview background colour. No embedded JPEG is extracted and no "Loading
image..." text is drawn.

A small spinner appears after 150 ms if decoding is still running. It is a
low-contrast 24-28 px arc and has no text or percentage. The spinner is
removed as soon as the first real image is available.

The first real image is a deliberately rough raw render. It is produced at
twice the final preview skip, so its linear resolution is about half the final
fit preview. It uses the same decoded raw, white balance, profiles, tone curve
and monitor conversion as the final image. The rough pass bypasses
`PreviewBaseCache`; only the final pass may populate it.

The rough image is kept on screen while the normal fit-quality render is
requested. That final image replaces the rough image in place. There is no
crossfade and no intermediate three-stage blur-up, matching the existing rough
preview -> detailed-crop handoff. Editing remains disabled during decoding and
the rough preview, and is enabled only when the final image arrives.

The final pass must request the complete post-base preview pipeline (RGB
curves, Lab curves, luminance, colour and monitor output, plus the optional
HDR/spot/denoise stages), not just the base and monitor blocks. The preview
scale change reallocates the Lab buffers, so reducing the request to the base
and monitor stages leaves `oprevl` and `nprevl` empty. The navigator and the
100% panning fallback both use the resulting preview image and would show a
black frame until an unrelated edit rebuilds the pipeline. The final request is
the canonical full preview request and deliberately omits raw preprocessing and
the raw source; the rough pass has already decoded and preprocessed the source. The
handoff adopts the coordinator's actual scale after `setPreviewScale()`, so a
profile-induced scale normalisation is not compared with a stale request.

Regression check on the 45.7 MP NEF and 10 MP PEF: the navigator is populated
as soon as the final frame arrives, and dragging at 100% no longer turns the
image black. The parent package without the final-pipeline fix reproduced the
black navigator and black drag frame.

A new image with no remembered zoom/pan is fitted to the viewport using the
same path as the `f` shortcut. When `RememberZoomAndPan` is enabled and the new
image has the same dimensions as the previous image, the remembered zoom and
pan are kept instead. In that case the rough fitted frame is not shown, because
it would jump to the remembered view at handoff; the neutral canvas/spinner
stays until the final remembered-view frame.

Selecting another image during the sequence discards the old request. Load
errors clear the loading state on the GUI thread and use the existing error
dialog. Non-raw inputs keep their normal loading behaviour.

Manual CPU test with a 45.7 MP NEF selected from the file browser, cold cache,
`RT_VIEWPORT_FAST=0`:

- neutral canvas and spinner visible while the raw decoded
- no embedded JPEG and no loading text
- rough frame handed over after about 12.1 s
- final fit frame handed over about 54 ms later
- editing controls insensitive during decoding and rough preview, enabled at
  the final frame

A 10 MP PEF gave a rough frame after about 7.0 s and the final frame about
63 ms later. The long first interval is the existing raw decode and first
full pipeline; the two-stage reveal does not try to hide it.

## FAST preview toggle

The per-tab **FAST** control overrides the preview demosaic at every zoom.
With it off, below 100% the Preview Demosaic Method preference applies and at
100% and above the full `.pp3` method is used. See
`devnotes/perf/FAST_PREVIEW_TOGGLE.md` for the truth table and evidence.
