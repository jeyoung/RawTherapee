# Parity results

All comparisons use 16-bit TIFF output and `OMP_NUM_THREADS=1`, because the
multi-threaded CPU path is itself not bit-reproducible. Two identical Debian 12
runs of a 10.2 MP PEF differed by 40 LSB on one pixel without it, and a host
versus container pair differed by 754 LSB on 2 608 pixels; with it, the same
pairs are exact. The gate is the peak absolute error (PAE) in normalised units;
1 LSB at 16-bit is 1/65535 = 1.53e-5.

| Date | Commit | Device | Image/profile | Mode | PAE | Result |
|---|---|---|---|---|---|---|
| 2026-10-20 | 300a28c16 | CPU (stub) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | 300a28c16 | POCL (CPU) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | 300a28c16 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | 7d767b314 | POCL (CPU) | colorspace_flowers.pef / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | 7d767b314 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | 7d767b314 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / resize 1.5x | off vs auto | 0 | PASS |
| 2026-10-20 | f6cc5e236 | POCL | colorspace_flowers.pef / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | colorspace_flowers.pef / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | colorspace_flowers.pef / rotate standard | off vs auto | 0 | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | colorspace_flowers.pef / rotate log + 0.8 scale | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | 45.7 MP NEF / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | 45.7 MP NEF / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | c652e3505 | CPU build vs OpenCL `--gpu=off` | colorspace_flowers.pef / resize 0.5x | cross-build | 0 | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 0.75, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | uncommitted | POCL (`RT_OPENCL_ALLOW_CPU=1`) | colorspace_flowers.pef / capture 0.75, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 1.5, 10 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 2.0, 10 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 0.75, no early stop | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | 45.7 MP NEF / capture 0.62, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | POCL (`RT_OPENCL_ALLOW_CPU=1`) | 45.7 MP NEF / capture 0.62, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | colorspace_flowers.pef / neutral | off vs auto | 1 LSB | PASS |
| 2026-10-22 | f416c9fcb | AMD RX 550 | colorspace_flowers.pef / capture 3x3-13x13 | off vs forced GPU | 1 LSB | PASS |
| 2026-10-22 | uncommitted | AMD RX 550 | colorspace_flowers.pef / capture Auto | off vs auto | 1 LSB | PASS |
| 2026-10-22 | uncommitted | AMD RX 550 | 45.7 MP NEF / capture 0.75, 20 it | off vs forced GPU and Auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | 45.7 MP NEF / neutral | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | random Lab / RTv4_sRGB, RTv2_sRGB, sRGB, sRGB2014, RTv4_Medium monitor | GPU vs lcms | 1 byte | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | colorspace_flowers.pef / neutral, `RT_OPENCL_RGBPROC=1` | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | exposure/highlight/black profile, `RT_OPENCL_RGBPROC=1` | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | public PEF baseline, `RT_OPENCL_RGBPROC=1` | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | colorspace_flowers.pef / neutral (`--quick`) | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | colorspace_flowers.pef / bayer14 (`--formats`) | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | 45.7 MP NEF / resize 0.5x | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | 45.7 MP NEF / sidecar (crop + resize) | off vs auto | 1 LSB | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / `lanczos3` node via `stage::run` | device vs host, `StageExecution` | 0 | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / resize.pp3, 16-bit TIFF, `--gpu=off` | switch off vs on (host policy) | AE=0 | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / preview sweep, 16 states | switch off vs on | 0 (32 images AE=0) | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / crop harness, 3 states + sequences | switch off vs on | AE=0 (30 PPMs) | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-sh.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | 12-case shadows/highlights matrix, `stage-residency-test` | device vs host block | 0.054 LSB (worst) | PASS |
| 2026-10-30 | 52a7a99df | POCL (CPU) | colorspace_flowers.pef / `localadjustments-soft.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-30 | 52a7a99df | POCL (CPU) | soft-light matrix, `stage-residency-test` | device vs host block | 0.054 LSB (worst) | PASS |
| 2026-10-30 | c79ea2579 | POCL (CPU) | colorspace_flowers.pef / `localadjustments-cbdl.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-30 | 4129076f6 | POCL (CPU) | 24-case CBDL matrix, `stage-residency-test` | device vs host block | 0.068 LSB (worst) | PASS |
| 2026-10-30 | 4129076f6 | POCL (CPU) | CBDL chroma regressions (`chromacbdl=0.01`, `balan=2.5`), `stage-residency-test` | device vs host block | 0.029 LSB (worst) | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-dehaze.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | 17-case dehaze matrix (alone, negative, colour, CBDL, vibrance, SH, soft light, local contrast, all tools, MUNS, multispot, inactive, ELI/RECT, 179x49, 238x64, 8192x64, 1024x1793 rescale), `stage-residency-test` | device vs host block | 0.053 LSB (worst) | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | 10-case dehaze residual matrix, `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-log.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | 21-case log matrix (alone, no compression, both senstype-11 arms, colour, dehaze, CBDL, vibrance, SH, soft light, local contrast, all tools, MUNS, multispot, inactive, ELI/RECT, 179x49, 238x64, 8192x64), `stage-residency-test` | device vs host block | 0.877 LSB (worst) | PASS |
| 2026-10-30 | uncommitted | POCL (CPU) | 11-case log residual matrix, `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-sharpen.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 21-case sharpen matrix (alone, 3x3/5x5/7x7 radii, high sensitivity, colour, log, dehaze, CBDL, vibrance, SH, soft light, local contrast, all tools, MUNS, multispot, inactive, ELI/RECT, partial zero-transition, 179x49, 238x64, 8192x64), `stage-residency-test` | device vs host block | 0.926 LSB (worst) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 12-case sharpen residual matrix, `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `crop.pp3`, `crop` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 6-case crop copy matrix (full frame, odd offset, bottom right, single row, single pixel, 8192 wide), `stage-residency-test` | device vs host row copy | 0 (AE=0) | PASS |
| 2026-10-31 | f744f7bc3 | POCL (CPU) | colorspace_flowers.pef / `labtools-blackwhite.pp3`, `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | f744f7bc3 | POCL (CPU) | 8-case lab.tools black-and-white matrix (B&W alone, B&W + SH/local contrast/vibrance and the three combined; B&W + LabGrid, B&W + regions and both as colour-toning refusals), `stage-residency-test` | device vs host block | 0 (device) and 0 (host refusals) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-toning.pp3` (Splitco + shadows/highlights), `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 3-case lab.tools tone-method matrix (Splitco, Splitlr, RGBSliders with shadows/highlights), `stage-residency-test` | device vs host block | 0 | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-blackwhite-labcurve.pp3` (B&W through the `bwToning` arm, L and a/b tables plus the C=f(L), C=f(C) and L=f(C) tables), `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 7-case lab.tools black-and-white Lab-curve matrix (identity tables, L curve, a/b tables, C=f(L), L=f(C), the three chroma tables, and the full set with vibrance, shadows/highlights and local contrast at 179x48 and 238x64), `stage-residency-test` | device vs host block | 0 (exact, a/b exact) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-huecurve.pp3` (Lab curves with chromaticity 40, C=f(H), L=f(H) and H=f(H)), `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 11-case lab.tools hue flat-curve matrix (chromaticity -100 ignoring the curves, 0, 50 with red/skin protection and 100; C=f(H), L=f(H), H=f(H) alone; all three with the slider; with a/b tables; with vibrance and shadows/highlights; B&W + L=f(H)), `stage-residency-test` | device vs host block | 0.023438 LSB16 (worst) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | lab.tools hue-family non-vacuity and residuals: same profile with the family off changes the image; a 40 000-point dense C=f(H) is refused for table fidelity and runs the block | non-vacuity PAE 12 400-52 610 LSB16; refusal 0 (host) | PASS |

