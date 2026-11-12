# Snapshot comparison

Status: implemented on `issue-0-snapshot-compare`; GUI smoke checks and the
geometry unit test pass. Packaging and the full acceptance matrix are not yet
complete.

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
- The viewport is frozen while comparing: no pan, zoom, wheel, double-click or
  editing. To compare another area, release `\`, navigate, then press `\`
  again.
- The comparison keeps the live view's geometry (crop, coarse/common
  transform, straighten, distortion, perspective, lens profile) and takes the
  snapshot's non-geometry adjustments. The visible area therefore cannot
  shift.
- Editing controls and keyboard shortcuts are locked while comparing. The
  lock is released when the live preview is restored, or after a bounded
  timeout.

## Architecture

The implementation uses a parameter swap on the single main
`StagedImageProcessor`. It does not allocate a second `InitialImage`,
`PreviewHandler`, `ImageArea` or detail crop.

Key files:

- `rtgui/history.*`: Base snapshot, comparison selection, markers, guarded
  deletion, context menu.
- `rtgui/editorpanel.*`: live-state save/restore, parameter swap, frozen
  viewport, interaction lock, badge, key press/release.
- `rtgui/imagearea.*`: `interactionsEnabled` guard.
- `rtgui/cropwindow.*`: `showCropGuides` flag.
- `rtgui/windows/rtwindow.cc`, `editwindow.cc`: key-release routing, focus-out
  and tab-change restore.
- `rtengine/compareparams.*`: `makeComparisonParams` and `geometryDiffers`.
- `rtengine/rtengine.h`, `improccoordinator.h`: `hasTweakOperator`.

The live `ProcParams` and view anchor/zoom are saved on entry. The snapshot's
parameters are merged with the live geometry and applied with
`beginUpdateParams` / `endUpdateParams(EvProfileChanged)`. The GUI model,
history and sidecar are not notified. On release the saved live parameters are
applied and the saved view is restored.

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

`tools/build-rt.sh portable` produced:

```text
rawtherapee-5.13_5.13+git0.local-1_amd64.deb
SHA-256 71fcdaf6e8940b241f71b5522eb1e2c0d7de292df255c27060cf869f63b99635
```

The package installed and `rawtherapee-cli --list-formats` passed in the
Debian 12 build container and in a Debian 13 container. The Debian 13 check
needed `libhwy1` installed explicitly; the package's dependency metadata lists
it, so a normal `apt` install resolves it.

The GUI smoke test was run from the CPU build under Xvfb, not from the
installed package. GPU tests were not run in the containers.

## Known limitations

- Source-coordinate alignment is not implemented. Geometry-different
  snapshots use the live geometry, so the snapshot's own crop/rotation is not
  compared.
- The comparison is not instant; it performs a pipeline render.
- The comparison badge is drawn as an overlay and does not change the viewport
  allocation.
- A runtime language change does not rename an existing `Base` row.
- The full package build and Debian install checks have not been run yet.

## Verification commands

```bash
tools/build-rt.sh cpu -j 6
tools/build-rt.sh opencl -j 6
tools/perf/test-snapshot-compare-geometry.sh build/cpu-issue-0-snapshot-compare
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
| AC8 lock | PASS | Controls greyed; shortcuts consumed. |
| AC9 before/after exclusion | Not tested in this pass | Code restores compare first. |
| AC10 memory | PARTIAL | No second-source step; extra growth not fully attributed. |
| AC11 latency | PARTIAL | Measured; 10 MP target missed. |
| AC12 interruption | Not fully tested | Timeout and restore paths implemented. |
| AC13 multi-tab | Not tested | Tab-change restore implemented. |
| AC14 builds | PASS | CPU and OpenCL compile. |
| AC15 i18n/docs | PARTIAL | Strings and diffs generated; docs added. |
| AC16 no side effects | PASS | No history/sidecar write observed. |
