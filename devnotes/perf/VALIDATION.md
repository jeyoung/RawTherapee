# Graph validation matrix

Evidence for the install, export, GUI and device-parity runs of the integrated
graph workstream. The export matrix and the device gate were measured at
commit `6fd009d74`. The full GUI table was measured at `b4943e8f9`; the later
engine changes add two opt-in device nodes (`resize`, `transform`), which are
inactive unless `RT_STAGE_OPENCL` selects a node, and the classic paths were
confirmed byte-identical to the pre-change package. A startup smoke at the tip
is recorded below.

## Artefact

```
rawtherapee-graph-5.13-365-g6fd009d74_5.13+git13386.6fd009d74-1_amd64.deb
sha256 d4337ee48ce86c2ac1e996bf82495c49b99dd7fb17973058afcb27e0639cdb3f
```

Built in the Debian 12 container from clean `6fd009d74`; the commit after it is
documentation only. Earlier packages in
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

At the validation commit a NEF was also opened with the tip build: the window
appeared at 1200x680, the preview and crop graphs ran, and the process stayed
alive under the same interactions.

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
stage-residency: gamut rec2020 238x24 (pitch 240): PAE 0.031250 LSB16, 9976 of 17136 samples differ
stage-residency: output.sharpen defaults 63x31: PAE 0.089844 LSB16, 542 of 1953 samples differ
stage-residency: output.sharpen threshold two values 63x31: PAE 0.824219 LSB16, 1841 of 1953 samples differ
stage-residency: output.sharpen padded width 238 238x24: PAE 0.097656 LSB16, 1633 of 5712 samples differ
stage-residency: output.sharpen skip sigma 63x31: PAE 0.000000 LSB16, 0 of 1953 samples differ
stage-residency: run() parity: PAE 0.000000 LSB16, 0 of 4608 samples differ
stage-residency-test: PASS (0 skipped)
```

This is POCL CPU parity only and is labelled as such. It is not GPU evidence.

## Stage-device gate

`tools/opencl/verify.sh --stage-device`, which `--quick` also runs, proves the
node ran on the device rather than only comparing pixels.

| Case | Result |
|---|---|
| Resize, POCL CPU, `RT_STAGE_OPENCL=1`, preference on | PASS, `max_pae=1.5259e-05` (1 LSB at 16-bit) |
| Transform (rotate 5 degrees) | PASS, `max_pae=1.5259e-05` |
| Transform identity, device | exact, and fails when `sint` is mutated to 0.05 |
| Output conversion (neutral) | PASS, `max_pae=1.5259e-05` |
| RGB tone stage (`rgbproc-exposure`) | PASS, `max_pae=1.5259e-05` |
| Framing (`framing.pp3`, 1080x725 frame) | PASS, `max_pae=1.5259e-05`; the record is `"stage":"framing".*"impl":"opencl"` |
| Gamut (`gamut.pp3`, gain 0.25 EV, Rec2020 compression) | PASS, `max_pae=1.5259e-05`; the record is `"stage":"gamut","impl":"opencl","source":"opencl"` |
| Output sharpening USM (`sharpen-usm.pp3`, 0.5 scale, radius 0.5) | PASS, `max_pae=1.5259e-05`; the record is `"stage":"output.sharpen","impl":"opencl","source":"opencl"` |
| Output sharpening RLD (`sharpen-rld.pp3`, default radius 0.45, 100 iterations, no damping) | PASS, `max_pae=1.5259e-05`; the record is `"stage":"output.sharpen","impl":"opencl","source":"opencl"` |
| Output sharpening damped RLD (`sharpen-rld-damped.pp3`) | PASS as a host residual: `"stage":"output.sharpen","impl":"host","source":"device-fallback"`, not counted as device parity |
| Output sharpening USM device test | 25 USM cases at widths 8 to 238 (including a padded 238 row), radius 0.3 to 3.0, contrast 0/20/100, amounts 1/100/200/1000 and threshold breakpoints; worst PAE 0.824219 LSB16, all finite; the flat-mask and skip cases are bit-exact |
| Output sharpening USM residuals | `halocontrol=true`, `blurradius=0.3`, `edgesonly=true` and `showMask=true` return `impl=host` through `stage::run()`; the first three are bit-identical to the CPU reference and the extracted core, and show-mask is bit-identical to the CPU mask and writes all samples |
| Output sharpening RLD device test | 15 cases at radius 0.3/0.45/0.75/1.0, iterations 1/5/30/100, deconvolution amounts 1/50/100/1000 and padded width 238; worst PAE 0.466797 LSB16, all finite; the zero-amount no-op is bit-exact |
| Output sharpening RLD residuals | `deconvdamping=50` and `deconvradius=1.5` return `impl=host` through `stage::run()` and are bit-identical to the CPU reference and the extracted core |
| Lab sharpening, no resize (`[Resize] Enabled=false`, `[Sharpening] Enabled=true Method=usm`) | PASS, `max_pae=1.5259e-05` (392 345 of 30 419 712 samples differ); the Lab call records `"stage":"output.sharpen","impl":"opencl","source":"opencl"` at 3888×2608 |
| Lab sharpening with `[SharpenEdge] Enabled=true` | PASS, `max_pae=1.5259e-05` (392 387 of 30 419 712 samples differ); ML sharpen edges stays host-side while the shared `output.sharpen` record is still `impl=opencl`, so the block is partial |
| Output sharpening packed-plane contract | padded row and padded plane descriptors are refused with `RT_STAGE_INVALID` on both the device and builtin paths, leaving all samples untouched |
| Output sharpening CPU refactor | USM and RLD exports with `--gpu=off`, `OMP_NUM_THREADS=1`: normalized 16-bit pixel hashes identical before and after |
| Working TRC default (`working-trc.pp3`, WorkingTRC=22, TrcExp), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05`; the record is `"stage":"working.trc","impl":"opencl","source":"opencl"` at 3888×2608 |
| Working TRC default, AMD RX 550 (rusticl) | PASS, `max_pae=1.5259e-05`; `"impl":"opencl"` |
| Working TRC `Gamut=false`, AMD RX 550 and POCL CPU | PASS, `max_pae=1.5259e-05`; `"impl":"opencl"` |
| Working TRC residual (`working-trc-residual.pp3`, soft light on) | PASS as a host residual: `"stage":"working.trc","impl":"host"`, `max_pae=1.5259e-05` informational |
| Working TRC device test | twelve cases at 238×24 and 179×17, gamut on/off, `wapsat` 0/0.5/1 and five TRC values; worst PAE 0.376953 LSB16, all finite; probe means bit-identical (`delta 0/0`); out-of-gamut, midtone-equaliser and soft-light residuals bit-identical to the CPU block with `impl=host` |
| Working TRC CPU path (`--gpu=off`, two runs) | identical, `PAE 0` |
| Lab tools (`labtools-sh.pp3`, shadows/highlights Lab mode), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05`; the record is `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608 |
| Lab tools (`labtools-sh-rgb.pp3`, RGB mode) | PASS, `max_pae=1.5259e-05`; `"impl":"opencl"` |
| Lab tools (`labtools-labgrid.pp3`, LabGrid only) | PASS, `max_pae=1.5259e-05`; `"impl":"opencl"` |
| Lab tools (`labtools-vibrance.pp3`, vibrance only), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05` (1 LSB16); `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608 |
| Lab tools vibrance on RX 550 (rusticl) | fail-closed: `"stage":"lab.tools","impl":"host","source":"device-fallback"`, no `impl=opencl`; the export matches `--gpu=off` at 1.5259e-05 |
| Lab tools vibrance device test | 21 cases at 125x160, 179x48, 238x64 and 8192x64: pastels/saturated combinations, negative saturated, thresholds 0/75/100 and split, protect-skins, avoidcolorshift on/off, a skin curve, highlight reconstruction, neutral and saturated images, padded widths, the 8192-wide case and the two-pixel Munsell-limit case; 2,570,352 samples, measured PAE 0 and asserted within 1 LSB16, finite, `impl=opencl` on POCL; all 197 Munsell tables (27,580 entries) equal the host LUTs and the correction changes 11,696 of 60,000 domain samples |
| Lab tools residual (`labtools-residual.pp3`, shadows/highlights + vibrance + local contrast) | PASS as a host residual: `"stage":"lab.tools","impl":"host","source":"device-fallback"`, not counted as device parity |
| Lab tools vibrance residual combinations (Lab curves, local contrast, EPD, Lab regions, black-and-white), a vibrance no-op and pastels 200 (out of range) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block |
| Lab tools device test | 23 cases at 238x64, 179x48 and 8192x64: LabGrid only, shadows/highlights Lab and RGB modes, both tools, single rounds, amounts 1/50/200/1000, tonal widths 0/100/split, radii 1/10/100 and padded widths; Lab mode bit-exact (PAE 0), RGB mode worst 0.23 LSB16, all finite; six residuals and two steep-table refusals are `impl=host` and bit-identical to the block; a forced launcher refusal is `impl=host` and bit-identical |
| Lab tools CPU refactor | LabGrid, Lab-mode and RGB-mode exports with `--gpu=off`, `RT_STAGE_OPENCL` unset: pixel data identical to the pre-change binary (`tiffcmp`), only the Software tag differs |
| Device records | `"stage":"lanczos3"`, `"stage":"transform"`, `"stage":"output.convert"`, `"stage":"gamut"`, `"stage":"framing"` and `"stage":"output.sharpen"`, all `impl":"opencl"`, `source":"opencl"`; the classic log has no `opencl resize`, `opencl transform` or `opencl lab2srgb` line |
| `monitor.output`, `viewport.output` in the GUI | device records with `impl=opencl`, `source=opencl`; preview image area identical to a CPU-only run (0 LSB8) |
| `thumbnail.output` via the file browser | 12 device records; thumbnail cache byte-identical to a CPU-only run |
| `thumbnail.transform` (float and U8) | sixteen rotation/flip combinations match the reference in both formats; not reached through the GUI harness |
| `thumbnail.whitebalance` via the file browser | device record present; the 45.7 MP NEF cache is byte-identical to a CPU-only run; the device test matches its reference exactly |
| Packed 8-bit Lab to RGB | byte-exact against an independent reference, and fails when the device matrix is scaled by 1.001 |
| Forced refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`, `RT_REQUIRE_GPU=1`) | FAIL, exit 1, with `"impl":"host","source":"device-fallback"` on the first refused node |
| No device selected (no `RT_OPENCL_ALLOW_CPU`) | SKIP |
| `RT_STAGE_OPENCL` unset | classic backend logs `opencl resize`, no stage record; default path unchanged, and at the gamut profile the export is byte-identical to the pre-change binary |

The forced refusal proves the gate can fail, so the PASS is not vacuous. A
targeted gamut refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`) leaves the gamut
record absent, so the gate predicate fails, while the export still succeeds
through the host block and is pixel-identical to the CPU-only export. A
targeted framing refusal with a 4900x4604 fixed frame and a 150 MB device
budget leaves the resize and output nodes on the device while the framing
record is absent; `drawFrame()` completes on the host and the framing gate
predicate fails. The device-versus-CPU difference is 1 LSB on 96 426 of
2 534 980 pixels (3.8%) for resize and on 384 601 of 10 139 424 pixels (3.8%)
for transform, both inside the limit. The residency test's synthetic images are exact, including the
identity transform, whose assertion was mutation-checked.

