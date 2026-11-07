# Inspector

The Inspector previews the thumbnail under the pointer in the File Browser and
editor filmstrip. It has two hosts:

- the embedded **Inspect** tab in the File Browser right notebook
  (`InspectorWindow=false`);
- a separate fullscreen Inspector window (`InspectorWindow=true`).

## Display modes

The embedded tab is a 100 % loupe by default: it follows the hovered position
and shows the image at native resolution. The separate window already has
fit-to-window on `f`, 100 % on `z`/`F`, and scroll/drag zoom and pan.

The embedded tab has a persistent **Fit to window** checkbox at its top. When
it is on, the tab shows the whole hovered image, aspect-preserved and centred,
using the same no-upscale fit as the separate window. When it is off, the
100 % loupe returns. The separate Inspector window has no such checkbox and is
unchanged.

The checkbox state is stored as `[GUI] InspectorFitWindow` in the options file
and is restored on the next start. It is written when the options are saved on
a normal shutdown.

## Implementation

- `FilePanel` wraps the embedded `Inspector` in a vertical `Gtk::Box` with the
  checkbox; the separate-window mode adds the Inspector directly to its window.
- `Inspector::on_draw()` checks `!window && inspectorFitWindow` and takes the
  fit branch: `scale = min(1, window/image)`, the source offset is zero and the
  destination is centred. The loupe centre and `zoomScale` are ignored in that
  branch.
- No crop, thumbnail re-processing or new engine path is involved.

## Verification

Host CPU build under Xvfb 1600x1000 with fresh `RT_SETTINGS` and `RT_CACHE`,
using the public 10.2 MP PEF and a 25.9 MP X-Trans RAF:

- With the Inspect tab selected and the toggle off, hovering a thumbnail shows
  the 100 % loupe.
- Enabling **Fit to window** shows the whole image, aspect-preserved and
  centred. The 10.2 MP PEF occupied 368x247 of the 369 px-wide tab, matching
  its 3:2 aspect; there was no crop and no upscaling.
- Disabling the toggle restores the loupe.
- Hovering the second image with the toggle on fits that image.
- Quitting cleanly wrote `InspectorFitWindow=true`; after restart the checkbox
  was still selected and the fit view was active without any interaction
  beyond selecting the Inspect tab.
- `GDK_SCALE=2` on a 2400x1600 display kept the whole image fitted and centred.
- `InspectorWindow=true` produced the Filter, Batch Edit and Fast Export tabs
  with no Inspect tab and no checkbox. The separate Inspector window, opened
  through the **Inspect** context-menu entry, has no toggle and keeps its
  existing draw path.

The CPU and OpenCL configurations compiled, and
`tools/perf/test-fast-preview.sh` and `tools/perf/test-preview-base-cache.sh`
passed on the CPU build.

Package validation, including the Debian 12 Fit to window GUI check, is
recorded in `devnotes/opencl/PORTABLE.md`.
