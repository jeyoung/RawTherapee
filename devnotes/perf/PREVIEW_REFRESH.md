# Preview Refresh

The interactive preview uses one dynamic refresh period per editing session.
There is no user-visible preference: the engine measures the cost of the
current image and zoom and derives the period from it. The same period is used
for the adjuster debounce of the active tab and for the settle window and
max-wait cap of the deferred full-frame analysis.

## Model

`rtengine/previewrefresh.h` holds the shared constants and helpers:

| Property | Value |
|---|---|
| Range | 100 to 1000 ms |
| Fallback before a measurement | 125 ms |
| Smoothing | three-quarter weight on the previous period |

The range is wider than the old 200 ms preference ceiling because a heavy
image at 100 % can legitimately need more than 200 ms per complete refresh.
`previewRefreshClamp()` enforces the range, `previewRefreshResolved()` supplies
the 125 ms fallback, and `previewRefreshSmooth()` blends a new measurement into
the previous period.

## Measurement

Each `ImProcCoordinator` measures its own complete refresh:

- `lastCropMs` is the time the fast path spent rendering the visible crop;
- `lastAnalysisMs` is the wall time of the deferred `updatePreviewImage()`
  pass that rebuilt the full-frame analysis;
- the measured period is `previewRefreshClamp(lastCropMs + lastAnalysisMs)`.

`previewRefreshSmooth()` then blends the measurement into
`sessionRefreshMs`. A single noisy pass cannot swing the debounce or the
settle: the period moves by one quarter of the difference per completed
refresh. The first complete refresh seeds the average. Until then
`getSessionPreviewRefreshMs()` returns -1 and every consumer uses the 125 ms
fallback.

The measurements and the smoothed period reset to -1 on image switch, buffer
reallocation, scale change and viewport invalidation through
`invalidateViewportState()`. A `timingGeneration` counter invalidates an
in-flight deferred pass, so a pass that started before the switch cannot store
a duration from the previous image.

The period is per coordinator. A background tab keeps its own deferred
schedule; only the GUI adjuster debounce follows the active tab.

## Active-tab debounce

Every `Adjuster` registers itself with a file-static registry and applies the
period through `Adjuster::setSessionDelay()`, which clamps the value to the
dynamic range and applies it to every live adjuster whose delays do not already
match. The update uses `DelayedConnection::setDelayValues()` rather than
`setDelay()`, so a pending callback keeps its armed timeout and still delivers
the user's last edit.
`Adjuster::sessionDelayMs()` exposes the current value to widgets that schedule
their own timers. The static value starts at the 125 ms fallback. Matching
adjusters are skipped, so an update never disconnects a pending callback.

The registry is process-wide, so a session update also reaches adjusters in
other windows, such as the ICC profile creator. The value is a debounce hint,
not a processing parameter, and `setDelayValues()` preserves an armed timeout,
so the cross-window effect cannot drop an edit.

- `RTWindow::on_mainNB_switch_page()` applies the incoming panel's
  `getSessionPreviewRefreshMs()` (or the fallback) when the active page
  changes, and marks it with `EditorPanel::setActive()`.
- `EditWindow::on_mainNB_switch_page()` does the same for multi-display
  editors, so they follow their measured period rather than the fallback.
- `EditorPanel::onSessionPreviewReady()` runs on the preview-ready callback and
  applies the period again when the active panel's smoothed value has changed
  after a completed refresh. It returns immediately unless the panel is marked
  active by its hosting window.

`ThresholdAdjuster::selectorChanged()` and `LabGridArea` read
`Adjuster::sessionDelayMs()` per change instead of the options. Per-tool
`setDelay()` calls and the legacy `adjusterMinDelay`/`adjusterMaxDelay` options
values remain as an invisible fallback until the next active preview applies
the measured period. The Retinex gamut and slope adjusters use the same period
as every other tool.

## Deferred analysis

The viewport fast path serves the crop immediately and defers the full-frame
analysis. Both the settle window and the max-wait cap equal the measured
session period, snapshotted once per request, so the two values cannot diverge
mid-pass. Equal settle and cap make the deadline one period after the first hit
of a burst, so a continuous drag produces one analysis per period rather than
one per 250-500 ms window.

`RT_VIEWPORT_DEFER_MS` and `RT_VIEWPORT_DEFER_MAX_MS` still override the two
values independently and keep the wider 0 to 1000 ms range.
`RT_VIEWPORT_DEFER_MS=0` queues the analysis immediately. The effective
deadline is `min(settle, max-wait)`, so set both variables when an experiment
needs a settle longer than the current cap. The measured period keeps the
cadence at or above the actual refresh cost; the overrides are for experiments
and for restoring the older coalescing behaviour.

See `devnotes/perf/VIEWPORT_FAST_PATH.md` for the fast-path design and
`devnotes/perf/PREVIEW.md` for the latency baseline.

## Profile log

With `RT_PREVIEW_PROFILE=1`, each fast-path line includes the session period
for that request:

```
preview update: <total> ms todo=<mask> cache=<0|1> viewport=fast \
  crop_ms=<crop> deferred_todo=<mask> deferred_hits=<n> \
  session_refresh_ms=<period>
```

The deferred catch-up line also reports `session_refresh_ms`. The value is the
smoothed session period at the start of the request, after the 125 ms fallback
is applied. It is not the effective settle when `RT_VIEWPORT_DEFER_MS` or
`RT_VIEWPORT_DEFER_MAX_MS` overrides one of the timer values: the environment
overrides change the timers, while `session_refresh_ms` keeps reporting the
measured period.

## Residual lag

The deferred pass publishes the crop and then the histogram after the
remaining analysis. The residual histogram lag is the analysis time minus the
crop time: the catch-up now runs at the next preview tick, and only the
analysis-minus-crop difference remains. The old fixed 250 ms settle with a
500 ms cap added a 250 to 500 ms timer mismatch on top of that difference.

The residual is inherent to running the full-frame analysis after the crop;
moving the analysis earlier brings back one full pass per update. Reducing it
is the progressive lower-resolution crop work listed in
`VIEWPORT_FAST_PATH.md`, not another timer.

## Verification

`tools/perf/fast-preview-test.cc` checks the clamp boundaries (50 maps to 100;
1200 maps to 1000; 100, 125 and 1000 are unchanged), the 125 ms fallback and
the smoothing helper (seed, fixed point, quarter-step, clamped measurement).

Host GUI check on an otherwise idle host, fit zoom, Neutral profile:

- Preferences, Performance shows no Preview Refresh control, slider, marks or
  recommendation label.
- 10.2 MP PEF: the first fast hit used the 125 ms fallback, then the session
  settled at 132-145 ms across complete refreshes.
- 25.9 MP X-Trans RAF: switching images reset the period to 125 ms, then it
  settled at 112-130 ms.
- Both images reported `session_refresh_ms` on the fast and deferred lines.

This was a functional check, not a benchmark. The local corpus did not include
the 45.7 MP NEF, so its fit and 100 % checks remain uncovered.

Validation still required:

- the 45.7 MP NEF at fit and 100 %;
- a Debian package build and installation check (filename and SHA-256 to be
  recorded) with `--list-formats`, a single-threaded 16-bit TIFF export
  comparison against the host and a GUI startup under Xvfb.

CLI, batch and export paths do not read the session period, so their output is
unchanged.