The lab.tools rows above were produced with `tools/opencl/verify.sh`, which
sets `RT_OPENCL_RGBPROC=1`. Without it, a bare
`rawtherapee-graph-cli --gpu=auto` export with `RT_STAGE_OPENCL=1` segfaults in
`opencl::initLocked()` through `ImProcFunctions::lab2rgbOut()` - a null call in
the classic backend's lazy OpenCL initialisation - for every profile, the
neutral default included. It reproduces at `03c94581c`, before the lab.tools
node existed, so it is a pre-existing classic-backend defect, not a lab.tools
defect; it is recorded as a P2 residual in `STAGE_RUNTIME.md`.

## Interactive nodes on the host

The GUI was run on the host with the OpenCL build, an explicit monitor profile
and the POCL-only ICD directory by absolute path. Two 45.7 MP runs captured the
same state (fit, then 100%): one with `RT_STAGE_OPENCL=1`, one with
`UseOpenCL=false`. Both the `monitor.output` and `viewport.output` records
appear only in the first, with `impl=opencl`, and the preview image area is
identical (`0` LSB8, 0 of 115 920 pixels differ).

The capture script is `build/test-scratch/interactive-parity.sh` (scratch, not
tracked). The three preconditions above are the whole recipe; without any one
of them the GUI silently uses the CPU path.

