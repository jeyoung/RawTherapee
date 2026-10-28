# Graph validation matrix

Evidence record for the validating install, export, GUI and device-parity runs
of the integrated graph workstream. All rows were produced on 2026-10-28 from
commit `b4943e8f9`, which is the tip of `perf-graph` at the time of writing.

## Artefact

```
rawtherapee-graph-5.13-324-gb4943e8f9_5.13+git13345.b4943e8f9-1_amd64.deb
sha256 9c4d48286d51894d1ae8374bcbb318b415bf8e36c0efb2db54d068bdd55d3a40
```

Only the graph package is tested here. Two earlier package builds remain in
`build/portable` for comparison and are not part of this record.

The commits that follow `b4943e8f9` on this branch are documentation and
comment-only, so the binary in the artefact above is unchanged. A commit that
alters behaviour invalidates this table and needs a fresh package and a fresh
run.

## Environment

| Item | Value |
|---|---|
| Host CPU | Intel Core i7-3770, 4 cores / 8 threads |
| Host toolchain | GCC, Release, `PROC_TARGET_NUMBER=1` (generic), ccache |
| Host build trees | `build/cpu-perf-graph`, `build/opencl-perf-graph` |
| Container images | `debian:12` `sha256:94926f0fd968cd68ac486502185a5dd78687874a9ec7727f64fe2af696e28311`; `debian:13` `sha256:99339bdd341ab316eeae3dcf870b98bc7243d99269d01666b0f49a565c1399da` |
| Corpus | `$RT_TESTDATA/colorspace_flowers.pef` (10.2 MP, public) and four inputs under the gitignored `$RT_TESTDATA/local/` |

The private corpus stays outside the repository. This record names its inputs
only as `nef45`, `tif16` and `jpg8`; the copies are 45.7 MP class NEFs and a
16-bit TIFF and an 8-bit JPEG generated from one of them with the same CLI.
Timing was not measured, so the container runs are functional checks only.

## Install and export matrix

Both containers installed the package with `apt-get install ./pkg.deb`, which
resolves the dependencies; `dpkg -i` was not used. Debian 13 resolves the `t64`
renames (`libgtk-3-0t64`, `libgtkmm-3.0-1t64`) from the package's `libhwy1`,
`libgtk-3-0` and `libgtkmm-3.0-1v5` dependencies without a manual step.

| Distro | Install | `--list-formats` | nef45 | pef10 | tif16 | jpg8 |
|---|---|---|---|---|---|---|
| Debian 12 | PASS (exit 0) | 36 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 |
| Debian 13 | PASS (exit 0) | 36 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 |

Exports ran single-threaded (`-j1`) at 16-bit for TIFF and at the default
quality for JPEG, with the matching sidecar passed explicitly. The host
reference used `build/cpu-perf-graph/rtgui/rawtherapee-graph-cli` at the same
commit.

## Pixel comparison

TIFF rows compare the host source build with each container package. `maxLSB16`
is the largest absolute channel difference in 16-bit steps; `diffpixels` counts
pixels with any channel difference. JPEG rows compare decoded pixels, because
the encoder may emit different bytes for identical pixels.

| Input | Output | Pair | maxLSB | diffpixels | Verdict |
|---|---|---|---|---|---|
| nef45 | TIFF16 | host vs Debian 12 | 1 | 1 | PASS, within 1 LSB |
| nef45 | TIFF16 | host vs Debian 13 | 0 | 0 | PASS, exact |
| nef45 | TIFF16 | Debian 12 vs 13 | 1 | 1 | PASS, within 1 LSB |
| pef10 | TIFF16 | host vs Debian 12 | 1 | 2 | PASS, within 1 LSB |
| pef10 | TIFF16 | host vs Debian 13 | 1 | 1 | PASS, within 1 LSB |
| pef10 | TIFF16 | Debian 12 vs 13 | 1 | 1 | PASS, within 1 LSB |
| tif16 | TIFF16 | all three pairs | 0 | 0 | PASS, exact |
| jpg8 | TIFF16 | all three pairs | 0 | 0 | PASS, exact |
| all four | JPEG8 | all three pairs | 0 | 0 | PASS, decoded pixels identical |

Observations:

- The raw inputs differ by one 16-bit step in one or two pixels between the
  host and a container, and between the two containers. The non-raw inputs are
  exact. The difference is consistent with library and FMA variation between
  the host and the container images, not with the graph: `AE` is bounded at
  1 LSB16 and no pixel differs by more.
- The JPEG files differ in 2-3 bytes per file while their decoded pixels are
  identical. The differing bytes are encoder metadata; the expected libjpeg
  caveat does not apply to pixels here, and it is not recorded as a pass for
  byte identity.

