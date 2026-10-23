# FAST preview toggle

The editor top panel has a per-tab **FAST** toggle for the interactive preview.
It is a manual override: while it is on, the preview uses fast demosaicing at
every zoom level. While it is off, the preview follows the existing rules.

## Behaviour

| Main preview zoom | FAST | Preview demosaic |
|---|---|---|
| any | on | Fast, overriding the Preview Demosaic Method |
| 100% and above | off | Full method from the `.pp3` |
| below 100% | off | Preview Demosaic Method: `Fast` or `As in PP3` |

Detail windows share the main preview's base and therefore follow the same
choice. Export, the batch queue and the CLI are not affected; they always use
the full `.pp3` method.

The toggle is per editor tab and lasts for the application session. A new tab
starts off. Switching images within a tab keeps that tab's state; restarting
the application resets it. `RT_FAST_PREVIEW=0` is a global kill switch: it
disables the control and forces the off behaviour without changing the tab's
stored session state.

## Implementation

- `rtengine/previewquality.h` contains `needFullDemosaic()` and the kill-switch
  check.
- `ImProcCoordinator` stores the per-tab FAST state and an explicit
  `mainPreviewFullResolution` flag. Crop geometry is updated asynchronously, so
  the zoom state is recorded by `CropHandler::setZoom()` and
  `CropHandler::newImage()` rather than inferred from `Crop::skip`.
- `paramsReady` prevents a toggle from queuing work before the first real
  parameter update.
- An effective full/fast transition queues `ALL | M_MONITOR`, invalidates the
  viewport state and dirties the preview base cache. A no-op transition, such
  as turning FAST on below 100% while the preference is already `Fast`, does
  not queue a render.
- `M_HIGHQUAL` remains a detail-window crop refresh. It no longer selects the
  demosaic method.

## Verification

- `tools/perf/test-fast-preview.sh build/cpu-<branch>`: PASS for the truth
  table covering on/off, below/at 100%, and both preference values.
- Both OpenCL configurations compile with GCC and the generic target.
- GUI checks run under Xvfb, with `RT_SETTINGS` and `RT_CACHE` in a
  project-local scratch directory. Xephyr is not used.
  - at fit with `As in PP3`, FAST on produces `method: fast`;
  - FAST off produces the configured full method;
  - at 100%, FAST on produces `method: fast`;
  - at 100%, FAST off produces the configured full method.
- Export, package and host parity checks are recorded in the issue/handover.
  Container runs are functional checks only; performance is measured on the
  host.
