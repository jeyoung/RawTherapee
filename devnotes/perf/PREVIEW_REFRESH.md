# Preview Refresh

The Preview Refresh preference gives the interactive preview one debounce
period instead of the legacy adjuster minimum/maximum pair. It lives in
Preferences, Performance, and applies to every adjuster without a restart.

## Setting

| Property | Value |
|---|---|
| Range | 100 to 200 ms |
| Default | 125 ms |
| Orientation | Slow (200 ms) on the left, fast (100 ms) on the right |

The value is stored as `Settings::previewRefreshMs` and written to the options
file as `Performance/PreviewRefreshMs`. `Options::adjusterMinDelay` and
`Options::adjusterMaxDelay` are kept in sync with it, so the legacy keys are
still written for older builds. `Options::setDefaults()` reproduces the same
value, so Reset to default returns the slider to 125 ms.

A file without `PreviewRefreshMs` migrates the slower of the legacy
`AdjusterMinDelay`/`AdjusterMaxDelay` pair when either key exists. The value is
clamped to the supported range on read. The old defaults were a 100 ms minimum
and a 200 ms maximum; the single period replaces both.

## Adjuster debounce

Every `Adjuster` registers itself with a file-static registry and applies the
period through `Adjuster::setGlobalDelay()`. `Preferences::okPressed()` and
`Preferences::onResetToDefaultClicked()` call it after copying the options, so
a change reaches every live adjuster immediately. `ThresholdAdjuster` reads
the preference on each change because it schedules its own timer rather than
using the delayed connection. Per-tool `setDelay()` calls still run at
construction with the synced options; the global period is authoritative
afterwards. The Retinex gamut and slope adjusters no longer use a 2.5x
multiplier, so they follow the same period as every other tool.

## Deferred analysis

The viewport fast path serves the crop immediately and defers the full-frame
analysis. Both the settle window and the max-wait cap equal
`Settings::previewRefreshMs`; the value is taken once per request, so a live
change applies to the next hit. `RT_VIEWPORT_DEFER_MS` and
`RT_VIEWPORT_DEFER_MAX_MS` still override the two values independently and keep
the wider 0 to 1000 ms range. `RT_VIEWPORT_DEFER_MS=0` queues the analysis
immediately. The effective deadline is `min(settle, max-wait)`, so set both
variables when an experiment needs a settle longer than the current cap.

Equal settle and cap deliberately trade the old burst coalescing for cadence
alignment. The deadline is always one period after the first hit of a burst,
so a continuous drag produces one analysis per period rather than one per
250-500 ms window. The analysis can therefore run close to continuously on a
large image; the measured recommendation keeps the period at or above the
actual refresh cost. Restore a longer settle and cap with
`RT_VIEWPORT_DEFER_MS` and `RT_VIEWPORT_DEFER_MAX_MS` when coalescing matters
more than cadence. See
`devnotes/perf/VIEWPORT_FAST_PATH.md` for the fast-path design and
`devnotes/perf/PREVIEW.md` for the latency baseline.

## Recommended value

The engine measures the last complete refresh and shows the result under the
slider as `Recommended: N ms`, or `Recommended: —` when no complete refresh has
been measured yet:

- `lastCropMs` is the time the fast path spent rendering the visible crop;
- `lastAnalysisMs` is the wall time of the deferred `updatePreviewImage()`
  pass that rebuilt the full-frame analysis;
- the recommendation is `previewRefreshClamp(lastCropMs + lastAnalysisMs)`.

Both measurements reset to -1 on image switch, buffer reallocation, scale
change and viewport invalidation, so the label never reports a value from a
different image or zoom.

The recommendation is a guide, not an automatic setting. It reports the
measured cost of one complete refresh (crop plus analysis) on the current
image and zoom, clamped to the slider range. It is not stored with the
preference.

## Residual lag

The deferred pass re-renders the crop first and publishes the histogram after
the remaining analysis, so the residual histogram lag is the analysis time
minus the crop time. The old fixed 250 ms settle with a 500 ms cap added a
250 to 500 ms timer mismatch on top:

| Image and zoom | Crop | Analysis | Residual |
|---|---:|---:|---:|
| 10.2 MP, fit | 12-19 ms | 41-43 ms | about 23 ms |
| 45.7 MP, fit | 37-40 ms | 104-105 ms | about 65 ms |
| 45.7 MP, 100 % | 57-61 ms | 129-133 ms | about 70 ms |

The residual is inherent to running the full-frame analysis after the crop;
moving the analysis earlier brings back one full pass per update. Reducing it
is the progressive lower-resolution crop work listed in
`VIEWPORT_FAST_PATH.md`, not another timer.

## Verification

`tools/perf/fast-preview-test.cc` checks the clamp boundaries (50 and 250 map
to 100 and 200; 100, 125 and 200 are unchanged).

The host GUI check on the 10.2 MP PEF with the Neutral profile and highlight
reconstruction measured a 56 ms crop and a 99 ms deferred analysis, and the
Preferences label showed `Recommended: 155 ms`. The same dialog stored 200 ms
at the Slow end, 100 ms at the Fast end, and returned to 125 ms after Reset to
default. The three options values were read back from
`Performance/PreviewRefreshMs` and the legacy keys. Persistence, reset and live application are GUI-level
checks; the CLI and batch paths do not read the preference and their output is
unchanged.