| 2026-10-31 | 5bb35d3ce | POCL (CPU) | colorspace_flowers.pef / `labwavelet-chroma.pp3` (contrast + chroma `link`), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | 5bb35d3ce | POCL (CPU) | 12-case chroma matrix (alone at 6/3/7 levels, strength 100, sigmacol 0.5, with contrast, with the equaliser, positive and negative skin, equaliser plus skin, saturated hue, 179x49 padded), `stage-residency-test` | device vs host block | 0 (exact, a/b exact) | PASS |
| 2026-10-31 | 5bb35d3ce | POCL (CPU) | 9-case chroma residual matrix (CHmethod without/with, chro/sigmacol/skin ranges, non-identity hhcurve, contrast+skin, chroma-only without strength, edge panel), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-10-31 | c5ed40c3c | POCL (CPU) | colorspace_flowers.pef / `labwavelet-edge.pp3` (edge plain arm, active `ccwcurve`), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | c5ed40c3c | POCL (CPU) | 17-case edge matrix (strengths 50/100, radius 90, thresholds 60/-30, effect 0.6/2.5, reinforce more/none/less, identity and custom `ccwcurve`, with contrast, with chroma, contrast+chroma, 179x49 padded, 3 and 7 levels), `stage-residency-test` | device vs host block | 0.058594 LSB16 (worst) | PASS |
| 2026-10-31 | c5ed40c3c | POCL (CPU) | 12-case edge residual matrix (median level, NP low/high, Lipst, EDmethod SL, edgval/edgrad/edgthresh/edgeffect ranges, unknown Medgreinf, zero strength, skin protection), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-01 | 6a54860e1..873000037 | POCL (CPU) | colorspace_flowers.pef / `labwavelet-final.pp3` (final curves: non-identity `wavclCurve` and `opacityCurveWL`, sigmafin 1), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-11-01 | 6a54860e1..873000037 | POCL (CPU) | 13-case final-curve matrix (diagonal only, opacity only, both, sigmafin 0.5/2.0, default curves, with contrast, with chroma, with edge, 179x49 padded, 3 and 7 levels, inert balance controls), `stage-residency-test` | device vs host block | 0.015625 LSB16 (worst) | PASS |
| 2026-11-01 | 6a54860e1..873000037 | POCL (CPU) | 14-case final-curve residual matrix (softradend, softrad, BAmethod sli, tmr, expclari, sigmafin low/high, avoid, median, expbl, expresid, expnoise, CLmethod one, skin protection), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-01 | 0abd76894..5781f1c38 | POCL (CPU) | colorspace_flowers.pef / `labwavelet-toning.pp3` (zero-protection toning, non-identity `opacityCurveRG` and `opacityCurveBY`), `lab.wavelet` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-01 | 0abd76894..5781f1c38 | POCL (CPU) | 12-case toning matrix (a only, b only, both, sigmaton 0.5/2.0, with contrast, with chroma, with edge, with final curves, 179x49 padded, 3 and 7 levels), `stage-residency-test` | device vs host block | 0.011719 LSB16 (worst) | PASS |
| 2026-11-01 | 0abd76894..5781f1c38 | POCL (CPU) | 10-case toning residual matrix (protab on, sigmaton low/high, no opacity multiplier, expnoise, expbl, expresid, expclari, CLmethod one, skin protection), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-02 | 89e0a844b..47b9980fb | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` (Lab curves with contrast 30, L/a/b/cc/lc/cl tables), `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | 89e0a844b..47b9980fb | POCL (CPU) | 3-case Lab-contrast matrix (contrast 5, contrast 15 with L curve+brightness, contrast 30 with all six tables), `stage-residency-test` | device vs host block | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` with `Gamutmunse=XYZ`, `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` with `Gamutmunse=XYZREL`, `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | 4-case lab.tools XYZ/XYZREL Lab-curve matrix (XYZ with all six tables on the saturated image, XYZREL, XYZ neutral, XYZ with the three hue curves), `stage-residency-test` | device vs host block | 0.753906 LSB16 (worst: XYZ with hue curves, 0 samples over 1); XYZ 0.317383, XYZREL 0.066406, XYZ neutral 0 | PASS |