## GUI checks

Xephyr on `:2` at 1600x1000, no window manager, GTK forced to X11 because the
session runs Wayland. The build binary ran from `build/cpu-perf-graph` with its
own `RT_SETTINGS` and `RT_CACHE` under `build/test-scratch/`.

| Input | Window | Preview | Crop | Thumbnail | Pan | 100% | Zoom in |
|---|---|---|---|---|---|---|---|
| nef45 | 1200x680, present | graph active, 13 of 14 nodes | graph active, 13 nodes, 2 folded | graph active | PASS (screenshot differs) | PASS | PASS |
| tif16 | 1200x680, present | graph active, 13 of 14 nodes | graph active, 13 nodes, 2 folded | not graph-logged | PASS | PASS | PASS |
| jpg8 | 1200x680, present | graph active, 13 of 14 nodes | graph active, 13 nodes, 2 folded | not graph-logged | PASS | PASS | PASS |

Every step produced a distinct screenshot and the process was alive after the
interactions; no `error`, `failed`, `critical`, assertion or abort line
appeared in the logs. Keys were delivered through XTEST with the pointer over
the image area: `XSendEvent` keystrokes were ignored when the window was
unmanaged, which is why the recipe does not use `xdotool key --window`.

The absence of the thumbnail graph line for the TIFF and JPEG inputs is the
known P1 gap: non-raw thumbnails and inspector mode are not graph-covered. It
is recorded as uncovered, not as a pass.

## Device parity

`stage-residency-test` from the `opencl-perf-graph` build, restricted to the
POCL ICD:

```
stage-residency: device=cpu-ivybridge ... platform=Portable Computing Language type=CPU (POCL CPU run)
stage-residency: lanczos3 scale 0.5: PAE 0.000000 LSB16, 0 of 9216 samples differ, tolerance 1.0 LSB16
stage-residency: lanczos3 scale 1.5: PAE 0.000000 LSB16, 0 of 82944 samples differ, tolerance 1.0 LSB16
stage-residency: run() parity: PAE 0.000000 LSB16, 0 of 4608 samples differ
stage-residency-test: PASS (0 skipped)
```

This is POCL CPU parity only and is labelled as such. It is not GPU evidence.

## Not run, and why

- `tools/opencl/verify.sh --quick`: the probe targets the earlier OpenCL
  backend, not the stage device, and `resize` is the only device node, so it
  would report that no stage executed. Recorded as not applicable rather than
  as a pass. Teaching the probe to observe the stage device belongs with block
  4 coverage.
- Container POCL parity: the package does not ship the test binaries and the
  stage device path is packaging-independent. The host POCL run above covers
  the kernel; the container runs cover installation and export.
- `--gpu-test` and `--gpu-test-monitor`: no GPU is available on this host.
- Multi-threaded export parity: export is non-deterministic in the baseline, so
  parity runs are single-threaded by design.

## Publication scan

Scope: the complete `origin/perf-graph..HEAD` range (83 commits at the time of
the scan), the working tree, every commit message, every author and committer
identity, the tracked test-data manifest and the provider-side content on
`origin` (24 issues, no pull requests, no releases).

| Check | Result |
|---|---|
| Private keys, cloud and forge tokens | none |
| Absolute home paths (`/home/`, `/Users/`, drive paths) | none |
| Private corpus names and hashes | none |
| IPv4 addresses | none |
| Email addresses in added lines | none |
| Authors and committers | `Eddy Young <jeyoung@users.noreply.github.com>` only |
| Commit messages | clean |
| Provider-side issues, PRs, releases | clean |
| `devnotes/opencl/testdata.manifest` | public rows only |

The private rows that were published earlier remain in the history before
`origin/perf-graph` (`cf40fbc36` added them, `dd031f371` removed them). That is
a recorded incident, not a new finding; per the publication policy the pushed
history is not rewritten and the current content is clean.

No push was performed in this session.

## Decisions recorded with this validation
- `RT_WORK_SHADOW` is kept as a diagnostic marker with no engine effect. It
  exists so the sweep harness can report the mode of a recorded run; the
  comments that implied it turned on a second authority were corrected.
- The mask-era vocabulary sweep covers `opencl/{DECISIONS,REVIEWS,STATUS}.md`,
  `perf/{FAST_PREVIEW_TOGGLE,PREVIEW,STAGE_PIPELINE,STAGE_RUNTIME}.md`. Quoted
  removed code and the retirement guard still name `M_*` deliberately; current
  rules do not.
