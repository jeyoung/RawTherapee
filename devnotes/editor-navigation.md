# Editor selection and image reuse

## Behaviour

Selecting an image from the File Browser, the editor filmstrip, or the command
line ends in `FileCatalog::openRequested()`, which calls
`FilePanel::fileSelected()`. Before any load is started, `fileSelected()` asks
`RTWindow::selectEditorPanel()` whether an editor already shows the same file:

- Multi-tab mode: the `epanels` map resolves the path to the existing tab.
- Multi-display mode: `EditWindow` resolves the path to the existing editor
  window.
- Single-tab mode: one persistent `RTWindow::epanel` is reused when its
  `getFileName()` matches.

When an existing editor is selected, no `InitialImage::load()` is started. The
editor keeps its zoom, pan, history and in-memory processing parameters.

Before this change, only multi-tab and multi-display modes took the reuse path.
Single-tab mode always decoded the image again, even when the same `Thumbnail`
was already open in the persistent editor.

## Single-tab guards

Two conditions must hold before the persistent editor is reused:

1. **No load is pending.** `FilePanel::hasPendingLoads()` returns true while a
   load is queued or in flight. The pending load is the user's latest request;
   the current request must be queued behind it, so the last click wins. For
   example, if image B is loading and the user double-clicks the still-visible
   image A, A is queued after B and the editor ends on A.
2. **The profile is not cleared.** While `Thumbnail::isProfileCleared()` is
   true, the image is reloaded so `createProcParamsForUpdate()` can run the
   configured default or dynamic profile rule. This preserves the behaviour
   required by issue #14. Once the reload applies a profile, the flag is
   cleared and later selections reuse the editor.

The fast path releases the reference added by `FileCatalog::openRequested()`
exactly once, matching the existing multi-tab path. The open editor holds its
own reference, so the release cannot destroy the thumbnail.

Multi-tab and multi-display paths keep their previous behaviour, including
selecting an open image while another load is pending.

## Editor mode switching

Preferences applies the editor layout through
`Preferences::workflowUpdate()`, which calls `RTWindow::setEditorMode()`. The
layout fields are written to the live options before the switch, so
`isSingleTabMode()`, `createSetmEditor()` and pending image-load callbacks
agree on the target mode.

When switching from single-tab to multi-tab, the persistent editor is closed
while its widget hierarchy is alive:

- `EditorPanel::close()` stops processing, closes the crop handler and
  releases the thumbnail reference;
- the panel is then removed from the notebook;
- `~EditorPanel()` clears `toggleFastPreview` before calling `close()`,
  because GTK may already have destroyed that managed child;
- `close()` runs before `delete iareapanel`, so the image area and its crop
  handler are still valid;
- `ImageArea::setImProcCoordinator(nullptr)` calls `CropHandler::close()`
  to detach the processor and delete the crop before the processor is
  destroyed.

Without this ordering, pressing OK with an image open crashed in
`Gtk::Widget::set_sensitive()` from `EditorPanel::close()`. After that was
fixed, the next teardown step crashed in `CropHandler::~CropHandler()`.

## Known limits

Selecting the image that is already open does not re-read it from disk, so
external changes are not picked up. This matches multi-tab behaviour. Open a
different image first, or clear the processing profile, when a fresh load is
required.

## Verification

Environment: Debian 13 container with Xvfb 1600x1000x24 and openbox, CPU builds
`build/cpu-issue-19-single-tab-reuse` and
`build/cpu-issue-20-editor-mode-crash`, the public 10.1 MP
`colorspace_flowers.pef` sample, and project-local settings and cache
directories. `RT_LOAD_PROFILE=1` reports each `InitialImage::load()`.
Functional GUI checks ran in the container because host Xvfb is not installed;
they are functional checks, not host performance measurements. The CPU and
OpenCL builds (`tools/build-rt.sh cpu`, `tools/build-rt.sh opencl`) passed.