The XYZ and XYZREL rows use `ICCStore::workingSpaceMatrix()` as
`Color::gamutmap()`'s `p`, which is the host's `wp` in
`chromiLuminanceCurve()`: row-major, with the RGB tristimulus primaries in
the columns. `Color::primaries_to_xyz()` builds the same matrix for the
output-gamut control in `iplab2rgb.cc`; it is not this block's argument. The
device export records `"impl":"opencl"` for `lab.tools` in both rows, and the
device runs `XYZREL` with the block's `0.5` halfway blend, not the local
adjustments' `0.3`.

`47c50ee77` changes only CPU export buffer handling; the OpenCL rows above
re-run the existing GPU stages against the new CPU neighbour.

The `--quick` neutral row and the `--stage-device` sweep could not complete on
this host: the neutral export records no classic `lab2srgb` stage, and the
sweep stops at its baseline `output.convert` row. The final-curve gate was
therefore run directly with the same off/auto commands and profile; it records
`impl=opencl` for `lab.wavelet` at 1 LSB16. The stage-residency POCL run above
is complete and is the parity evidence for this unit.
`verify.sh --quick` also re-ran the monitor self-test: max 1 byte, 14 of
196608 samples. The exact CPU export parity for that commit (neutral, sidecar,
sidecar `-f`, both `WorkingTRC` profiles, `AE=0`) is recorded in
`devnotes/perf/FUSION.md`.

