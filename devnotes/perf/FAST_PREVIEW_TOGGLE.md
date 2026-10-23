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
- A 100% crossing queues `ALL | M_MONITOR` only when it changes the effective
  demosaic decision returned by `needFullDemosaic()`, invalidating the viewport
  state and dirtying the preview base cache. The comparison uses the
  main-preview resolution flag on both sides of the crossing and the current
  FAST state. A crossing that leaves the decision unchanged, such as
  `As in PP3` with FAST off or `Fast` with FAST on, queues no base refresh; the
  viewport crop is still re-rendered for the new zoom.
- The guard also refreshes when the Preview Demosaic Method differs from the
  value the base was built with. `ImProcCoordinator` publishes that value
  whenever the preview base is rebuilt or restored from the base cache, so
  changing the preference with an image open cannot leave the wrong method on
  screen after a crossing. A preference change on its own still does not
  refresh the below-100% preview.
- Toggling FAST while the main preview is below 100% does not queue a render.
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
- Host verification on the OpenCL build, single-threaded:
  - `--quick`: PASS, `pef/neutral max_pae=1.5259e-05`; monitor self-test PASS,
    max 1 byte, 14 of 196608 samples differ.
  - `--formats`: PASS for `pef/bayer14`, `max_pae=1.5259e-05`; the remaining
    corpus entries were not available locally and are not counted as PASS.
  - `--rgbproc`: PASS for `pef/rgbproc-neutral` and
    `pef/rgbproc-exposure`, both `max_pae=1.5259e-05`.
  - `--list-coverage`: not PASS; the local test-data corpus is incomplete.
- Debian 12 package tests: install, `--list-formats` with JPEG XL, CLI export,
  GPU diagnostics and GUI startup under Xvfb PASS.
- Debian 13 package tests: the same install/CLI/GUI-startup smoke checks PASS.
- Interactive host Xvfb checks at 100% showed `method: fast` with FAST on and
  the full method with FAST off. The automated fit-zoom pointer interaction was
  unreliable under Xvfb without a window manager; the fit behaviour is covered
  by the truth-table test. Xephyr runs are not counted.
- Package artefact:
  `rawtherapee-5.13-143-g6ed4b80c2_5.13+git13164.6ed4b80c2-1_amd64.deb`
  SHA-256 `683134642cdaf1266aee0d39eace33786a76d4996858154b0e1592ecdea3d242`.
- Container runs are functional checks only; performance is measured on the
  host. No host performance timing is claimed for this change.

### 100% crossing refresh check (issue #21)

- `tools/perf/test-fast-preview.sh build/cpu-<branch>`: PASS for the crossing
  table covering no-op crossings, genuine transitions, a changed preference and
  an unknown base preference.
- GUI checks on the host under Xvfb at 1600x1000, with `RT_SETTINGS` and
  `RT_CACHE` in a project-local scratch directory, `RT_PREVIEW_PROFILE=1`,
  verbose output and `colorspace_flowers.pef` (10.2 MP):
  - `As in PP3`, FAST off: fit -> 100% -> fit queued no `preview update:` line;
    the base was already full on both sides.
  - `Fast`, FAST off: one full refresh per crossing; at 100% `method: amaze`
    (935 ms), back at fit `method: fast` (797 ms).
  - `As in PP3`, FAST on: one full refresh per crossing; at 100% `method: fast`
    (890 ms), back at fit `method: amaze` (1288 ms).
  - `Fast`, FAST on: no refresh in either direction.
  - Preference changed from `Fast` to `As in PP3` at fit, then 100%: the base
    was still `Fast`, so the crossing forced one refresh (1041 ms) and used
    `method: amaze`. Without the published-preference check the decision
    comparison alone would have skipped it.
- `tools/perf/test-preview-base-cache.sh`: PASS.
- Both OpenCL configurations compile with GCC and the generic target.
- OpenCL host verification, single-threaded: `--quick` PASS,
  `pef/neutral max_pae=1.5259e-05`; monitor self-test PASS, max 1 byte,
  14 of 196608 samples differ (AMD Radeon RX 550).
- The 45.7 MP NEF was not available locally; that sample is not covered.
- The times above are the coordinator's own `preview update:` durations on the
  host with default threading. They are recorded as context, not as a
  benchmark.
