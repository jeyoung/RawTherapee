# Snapshot comparison

Status: implemented on `issue-0-snapshot-compare`; the teardown crash and
interaction changes were fixed on `issue-43-snapshot-compare-fix`. GUI smoke
checks, the geometry unit test and the preview base-cache test pass.

## Behaviour

- An un-deletable `Base` snapshot is created when an image is loaded. It holds
  the image as loaded, including the sidecar or default profile.
- `Base` is the default comparison snapshot. `Ctrl+click` a user snapshot to
  make it the comparison snapshot; `Ctrl+click` `Base` restores the default.
- Deleting the marked comparison snapshot restores `Base` first. Deleting
  `Base` is refused by an insensitive control, an insensitive context-menu
  item and a guarded deletion path.
- Hold `\` to show the comparison snapshot. Release `\` to return to the live
  edit.
- The viewport is frozen while comparing: no pan, zoom, wheel or
  double-click. To compare another area, release `\`, navigate, then press
  `\` again.
- The comparison keeps the live view's geometry (crop, coarse/common
  transform, straighten, distortion, perspective, lens profile) and takes the
  snapshot's non-geometry adjustments. The visible area therefore cannot
  shift.
- Tool panels, the toolbar and the history panel stay enabled and visually
  unchanged during comparison. There is no sensitivity change to draw the eye
  away from the image.
- Any real parameter edit, any non-`\` shortcut, or a mouse action such as
  Save As, Queue or Send to external editor ends the comparison and then
  performs the action. Releasing `\` without an interaction still restores the
  live view. Auto-repeat of the held `\` cannot re-enter the comparison after
  an edit ended it.
- Closing an editor tab while comparing is safe, including with the tab close
  button, `Ctrl+W`, and releasing `\` after the tab has gone. Teardown never
  starts a processing pass.

## Architecture

The implementation uses a parameter swap on the single main
`StagedImageProcessor`. It does not allocate a second `InitialImage`,
`PreviewHandler`, `ImageArea` or detail crop.

Key files:

- `rtgui/history.*`: Base snapshot, comparison selection, markers, guarded
  deletion, context menu.
- `rtgui/editorpanel.*`: live-state save/restore, parameter swap, frozen
  viewport, badge, key press/release, comparison teardown.
- `rtgui/toolpanelcoord.*`: `beforeParamChange`, emitted before a parameter
  edit reaches the image processor.
- `rtgui/imagearea.*`: `interactionsEnabled` guard.
- `rtgui/cropwindow.*`: `showCropGuides` flag.
- `rtgui/windows/rtwindow.cc`, `editwindow.cc`: key-release routing, focus-out
  and tab-change restore, `prepareForRemoval()` before page removal.
- `rtengine/compareparams.*`: `makeComparisonParams` and `geometryDiffers`.
- `rtengine/rtengine.h`, `improccoordinator.h`: `hasTweakOperator`.

The live `ProcParams` and view anchor/zoom are saved on entry. The snapshot's
parameters are merged with the live geometry and applied with
`beginUpdateParams` / `endUpdateParams(EvProfileChanged)`. The GUI model,
history and sidecar are not notified. On release the saved live parameters are
applied and the saved view is restored.

`ToolPanelCoordinator` emits `beforeParamChange` immediately before
`panelChanged`, `refreshPreview` and `profileChange` write parameters. The
editor connects it to `finishCompareForEdit()`, which returns the processor to
the live parameters, unlocks the viewport and hides the badge before the edit
is applied. The edit is therefore never merged onto the comparison
parameters, and `profileChange` merges a partial profile onto live rather than
comparison state.

## Teardown

Removing an editor tab calls `EditorPanel::prepareForRemoval()` before the
page is detached. It cancels any comparison, disconnects the History
comparison signal and the restore timeout, and leaves the callbacks inert.
The destructor also disconnects the restore timeout and keeps the callbacks
inert. It does not disconnect the History connection again: that connection
is disconnected while the widget is alive and otherwise dies with the History
widget, and disconnecting it after GTK has destroyed the widget would raise a
`g_signal_handler_disconnect` critical.

`cancelCompareForClose()` must not apply parameters during teardown: doing so
starts a processing pass while the tool panels are being destroyed. If a
comparison is active, it sets `liveParamsForSave`, so `saveProfile()` and
`refreshProcessingState()` read the saved live parameters instead of the
processor's comparison parameters. The thumbnail and profile state are still
written correctly, without a render.

The original crash was a `SIGSEGV` in
`BayerProcess::FrameCountChanged()`'s idle callback (`sigc::slot_base::block`)
after a comparison-time parameter apply ran during panel destruction. Closing
the tab while holding `\` and reopening the same image reproduced it. The
teardown rule above removes the processing pass and the use-after-free.

## Evidence

Environment: CPU build, GCC 14.2.0, 8 physical cores / 16 threads, Xvfb
1600x1000, 10.2 MP PEF sample, default profile.

| Check | Result |
|---|---|
| Base snapshot created | PASS: one `Base` row, non-editable, marked as comparison. |
| Base deletion | PASS: delete control insensitive, context item insensitive, row remains. |
| Ctrl+click user snapshot | PASS: row not selected, comparison marker moves, badge names it. |
| Hold/release `\` | PASS: snapshot shown, live restored, no comparison history row. |
| Frozen viewport | PASS: before/after drag and wheel during hold were pixel-identical (central viewport crop, AE=0). |
| Live view restore | PASS: live before/after a compare cycle were pixel-identical (central viewport crop, AE=0). |
| Panels unchanged | PASS: right panel, left panel and toolbar crops were pixel-identical between comparison and live (`AE=0`). |
| Edit ends comparison | PASS: dragging Exposure compensation while holding `\` hid the badge immediately, added the History row, and kept the edit after releasing `\`. |
| Non-`\` key | PASS: `z` while holding `\` ended the comparison and zoomed to 100% with the live edit retained. |
| Auto-repeat suppression | PASS: the badge did not return while `\` stayed held after an edit ended the comparison. |
| Close while comparing | PASS: 10 cycles (5 tab close button, 5 `Ctrl+W`) with hold, close, release and reopen of the same image; no crash and no new coredump. |
| Delete marked snapshot | PASS: only `Base` remains and becomes the comparison. |
| Context menu | PASS: visible for a row; delete disabled for `Base`. |
| Geometry merge unit test | PASS: `snapshot compare geometry test: PASS`. |
| CPU and OpenCL builds | PASS: both compile. |

The parity check against applying the snapshot normally was `AE=0` in the
central image area. A tooltip from the delete button caused a non-zero diff at
the bottom-left of one capture; that was a test artefact, not an image
difference.

## Measurements

`RT_COMPARE_PROFILE=1`, 10.2 MP PEF, fit zoom:

| Event | Time |
|---|---:|
| Snapshot ready | 364.5 ms |
| Live ready | 346.0 ms |
| Repeat snapshot ready | 355.8–363.7 ms |
| Repeat live ready | 342.8–347.2 ms |

RSS (process `VmRSS`, approximate):

| Point | RSS |
|---|---:|
| After image load | 668 MB |
| After first compare and snapshot creation | 731 MB |
| After four compare cycles | 770 MB |

There is no second-source-sized step. The extra growth is not fully attributed;
it includes the saved parameter copies, preview-base-cache activity and
allocator growth. The 10 MP fit target of 250 ms was not met; the measured
value is recorded rather than relaxed.

## Review fixes

- `refreshProcessingState(false)` writes the saved live parameters while a
  comparison is active, so a completed comparison render cannot store the
  snapshot parameters on the thumbnail/cache.
- The geometry test uses explicit failure checks instead of `assert()`, which
  is compiled out in release builds.
- The snapshot name cell is explicitly committed on edit; the custom column no
  longer relies on the removed `append_column_editable` helper.
- `ImageArea::setScrollPosition` and `setZoom` ignore normal calls while
  interactions are disabled; the live-view restore bypasses the guard
  explicitly.
- The `\` binding does not test AltGr, because on Windows `GDK_MOD2_MASK` is
  also NumLock.

## Packaging

`tools/build-rt.sh portable` was run from a clean clone of commit `46de4f3de`,
so the Git-derived version is available inside the container. A linked
worktree cannot be used: its `.git` file points outside the mounted source
directory, so the container cannot read the version. The package is stored in
the clone under `build/portable/`:

```text
rawtherapee-5.13-184-g46de4f3de_5.13+git13205.46de4f3de-1_amd64.deb
Version 5.13+git13205.46de4f3de-1
SHA-256 6d5cd5c2e88f40febe8f3ca915134b57ff1ea609ba4c37ea5ba902a85e35ca95
```

The package installed and `rawtherapee-cli --list-formats` passed in a
`debian:12` and a `debian:13` container. A single-threaded 16-bit TIFF export
with `devnotes/opencl/pp3/rgbproc-exposure.pp3` was byte-identical to the
single-threaded host export except for the embedded version string and
timestamp; the pixel data and file size matched exactly.

The GUI smoke test was run from the installed package on Debian 12 under
Xvfb: the image loaded, hold and release of `\` worked, and the application
quit cleanly. The multi-tab close/reopen regression was run on the host CPU
build. GPU tests were not run in the containers.

## Known limitations

- Source-coordinate alignment is not implemented. Geometry-different
  snapshots use the live geometry, so the snapshot's own crop/rotation is not
  compared.
- The comparison is not instant; it performs a pipeline render.
- The comparison badge is drawn as an overlay and does not change the viewport
  allocation.
- A runtime language change does not rename an existing `Base` row.

## Verification commands

```bash
tools/build-rt.sh cpu -j 6
tools/build-rt.sh opencl -j 6
tools/perf/test-snapshot-compare-geometry.sh build/cpu-<branch>
tools/perf/test-preview-base-cache.sh build/cpu-<branch>
```

For GUI checks, use Xvfb with `GDK_BACKEND=x11`, a scratch `RT_SETTINGS` and
`RT_CACHE`, and `RT_COMPARE_PROFILE=1`.

## Acceptance criteria status

| AC | Status | Evidence |
|---|---|---|
| AC1 Base/selection | PASS | GUI checks and source review. |
| AC2 lifecycle | PASS | Delete marked/restore Base and Base deletion refusal. |
| AC3 hold-to-show | PASS | GUI hold/release. |
| AC4 no second pipeline | PASS | Code review; no `InitialImage::load` on swap. |
| AC5 parity | PASS for identical geometry | Central image `AE=0`. |
| AC6 live state | PASS | Pixel-identical live before/after; no history row. |
| AC7 frozen viewport | PASS | Drag/wheel during hold produced `AE=0`. |
| AC8 interaction | PASS | Panels stay enabled and unchanged; an edit or non-`\` key ends comparison and acts. |
| AC9 before/after exclusion | Not tested in this pass | Code restores compare first. |
| AC10 memory | PARTIAL | No second-source step; extra growth not fully attributed. |
| AC11 latency | PARTIAL | Measured; 10 MP target missed. |
| AC12 interruption | Not fully tested | Timeout and restore paths implemented. |
| AC13 multi-tab | PASS | 10 close/reopen cycles while comparing, including `Ctrl+W`; no crash. |
| AC14 builds | PASS | CPU and OpenCL compile. |
| AC15 i18n/docs | PARTIAL | Strings and diffs generated; docs added. |
| AC16 no side effects | PASS | No history/sidecar write observed. |