## Not run, and why

- Container POCL parity: the package does not ship the test binaries and the
  stage device path is packaging-independent. The host POCL run covers the
  kernel; the container runs cover installation and export.
- None outstanding for the Lab-to-RGB family: `thumbnail.output` is now reached
  as well. The file browser generates its thumbnails on the device and the
  cache is byte-identical to a CPU-only run.
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
- The transform node and the classic path share one map builder and one set of
  sampling helpers; the Lab-to-RGB family shares one arithmetic fragment with
  the classic kernel. The classic resize, rotate and neutral exports are
  byte-identical to the pre-change package (0 LSB), so the refactors changed no
  pixels.
- The lab.tools guided filter is a port, not an approximation: the subsampling,
  the box radius clamps, `boxblur()`'s running sums and the bilinear rescale and
  readout follow `guidedfilter.cc` and `boxblur.cc` operation for operation, so
  a Lab-mode round is bit-exact. RGB mode keeps single precision because the
  POCL device used for the gate exposes no `cl_khr_fp64`; the host's `xcbrtf()`
  is ported so the cube root matches, and the setup refuses a tone table whose
  steepest step could turn the closing round trip's one-ulp channel difference
  into more than one LSB16. That bound is crossed only above the 0..100 slider
  range.
- The shared kernel fragment is a namespace-scope constant, not a macro: GCC 12,
  the portable baseline, rejects a multi-line raw string inside a `#define` while
  the host compiler accepts it. The portable build found this; the host build
  did not.