`verify.sh` now reports `SKIP` instead of `PASS` when no OpenCL device is
selected, so a CPU-only build cannot produce a vacuous parity claim.

Cross-build check: `WITH_OPENCL=OFF` and `WITH_OPENCL=ON --gpu=off` produced
identical pixels (PAE 0) for the baseline image with `OMP_NUM_THREADS=1`.

Notes:

- `--gpu=off` and `--gpu=auto` on the stub build produce identical pixels.
- Multi-threaded runs of the same binary differ by up to 5 LSB, so CPU
  regression tests must either pin `OMP_NUM_THREADS=1` or allow that
  tolerance. This is a pre-existing RawTherapee characteristic, not an OpenCL
  effect.
- The TIFF files still differ byte-wise because of embedded timestamps; do not
  use SHA-256 of the file as a gate.
- Output conversion is compared only for the shipped `RTv4_sRGB` profile and
  8/16-bit integer output. Other profiles and float output continue to use
  lcms and are pixel-identical between `--gpu=off` and `--gpu=auto`.
- `compare_tiff.py` now normalises through ImageMagick to uncompressed 16-bit
  TIFF and reads the encoded samples. The previous ImageMagick `rgb:` path
  linearised the samples, which amplified a 1 LSB difference into a 26 LSB
  PAE.
- Monitor output is 8-bit, so its parity gate is 1 byte. `verify.sh --quick`
  and `verify.sh --monitor` run `rawtherapee-cli --gpu-test-monitor`, which
  compares the GPU monitor conversion with lcms on a deterministic 256×256
  full-range Lab image.
- `verify.sh --rgbproc` sets `RT_OPENCL_RGBPROC=1`, asserts that
  `opencl rgbproc:` ran, and compares neutral and exposure profiles. The
  stage is batch-only because preview supplies a tone-curve histogram.

## Stage device residency

The graph device path runs `lanczos3` (resize), `transform` (rotation/scale)
and the Lab-to-RGB family (`output.convert` on export; `monitor.output`,
`viewport.output` and `thumbnail.output` on the interactive paths), plus
`rgb.proc` when `RT_OPENCL_RGBPROC=1`. Device runs use
`RT_OPENCL_ALLOW_CPU=1` and an ICD directory that contains only `pocl.icd`
(`OCL_ICD_VENDORS`), so the selected device is POCL's CPU device
(`cpu-ivybridge-Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz`). These are
CPU-device results and must not be read as GPU parity. Restricting the ICD set
pins the device identity; it is no longer needed to avoid the rusticl crash
described under "Broken and unsupported ICDs", because a broken ICD is skipped.

The parity gate is 1 LSB at 16-bit (1/65535 = 1.53e-5 normalised). The measured
peak absolute error is zero for the synthetic node tests:

- The node itself, through the same `stage::run()` entry point that
  `ImProcFunctions::Lanczos` uses, on a synthetic 128x96 three-plane F32 image
  at scales 0.5 and 1.5, differs from the host implementation in 0 of 9216 and
  0 of 82944 samples. `StageContext::execute()` reports `StageExecution`
  `device=OPENCL`, `asynchronous=true`, so the parity is attributed to an
  executed device kernel rather than to the selection. If the queue had fallen
  back to the host it would report `device=CPU` and the test would fail.
- An over-budget node runs on the host: `StageExecution` reports `device=CPU`,
  `asynchronous=false`, and its output is exact against the CPU reference.
- A 16-bit TIFF export with the resize profile through the device, against the
  `--gpu=off` CPU reference, has a peak error of 1 LSB16 on 96 426 of 2 534 980
  pixels (3.8%), inside the 1 LSB gate. The same export with `RT_STAGE_OPENCL`
  unset is handled by the classic backend and emits no stage record, so the
  default path is unchanged.
