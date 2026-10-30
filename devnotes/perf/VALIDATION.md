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
The same run also covers `lab.tools`: the LabGrid, shadows/highlights and
vibrance rows keep their measured parity, the Lab-curve rows add 21
supported cases (2,347,344 samples, every sample bit-exact, PAE 0) plus seven
residuals that run `impl=host` at PAE 0, and the local-contrast rows add 23
supported cases (2,461,164 samples, every sample bit-exact, PAE 0) plus nine
residuals that run `impl=host` at PAE 0. The region rows add 27 supported
cases (2,741,040 samples, worst 0.500000 LSB16, none over 1 LSB16) plus seven
residuals and one unresolvable-table refusal at PAE 0; the three combination
cases fail at 128.45/464.08/415.08 LSB16 before the start-of-regions snapshot
fix. The Lab-curve detail, including the host-built LUT
provenance, the precompute tail and the fp64 program, the local-contrast
detail, including the host-built IIR coefficients and both blur paths, and the
region detail, including the host-sampled mask tables, the start-of-regions
Lab snapshot and the double working matrices, are in `STAGE_RUNTIME.md`.

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
| Lab tools (`labtools-labcurve.pp3`, L/a/b/C=f(C)/L=f(C)/C=f(L) with MUN), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05` (1 LSB16); `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608. The host builds all six tables with `CurveFactory`, and the pass runs in the fp64 `lab.tools.labcurve` program |
| Lab tools (`labtools-localcontrast.pp3`, radius 60, amount 0.3, darkness 0.9, lightness 1.1), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05` (1 LSB16); `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608. Radius 60 is above the sigma-25 split, so the row runs the fp64 `lab.tools.localcontrast` double recurrence |
| Lab tools (`labtools-regions.pp3`, two regions), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05` (1 LSB16); `"stage":"lab.tools","impl":"opencl","source":"opencl"` at 3888x2608. Region 1 has curved hue/chroma/lightness masks, CDL slope 1.1/offset 0.02/power 0.9 and saturation 40; region 2 has open masks, power 1.2 and a red channel copy, so the row exercises both mask radii and both CDL and channel paths |
| Lab tools vibrance on RX 550 (rusticl) | the gate prints `SKIP` for `pef/stage-device-lab-tools-vibrance` with `the row is CPU-device-only; the selected device is not a CPU OpenCL device`; the row stops before `compare_images` and prints no PAE. A manual `--gpu=auto` export records `"stage":"lab.tools","impl":"host","source":"device-fallback"` and is not counted as device parity. A manual `--gpu=off RT_STAGE_OPENCL=1` export is pixel-identical to `--gpu=off` (AE 0); only the Software tag differs |
| Lab tools vibrance on RX 550 with `RT_REQUIRE_GPU=1` | the vibrance row FAILs intentionally with the same CPU-device-only reason; POCL is the authoritative gate |
| Lab tools vibrance device test | 21 cases at 125x160, 179x48, 238x64, 8192x64 and 64x16: pastels/saturated combinations, negative saturated, thresholds 0/75/100 and split, protect-skins, avoidcolorshift on/off, a skin curve, highlight reconstruction, neutral and saturated images, padded widths, the 8192-wide case and the two-pixel Munsell-limit case; 2,570,352 samples, measured PAE 0 and asserted within 1 LSB16, finite, `impl=opencl` on POCL; all 197 Munsell tables (27,580 entries) equal the host LUTs and the correction changes 11,696 of 60,000 domain samples |
| Lab tools residual (`labtools-residual.pp3`, shadows/highlights + vibrance + local contrast) | PASS as a host residual: `"stage":"lab.tools","impl":"host","source":"device-fallback"`, not counted as device parity |
| Lab wavelet (`labwavelet.pp3`, equaliser in after-CBDL position), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05` (1 LSB16); the record is `"stage":"lab.wavelet","impl":"opencl","source":"opencl"` at 3888x2608 |
| Lab wavelet device test | 14 cases at 179x48, 238x64 and 8192x64: six levels, finest only, three levels, threshold 0 and 1, skin protection +60 and -60, gamutlab with and without skin, padded 179/238, a saturated hue circle and 8192-wide plain and skin cases; 3,574,560 samples, all finite and `impl=opencl` on POCL, worst 0.115234 LSB16 and 0 samples over 1 LSB16. Skin protection changes 574 of 45,696 fixture samples (PAE 108.908203 LSB16) and the gamutlab pre-pass changes 29,542 (PAE 20,453.597931), so the comparison is not vacuous |
| Lab wavelet contrast (`labwavelet-contrast.pp3`, wavelet contrast only, equaliser off), POCL CPU, `RT_STAGE_OPENCL=1` | PASS, `max_pae=1.5259e-05` (1 LSB16); the record is `"stage":"lab.wavelet","impl":"opencl","source":"opencl"` at 3888x2608 |
| Lab wavelet contrast device test | 14 cases at 179x48, 179x49, 179x160, 238x64 and 8192x64: transform only (all contrast multipliers zero), contrast with and without HS, negative multipliers, sigma 0.5 / offset 1.4, strength 60, padded 179x49, levels 1/2/3/7, the 8192-wide case, the equaliser plus wavelet in one submission, and two trailing zero multipliers with `Thrend=5`; 2,068,233 samples, measured PAE 0 and asserted bit-exact (0 differing samples), finite and `impl=opencl` on POCL. The `Thrend` case diverged by PAE 70.428223 LSB16 (10,061 samples over 1 LSB16) before the restore was mirrored. The contrast path changes 14,651 of 45,696 fixture samples against the identity (PAE 3,933.345703 LSB16), so the comparison is not vacuous |
| Lab wavelet contrast residuals (an export panel other than contrast, another level or tile method, soft radius, skin protection, avoid-colour-shift, the blue-sky median, `ContExtra`, another Daubechies length, an unknown HS method, a non-identity chroma-by-hue curve, sigma/offset/lowthr/gate/strength/Thrend/`c[]` out of range, a zero finest multiplier, zero levels, nothing enabled) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block (30 x 45,696 samples) |
| Lab wavelet residuals (wavelet enabled without contrast, `cbdlMethod=bef`, multiplier 5, threshold 2, skin 200, an all-neutral pyramid, a closed CIE-CAM gate) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block (7 x 25,776 samples) |
| Lab wavelet default path (`RT_STAGE_OPENCL` unset, no node override), equaliser and contrast wavelet | `run()` resolves to the built-in, records `impl=builtin`, and is byte-identical to `labWaveletHostBlock`; the call site takes the same extracted host block when the node is not selected |
| Lab wavelet on a GPU | not run; the launcher requires `CL_DEVICE_TYPE_CPU` (and fp64 for the wavelet) and refuses before any upload, so a GPU submission records `impl=host`. The device test's GPU residual is skipped on POCL and runs when a non-CPU device is selected |
| Lab wavelet final sources, OpenCL-enabled build (`build/opencl-perf-graph`) | PASS: `rth-cli` and `stage-residency-test` compile with GCC, generic target, no warnings |
| Lab wavelet final sources, OpenCL-disabled build (`build/cpu-perf-graph`) | PASS: `rth-cli` and `stage-residency-test` compile with GCC, generic target, no warnings; the CPU-only test prints `OpenCL unavailable (built without OpenCL)` and reports `PASS (24 skipped)` |
| Lab tools vibrance residual combinations (Lab contrast, local contrast with an out-of-range amount, EPD, a Lab region mask display, black-and-white), a vibrance no-op and pastels 200 (out of range) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block |
| Lab tools Lab-curve device test | 21 cases at 238x64, 179x48 and 8192x64: L only, brightness, an all-identity pass, a only, a/b, C=f(C), L=f(C), C=f(L), all six combined, MUN and NONE, three red/skin protection settings, `lcredsk` off, neutral and saturated images and padded widths; 2,347,344 samples, all bit-exact (PAE 0, 0 differing samples), all finite and `impl=opencl` on POCL; the MUN correction and red/skin protection each change the fixture, so the comparison is not vacuous |
| Lab tools Lab-curve residuals (contrast 15, chromaticity 25, a C=f(H)/L=f(H)/H=f(H) flat curve, the XYZ Munsell method, black-and-white output) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block |
| Lab tools local-contrast device test | 23 cases at 238x64, 179x48, 179x50, 238x63 and 8192x64: radii 1/10/40/80/100 (both blur paths), amounts 0/0.1/1/2, darkness and lightness 0.5/1/2 and the two-way split, padded 179/238, an odd height of 50 (the horizontal tail rows), the 8192-wide case, sigma 24 on the integer fixture and on three wide non-integer fixtures (both tails), and combinations with the Lab curves, shadows/highlights and vibrance; 2,461,164 samples, all bit-exact (PAE 0, 0 differing samples), all finite and `impl=opencl` on POCL; radius 10, amount 1 changes 8,592 of 25,776 fixture samples, so the comparison is not vacuous |
| Lab tools local-contrast residuals (radius 0/300, amount 3/-0.1, darkness 4, lightness -0.5, a NaN amount, EPD, a non-zero Lab contrast) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block (231,984 samples) |
| Lab tools local-contrast tail mutation | with the kernel's boundary deviation kept in double - the pre-fix code - the sigma-24 wide amount-2 case at 179x50 moves 7 of 26,850 samples by up to 0.164062 LSB16 (26,843 exact); the float-deviation kernel is bit-exact on all four sigma-24 cases |
| Lab tools region device test | 27 cases at 238x64, 179x48 and 8192x64: one/two/four regions, all masks open, no masks, hue/chroma/lightness-only and two-/three-point curved masks, CDL slope/offset/power/all, saturation +80/-60, channels all/red/green/blue, mask blur 0/-2/5, regions after shadows/highlights, after the Lab curves and after vibrance, padded 179/238 and the 8192-wide case; 2,741,040 samples, `impl=opencl` on every case, no NaN/Inf, worst 0.500000 LSB16 and 0 samples over 1 LSB16; the single-region and open-mask cases are bit-exact and the single-region case changes the block's output, so the comparison is not vacuous |
| Lab tools region combinations | shadows/highlights + regions, Lab curves + regions and vibrance + regions are within 0.003906, 0.500000 and 0.003906 LSB16 (0 samples over 1). Before the start-of-regions snapshot fix they were 128.453125, 464.082031 and 415.078125 LSB16, with 42,410, 44,510 and 44,447 of 45,696 samples over 1 |
| Lab tools region residuals (mask display, five regions, an `FCT_Unchanged` curve, slope 0.05, channel 3, saturation 200, blur -20) | refused, `impl=host` with no `impl=opencl`, PAE 0 against the block (7 x 25,776 samples) |
| Lab tools region table refusal | a 2000-node zigzag curve is inside the predicate but outside the table's 5e-7 budget; the setup refuses it, `impl=host`, no `impl=opencl`, PAE 0 |
| Lab tools shared helper change | `rt_lt_cache_lookup` now clips above at `LUTf`'s `size-2` bound and `rt_lt_compute_f/_fy` use the double negative branches of `computeXYZ2Lab()`. Affected paths: the RGB-mode shadows/highlights blend and the region apply kernel. Reverted helpers gave 24,437/13,637/23,643/16,950/14,188/24,613 differing RGB SH samples (PAE 0.062500/0.046875/0.076172/0.044922/0.045898/0.228516 LSB16); the fix gives 24,237/13,522/23,485/16,648/14,056/24,508 at the same PAEs except 0.216797 for amount 100. The real-PEF RGB SH export is identical old vs new (AE 0) and 425,762 samples differ from the host by 1 LSB in both. Fidelity improvement, no regression |
| OpenCL-disabled build | `build/cpu-perf-graph` compiles `rtengine` and `stage-residency-test`; the test reports `built without OpenCL` and passes with 21 skipped, so the host-only tree stays green |
| Lab tools local-contrast host capability | a host built without `__SSE2__`/`RT_SIMDE` refuses local contrast below sigma 25 before upload; the compile-time branch cannot be reached in these builds, so it is documented, not measured |
| Lab tools device test | 23 cases at 238x64, 179x48 and 8192x64: LabGrid only, shadows/highlights Lab and RGB modes, both tools, single rounds, amounts 1/50/200/1000, tonal widths 0/100/split, radii 1/10/100 and padded widths; Lab mode bit-exact (PAE 0), RGB mode worst 0.216797 LSB16 (amount 100; was 0.228516 before the cache-helper fix), all finite; six residuals and two steep-table refusals are `impl=host` and bit-identical to the block; a forced launcher refusal is `impl=host` and bit-identical |
| Lab tools CPU refactor | LabGrid, Lab-mode and RGB-mode exports with `--gpu=off`, `RT_STAGE_OPENCL` unset: pixel data identical to the pre-change binary (`tiffcmp`), only the Software tag differs. The `labtools-labcurve.pp3` export was checked the same way in this change (ImageMagick AE 0). The `labtools-localcontrast.pp3` and a Lab-curve + local-contrast profile are pixel-identical to the binary built at `e2e7011e5` (AE 0 and `tiffcmp` identical). With `OMP_NUM_THREADS=1`, `labtools-regions.pp3` is pixel-identical to the binary built at `e41b62b16` (AE 0 and PAE 0); without that serialisation the pre-existing parallel export varies by about 2,800 of 30.4M samples between two runs of either binary |
| Device records | `"stage":"lanczos3"`, `"stage":"transform"`, `"stage":"output.convert"`, `"stage":"gamut"`, `"stage":"framing"` and `"stage":"output.sharpen"`, all `impl":"opencl"`, `source":"opencl"`; the classic log has no `opencl resize`, `opencl transform` or `opencl lab2srgb` line |
| `monitor.output`, `viewport.output` in the GUI | device records with `impl=opencl`, `source=opencl`; preview image area identical to a CPU-only run (0 LSB8) |
| `thumbnail.output` via the file browser | 12 device records; thumbnail cache byte-identical to a CPU-only run |
| `thumbnail.transform` (float and U8) | sixteen rotation/flip combinations match the reference in both formats; not reached through the GUI harness |
| `thumbnail.whitebalance` via the file browser | device record present; the 45.7 MP NEF cache is byte-identical to a CPU-only run; the device test matches its reference exactly |
| Packed 8-bit Lab to RGB | byte-exact against an independent reference, and fails when the device matrix is scaled by 1.001 |
| Forced refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`, `RT_REQUIRE_GPU=1`) | FAIL, exit 1, with `"impl":"host","source":"device-fallback"` on the first refused node |
| No device selected (no `RT_OPENCL_ALLOW_CPU`) | SKIP |
| `RT_STAGE_OPENCL` unset | classic backend logs `opencl resize`, no stage record; default path unchanged, and at the gamut profile the export is byte-identical to the pre-change binary |

### Local adjustments

| Case | Result |
|---|---|
| Single-spot parity, 16 cases | POCL CPU: 2,219,001 samples, `impl=opencl`, finite, worst 0.031250 LSB16 |
| Complex parity, 72 cases | two/three ELI, reversed order, ELI and RECT, inactive/active both orders, each L/C/H curve alone, L=f(H) alone, all curves, one MUNS spot, MUNS at 179x49, MUNS and NONE, inactive then MUNS, all curves with MUNS, the gate multi-spot profile, and the vibrance/local-contrast set: vibrance alone, with a skin curve, skin-only (a host no-op), MUNS, padded 179x49 and 8192x64; local contrast alone, deltaE off, full strength, the SSE sigma-10 path, MUNS, padded 179x49 and 8192x64; the soft-light set: alone at 238x64, 179x49 and 8192x64, RECT, MUNS, strength 3, colour+soft, SH+soft, soft+local contrast, all five tools and a second spot; colour+vibrance, colour+local contrast, all three, all three with MUNS, three tools across three spots and an inactive spot carrying tool flags, plus the 20-case CBDL set below: `impl=opencl` throughout, worst 0.030762 LSB16 on the vibrance/local-contrast cases (0.053711 on the soft-light second spot; 0.125000 overall from the all-curves case). Soft light is non-vacuous: 22,897 of 45,696 samples change at PAE 3206.698242 LSB16 against the same spot with soft light off |
| Shadows/highlights parity, 12 cases | alone, highlight-only, shadow-only, RECT, MUNS, 179x49, 8192x64, colour+SH, vibrance+SH, SH+local contrast, all four tools and a second SH spot: 2,056,137 samples, `impl=opencl`, finite, worst 0.053711 LSB16 (second spot; all four tools 0.031250), every case within 1 LSB16. Non-vacuity: the tool changes 17,023 of 45,696 samples (PAE 362.355469 LSB16) against the same spot with the tool off |
| Clamp parity, 6 cases | inactive spots at 238x64 (prefilter and tail only), 237x49 (prefilter, tail 1), 179x49 (both) and 8192x64 (prefilter): 1,751,721 samples, `impl=opencl`, bit-exact (PAE 0, 0 differing samples). The pre-fix clamp failed the same cases at up to 0.045898 LSB16 |
| Spot order | the three-spot forward and reversed blocks differ on 27,040 of 45,696 samples (PAE 270.833984 LSB16) |
| CBDL parity, 24 cases | alone, luma-only, chroma-only, the scope-disabled pair, RECT, MUNS, 179x96, 8192x96, the 179x49/238x64/8192x64 no-op widths, colour+CBDL, vibrance+CBDL, CBDL+SH, CBDL+soft, CBDL+local contrast, all six tools, a second spot, an inactive-then-CBDL spot, the `chromacbdl=0.01` default-multiplier/default-clarity case and three `balan` cases: `impl=opencl`, finite, worst 0.068359 LSB16; the three sub-65-px rows are the block's no-op and bit-exact. Non-vacuity: CBDL alone changes 34,170 of 68,544 samples at PAE 2,887.008667 LSB16 against the same spot with the tool off |
| CBDL chroma regressions | `chromacbdl=0.01`, default multipliers and clarity: pre-fix 16,178.705 LSB16 on 26,091 of 68,544 samples, post-fix 0.022949 (341 samples). The same spot with `balan=2.5`: pre-fix 12,771.615 LSB16 on 26,140 samples, post-fix 0.029297 (549 samples). A fully active `balan=2.5` block is 0.023438. The pre-fix device skipped the chroma block when its derived multipliers trimmed to all-ones, so it missed the senstype-7 blend of the luminance result |
| Residual parity, 39 cases | mask, soft radius, exposure, `reparcol` 50/1, highlight reconstruction, recursion, `exc`, merge, LAB, XYZ, `avoidrad`, RGB curve, GHS clamp leftovers, vibrance gamma/gradient/warm/pastels range, the local-contrast wavelet and empty methods, amount/radius/darkness ranges, `reparw` 50 and a non-zero `strwav`, the soft-light `"reti"` method, explicit mask, soft radius, zero strength, strength 101 and sensitivity 101, and CBDL soft radius, `contresid`, mask, multiplier/chroma/threshold/clarity/sensitivity ranges: `impl=host`, no `impl=opencl`, PAE 0. The SH methods, tone-eq, recovery, gradient and range residuals are covered by the shadows/highlights record |
| Empty local-contrast method before/after | the pre-fix predicate admitted `localcontMethod == ""` and recorded `impl=opencl`; the host's uninitialised `lp.locmet` gave a different arm, and the case diverged by **10,216.751953 LSB16** on 22,508 of 45,696 samples. Only `"loc"` is admitted now; empty and `"wav"` are host residuals at PAE 0 |
| GHS clamp leftovers before/after | before the fix, an inactive spot with `ghs_D=1` and `ghs_BLP=1` recorded `impl=opencl` and diverged by **1,296 LSB16** on 414,331 of 30,419,712 samples. The predicate now refuses the pair because the host takes the `clipR()` arm; the case is a host residual at PAE 0 |
| 4-px ROI | host, PAE 0 |
| `localadjustments.pp3` | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl` |
| `localadjustments-multispot.pp3` | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; now a device row, not a SKIP |
| `localadjustments-residual.pp3` | host row, informational 1 LSB16 |
| `localadjustments-munsell.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; the device MUNS pass differs by 1 LSB16 on 389,860 of 30,419,712 samples, and the same profile with `NONE` differs by 1 LSB16 on 389,597, so the remaining bit is not a MUNS zone flip. The pre-fix clamp made the MUNS row **136 LSB16** (416,889 samples) |
| `localadjustments-vibrance.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; vibrance 40/20 with the skip threshold 0/75 and no Colour & Light. The host export differs from neutral on 3,837,490 pixels, so the row is not vacuous |
| `localadjustments-localcontrast.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; Gaussian local contrast at radius 40 (the fp64 recurrence), amount 0.4, darkness 0.9 and the Colour & Light lightness 1. The host export differs from neutral on 5,151,390 pixels, so the row is not vacuous |
| `localadjustments-sh.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; Lab-mode `"std"` shadows/highlights, highlights 60, shadows 60, radius 40. The host export differs from neutral by PAE 28 codes on 3,073,092 of 10,139,904 pixels (8,144,216 of 30,419,712 channel samples), so the row is not vacuous |
| `localadjustments-soft.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; per-pixel soft light at strength 60 and sensitivity 30 |
| `localadjustments-cbdl.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; local CBDL with all six multipliers above one, chroma 0.5, threshold 0.2, sensitivity 60 and clarity 2. The host export differs from neutral on 14,240,286 of 30,419,712 channel samples (PAE 3,420 codes), so the row is not vacuous |
| `localadjustments-dehaze.pp3` (CPU-device row) | PASS, `max_pae=1.5259e-05` (1 LSB16), `impl=opencl`; local dehaze at strength 50, depth 25, saturation 50 and the senstype-30 sensitivity 60, gated by `expreti` with zero retinex strength. The host export differs from neutral by PAE 10,603 codes on 15,166,414 of 30,419,712 channel samples, so the row is not vacuous |
| Dehaze parity, 17 cases | alone, negative, colour, CBDL, vibrance, SH, soft light, local contrast, all seven tools, MUNS, multispot, inactive, ELI/RECT, 179x49, 238x64, 8192x64 and a 1024x1793 ROI: POCL CPU, `impl=opencl`, finite, worst 0.052734 LSB16 (the 1024x1793 case; 0.046875 on 8192x64). The 1024x1793 ROI has a shorter side above 200 px, so it is the nearest-neighbour rescale path: 1024x1793 rescales to 200x350 before the ambient estimate. Non-vacuity: dehaze alone changes 22,752 of 45,696 samples (PAE 2,365.349609 LSB16) against the same spot with the enable and dehaze off |
| Dehaze residuals, 10 cases | `dehazeblack`, show-depth, an explicit mask, a soft radius, an active retinex strength, strength/depth/saturation/sensitivity out of range and dehaze without the retinex enable: `impl=host`, no `impl=opencl`, PAE 0 |
| Dehaze CPU path versus pre-change binary | `localadjustments-dehaze.pp3`, `--gpu=off`, `RT_STAGE_OPENCL` unset, `OMP_NUM_THREADS=1`: the current CPU build (`ccca72646`) and the pre-change binary (`7544aa258`, built from a detached worktree) are **AE 0, 0 of 30,419,712 samples differ** |
| CPU tree at HEAD | `rth-cli` and `stage-residency-test` rebuilt in `build/cpu-perf-graph`; `stage-residency-test` reports **PASS (25 skipped)**, with the ten dehaze residual rows host/PAE 0 |
| Full `--stage-device` | PASS, **31 PASS and 0 SKIP** on the POCL CPU device, including the dehaze row. MUNS and the six tool rows are CPU-device rows, consistent with the launcher's CPU-only requirement; `working.trc` 1 LSB16 |
| Default path | `RT_STAGE_OPENCL` unset runs the host block; CPU-only test `PASS (25 skipped)`. The `localadjustments-cbdl.pp3` and `localadjustments-dehaze.pp3` host exports with `--gpu=off` are pixel-identical to the pre-change binary (`compare -metric AE` 0) |
| Builds | both `build/cpu-perf-graph` and `build/opencl-perf-graph` compile |

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