## Publication scan

Scope: the complete `origin/perf-graph..HEAD` range at `8fdcb2fb3` (166
commits), the working tree, every commit message, every author and committer
identity, the tracked test-data manifest and the provider-side content on
`origin` (24 issues, no pull requests, no releases). The scan was run again on
the commit that records this result before the push.

| Check | Result |
|---|---|
| Private keys, cloud and forge tokens | none |
| Absolute home paths (`/home/`, `/Users/`, drive paths) | none |
| Private corpus names and hashes | none |
| IPv4 addresses | none |
| Email addresses in added lines | only the noreply identity recorded in this table |
| Authors and committers | `Eddy Young <jeyoung@users.noreply.github.com>` only, all 166 commits |
| Commit messages | clean |
| Tags and releases created by this range | none |
| Provider-side issues, PRs, releases | unchanged by this range |
| `devnotes/opencl/testdata.manifest` and `TESTDATA.md` | public rows only; private samples are gitignored and unlisted |
| Windows drive paths, machine identifiers, serials, phone numbers | none |

The private rows published before `origin/perf-graph` (`cf40fbc36` added them,
`dd031f371` removed them) remain a recorded incident. Per policy the pushed
history is not rewritten and the current content is clean.

The branch is fast-forward from `origin/perf-graph`; the push publishes this
range only. Push is now authorised for each stable, scanned range.