- The device record carries the attribution: with the switch on and
  `--gpu=auto` the classic log no longer has an `opencl resize` line, and the
  stage record says `"stage":"lanczos3","impl":"opencl","source":"opencl"`.
  Forcing a refusal (`RT_STAGE_DEVICE_BUDGET_MB=16`) records `impl=host`,
  `source=device-fallback`; the gate then reports FAIL when `RT_REQUIRE_GPU=1`
  and SKIP otherwise, so the assertion is not vacuous.
- The device `transform` node is pixel-exact for identity geometry on a
  synthetic three-plane image, which `stage-residency-test` asserts; rotating
  it by 0.05 radians makes that test fail, so the assertion is not vacuous. A
  16-bit TIFF export with the rotate profile, against `--gpu=off`, has a peak
  error of 1 LSB16 on 384 601 of 10 139 424 pixels (3.8%), the same order as
  the classic GPU path. The interpolation helpers and the map builder are
  shared with the classic backend, so only the addressing differs.
- `thumbnail.transform` and `thumbnail.transform.u8` match an independent
  rotate/flip reference for all sixteen combinations in both formats; the GUI
  path did not reach the node in the harness and is recorded as uncovered.
- `thumbnail.whitebalance` runs on the device for the file-browser thumbnails;
  a 45.7 MP NEF thumbnail cache is byte-identical to a CPU-only run, and the
  device test matches an independent reference exactly.
- `rgb.proc` through the device, against the `--gpu=off` reference, has a peak
  error of 1 LSB16 on 401 708 of 10 139 424 pixels (4.0%). The classic path is
  byte-identical to the package built before the kernel moved.
- `output.convert` through the device, against the `--gpu=off` reference, has a
  peak error of 1 LSB16 on 385 525 of 10 139 424 pixels (3.8%). The packed
  8-bit nodes are not reachable from the CLI; `stage-residency-test` compares
  the kernel byte for byte with an independent reference on a synthetic Lab
  image, and scaling the device matrix by 1.001 makes it fail.
- Local dehaze, through `local.adjustments`, matches the host block in 17
  configurations (alone, a negative strength, colour, CBDL, vibrance,
  shadows/highlights, soft light, local contrast, all tools, MUNS, multispot,
  inactive, ELI/RECT, the 179x49, 238x64 and 8192x64 widths, and a 1024x1793
  ROI that exercises the nearest-neighbour rescale to the 200 px cap) with a
  worst peak error of 0.053 LSB16 (0.052734 on the 1024x1793 rescale ROI).
  The pass is device-side except for two documented host reductions: the
  normalize maximum is read back and reduced on the host (an exact maximum),
  and the ambient-light estimate (`get_dark_channel_downsized()` and the two
  exact 95th percentiles) runs in `dehazeAmbientPrefix()` on the read-back
  filtered channels, because `std::nth_element` cannot be reproduced
  order-independently on the device. The three colour guided filters, the
  dark channel, the guided filter of the dark channel and the per-pixel
  recovery run on the device with the lab.tools guided-filter port. A 16-bit TIFF export with
  `localadjustments-dehaze.pp3` has a peak error of 1 LSB16, and ten residual
  configurations (`dehazeblack`, show-depth, mask, soft radius, active
  retinex, four out-of-range sliders and dehaze without the retinex enable)
  run the host builtin byte for byte.
- Local log encoding, through `local.adjustments`, matches the host block in
  21 configurations (alone, no compression, both senstype-11 sensitivity
  arms, colour, dehaze, CBDL, vibrance, shadows/highlights, soft light,
  local contrast, all tools, MUNS, multispot, inactive/active both orders,
  ELI/RECT and the 179x49, 238x64 and 8192x64 widths) with a worst peak error
  of 0.877 LSB16 on log+dehaze at `sensilog = 60`; the all-tools row at
  `sensilog = 30` is 0.334 and log alone is 0.015. The pass is device-side: the ROI Lab-to-RGB
  round trip, the per-pixel `apply` with the host-built `find_gray()` base,
  the saturation control, the `repar` blend and the senstype-11 deltaE blend
  all run in the kernel. `localadjustments-log.pp3` has a peak error of
  1 LSB16, and eleven residual configurations (`autocompute`, `expcie`,
  `ciecam`, non-zero `detail`, `strlog`, mask, soft radius and four
  out-of-range sliders) run the host builtin byte for byte.