| Check | Result | Evidence |
|---|---|---|
| Baseline reproduction | FAIL as expected | Opening the image, returning to the File Browser and opening it again produced two `initial_image.load` lines (557.5 ms and 486.4 ms). |
| Single-tab reuse | PASS | The same sequence after the change produced one load (464.8 ms) and selected the editor. |
| State retention | PASS | The editor stayed at 100 % zoom and the same pan position (x=1886, y=372) across the round trip; no loading placeholder appeared. |
| Different image | PASS | Two images loaded once each; reopening the second one added no load and kept its editor. |
| Pending load ordering | PASS | With a slow private raw sample loading, reopening the current image queued it; the load order was current, slow, current, and the editor ended on the current image. |
| Multi-tab regression | PASS | Two images opened in two tabs; selecting the first tab again added no load. |
| Cleared profile | PASS | Clear Profile left one load; reopening reloaded (second load) and regenerated the default profile sidecar; reopening again added no load. |
| Reference stability | PASS | Twenty reuse cycles kept one load and the process remained alive. |
| Single-tab to multi-tab, image open | PASS | Pre-fix build crashed in `gtk_widget_set_sensitive` from `EditorPanel::close()`, then in `CropHandler::~CropHandler()` after the first guard. After the full fix, both the direct `setEditorMode(true)` call and the real `Preferences::okPressed` path returned, `TabbedEditor=true` was saved and the process stayed alive. |
| Single-tab to multi-tab, no image | PASS | The direct mode-switch call returned with the empty persistent editor. |
| Mode switch during loading | PASS | The direct switch returned while the image was loading. The layout fields are applied before removal, so the pending load completion enters the multi-tab branch instead of dereferencing the removed single-tab panel. |
| Debian 12 package | PASS | Fresh Debian 12 container installed the `.deb`, listed formats, exported the public sample and passed the same reuse check (one load, 641.7 ms). |
| Debian 13 package | PASS | Fresh Debian 13 container installed the `.deb`, listed formats, exported the public sample and passed the same reuse check (one load, 692.7 ms). |
| CLI export parity | PASS | Package exports from Debian 12 and 13 and the host CPU build, all with `OMP_NUM_THREADS=1`, compared at 16 bits: `max_abs=0`, `differing=0` for all three pairs. |
| Debian 12 package (#20) | PASS | Fresh Debian 12 container installed the #20 `.deb`, listed formats, exported the public sample with `OMP_NUM_THREADS=1`, and opened the image in the GUI. |
| Debian 13 package (#20) | PASS | Fresh Debian 13 container installed the #20 `.deb`, listed formats, exported the public sample with `OMP_NUM_THREADS=1`, and opened the image in the GUI. |
| Packaged mode switch | Not run | The Preferences switch was not scripted in the package containers. The crash fix was verified with the host build under gdb; the package smoke only opened the image. |

## Package verification

Package: `rawtherapee-5.13-147-ge648b8442_5.13+git13168.e648b8442-1_amd64.deb`
(26 MiB), built with `tools/build-rt.sh portable` from the clean tree at
`e648b8442` (behaviour commit `80f6cad6e`).

SHA-256:
`8d743c948cb12c653f337e2e051ac7cad30421cdb781791b030a09c4d6ea1aae`

Both package tests used fresh `debian:12` and `debian:13` containers with
Xvfb and openbox. GPU self-tests were not run because the containers have no
OpenCL device; they are recorded as not run, not as PASS.

### #20 package

Package: `rawtherapee-5.13-150-g79c27250c_5.13+git13171.79c27250c-1_amd64.deb`
(26 MiB), built from the clean tree at `79c27250c` (behaviour commit
`975545582`).

SHA-256:
`3bccddfb3331180b2b6115b61b2305f3523bd4414a03e5c31a555671c056539d`

Debian 12 and 13 installed the package, listed the raw formats, exported the
public sample with `OMP_NUM_THREADS=1` and opened the image in the GUI. The
two exports compare at 16 bits with `max_abs=0` and `differing=0`. The
packaged Preferences switch was not scripted; the switch itself was verified
on the host build under gdb.

## Change history

- Issue #19: reuse the open editor in single-tab mode.
- Issue #20: close the editor and crop handler safely when switching editor
  modes.
