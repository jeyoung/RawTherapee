# Graph validation matrix

Evidence for the install, export, GUI and device-parity runs of the integrated
graph workstream. The export matrix and the device gate were measured at
commit `3a1a55559`. The GUI checks were measured at `b4943e8f9`; the only later
engine change is the opt-in stage-device precedence, which is inactive unless
`RT_STAGE_OPENCL` selects a node, so those rows still describe the default
path.

## Artefact

```
rawtherapee-graph-5.13-331-g3a1a55559_5.13+git13352.3a1a55559-1_amd64.deb
sha256 fd53e9d9239ed9c078d62a86f2093dcbd067506ab486c9fd3bc91c52a8362ffe
```

Built in the Debian 12 container from clean `3a1a55559`. Earlier packages in
`build/portable` are retained for comparison and are not part of this record.

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

## Determinism protocol

`-j1` alone does not make a 16-bit export reproducible: OpenMP still splits
work unless `OMP_NUM_THREADS=1` is exported. Measured on the PEF with the same
package and container:

- two Debian 12 runs without `OMP_NUM_THREADS` differed by 40 LSB on one pixel;
- one host-versus-Debian 12 pair differed by 754 LSB on 2 608 pixels;
- repeated runs with `OMP_NUM_THREADS=1` were identical, and the same
  host-versus-container pair became exact.

Every number below therefore uses `OMP_NUM_THREADS=1` and `-j1`. A comparison
taken without it measures thread scheduling, not the code.

## Install and export matrix

Both containers installed the package with `apt-get install ./pkg.deb`, which
resolves dependencies; `dpkg -i` was not used. Debian 13 resolves the `t64`
renames from the package's `libgtk-3-0`, `libgtkmm-3.0-1v5` and `libhwy1`
dependencies without a manual step.

| Distro | Install | `--list-formats` | nef45 | pef10 | tif16 | jpg8 |
|---|---|---|---|---|---|---|
| Debian 12 | PASS (exit 0) | 36 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 |
| Debian 13 | PASS (exit 0) | 36 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 | TIFF, JPEG exit 0 |

Exports ran at 16-bit for TIFF and at the default quality for JPEG, with the
matching sidecar passed explicitly. The host reference used
`build/cpu-perf-graph/rtgui/rawtherapee-graph-cli` at the same commit.

## Pixel comparison

TIFF rows compare the host source build with each container package; JPEG rows
compare decoded pixels, because the encoder may emit different bytes for
identical pixels. `maxLSB16` is the largest absolute channel difference in
16-bit steps; `diffpixels` counts pixels with any channel difference.

| Input | Output | Pairs compared | maxLSB | diffpixels | Verdict |
|---|---|---|---|---|---|
| nef45 | TIFF16 | host/12, host/13, 12/13 | 0 | 0 | PASS, exact |
| pef10 | TIFF16 | host/12, host/13, 12/13 | 0 | 0 | PASS, exact |
| tif16 | TIFF16 | host/12, host/13, 12/13 | 0 | 0 | PASS, exact |
| jpg8 | TIFF16 | host/12, host/13, 12/13 | 0 | 0 | PASS, exact |
| all four | JPEG8 | host/12, host/13, 12/13 | 0 | 0 | PASS, decoded pixels identical |

The JPEG files differ in 2-3 bytes per file while their decoded pixels are
identical; those bytes are encoder metadata. The earlier 1 LSB readings on the
raw inputs were OpenMP variance, not library or FMA difference: see
"Determinism protocol".

## GUI checks

Xephyr on `:2` at 1600x1000, no window manager, GTK forced to X11 because the
session runs Wayland. The build binary ran from `build/cpu-perf-graph` with its
own `RT_SETTINGS` and `RT_CACHE` under `build/test-scratch/`.

| Input | Window | Preview | Crop | Thumbnail | Pan | 100% | Zoom in |
|---|---|---|---|---|---|---|---|
| nef45 | 1200x680, present | graph active, 13 of 14 nodes | graph active, 13 nodes, 2 folded | graph active | PASS | PASS | PASS |
| tif16 | 1200x680, present | graph active, 13 of 14 nodes | graph active, 13 nodes, 2 folded | not graph-logged | PASS | PASS | PASS |
| jpg8 | 1200x680, present | graph active, 13 of 14 nodes | graph active, 13 nodes, 2 folded | not graph-logged | PASS | PASS | PASS |

Every step produced a distinct screenshot and the process was alive after the
interactions; no `error`, `failed`, `critical`, assertion or abort line
appeared. Keys were delivered through XTEST with the pointer over the image
area, because `XSendEvent` keystrokes were ignored when the window was
unmanaged.

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

## Stage-device gate

`tools/opencl/verify.sh --stage-device`, which `--quick` also runs, proves the
node ran on the device rather than only comparing pixels.

| Case | Result |
|---|---|
| POCL CPU selected, `RT_STAGE_OPENCL=1`, preference on | PASS, `max_pae=1.5259e-05` (1 LSB at 16-bit) |
| Device record | `"stage":"lanczos3","impl":"opencl","source":"opencl"`, and the classic log has no `opencl resize` line |
| Forced refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`, `RT_REQUIRE_GPU=1`) | FAIL with `impl":"host","source":"device-fallback"` |
| No device selected (no `RT_OPENCL_ALLOW_CPU`) | SKIP |
| `RT_STAGE_OPENCL` unset | classic backend logs `opencl resize`, no stage record; default path unchanged |

The forced refusal proves the gate can fail, so the PASS is not vacuous. The
device-versus-CPU difference on the export is 1 LSB on 96 426 of 2 534 980
pixels (3.8%), inside the limit; the residency test's synthetic images are
exact.

## Not run, and why

- Container POCL parity: the package does not ship the test binaries and the
  stage device path is packaging-independent. The host POCL run covers the
  kernel; the container runs cover installation and export.
- `--gpu-test` and `--gpu-test-monitor`: no GPU is available on this host.
- Multi-threaded export parity: export is non-deterministic without
  `OMP_NUM_THREADS=1`, so parity runs are single-threaded by design.

## Decisions recorded with this validation

- `RT_WORK_SHADOW` is kept as a diagnostic marker with no engine effect.
- The mask-era vocabulary sweep covers
  `opencl/{DECISIONS,REVIEWS,STATUS}.md` and
  `perf/{FAST_PREVIEW_TOGGLE,PREVIEW,STAGE_PIPELINE,STAGE_RUNTIME}.md`. Quoted
  removed code and the retirement guard still name `M_*` deliberately.
- The stage device runs before the classic backend when `RT_STAGE_OPENCL`
  selects the node; the default order is unchanged.

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

The private rows published before `origin/perf-graph` (`cf40fbc36` added them,
`dd031f371` removed them) remain a recorded incident. Per policy the pushed
history is not rewritten and the current content is clean.

Commits made after the scan (the precedence fix, the verifier mode and the
record updates) are not yet scanned; scan the then-current range again before
any push. No push was performed.