- `crop` copies the requested rectangle out of the three-plane F32 Lab buffer
  on the device. The node is exact by construction, so the test compares all
  three planes with `AE=0` at full frame, an odd offset, the bottom-right
  corner, a single row, a single pixel and an 8192-wide rectangle; every
  invalid rectangle (negative or out-of-range offset, oversized destination,
  aliased buffers) is refused without touching the destination, and the
  call site's row copy stays the fallback. The export gate is 1 LSB16 on the
  final TIFF because `crop.pp3` also runs the resize node.
- Local sharpening, through `local.adjustments`, runs the output sharpening
  core's device mask, undamped RLD and damped kernels on the block's
  zero-padded extent and reproduces `Sharp_Local()`'s full-image deltaE
  reference blur and senstype-0 (`lp.senssha`) blend. It matches the host
  block in twenty-one undamped configurations (alone, the 3x3/5x5/7x7 radii,
  high sensitivity, colour, log, dehaze, CBDL, vibrance, shadows/highlights,
  soft light, local contrast, all tools, MUNS, a second spot,
  inactive/active, ELI/RECT, a partial zero-transition spot and the
  179x49/238x64/8192x64 widths) with a worst peak error of 0.926 LSB16 at
  sensitivity 90 (kernel-7 0.766, sharpen+soft light 0.626, all tools 0.428,
  sharpen+dehaze 0.355, 8192x64 0.516, sharpen alone 0.207); no sample is
  non-finite. Sharpen alone changes 33,709 of 45,696 samples at PAE
  8,866.826172 LSB16, so the parity is not vacuous. The damped recurrence is
  within 1 LSB16 in nineteen cases (the default `"bef"` fixup, the 0.6
  minimum radius, damping 1/5/10/100, both methods, the combined tools, MUNS, a
  second spot and the three widths) with a worst of 0.871 LSB16; the fixup
  changes 32,446 samples at PAE 9,288.169922 LSB16 against the tool off, and
  damping changes 34,031 at PAE 2,806.058594 LSB16 against the same spot
  undamped. `localadjustments-sharpen.pp3` and
  `localadjustments-sharpen-damped.pp3` are 1 LSB16; the latter exercises the
  default fixup. Thirteen residual configurations (capture sharpening,
  damping range (101), a damped radius below 0.6, gamma, blur, mask view, a
  separable radius, radius/amount/iteration/contrast/sensitivity out of range
  and `reparsha`) run the host builtin byte for byte; a partial spot with a
  non-zero transition is also a host residual because the block's padded
  extent does not cover the transition ring the host reads. Negative damping
  is refused by the predicate and is not a runtime case.
- The `lab.wavelet` final-curve arm runs `finalContAllL()` on the statistics
  `Evaluate2()` recomputes after the contrast and edge passes, then applies
  the pixel-domain `wavclCurve` before the block's `L <= 0 -> 1` clamp. Both
  the 501-point `opacityCurveWL` and the 65536-entry diagonal LUT are sampled
  on the host through the block's own curves. `Expfinal` also keeps the a/b
  round trip alive, so the device runs it without `Expchroma`/`Expedge`.
  Thirteen device cases (diagonal only, opacity only, both, sigmafin 0.5/2.0,
  default curves, with contrast, with chroma, with edge, 179x49 padded, 3 and
  7 levels, inert balance controls) match the host block with a worst peak
  error of 0.015625 LSB16; the panel-off comparison changes 15,232-76,640
  samples in the twelve checked cases, so the parity is not vacuous.
  Fourteen residual configurations (softradend, softrad, `BAmethod` sli,
  `tmr`, `expclari`, sigmafin below/above its slider range, avoid, median,
  expbl, expresid, expnoise, CLmethod one and skin protection) run the host
  builtin byte for byte. `labwavelet-final.pp3` is 1 LSB16 on the export TIFF.
