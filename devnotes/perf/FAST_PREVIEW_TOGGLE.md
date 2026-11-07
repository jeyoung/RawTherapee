# Fast preview toggle

The editor top panel has an icon-only **Fast** toggle for the interactive
preview. It is available only when **Preferences → Performance → Preview
Demosaic Method** is **Fast**. In **As in PP3** the control and its separator
are hidden and any active fast state is cleared.

## Behaviour

| Preview Demosaic Method | Main preview zoom | Fast toggle | Preview demosaic |
|---|---|---|---|
| Fast | below 100% | either | Fast |
| Fast | 100% and above | off | Full method from the `.pp3` |
| Fast | 100% and above | on (default) | Fast |
| As in PP3 | any | hidden | Full method from the `.pp3` |

The toggle is per editor tab and lasts for the application session. It starts
on when Fast is selected; the user can turn it off to render the full method at
100% and above. Switching images within a tab keeps that tab's state. Changing
the preference to Fast, or opening a new tab while Fast is selected, starts the
toggle on. Detail windows share the main preview's base and follow the same
choice. Export, the batch queue and the CLI always use the full `.pp3` method.

`RT_FAST_PREVIEW=0` is a global kill switch. It leaves the control insensitive
and forces the off behaviour without changing the stored tab state.

## Implementation

- `rtengine/previewquality.h` contains `needFullDemosaic()`, the 100% crossing
  decision and the kill-switch check.
- `ImProcCoordinator` stores the per-tab fast state and an explicit
  `mainPreviewFullResolution` flag. Crop geometry is updated asynchronously, so
  the zoom state is recorded by `CropHandler::setZoom()` and
  `CropHandler::newImage()` rather than inferred from `Crop::skip`.
- `paramsReady` prevents a toggle from queuing work before the first real
  parameter update.
- A 100% crossing queues `ALL | M_MONITOR` only when it changes the effective
  demosaic decision returned by `needFullDemosaic()`, invalidating the viewport
  state and dirtying the preview base cache. The comparison uses the main
  preview resolution flag on both sides of the crossing and the current fast
  state.
- The guard also refreshes when the Preview Demosaic Method differs from the
  value the base was built with. `ImProcCoordinator` publishes that value when
  the preview base is rebuilt or restored from the base cache.
- `EditorPanel::previewDemosaicMethodChanged()` owns the control visibility,
  the default-on transition and the deactivation when the preference leaves
  Fast. `RTWindow::showPreferences()` propagates the change to the current
  editor, every open editor tab and the multi-display editor window.
- The toggle is excluded from `show_all()` so its visibility can follow the
  preference. Its icon child is shown explicitly when the toggle becomes
  visible.
- `M_HIGHQUAL` remains a detail-window crop refresh. It does not select the
  demosaic method.

## Verification

- `tools/perf/test-fast-preview.sh build/cpu-<branch>`: PASS for the on/off,
  below/at-100% and both-preference truth table.
- Isolated Debian 13 Docker GUI checks with Xvfb at 1920x1200:
  - Fast selected: the icon-only toggle is visible and starts checked; the
    dark F and speed streaks remain legible against the checked background.
  - As in PP3 selected: the toggle and separator are hidden.
- Package validation is recorded in `devnotes/opencl/PORTABLE.md`.