- The `lab.wavelet` toning panel's zero-protection arm reproduces
  `ContAllAB()`'s opacity-curve multiply on the a/b bands: the nine
  `mulopaRG`/`mulopaBY` samples are built on the host, the per-level
  `calceffect()` thresholds use `sigmaton`, and the coefficient multiply runs
  after the chroma arm. Twelve device cases (a only, b only, both, sigmaton
  0.5/2.0, with contrast, with chroma, with edge, with final curves, 179x49
  padded, 3 and 7 levels) match the host block with a worst peak error of
  0.011719 LSB16; the panel-off comparison changes 17,112-57,280 samples per
  checked case. Ten residual configurations (protab above zero, sigmaton
  below/above its slider range, no opacity multiplier, expnoise, expbl,
  expresid, expclari, CLmethod one and skin protection) run the host
  builtin byte for byte. `labwavelet-toning.pp3` is exact on the export TIFF.
- Impulse denoise and defringe, through the `lab.sharpening` node, run as one
  atomic pair. The device test compares the observable corrected sets and all
  three planes: the impulse fixture and the combined pair correct exactly the
  host's pixel sets, the impulse pass is bit-identical on a 128x80 random
  field and on the fixture (`AE=0`), and the defringe pair is `PAE=0` on L and
  0.000977 LSB16 on a/b. `xatan2f()` is ported from `rtengine/sleef.h` because
  a device `atan2` one ulp away moved a fringe across the `fringe <
  threshfactor` decision: the export corpus went from 1010 LSB16 to 1 LSB16
  with the port. `labsharpening.pp3` (impulse 50, defringe radius 2.0 and
  threshold 13) is 1 LSB16, and a one-refused pair (impulse on, defringe
  radius 1.0) is `impl=host`, byte-identical to the CPU path. A defringe
  radius below 1.15, a threshold outside 0..100, the CIE-CAM arm, an image
  narrower than its defringe window and a non-fp64 device are residuals; a
  radius below 0.25 is the host no-op and leaves every plane untouched.
- `tools/opencl/verify.sh --stage-device`, which `--quick` also runs, asserts
  each record and its parity in one step:

  ```bash
  RT_BUILD_DIR=... RT_OPENCL_ALLOW_CPU=1 OCL_ICD_VENDORS=<pocl-only> \
      tools/opencl/verify.sh --stage-device
  # PASS  pef/stage-device-resize     max_pae=1.5259e-05
  # PASS  pef/stage-device-transform  max_pae=1.5259e-05
  # PASS  pef/stage-device-output     max_pae=1.5259e-05
  # PASS  pef/stage-device-rgbproc    max_pae=1.5259e-05
  ```
- The preview sweep (16 states, 32 images) and the crop harness (30 PPM files)
  are identical with the switch off and on, but neither graph runs a stage-ABI
  node; they are CPU-path regression checks with zero device calls.

Kill switch: `--gpu=off` with `RT_STAGE_OPENCL=1` records `impl=module` (the
host) and not `impl=opencl`. Before the batch-path wiring it recorded
`impl=opencl`, which was the defect this record corrects.

Timing on POCL CPU, `OMP_NUM_THREADS=1`, same image and profile, interleaved
18 samples per side: the preview sweep is 327.67 ms mean (326.99 median) at
the parent commit `9e8df3493`, 331.48 (328.69) with the switch off and 328.99
(324.72) with it on. The spread is run-to-run noise on this host, whose
samples ranged from 319 to 363 ms. No performance claim is made for the device
node on a CPU OpenCL device: it is slower than the host and exists for parity
and correctness.

## Broken and unsupported ICDs

Device selection must survive whatever the ICD set contains. The stage device
enumerates the platforms the ICD loader reports and validates each one before it
dispatches to it.

Failure found on 2026-10-27 at `3339afc62`: with the distribution vendor set
(`mesa.icd`, `pocl.icd`, `rusticl.icd` in `/etc/OpenCL/vendors`)
`stage-device-test` and `stage-residency-test` died with SIGSEGV inside the ICD
loader, while an `OCL_ICD_VENDORS` directory holding only `pocl.icd` worked. The
fault is the rusticl platform's ICD dispatch chain: ocl-icd 2.3.3 tail-jumps
through a null entry while answering the first platform query. A 20-line C
program calling `clGetPlatformInfo` on the second platform, and `clinfo`,
crash identically, so this is an ICD defect and not a RawTherapee or merge
defect. It is Debian bug #1117930 (`mesa-opencl-icd`, rusticl dispatch table not
initialised). `clGetPlatformIDs` still reports both platforms without an error
and the crash happens before a CL error code exists, so a platform cannot be
validated in-process.

The trigger is order dependent: rusticl second, after pocl, is broken, while
rusticl first is fine for the same pair (`mesa.icd` before `rusticl.icd` is also
fine). The distribution file order (`pocl.icd`, then `rusticl.icd`) is the
broken one, which is why the ambient set reproduced it.

Fix: `probe_platforms()` in `stage_opencl_device.cc` asks every platform the
questions the selector will ask (platform name, device ids, first device name,
type and global memory) in a forked child, one child per platform, with
`RLIMIT_CORE=0` and a 30 s alarm. The parent dispatches only to platforms whose
child answered; a platform whose child dies or times out is skipped, with a
diagnostic such as:

    no GPU OpenCL device; set RT_OPENCL_ALLOW_CPU=1 to use a CPU device;
    1 of 2 OpenCL platform(s) skipped: platform 1 crashed the ICD query;
    check the installed ICDs (/etc/OpenCL/vendors, or OCL_ICD_VENDORS)

and the node falls back to the host. `StageOpenCLDeviceInfo::note` carries the
same text when a device was still selected, so a run that loses a vendor is
visible without failing. One child per platform is deliberate: a broken
platform does not hide the platforms behind it. The probe is a one-off cost of
about 30 to 40 ms per process on this host, measured on the two device tests.

Limitations:

- rusticl is unsupported on this host: its platform kills the query when it is
  loaded after pocl, so its devices are unusable here even though the ICD is
  installed. Only POCL is selected. The probe cannot say which vendor is at
  fault beyond reporting that the platform at that index crashed.
- The probe covers enumeration only. A vendor that fails or crashes later, in
  context, queue, kernel or a transfer, is still reported through the existing
  fallback paths; the crash path is not additionally protected.
- The probe needs `fork()`, so it runs on POSIX systems only. Elsewhere the
  platforms are queried directly as before, because the defect is specific to
  the ocl-icd loader. If `fork()` or `pipe()` fails the affected platforms are
  skipped and the device path stays on the host rather than querying an
  unvalidated platform. Forking a process that other threads are using can
  inherit a loader or allocator lock; that only delays the child, which the
  alarm then kills, so the platform is skipped instead of hanging RawTherapee.

Evidence, 2026-10-27, `build/opencl-perf-graph-final`, GCC, ambient ICD set
unless stated. "Ambient" is the `/etc/OpenCL/vendors` set, `OCL_ICD_VENDORS`
unset.

| Check | Result |
|---|---|
| `stage-device-test`, ambient ICD set | PASS, exit 0; host fallback with the diagnostic above, no crash |
| `stage-residency-test`, ambient ICD set | PASS, exit 0, 5 skipped; no crash |
| `stage-residency-test`, ambient ICD set, `RT_OPENCL_ALLOW_CPU=1` | PASS, 0 skipped; POCL CPU device, platform 1 skipped; PAE 0.000000 LSB16 at scales 0.5 and 1.5 (0 of 9216 and 0 of 82944 samples) and 0 of 4608 through `run()`; `StageExecution` reports `device=OPENCL`, `asynchronous=true` |
| `stage-residency-test`, `OCL_ICD_VENDORS` = `pocl.icd` only, `RT_OPENCL_ALLOW_CPU=1 RT_REQUIRE_GPU=1`, `OMP_NUM_THREADS=1` | PASS, 0 skipped, no platform skipped; PAE 0.000000 LSB16 with the same sample counts. This is the device-parity row |
| `stage-residency-test`, `OCL_ICD_VENDORS` with `rusticl.icd` before `pocl.icd` | PASS, 0 skipped; no platform skipped and POCL selected, so the order dependence is covered from both sides |
| `OCL_ICD_VENDORS` = empty directory, `RT_REQUIRE_GPU=1` | FAIL, exit 1, "no OpenCL platform" and the host fallback; a missing device still cannot pass as a skip |
| CPU build (`WITH_OPENCL=OFF`) and OpenCL build, 16 stage test executables plus `editorrequest-test` | both compile without warnings, 17 of 17 executables PASS in both trees (34 runs) |
| kill switch, `--gpu=off` with `RT_STAGE_OPENCL=1`, resize profile | the stage profile records one `lanczos3` node with `impl=module`; no `impl=opencl` record |
