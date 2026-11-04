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
| 2026-11-03 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-blur.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 14-case blur matrix (radii 2/5/10/18/28, luminance/chroma/all planes, sensitivity 0/100, no scope, sub-rectangle ROI, RECT, colour, log, soft light, MUNS, 179x49), `stage-residency-test` | device vs host block | 0.023 LSB (worst) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 13-case blur residual matrix, `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-blur-denoise.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 15-case Blur & Noise median and denoise-curve matrix (3x3/5x5/7x7/9x9, 1/3/4 passes, chroma, ROI edges, colour, log, 179x49, blur-then-median, the default curve and flat curves 0.2/0.9), `stage-residency-test` | device vs host block | 0.031 LSB (worst) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-tonemap.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB (AE 3 of 30,419,712; PAE 1 code) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 16-case tone-map matrix (strengths 0.5/1.2/-0.4/0.8, gamma 1/2, edge stopping 0.5/1.4/3.5, scale 1/4, reweight 2, the equalise off arm alone and at sensitivity 100, sensitivity 0/100, colour, blur, an offset ROI, a zero-strength no-op and a ROI below the block's mDEN gate), `stage-residency-test` | device vs host block | 0.023 LSB (worst) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 10-case tone-map residual matrix (strength/gamma/edge-stopping/scale/repart/saturation/reweight/sensitivity ranges, the recovery threshold and the global EPD pass), `stage-residency-test` | predicate refuses | 0 (host) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `crop.pp3`, `crop` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 6-case crop copy matrix (full frame, odd offset, bottom right, single row, single pixel, 8192 wide), `stage-residency-test` | device vs host row copy | 0 (AE=0) | PASS |
| 2026-10-31 | f744f7bc3 | POCL (CPU) | colorspace_flowers.pef / `labtools-blackwhite.pp3`, `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | f744f7bc3 | POCL (CPU) | 8-case lab.tools black-and-white matrix (B&W alone, B&W + SH/local contrast/vibrance and the three combined; B&W + LabGrid, B&W + regions and both as colour-toning refusals), `stage-residency-test` | device vs host block | 0 (device) and 0 (host refusals) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-toning.pp3` (Splitco + shadows/highlights), `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 3-case lab.tools tone-method matrix (Splitco, Splitlr, RGBSliders with shadows/highlights), `stage-residency-test` | device vs host block | 0 | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-blackwhite-labcurve.pp3` (B&W through the `bwToning` arm, L and a/b tables plus the C=f(L), C=f(C) and L=f(C) tables), `lab.tools` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 7-case lab.tools black-and-white Lab-curve matrix (identity tables, L curve, a/b tables, C=f(L), L=f(C), the three chroma tables, and the full set with vibrance, shadows/highlights and local contrast at 179x48 and 238x64), `stage-residency-test` | device vs host block | 0 (exact, a/b exact) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-huecurve.pp3` (Lab curves with chromaticity 40, C=f(H), L=f(H) and H=f(H)), `lab.tools` | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30 419 712) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | 11-case lab.tools hue flat-curve matrix (chromaticity -100 ignoring the curves, 0, 50 with red/skin protection and 100; C=f(H), L=f(H), H=f(H) alone; all three with the slider; with a/b tables; with vibrance and shadows/highlights; B&W + L=f(H)), `stage-residency-test` | device vs host block | 0 (exact) | PASS |
| 2026-10-31 | uncommitted | POCL (CPU) | lab.tools hue-family non-vacuity and residuals: same profile with the family off changes the image; a 100 000-point dense C=f(H) is refused for table fidelity and runs the block | non-vacuity PAE 12 400-52 610 LSB16; refusal 0 (host) | PASS |

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
| 2026-11-02 | 1005b12da..679f5b4d8 | POCL (CPU) | colorspace_flowers.pef / `labwavelet-denoise.pp3` (Expnoise, `Lev0..4` non-zero, `quaMethod=cons`, no user chroma), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB | PASS |
| 2026-11-02 | 1005b12da..679f5b4d8 | POCL (CPU) | 17-case denoise matrix (levels 3/5/6, ballum 10/7/5/0/-2, high-level sliders only, a light slider, odd width 179x160, odd height 180x161, odd both 179x161, with contrast, edge, final curves, toning, all panels and all panels padded), `stage-residency-test` | device vs host block | 0.015625 LSB16 (worst: ballum 0) | PASS |
| 2026-11-02 | 66613d0a9..6db958afe | POCL (CPU) | 11-case denoise residual matrix (`quaMethod=agre`, user chromfi/chromco, balchrom, `Wavhuecurve`, `CBenab`, Thrden, Ballum range, level range, all sliders zero, sub-128 window), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-03 | `6bcd826f9..dca455499` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-denoise-cur.pp3` (Expnoise, `sliMethod=cur`, `mixMethod=mix7`, non-identity `wavdenoise`), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB (AE=1, 21,068 of 30,419,712) | PASS |
| 2026-11-03 | `6bcd826f9..dca455499` | POCL (CPU) | 10-case "cur" matrix (mixmet 0/1/2/3, identity curve, contrast, edge, final curves, odd 179x161, all panels), `stage-residency-test` | device vs host block | 0.043945 LSB16 (worst: final curves) | PASS |
| 2026-11-03 | `6bcd826f9..dca455499` | POCL (CPU) | "cur" residual matrix (sigm above range; the expert pairing was a residual then and is a device case since `7fc43d8a3`) | device vs host block | 0 (host) | PASS |
| 2026-11-03 | `43732fb51..8cef1c71b` | POCL (CPU) | 3-case user chroma matrix (`chromfi` 40, `chromco` 60, both 100), `stage-residency-test` | device vs host block | 0.011719 LSB16 (denoise pair) | PASS |
| 2026-11-03 | `4338a0570..27c9cffac` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-thrden.pp3` (Expnoise, `sliMethod=sli`, `Thrden=5`), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB (24,753 of 30,419,712) | PASS |
| 2026-11-03 | `4338a0570..27c9cffac` | POCL (CPU) | 4-case Thrden matrix (1, 5, 20 and 20 with the final curves), `stage-residency-test` | device vs host block | 0.050781 LSB16 (worst: with final curves) | PASS |
| 2026-11-03 | `e54056e0a..29cc140ad` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-softrad.pp3` (clarity, `Softrad=5`), `lab.wavelet` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-03 | `e54056e0a..29cc140ad` | POCL (CPU) | 9-case soft-radius matrix (softrad 0.5/2/5/100, zero merge, chroma, denoise, 703x67 subsampled, 8192x64), `stage-residency-test` | device vs host block | 0.011719 LSB16 (denoise pair; the rest exact) | PASS |
| 2026-11-03 | `4f476722d..aaa41f842` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-softradend.pp3` (final panel, identity `wavclCurve`/`opacityCurveWL`, `Softradend=20`, `Strend=70`, `Detend=1`, `Thrend=0`), `lab.wavelet` | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30,419,712) | PASS |
| 2026-11-03 | `4f476722d..aaa41f842` | POCL (CPU) | 18-case final soft-radius matrix (softradend 1/2/3/5/7/10/20/40/1000, strend 0/40/50/60/70/80/100, detend -10/-2/0/2/4/10, final curves, contrast, denoise, 179x161, 703x67, 1024x64, 400x300 smooth, 1024x64 and 703x67 subsampled smooth), `stage-residency-test` | device vs host block | 0.765625 LSB16 (worst: curves); plain, odd, subsampled and smooth cases exact | PASS |
| 2026-11-03 | `4f476722d..aaa41f842` | POCL (CPU) | 10-case final soft-radius refusal matrix (Thrend, non-identity wavguidcurve, no Expfinal, softradend 1001/-1, strend 101, detend ±11, clarity soft radius, clarity arm), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-03 | `294e4eec6..062403ad9` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-softradend-hue.pp3` and `labwavelet-softradend-thrend.pp3`, `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB (231 and 1 of 30,419,712) | PASS |
| 2026-11-03 | `294e4eec6..062403ad9` | POCL (CPU) | final soft-radius coefficient/hue matrix (thrend 1/5/20, hue curves 5..60, each with final curves, contrast, denoise, odd, wide and smooth grids), `stage-residency-test` | device vs host block | 0.964844 LSB16 (worst: hue 20 with curves) | PASS |
| 2026-11-04 | `99db626cb..b171e0e1a` | POCL (CPU) | colorspace_flowers.pef / `localadjustments-retinex.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB (1,817 of 30,419,712) | PASS |
| 2026-11-04 | `99db626cb..b171e0e1a` | POCL (CPU) | local Retinex matrix (1/5/20 % and scale 2/3, ROI edges, combined with the colour spot and blur), `stage-residency-test` | device vs host block | 0.357422 LSB16 | PASS |
| 2026-11-04 | `638566a4e..9bb068bbd` | POCL (CPU) | colorspace_flowers.pef / `localadjustments-cie.pp3`, `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB (AE=1, 34,733 of 30,419,712) | PASS |
| 2026-11-04 | `638566a4e..9bb068bbd` | POCL (CPU) | 18-case local CIE CAM matrix (defaults, dim/dark/exDark source and viewing surrounds, grey and absolute luminance, warm adaptation, saturation/chroma/hue/colourfulness/red protection alone, a combined colour case, light and recovery, ROI edges, colour, blur and tone-map combinations), `stage-residency-test` | device vs host block | 0.399902 LSB16 (worst: combined colour sliders) | PASS |
| 2026-11-04 | `638566a4e..9bb068bbd` | POCL (CPU) | 26-case CIE CAM refusal matrix (log/Jz modes, PQ remap, smoothing, contrast and per-hue sliders, the CIE, per-hue and Jz curves, pre-tone-map, local contrast, gradient, final gain and gamut compression, black and white, recovery and explicit masks, source/target grey, absolute luminance and adaptation ranges, unknown surrounds, sigmoid Q), `stage-residency-test` | predicate refuses | 0 (host) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-mask.pp3` (CIE mask, band luminance curve, recovery 0.5/decay 3), `local.adjustments` | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30,419,712) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 15-case common-mask matrix (log and CIE bands, narrow and inverted bands, recovery at decay 2 and 4, colour, blur and a no-mask control), `stage-residency-test` | device vs host block | 0.106689 LSB16 (worst: CIE recovery decay 4) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 16-case common-mask refusal matrix (deltaE, blend, radius, chroma, chroma/hue curves, recovery and decay ranges, the recovery upper half, CIE blur/gamma/contrast/structure/tool, the local and wavelet mask curves), `stage-residency-test` | predicate refuses | 0 (host) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-capture.pp3` (manual contrast, 5x5, 25 iterations), `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB (AE=25,669 of 30,419,712, PAE 1 code) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 12-case local capture-sharpening matrix (3x3/5x5/7x7/9x9/13x13, 1..40 iterations, early stop on/off, recovery 50, an ROI clipped at the image corner, combined with blur, tone map and CIE), `stage-residency-test` | device vs host block | 0.091553 LSB16 (worst: combined with CIE; the pure capture cases are exact) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 9-case local capture-sharpening refusal matrix (auto contrast, gamma, corner boost, sigma/iteration/contrast/recovery/sensitivity ranges, mask view), `stage-residency-test` | predicate refuses | 0 (host) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `localadjustments-exposure.pp3` (128x65 ROI, `laplacexp=0.4`, `linear=0.2`, `expcomp=0.7`), `local.adjustments` | off vs auto, 16-bit TIFF | 1 LSB (62 of 30,419,712) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 15-case local exposure PDE matrix (64x64, 65x65, 80x80, 96x96, 100x100 and 128x128, `laplacexp` 0.11/0.3/0.4/0.5/0.8/1.0, `linear` 0.05/0.2/0.3/0.9, `expcomp` 0/0.4/0.7/1.5/-0.5, highlight compression and thresholds, `balanexp` 0.8/1.0/1.2, `gamm` 0.2/0.4/0.6, sensitivity 0/30/50/60/90/100, the 0.001 exposure fixup), `stage-residency-test` | device vs host block | 0.030518 LSB16 (worst: 0.7 and 1.5) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 2-case local exposure ROI refusal matrix (200x200 and 50x50, outside the admitted `fftw_size[]` set) and the gamma-wrapper residual, `stage-residency-test` | predicate/launcher refuses | 0 (host) | PASS |
| 2026-11-03 | `7fc43d8a3..uncommitted` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-denoise-expert.pp3` (Expnoise, `complexMethod=expert`, `sliMethod=cur`, `mixMethod=mix7`, non-identity `wavdenoise`, `Levelsigm=0.7;1.6`, `Leveldenoise=5;4`, `Limden=0.4`), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB (AE=1, 15,637 of 30,419,712) | PASS |
| 2026-11-03 | `7fc43d8a3..uncommitted` | POCL (CPU) | 8-case expert-pairing matrix (`sliMethod` cur and sli+Thrden, `Levelsigm` 1/1, 3/3, 3/0.05, 0.05/3, 2.5/1.5, 0.05/0.05, 1.5/0.75, `Limden` 0, 0.5, 0.75 and 1, contrast, edge, final curves, odd 179x161), `stage-residency-test` | device vs host block | 0.158691 LSB16 (worst: odd all panels) | PASS |
| 2026-11-03 | `7fc43d8a3..uncommitted` | POCL (CPU) | 4-case expert refusal matrix (`Levelsigm` top and bottom out of range, `Limden` above range, non-identity `Wavdenoiseh`), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-03 | `b3fed699f..uncommitted` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-ba-sli.pp3` (`Expfinal`, `BAmethod=sli`, `TMr`, `Balance=60`, `Iter=1`, `Sigmadir=1.2`) and `labwavelet-ba-cur.pp3` (`BAmethod=cur`, non-identity `opacityCurveW`, `Balance=-20`, `Iter=-2`), `lab.wavelet` | off vs auto, 16-bit TIFF | 0 (AE=0, PAE=0, both) | PASS |
| 2026-11-03 | `b3fed699f..uncommitted` | POCL (CPU) | 10-case balance-chroma matrix (both methods, `Expfinal` on/off, `TMr` on/off, `Iter` -3/3, `Sigmadir` 0.4/2.5, `Balance` -30/100, final curves with/without, `TMr`+chroma, `TMr`+contrast, denoise and the combined set), `stage-residency-test` | device vs host block | 0.052734 LSB16 (worst: `cur`+`TMr`+contrast; the rest exact or <=0.04) | PASS |
| 2026-11-03 | `b3fed699f..uncommitted` | POCL (CPU) | 11-case balance-chroma refusal matrix (unknown method, `cur` without a curve, `Balance`/`Iter`/`Sigmadir` out of range, no gate, `TMr` without a method, `TMr`-only dead trim), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-03 | `ed9f2a1e7..f53d06d4c` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-edge-median.pp3`, `labwavelet-edge-nplow.pp3`, `labwavelet-edge-nphigh.pp3`, `labwavelet-edge-lipst.pp3` and `labwavelet-edge-sl.pp3` (Medianlev, NPMethod low/high with Lipst, Lipst influence, EDmethod SL), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB (940-1,196 of 30,419,712) | PASS |
| 2026-11-03 | `ed9f2a1e7..f53d06d4c` | POCL (CPU) | 15-case edge Lipschitz matrix (3x3 thresholds 20/40/65, 5x5 85/95, Lipst influence, NPMethod low/high and low at threshold 0, EDmethod SL, contrast, denoise, final curves, combined arms, padded 179x49), `stage-residency-test` | device vs host block | 0.023438 LSB16 (worst: 5x5 threshold 95) | PASS |
| 2026-11-03 | `ed9f2a1e7..f53d06d4c` | POCL (CPU) | 14-case edge refusal matrix (unknown NPmethod/EDmethod, `edgedetect`/`edgedetectthr`/`edgedetectthr2`/`edgesensi`/`edgeampli` out of range, plain slider ranges, unknown `Medgreinf`, zero strength, L skin protection), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-03 | `d8b09ee47..uncommitted` | POCL (CPU) | colorspace_flowers.pef / `labwavelet-denoise-agre.pp3` (Expnoise, `quaMethod=agre`, `Lev0..4` non-zero, `Ballum=7`), `lab.wavelet` | off vs auto, 16-bit TIFF | 1 LSB (30,024 of 30,419,712) | PASS |
| 2026-11-03 | `d8b09ee47..uncommitted` | POCL (CPU) | 13-case BiShrink matrix (levels 3/5/6, ballum 10/7/5/0/-2, odd 179x161, odd height 180x161, contrast, edge, final curves, toning, all panels padded, Thrden 5), `stage-residency-test` | device vs host block | 0.037109 LSB16 (worst: all panels 179x161) | PASS |
| 2026-11-03 | `d8b09ee47..uncommitted` | POCL (CPU) | 4-case BiShrink refusal matrix (unknown `quaMethod`, `agre` with Ballum above range, `agre` with a level slider above range, `agre` at the 128-pixel window), `stage-residency-test` | device vs host block | 0 (host) | PASS |
| 2026-11-02 | 89e0a844b..47b9980fb | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` (Lab curves with contrast 30, L/a/b/cc/lc/cl tables), `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | 89e0a844b..47b9980fb | POCL (CPU) | 3-case Lab-contrast matrix (contrast 5, contrast 15 with L curve+brightness, contrast 30 with all six tables), `stage-residency-test` | device vs host block | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` with `Gamutmunse=XYZ`, `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` with `Gamutmunse=XYZREL`, `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | 4-case lab.tools XYZ/XYZREL Lab-curve matrix (XYZ with all six tables on the saturated image, XYZREL, XYZ neutral, XYZ with the three hue curves), `stage-residency-test` | device vs host block | 0.753906 LSB16 (worst: XYZ with hue curves, 0 samples over 1); XYZ 0.317383, XYZREL 0.066406, XYZ neutral 0 | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-labcurve-contrast.pp3` with `Gamutmunse=LAB`, `lab.tools` | off vs auto, 16-bit TIFF | 0 (exact) | PASS |
| 2026-11-02 | uncommitted | POCL (CPU) | 3-case lab.tools LAB Lab-curve matrix (LAB with all six tables on the saturated image, LAB neutral, LAB with the three hue curves), `stage-residency-test` | device vs host block | 0.011719 LSB16 (worst: LAB with hue curves); LAB all tables 0, LAB neutral 0 | PASS |
| 2026-11-02 | 5ff287bda..dba2cc58a | POCL (CPU) | colorspace_flowers.pef / `labwavelet-clarity.pp3` (contrast + clarity `one`, black background, `MergeL/MergeC=60`), `lab.wavelet` | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30 419 712) | PASS |
| 2026-11-02 | 5ff287bda..dba2cc58a | POCL (CPU) | colorspace_flowers.pef / `labwavelet-clarity-sup.pp3` (contrast + clarity `sup`, residual background, `MergeL/MergeC=30`), `lab.wavelet` | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30 419 712) | PASS |
| 2026-11-02 | 5ff287bda..dba2cc58a | POCL (CPU) | 20-case clarity matrix (`one`/`inf`/`sup`, every `Backmethod` branch, zero and non-zero merge sliders, Dirmethod one/two/thr, Lmethod 2..7, trimmed 7-level first pass with highlight/shadow, contrast, chroma and denoise), `stage-residency-test` | device vs host block | 0 (exact) except denoise 0.015625 LSB16 | PASS |
| 2026-11-02 | 5ff287bda..dba2cc58a | POCL (CPU) | 8-case clarity residual matrix (unknown CLmethod/Backmethod/Dirmethod, Lmethod -1/11, mergeL 101, mergeC -51, softrad), `stage-residency-test` | device vs host block | 0 (host) | PASS |

| 2026-11-03 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-blackwhite-toning.pp3` (B&W ChannelMixer mixer weights, gamma TRC and before/after curves; LabGrid; Lab L/a/b curves; shadows/highlights; vibrance), `lab.tools` | off vs auto, 16-bit TIFF, `impl=opencl` | 0 (AE=0, PAE=0, 0 of 30 419 712) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 14-case `lab.tools` black-and-white matrix (B&W alone and with shadows/highlights, local contrast and vibrance; B&W + LabGrid, with and without Lab curves, shadows/highlights and vibrance; B&W + Splitco; B&W + Splitlr with Lab curves; B&W + B&W toning controls, with and without LabGrid; B&W + Lab regions; B&W + LabGrid + regions), `stage-residency-test` | device vs host block | 0.75 LSB16 (worst: B&W + Splitlr + Lab curves, 1 sample of 25 776); the rest exact | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | black-and-white non-vacuity and flag encoding under colour toning: with B&W off the node's output is bit-identical, the exported black-and-white flag is 0 (and 1 with the toning off), and the toning keeps chroma on both sides (max 20 007-125 273); a `LabRegions` profile outside the bounded set stays a host refusal in the existing residual matrix | device vs host block | 0 (flag and non-vacuity), 31 511 LSB16 for the toning-off variant | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labtools-huecurve-dense.pp3` (chromaticity 50 with a 64-knot C=f(H), a 48-knot L=f(H) and a 32-knot H=f(H), all linear so the polyline is exact), `lab.tools` | off vs auto, 16-bit TIFF, `impl=opencl` | 0 (AE=0, PAE=0, 0 of 30 419 712) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 8-case dense C=f(H) matrix (512, 4 096 and 40 000 steep knots; 8 000 smooth knots; 40 000 with the chromaticity slider at 50; 40 000 with the L curve and a/b tables; 8 000 smooth with the L curve and a/b tables; 4 096 knots from x=0.2, which wraps the polyline through the period), `stage-residency-test` | device vs host block | 0 (exact, every case) | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | dense hue-curve non-vacuity: every dense case carries one exported table node per control point (512-40 000, plus the wrap boundary) and differs from the same profile with the family off (PAE 4 086-30 851 LSB16); `labtools-huecurve-dense.pp3` against the same profile with the three curves cleared changes 28 463 057 of 30 419 712 samples (PAE 22 575 LSB16) | device vs host block | 0 (device), non-zero against the family off | PASS |
| 2026-11-03 | uncommitted | POCL (CPU) | 100 000-point alternating C=f(H): more knots than the slot holds and more kinks than the refiner can subdivide, so the predicate refuses it | device vs host block | 0 (host, `impl=host`) | PASS |

| 2026-11-04 | uncommitted | POCL (CPU) | 4-case thumbnail RGB matrix (8-bit and 16-bit source grids x tone-only and tone+shadow+RGB-curves+channel-mixer, 24x17 padded rows), `stage-residency-test` | device vs independent reference | 0 (exact, PAE 0.000000 LSB16) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `thumb-rgb.pp3` (exposure + rotation), `thumbnail.rgb` via the thumbnail stage policy, no `RT_OPENCL_RGBPROC` | off vs stage device, PPM | 0 (byte-identical: embedded 239x160 and raw 238x160) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | the same sidecar plus `[Channel Mixer]` (admitted sub-tool) | off vs stage device, PPM | 1 LSB8 (AE=1 of 114 240 raw samples; PAE=1) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | the same sidecar plus `[Black & White] Enabled=true` (refused sub-tool) | off vs stage device, PPM | 0 (host; no `thumbnail.rgb` record, byte-identical) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 5-case thumbnail Lab matrix (8-bit and 16-bit Lab grids; LabGrid; Lab curves; Lab curves plus vibrance; black-and-white), 24x17 padded rows, `stage-residency-test` | device vs the reduced path's skip-16 host sequence | 0 (exact, PAE 0.000000 LSB16, 0 of 1 224 samples) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `labgrid.pp3` (LabGrid only; admitted sub-tool), `thumbnail.lab` via the thumbnail stage policy | off vs stage device, PPM | 0 (byte-identical: embedded 239x160 and raw 238x160) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | the same image with `labcurve.pp3` (Lab curves), `vib.pp3` (vibrance) and `labfull.pp3` (all three), `thumbnail.lab` | off vs stage device, PPM | 0 (byte-identical, both thumbnail branches, every profile) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | `thumbnail.lab` with no admitted tool (empty profile) | device request vs host fallback | no record; the call site keeps the block on the host | PASS |
| 2026-11-04 | `2b3fcd5a9..4a8b1ae85` | POCL (CPU) | colorspace_flowers.pef / `working-trc-residual.pp3` (soft light on, strength 50), `working.trc` | off vs stage device, 16-bit TIFF | 1 LSB (1.5259e-05; 547,600 of 30,419,712) | PASS |
| 2026-11-04 | `2b3fcd5a9..4a8b1ae85` | POCL (CPU) | 31-case `working.trc` matrix (five TRC values, gamut on/off, `wapsat` 0/0.5/1, soft light off/disabled/zero/3/30/50/60/100, a bright ramp, odd width, out-of-gamut, midtone-equaliser, opacity-curve, strength 101 and -1), `stage-residency-test` | device vs host block | 0.099609 LSB16 (soft light srgb worst) | PASS |
| 2026-11-04 | `2b3fcd5a9..4a8b1ae85` | POCL (CPU) | `working.trc` soft-light kernel against the host `softLight()` on the production 3888x2608 Lab, and non-vacuity at strength 50 | device vs host block / tool off | 0 (bit-exact in the kernel; 17,136 of 17,136 samples change at PAE 4,282.638672 LSB16 in the node) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 38-case output-sharpen switch matrix (blur radius 0.25-2.5, halo 0-100, edge radius 0.4-3.0 at tolerance 20/100/500, edges+halo, blur+halo, blur+edges, two smooth-plane edges cases, RLD damping 20/50/100 at radius 0.45/0.75/1.0, damping+blur and damping amount 500), `stage-residency-test` | device vs host block | 0.633 LSB16 (worst: edges only on the smooth plane with a flat mask; USM 0.541, RLD 0.523, damped 0.313) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | output-sharpen switch non-vacuity: halo, blur radius, edges only and RLD damping each change the device result against the plain USM arm on the same source | device vs plain arm | 319-1948 of 1953 samples, peak 50.8-7653.4 LSB16 | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | output-sharpen refusals: `showMask`, a non-positive edge tolerance, `blurradius=25` (double Gaussian), `deconvradius/scale=1.5` (separable), a padded row and a padded plane, on the device and in the builtin | predicate/launcher refuses | 0 (host; 1953 samples untouched) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `sharpen-usm.pp3`, `sharpen-rld.pp3`, `sharpen-rld-damped.pp3` and `sharpen-usm-halo.pp3`, `output.sharpen` | off vs auto, 16-bit TIFF | 1 LSB (1.5259e-05; 24,447 / 20,651 / 12,678 / 21,706 of 7,604,928), all `impl=opencl` | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / USM edges only with a flat mask, `output.sharpen` | off vs auto, 16-bit TIFF | 4 LSB (4.578e-05; 605,915 of 7,604,928), `impl=opencl` | NOTE |
| 2026-11-04 | uncommitted | POCL (CPU) | 9-case `raw.preprocess` scale matrix (padded 67-wide rect for a 63x31 view, full frame, one pixel, one column, one row, odd origin, RGGB/BGGR/GRBG/GBRG), `stage-residency-test` | device vs host loop | 0 (bit-exact, chmax exact on all three channels) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | `raw.preprocess` non-vacuity: every case changes the mosaic against its input | device vs input | non-zero, all cases | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | `raw.preprocess` refusals: wrong argument count, U8 image, two-plane image, seven coefficients, two-row coefficients, short row-max buffer, undersized row pitch, four-colour filters, negative origin | launcher refuses before upload | 0 (host; 153 samples untouched) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / neutral, `raw.preprocess` scale arm | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30 419 712), `impl=opencl` | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 45.7 MP NEF / neutral, `raw.preprocess` scale arm | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 136 918 080), `impl=opencl` | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 13-case `raw.demosaic` matrix (mono 63x31, 17x9, 17x1, one column, one pixel, idle filters word; split RGGB/BGGR/GRBG/GBRG, a mixed filter word at 17x17, one row, one column), `stage-residency-test` | device vs host loop, three planes | 0 (bit-exact, every output pixel written) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | `raw.demosaic` non-vacuity: every plane is written from the `-1` sentinel, the mono plane equals the mosaic and the split planes sum to it | device vs sentinel and mosaic | all cases | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | `raw.demosaic` refusals: wrong argument count, U8 mosaic, two-plane red, mismatched size, shared buffer, mono flag 2, filters out of range, four-colour filters | launcher refuses before upload | 0 (host; three planes untouched) | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | colorspace_flowers.pef / `raw-demosaic.pp3` (`Method=mono`), `raw.demosaic` MONO arm, default internal profiles | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 30 419 712), `impl=opencl` | PASS |
| 2026-11-04 | uncommitted | POCL (CPU) | 45.7 MP NEF / `raw-demosaic.pp3`, `raw.demosaic` MONO arm, default internal profiles | off vs auto, 16-bit TIFF | 0 (AE=0, 0 of 136 918 080), `impl=opencl` | PASS |

The `raw.preprocess` rows cover the Bayer arm of `scaleColors()` only. The
node is a partial block: the prelude that derives the black offsets and the
multipliers, and every other preprocess step, stays on the host. A profile that
selects the node records `impl=opencl`; an unselected run never calls the node,
so the default path is unchanged and emits no record. X-Trans, monochrome and
four-colour filter patterns are refused by the call site, not approximated.

The `raw.demosaic` rows cover the MONO and NONE arms of `nodemosaic()` only.
The block is partial: every interpolating method, and capture sharpening, stays
on the host. The node's arithmetic is assignments, so the three planes are
identical by construction rather than within a tolerance. `raw-demosaic.pp3`
selects `Method=mono`, because the neutral profile leaves AMaZE in place and an
AMaZE run never reaches the node. The fast arm cannot be ported exactly: its
gradient weights use `_mm_rcp_ps`, whose approximation has no portable OpenCL
equivalent.

The clarity rows are the block's two-pass sequence, not a single kernel.
`CLmethod`'s `one`/`inf`/`sup` selection is `finalContAllL()`'s and
`ContAllAB()`'s coefficient zeroing plus the L0 background paint; the device
runs it for every admitted panel set, so `CLmethod` no longer forces the host.
The clarity arm additionally runs `ip_wavelet()` twice: the first pass with
`CLmethod` forced to `"all"`, the second with the profile's selection, then the
`mergeL`/`mergeC` merge. The first pass uses the trimmed level count, because
its `CLmethod` is `"all"`; the second uses the untrimmed count. Highlight and
shadow make the difference observable, since `ContAllL()`'s `kH` table is
built from `maxlvl` even at levels whose multiplier is zero. The first pass
also skips the a/b planes when the block's own trim drops them to zero levels,
which is the contrast-only case; a profile whose `ch[]`, opacity curve,
`cbenab`, `CHmethod`/`chro` or residual sliders would stop the trim short of
zero stays a host residual. The merge's `one`/`inf` + black and `sup` + resid
arms run the second pass; every other `Backmethod` arm collapses to the
clamped first pass, which the device reproduces with the merge kernel's zero
factors. The clarity arm's guided soft radius (`softrad`) is a device case:
the host builds the block's own `FlatCurve`-free guide from the pre-pass
`provradius` Y, derives the subsampling, radius and four bilinear scales, and
the kernels port `guidedFilter()`'s box means, `a`/`b` reduction and bilinear
upsample. `ip_wavelet()`'s final tile copy raises an L at or below zero to one
before the radius reads it; the soft-filtered L itself may be negative and the
merge uses it unclamped.

The final panel's guided soft radius (`softradend`) is a device case for both
the plain path and its two refinements: the `Thrend > 0` coefficient-domain
second block (two extra decompositions, both `Evaluate2` aggregates, the
`wavguid` flat-curve polyline and the per-coefficient `intp`) and the hue
modulation over a non-identity `wavguidcurve`. `Expfinal` is required. The
host derives the blend `LIM01(strend / 100)`, the epsilon
`0.001 * 2^-detend`, the radius `max(int(softradend), 1)` and the subsampled
box grid and four bilinear scales with the block's arithmetic. The device
builds `guide = xlin2log(L / 32768)` and `src = xlin2log(L)`, reuses the
clarity arm's guided-filter chain, keeps the pre-filter L in its own plane and
finishes with `xlog2lin(q)`, `intp(blend, q, iL)`. It runs after the final
pixel curve in both the denoise and the plain branch. Combined with
`softradend`, the clarity arm and the clarity soft radius stay host
residuals; a `softradend` without `Expfinal` is refused.

The filter operates in the log domain, where the derivative back to L is
largest in the highlights, so it amplifies a one-LSB16 difference in its input
L by up to about five LSB16 on the smooth case. The export gate therefore uses
identity final curves, which makes the final pixel curve bit-exact and the
gate `AE=0`. A non-identity final curve can leave a sub-LSB difference that
the radius amplifies; the stage matrix covers that combination within one
LSB16 on identical input, but the end-to-end TIFF can exceed it.

`compare_tiff.py` now copies an already-uncompressed 16-bit TIFF instead of
calling ImageMagick. That is the form RawTherapee's `-t -b16` export writes,
and the gate hosts available here do not carry the ImageMagick binaries.

The XYZ and XYZREL rows use `ICCStore::workingSpaceMatrix()` as
`Color::gamutmap()`'s `p`, which is the host's `wp` in
`chromiLuminanceCurve()`: row-major, with the RGB tristimulus primaries in
the columns. `Color::primaries_to_xyz()` builds the same matrix for the
output-gamut control in `iplab2rgb.cc`; it is not this block's argument. The
device export records `"impl":"opencl"` for `lab.tools` in both rows, and the
device runs `XYZREL` with the block's `0.5` halfway blend, not the local
adjustments' `0.3`.

The LAB row is `Color::gamutLchonly()`'s RGB overload, which is a different
function from the saturation overload vibrance already uses. It converts the
current L and C to XYZ at the block's 65535 scale, multiplies by
`ICCStore::workingSpaceInverseMatrix()` as a double matrix, and iterates on
the RGB clip tests: a negative channel drives the chroma down by 0.96 and
lifts a very low L, and a clipped coloured highlight (`!isHLEnabled`, max
above 65535 with min at or below it) does the same with a lower L. `HH` is
passed only for the ultra-blue L floor, and the function returns nothing but
the corrected L, C and RGB. `isHLEnabled` is `toneCurve.hrenabled`, the same
flag the launcher already passes as `highlight`, and the lower and higher
factors are the block's `0.15` and `0.96`. The arm then rebuilds `lnew`'s L,
a and b from the corrected L and C and runs the Munsell arm after it, exactly
as the host orders the two. The device export above records `"impl":"opencl"`
and is pixel-identical, and the all-tables and neutral matrix cases are
bit-exact; the hue-curve cases are bit-exact as well since the host began
carrying the polyline (below).

The three hue flat curves - C=f(H), L=f(H) and H=f(H) - are carried, not
resampled. `FlatCurve::getVal()` is exactly the faceted polyline the curve
builds, so the host exports one node per knot and the kernel walks it with the
same binary search, the way the wavelet refine arms carry their polylines. The
nodes are doubles: a float knot abscissa in [0,1] is worth `slope * 6e-8` of
curve value, which is 1.2e-4 - 240 times the host's 5e-7 bound - for a 40 000
point alternating C=f(H). The slot holds 65 536 nodes, the most `FlatCurve`
can subdivide (`ppn` is capped at 65 500 and the periodic polylines add the
boundary knots). A hand-built knot list above that falls back to the chord
refiner, which needs a node per kink instead of a node per knot, and a curve
neither representation can follow is refused. An endpoint whose value differs
from its opening one is such a residual: `getVal(1.0)` reads the periodic copy
of the first point, so no continuous table can match the block at both ends of
the period, and the block itself jumps there.

The earlier `labtools-huecurve.pp3` wrote its points as two numbers each, which
`FlatCurve` reads as two control points whose odd slots are the identity value:
the three curves were the identity and only the chromaticity slider ran. The
pp3 now carries four numbers per point, so its row exercises the curves (28 557
791 of 30 419 712 samples change when they are cleared) and is pixel-exact. The
dense `labtools-huecurve-dense.pp3` covers the carried tables directly: 64, 48
and 32 knots of linear segments, all pixel-exact, and 28 463 057 samples
change when the three curves are cleared.

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
- `thumbnail.rgb` runs from the stage policy without `RT_OPENCL_RGBPROC`. Its
  device test matches an independent reference exactly in four cases (8-bit and
  16-bit source grids, tone curve alone and with the shadow curve, RGB curves
  and channel mixer). End-to-end thumbnail PPMs are byte-identical with the
  exposure profile; the channel mixer moves one of 114 240 raw samples by
  1 LSB8. A black-and-white sidecar is refused: no `thumbnail.rgb` record and
  byte-identical PPMs.
- `thumbnail.lab` reuses the `lab.tools` kernel under its own node name and is
  offered by the same stage policy. Its five device cases build the reduced
  path's skip-16 curve tables and split the two thumbnail cache depths (8-bit
  and 16-bit Lab grids) across LabGrid, the Lab curves, Lab curves plus
  vibrance and black-and-white output; all are exact against the production
  host sequence (PAE 0.000000 LSB16). End-to-end PPMs with LabGrid, Lab curves,
  vibrance and all three together are byte-identical on both the embedded and
  raw branches. The residual tools (shadows/highlights, local contrast, Lab
  regions, EPD, soft light, CIE CAM and the working TRC) keep the whole block
  on the host; an empty profile produces no record.
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
- Output sharpening, through `output.sharpen`, runs its residual switches on
  the device: the edges-only bilateral (all twenty-one sigma buckets and the
  `0x20000`-entry range table), halo control, the blur-radius pre-pass and
  damped RLD (both the 3x3 FIR and the IIR forward blur, with the SIMD/scalar
  `dcdamping()` split). Thirty-eight USM and twenty-four RLD cases are within
  0.633 LSB16; halo, blur radius, edges and damping each change the device
  result against the plain USM arm, so the parity is not vacuous.
  `sharpen-usm.pp3`, `sharpen-rld.pp3`, `sharpen-rld-damped.pp3` and
  `sharpen-usm-halo.pp3` are 1 LSB16 and record `impl=opencl`. Show-mask
  stays a host residual because the mask is the output and the host's SIMD
  `_mm_rsqrt_ps()` sigmoid differs from the exact form by 1.8 LSB16 on the
  mask's own scale; a non-positive edge tolerance, `blurradius >= 25` and the
  separable RLD radius are the other named refusals. An edges-only export
  measures 4 LSB16 even though the node is bit-exact on the same input,
  because the upstream device pipeline supplies a slightly different L plane
  and the bilateral's piecewise-constant range table amplifies it; that
  profile is therefore not added to the export gate.
- Local capture sharpening, through `local.adjustments`, runs the `"cap"`
  arm on the clipped ROI: the ROI is converted to working RGB, `RGB2L`
  builds the manual contrast mask, `CaptureDeconvSharpening_SE()`'s tiled
  division/multiplication recurrence runs on the Rec2020 luminance with the
  sigma-selected 3x3..13x13 kernels, each RGB channel is scaled by the
  new/old luminance ratio, and `transit_shapedetect2()`'s senstype-99
  (`lp.senssha`) blend follows. The deconvolution's end-of-row and
  end-of-column tiles overlap their predecessors; the device reproduces the
  host's last-writer chain in four ordered phases (interior, last column,
  last row, corner) so the overlap blends with the already-written value.
  The tile halo, the `itercheck` early exit, the 0.01 skipped-tile floor and
  the 3/5/7/8 border are the host's. The arm matches the host block in
  twelve cases (all five kernels at 0.55/0.75/1.00/1.30/2.00, 1..40
  iterations, early stop on and off, recovery 50, an ROI clipped at the
  image corner, and combined with Blur & Noise, tone mapping and CIE CAM)
  with a worst peak error of 0.091553 LSB16 (tone map 0.030518, blur
  0.015381; the eight pure capture cases are exact). Every case changes
  between 10,736 and 40,323 of 270,000 samples against the tool-off result,
  so the parity is not vacuous. `localadjustments-capture.pp3` is 1 LSB16
  (AE=25,669 of 30,419,712) and records `impl=opencl`. The automatic
  contrast, the gamma wrapper, the corner radius boost and the mask display
  stay fail-closed; the auto-contrast refusal is the same default profile
  the earlier residual matrix used.
- Local exposure, through `local.adjustments`, runs the PDE arm
  (`expmet == 1`, `linear > 0`, `laplacexp > 0.1`) on the device. The tone
  plane is `exlabLocal()`'s highlight/shadow/tone LUT sequence; its arithmetic
  grouping (`L *= hlfactor * pow_F(2, addcomp)`) and the tone curve's
  `LUT_CLIP_OFF` extrapolation are reproduced exactly, so the Laplacian input
  `pow_F(LIM01(L / 32768), gamm) * 32768` is bit-identical to the host
  (`memcmp` 0 of 4,096 samples on the 64x64 case and 0 on every admitted
  size). A direct double-precision REDFT10/REDFT01 pair and the host-built
  cosine/`rex_poisson_dct` tables reproduce FFTW's PDE to 0.005859 LSB16.
  Fifteen cases across 64x64..128x128 (the `fftw_size[]` entries; `optfft()`
  is then the identity) match the host block with a worst peak error of
  0.030518 LSB16; the 0.7 case changes 7,607 of 49,152 samples against the
  tool off, so the parity is not vacuous. `localadjustments-exposure.pp3`
  records `impl=opencl` and matches the `--gpu=off` 16-bit TIFF on 62 of
  30,419,712 samples at 1 LSB16. A 200x200 or 50x50 ROI, the gamma wrapper,
  black point, shadow recovery, chroma, gradient, structure, fat, masks and
  an ROI not in the admitted set fall back to the host block.
- Blur & Noise, through `local.adjustments`, has two arms. The Gaussian arm
  reuses the blur arm's line kernel with the context's host-built
  coefficients at the block's `lp.rad / (sk * 1.4)` sigma; the median arm
  reproduces `Median_Denoise()`'s sorted-window interior, its border copy and
  its pass ping-pong. Both blend with `BlurNoise_Local()`'s senstype-0
  (`lp.sensbn`) scope, whose `mindE` base is four rather than
  `transit_shapedetect2()`'s two and whose deltaE reference is the full-image
  3/sk blur. The Gaussian arm matches the host block in fourteen
  configurations (spot radius 2/5/10/18/28, the luminance, chroma and
  all-planes selectors, sensitivity 0/100, a no-scope deltaE-off case, a
  strict sub-rectangle ROI, RECT, colour, log, soft light, MUNS and 179x49)
  with a worst peak error of 0.023438 LSB16 (colour 0.022949, log 0.015625,
  all-planes 0.015625, ROI edges 0.001953, blur alone 0.007812). Blur alone
  changes 16,634 of 45,696 samples at PAE 7,162.726562 LSB16 against the tool
  off, so the parity is not vacuous. `localadjustments-blur.pp3` records
  `impl=opencl` and its 16-bit TIFF pair differs on 387 of 30,419,712 samples
  at 1 LSB16.
- The median arm matches in fifteen further configurations (3x3/5x5/7x7/9x9,
  1/3/4 passes, chroma, ROI edges, colour, log, 179x49, blur-then-median and
  the denoise curves) with a worst peak error of 0.031250 LSB16 (median 3x3
  and the default curve 0.031250, 179x49 0.023438, colour/log/chroma
  0.015625, ROI edges 0.007629). Median 5x5 changes 8,010 of 45,696 samples
  at PAE 26,979.698730 LSB16 against `medMethod "none"`, so the parity is not
  vacuous. `localadjustments-blur-denoise.pp3` records `impl=opencl` and its
  16-bit TIFF pair differs on 387 of 30,419,712 samples at 1 LSB16, the same
  pair as the blur-only profile.
- The denoise curve is admitted only when `DeNoise()` is idle. `DeNoise()`'s
  outer guard requires the Quality method to be other than "Off" and a
  non-zero curve amount, or the `"enhden"` artifact-shaping path; the GUI
  default is Quality "Off", so a profile carrying the default
  `LocwavCurveden` curve exports identically to one with an empty curve
  (AE=0 over 30,419,712 export samples and over the 45,696-sample device
  fixture, measured on the host). The curve's only
  effects are the `noiself*`/`noiselc` amounts, the `wavcurvedenoi` flag and
  `calc_ref()`'s `isdenoise` blur references, and the last of these is read
  only by `DeNoise_Local()`. Predicate residuals include the default radius
  below the Gaussian arm's 1.6 gate, the guided and inverse arms, a zero
  median method, out-of-range passes, a radius above the 30 FFT gate, an
  unknown chroma method, an out-of-range sensitivity, a non-zero curve with
  Quality "Conservative" and `qualityMethod "enhden"`. The wavelet denoise
  itself (Quality "Conservative"/"Aggressive"/"Non-local means only"), added
  noise, grain, bilateral and non-local means remain host residuals.
- Tone Mapping runs `EPDToneMaplocal()` on the device: the ROI lightness is
  scaled by the host's `gamma / maxL`, logged, decomposed by
  `EdgePreservingDecomposition` (a fixed-iteration preconditioned conjugate
  gradient on a five-diagonal finite-element Laplacian) and blended with
  `transit_shapedetect2()`'s senstype-8 arm, including the `equiltm`
  luminance normalisation the GUI enables by default. The decomposition's
  steps are order-sensitive, so the kernel runs the whole of it in one work
  item and reproduces the host's arithmetic: the ROI minimum and maximum (an
  exact reduction), the edge-stopping weights, the five-diagonal assembly,
  the incomplete-Cholesky column recurrence with its single-prefix fill map,
  the matrix-vector product's three summation regions, the double-precision
  dot products with separate even and odd accumulators, and both the scalar
  and the SSE vector `xexpf()` forms (the host's weight and compression loops
  take the vector form below `4 * (n / 4)` and `4 * (w1 / 4)`). Sixteen
  device cases (strength 0.5/1.2/-0.4/0.8, gamma 1/2, edge stopping
  0.5/1.4/3.5, scale 1/4, reweight 2, the equalise-off arm alone and at
  sensitivity 100, sensitivity 0/100, colour, blur, an offset ROI, a
  zero-strength no-op and a ROI below the block's 128 px mDEN gate) are all
  within 0.023438 LSB16, the equalise-off,
  scale-4, sensitivity and offset-ROI arms within 0.0156, and the zero
  strength and sub-mDEN cases exact. The tone map changes 155,998 of 156,000 fixture
  samples at PAE 350.917969 LSB16 against the same spot with the tool off, so
  the parity is not vacuous. `localadjustments-tonemap.pp3` records
  `impl=opencl` and its 16-bit TIFF pair differs on 3 of 30,419,712 samples
  at 1 LSB16, while the same export against the neutral profile differs by
  PAE 50 codes on 1,929,902 samples. Ten residual configurations (the
  strength, gamma, edge-stopping, scale, repart, saturation, reweight and
  sensitivity ranges, a recovery threshold other than one and the global EPD
  pass) are refused by the predicate. The block's own mDEN gate makes a ROI
  below 128 px on either axis a no-op, not a residual, and the arm needs 18
  ROI-sized scratch planes, so a ROI beyond the device budget (a full-frame
  spot on a 10 MP image) falls back to the host.
- The `lab.wavelet` edge panel's Medianlev arm reproduces `calckoe()` and
  the Lipschitz control. The raw pass runs the 3x3 or 5x5 convolution, the
  `thr` clamp and the `|tmC / temp|` ratio for the level's three
  directions; the running maximum `calckoe()` folds into every coefficient
  is a raster-order prefix maximum, so it is replayed through a rowmax,
  carry and resolve pass rather than approximated by the global maximum.
  The control pass rebuilds the direction-combination `interm` with the
  influence sliders' `eddlipinfl`/`eddlipampl`, `aamp` and the threshold
  clamp. `NPMethod` low/high with `Lipst` replays the in-place 28/40 or
  38/50 neighbour smoothing with one work item per level, because the host
  scan reads its already-updated neighbours. Fifteen device cases (3x3
  thresholds 20/40/65, 5x5 85/95, Lipst influence, NPMethod low/high and
  low at threshold 0, EDmethod SL, contrast, denoise, final curves, the
  combined arms and a padded 179x49 image) match the host block with a
  worst peak error of 0.023438 LSB16; the 3x3 default, 3x3 threshold 65,
  5x5 threshold 85, Lipst, NPMethod low/high and EDmethod SL cases are
  exact. Against the same profile with Medianlev off the host result
  changes by 8,654-114,223 samples, so the parity is not vacuous;
  `NPMethod` none moves the host result by up to 46,899 samples and `Lipst`
  off by up to 11,102, so the smoothing and influence arms are not no-ops.
  `EDmethod` SL is byte-identical to CU because the engine forces
  `EDmet == 2`; the `EDmet == 1` threshold adjuster is unreachable.
  Fourteen residual configurations (unknown `NPmethod` or `EDmethod`, the
  five detection sliders out of range, the plain slider ranges, an unknown
  `Medgreinf`, zero strength and L skin protection) run the host builtin
  byte for byte. The five export profiles record `impl=opencl` and are
  within 1 LSB16 (940-1,196 of 30,419,712 samples differ).
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
  Thirteen residual configurations (softrad, `BAmethod` cur without an
  opacity curve, `tmr`, `expclari`, sigmafin below/above its slider range,
  avoid, median, expbl, expresid, expnoise, CLmethod one and skin
  protection) run the host builtin byte for byte. The coefficient curve now
  evaluates `xlogf()`/`xexpf()` instead of `log()`/`exp()`, which matches the
  host's reconstructed L exactly and keeps the balance-chroma a/b arm, which
  reads that L, within one LSB16. `labwavelet-final.pp3` is exact on the
  export TIFF.
- The `lab.wavelet` balance-chroma arm reproduces `ContAllL()`/
  `ContAllAB()`'s BA multiply. The L arm runs inside the final panel's
  coefficient pass (`Expfinal`) and rebuilds the `sigmadir` beta ramp from
  its own `calceffect()` mea block; the a/b arm runs with `TMr` and
  multiplies by the raw `kba`. `BAmethod = sli` uses the block's
  luminance-dependent 600/300 divisor (the 4970/-4970 ramps outside 20..80)
  and `cur` reads the non-identity `opacityCurveW` through the 501-entry
  LUT; both repeat the multiply by the per-level iteration count (`Iter`
  -3..3). Ten device cases (both methods, `Iter` -3/3, `Sigmadir` 0.4..2.5,
  `Balance` -30..100, with and without the final curves, with and without
  `Expfinal`, `TMr` alone with chroma, contrast and denoise, and the
  combined set) match the host block with a worst peak error of 0.052734
  LSB16; most cases are exact. Each differs from the same profile with
  `BAmethod = none` by 14,156-114,169 samples (PAE 19,137-98,604 LSB16), so
  the arm is not vacuous. Eleven configurations (an unknown method, `cur`
  without an opacity curve, `Balance` outside -30..100, `Iter` outside
  -3..3, `Sigmadir` outside 0.025..2.5, neither gate, `TMr` without a
  method and the `TMr`-only profile whose L trim differs from the a/b
  count) run the host builtin byte for byte. `labwavelet-ba-sli.pp3` and
  `labwavelet-ba-cur.pp3` record `impl=opencl` and are pixel-exact (AE=0) on
  the export TIFF.
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
- The `lab.wavelet` standard denoise arm runs `WaveletDenoiseAllL()` and the
  chroma arm `ip_wavelet()` forces when `Expnoise` selects six levels: the
  block raises the L decomposition to seven levels, sets `cp.chromfi = 0.01`
  and denoises the a/b bands with `WaveletDenoiseAllAB()` using the L
  coefficients after the contrast, edge and final passes. The device keeps a
  second band set for those L coefficients, computes `Mad()` from a 32768-bin
  device histogram, reproduces the host's `noisevarlum`/`noisevarchrom` last
  write at the clamped `(2y+1, 2x+1)` sample, and ports `ShrinkAllL`/
  `ShrinkAllAB` with the lab.tools box blur. The a and b planes keep
  `ip_wavelet()`'s base decomposition level, which `levwava`/`levwavb` clamp
  to six even when the forced chroma denoise raises L to seven, so each plane
  runs its own analyse/statistics/synthesise count. The final tail reuses the
  pre-denoise statistics unless `cp.val > 0 || ref || contr || cp.diagcurv`;
  the host evaluates that condition and passes it as `final_refresh`. The
  kernels use `exp()` where the host uses `xexpf()`; the measured worst peak
  error is 0.015625 LSB16, well inside one LSB, so the SLEEF port is not
  needed. Seventeen device cases (levels 3/5/6, ballum 10/7/5/0/-2, high-level sliders only, a light slider, odd
  width, odd height, odd both, with contrast, edge, final curves, toning, all
  panels and all panels padded) match the host block; the panel-off comparison
  changes 85,918-114,240 samples per case, so the parity is not vacuous.
  Eleven residual configurations (an unknown `quaMethod`, user chromfi/chromco,
  balchrom, a non-identity `Wavhuecurve`, `CBenab`, Thrden,
  Ballum outside its widget range, a level slider outside its range, all
  sliders zero and the 128-pixel window) run the host builtin byte for byte.
  `labwavelet-denoise.pp3` is 1 LSB16 on the export TIFF (26,639 of
  30,419,712 samples) and records `impl=opencl`. Every comparison uses
  `OMP_NUM_THREADS=1`: the host's `noisevarlum` loop writes each half-grid
  cell four times under
  `#pragma omp parallel for`, so a multi-threaded host run is not
  deterministic and cannot be a parity reference. The denoise path holds a
  second seven-level L band set while a and b run; a 45.7 MP frame exceeds
  the 1 GiB default stage budget and falls back to the host, recorded as
  `impl=host`, so large-frame denoise is not a device case yet.
- The `lab.wavelet` denoise arm also admits `quaMethod == "agre"`, the
  BiShrink pair. `ip_wavelet()` runs `WaveletDenoiseAll_BiShrinkL()` and then
  `WaveletDenoiseAllL()` on L, and each a/b plane gets
  `WaveletDenoiseAll_BiShrinkAB()` and then `WaveletDenoiseAllAB()`. At the
  export call site's `edge == 6` the L pair's per-level formula is the same
  `ShrinkAllL()` arithmetic with the same `madL[lvl]` and `vari[lvl]`, and its
  coarsest level calls `ShrinkAllL()` outright, so the L arm is the standard
  tail twice over the same squared MAD; dropping the second pass moves the
  device result by 8,052 LSB16, so the pair is not an alias for a single
  pass. The chroma pair is a different formula: levels 0..4 multiply by
  `SQR(1 - exp(-(mag_ab / (nvc * noisevar_ab * madab)) - mag_L / (9 * madL)))`
  with no box blur, and level 5 runs `ShrinkAllAB()` with the pre-denoise
  squared MAD. The standard pass that follows rebuilds `Mad()` from the
  post-pair bands, which the host also does because `ShrinkAllAB()` measures
  the band it is about to shrink, so the device runs the MAD histogram and
  `compute_mad()` a second time for that plane. `noisevarab_r` is the block's
  hard-coded 100.f and lives in `RT_LAB_WAVELET_DENOISE_NOISEVAR_AB`, not in
  the parameter table. Thirteen device cases (levels 3/5/6, ballum
  10/7/5/0/-2, odd 179x161, odd height 180x161, contrast, edge, final curves,
  toning, all panels padded and the Thrden refinement) match the host block
  with a worst peak error of 0.037109 LSB16, and each differs from the same
  profile with `quaMethod = "cons"` by 15,956-60,007 LSB16, so the pair is not
  vacuous. Four residuals (an unknown `quaMethod`, `agre` with Ballum or a
  level slider outside its widget range, and `agre` at the 128-pixel window)
  run the host builtin byte for byte. `labwavelet-denoise-agre.pp3` is 1 LSB16
  on the export TIFF (30,024 of 30,419,712 samples) and records `impl=opencl`.
  Every comparison still pins `OMP_NUM_THREADS=1` for the host's
  `noisevarlum` race. `d8b09ee47`.
- The `lab.wavelet` denoise-curve refinement (`sliMethod == "cur"`) ports
  `ip_wavelet()`'s post-denoise curve multiply. The host samples the block's
  own `WavCurve` over `wavdenoise` into 501 entries, detects `cp.denoicurv`
  exactly as the block does (any of the first 500 entries differs from one)
  and uploads the curve in the refinement tail. `cp.denmet` is hard-coded to
  four, so the only level factors are 0.3 on levels 0 and 1 and one on levels
  2 and 3; each level iteration writes all four bands. The host evaluates
  `Evaluate2()` before and after `WaveletDenoiseAllL()`, reproduces the
  near-zero early-out, computes the `mixmet` weight (0, 0.5, 0.7, 1), the
  `intp()` post/pre mix and the per-level `logmax`/`rap`/`asig`/`bsig`/
  `amean`/`k`/`maxVal` on the same doubles the block uses, and passes them as
  scalars. The kernel ports `xlogf()`/`xexpf()`/`pow_F()` from
  `rtengine/sleef.h` rather than substituting the platform `log()`/`exp()`,
  because a one-ulp abscissa difference can move the refinement factor. The
  pre-denoise L set is snapshotted into the free `bands` set before
  `denoise_l`, so the memory budget is unchanged, and the refinement is
  launched direction-major, level-minor, writing levels 0..3 on every level
  iteration exactly as the block does. Ten device cases (mixmet 0/1/2/3, an
  identity curve, contrast, edge, final curves, odd 179x161, all panels) match
  the host block with a worst peak error of 0.043945 LSB16; the sli/cur host
  comparison changes 86,454-114,237 samples per case, so the refinement is not
  vacuous. The identity-curve case runs the device denoise without the
  refinement, as the block does. Three residual configurations (sigma above
  its widget range, the expert level pairing and Thrden) run the host builtin
  byte for byte. A user chroma strength is admitted and provably inert for
  this arm: the block's `variC` is dead at the forced `local == 1`, and
  `cp.chromco` is 0.1 times a 0..100 slider, so its `> 30` ramp test is
  unreachable. The three admitted cases are device-exact against the host and
  the host output is byte-identical with the sliders zeroed.  With `sliMethod == "sli"` and a non-zero `Thrden`, the refinement instead
  samples the block's four-point `wavlow` flat curve. The host builds the same
  `FlatCurve` the block builds, exports its faceted polyline through the new
  `FlatCurve::polylineX/Y/Slope()` accessors, and the kernel walks it with the
  block's periodic wrap, binary search and linear interpolation in double.
  Four cases (Thrden 1, 5, 20 and 20 with the final curves) match the host with
  worst 0.050781 LSB16, and each differs from `Thrden = 0` (PAE 12,476 LSB16 or
  more), so the refinement is not vacuous; `labwavelet-thrden.pp3` records
  `impl=opencl` at 1 LSB16. `labwavelet-denoise-cur.pp3` is 1 LSB16
  on the export TIFF (AE=1, 21,068 of 30,419,712 samples), records
  `impl=opencl`, and differs from the same profile with `sliMethod=sli` at
  30,064,859 of 30,419,712 samples (PAE 1600 LSB16), so the export exercises
  the refinement. An unknown `mixmethod` and a sigma outside the widget range
  stay host residuals.
- The expert level pairing (`complexMethod == "expert"`) runs the same
  refinement at `levref == 6`. `Levelsigm`'s top value is `siglh` for levels
  0..3 and its bottom for levels 4..5; every level iteration rewrites bands
  0..3 with the curve factor, the first four add the `wavmed` factor when
  `Limden` is positive, and all six rewrite bands 4..5 with `wavhigh`, whose
  breaking point is `Leveldenoise`'s lower value. `cp.denmet` still applies
  only to the low factor, so the `wavmed`/`wavhigh` factors are derived from
  their own curve values with the shared `reduceeffect`. The host exports
  `wavmed`/`wavhigh` in the same faceted-polyline encoding as `wavlow`, the
  kernel takes six post/pre bands plus an `expert` flag and leaves the
  standard path's writes untouched, and the launcher re-checks the level
  count, the two knot counts and the flag. Eight cases (both `sliMethod`
  arms, `Levelsigm` ends 1/1, 3/3, 3/0.05, 0.05/3, 2.5/1.5, 0.05/0.05 and
  1.5/0.75, `Limden` 0, 0.5, 0.75 and 1, contrast, edge, final curves and an
  odd 179x161 grid) match the host with worst 0.158691 LSB16. Each differs
  from `complexMethod=normal` by 0.25 to 32,124 LSB16, so the six-level
  pairing is not vacuous. Four configurations (`Levelsigm` top and bottom
  outside 0.05..3, `Limden` outside 0..1, and a non-identity `Wavdenoiseh`)
  run the host builtin byte for byte. The `Wavdenoiseh` curve is inert for
  this arm today — `cp.denoicurvh` only re-arms the `Evaluate2` gate at
  `ipwavelet.cc:1219`, and the `levwavL` restore at `1155-1160` is commented
  out — but the device does not model it, so the arm fails closed rather than
  depend on that coincidence. `labwavelet-denoise-expert.pp3` records
  `impl=opencl` at 1 LSB16 (AE=1, 15,637 of 30,419,712 samples) and differs
  from the same profile with `complexMethod=normal` on 29,442,428 samples
  (PAE 1084 LSB16), so the export exercises the pairing.
- Impulse denoise, defringe, ML sharpen edges and ML microcontrast, through the
  `lab.sharpening` node, run as one atomic block in production order
  (`impulsedenoise`, `defringe`, `MLsharpen`, `MLmicrocontrast`). The device
  test compares the observable corrected sets and all three planes: the
  impulse fixture and the combined pair correct exactly the host's pixel sets,
  the impulse pass is bit-identical on a 128x80 random field and on the
  fixture (`AE=0`), and the defringe pair is `PAE=0` on L and 0.000977 LSB16 on
  a/b. `xatan2f()` is ported from `rtengine/sleef.h` because a device `atan2`
  one ulp away moved a fringe across the `fringe <
  threshfactor` decision: the export corpus went from 1010 LSB16 to 1 LSB16
  with the port.
- ML sharpen edges is bit-identical to the host (`AE=0`) for one to four
  passes, both channel arms (L only and L/a/b) and the CIE-CAM arm; the
  working copy is the host's `channel / 327.68f`, and the interior 2..W-3 by
  2..H-3 write window, the `v < 92` L guard and the `SQR(f2 * f3)` product
  grouping are all part of the port. ML microcontrast is within 0.037 LSB16
  (worst 0.036133): the host's `buildBlendMask` sigmoid uses the SSE
  reciprocal square root, while the device evaluates `x / sqrt(1 + x*x)`
  exactly, so the mask differs by a fraction of an LSB. Eleven device cases
  cover edges at 1/2/3/4 passes and 35..100 strength, both channel arms, the
  3x3 and 5x5 microcontrast matrices across the amount/contrast/uniformity
  ranges, the impulse+defringe pair combined with each arm, and the CIE-CAM
  arm (edges only; impulse, defringe and microcontrast are skipped there).
  Each case changes pixels, so none is vacuous.
- `labsharpening.pp3` (impulse 50, defringe radius 2.0 and threshold 13) is
  1 LSB16, `labsharpening-edges.pp3` (three-channel edges, two passes,
  strength 50) is `AE=0` on the export corpus, `labsharpening-micro.pp3`
  (5x5 matrix, amount 20, contrast 20, uniformity 5) is 1 LSB16, and the two
  radius gates `labsharpening-fir3.pp3` (radius 0.5) and
  `labsharpening-iir.pp3` (radius 1.0) are 1 LSB16; all record
  `impl=opencl`. A one-refused pair (impulse on, a prescaled defringe
  threshold 101) is `impl=host`, byte-identical to the CPU path.
- The defringe radius covers the whole legal range on the device because
  `PF_correct_RT()` requests the default `GAUSS_STANDARD` blur: below 0.25 it
  is the identity (a host no-op), from 0.25 to below 0.6 the 3x3 convolution,
  from 0.6 the separable-IIR pair, and the 5x5/7x7 kernels are only selected
  for `GAUSS_MULT`/`GAUSS_DIV`. The 3x3 arm reuses `rt_sharpen_fir3` and is
  bit-identical to the host (`PAE 0`) at radii 0.25, 0.5 and 0.59; the IIR arm
  is within 0.000732 LSB16 at 0.6, 1.0 and 1.15. Combined with edges and
  microcontrast the worst radius case is 0.012695 LSB16 on L and 0.004883 on
  a/b, and every case changes pixels. Eleven radius device cases cover the
  four arms, both channel combinations and the main pass beside the block.
  A radius outside 0..25, a
  threshold outside 0..100, an edge pass count outside 0..4, an edge amount
  outside 0..100, a microcontrast amount outside 0..100, a contrast outside
  0..200, a uniformity outside 0..10, an image narrower than its defringe
  window and a non-fp64 device are residuals. A defringe radius below 0.25,
  zero edge passes, an edge amount of 0, microcontrast amount below 1 and
  contrast exactly 100 are the host no-ops and leave every plane untouched.
- The CIE-CAM arm of `lab.sharpening` is device for the only pass it owns on
  the Lab image: ML sharpen edges at radii 0.5 and 1.0, with and without the
  main sharpen pass, is bit-identical (`AE=0`) and non-vacuous. The defringe
  hue table is not built there, with defringe disabled, or below the Gaussian
  identity, so a curve the device table cannot follow cannot refuse a pass
  that never reads it; impulse denoise, defringe and microcontrast run in the
  CIE-CAM pipeline on the `CieImage`.
- The separate `ciecam` node runs the single-pass CIE-CAM 02 model on the
  device: the CAT02 forward and inverse transforms, the four algorithm arms,
  the two brightness tables, explicit and automatic scene grey, the surround
  classifications, the hue rotation and the optional Lab gamut control. Twelve
  residency cases cover those axes at 68x16, 64x16 and the odd-width 179x17;
  the worst peak error is 0.107422 LSB16, every device case changes pixels, and
  the two residual cases plus the colourfulness case are byte-identical host
  executions. On the export corpus `ciecam.pp3` (tone, hue, auto grey and
  gamut) has a peak error of 1 LSB16 with `impl=opencl`, and
  `ciecam-residual.pp3` (CAM16) is `AE=0` with `impl=host`. The chroma,
  saturation and colourfulness sliders, the per-hue hue/saturation sliders and
  red protection stay host residuals: they drive `Color::skinredfloat()`,
  whose skin-range boundary turns a last-bit hue difference into a visible
  step. Interactive tone mapping (`tonecie` with EPD), automatic CIELAB
  sharpening, `datacie` and the CAM16/PQ models are residuals too.
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

## Vendor containment

The stage binaries load the ICD loader at runtime, so the OpenCL platform is
chosen by `OCL_ICD_VENDORS` or, when it is unset, by the ambient
`/etc/OpenCL/vendors` set. On a machine with a vendor ICD a faulty kernel
therefore reaches the real GPU: an out-of-bounds access that POCL turns into a
process crash becomes a `gfxhub` page fault, a ring timeout and a mode1 reset
under the vendor driver, losing VRAM and the desktop session.

That happened once, on 2026-11-02 at 22:00:52, when a `stage-residency`
process ran without the override against `amdocl64` on `gfx1200`. Every
device run now goes through `tools/opencl/pocl-run.sh`, which:

- exports a pocl-only `OCL_ICD_VENDORS`, `POCL_DEVICES=cpu` and, when
  unset, `OMP_NUM_THREADS=1`;
- fails closed when the ICD directory holds a non-POCL entry or when
  `clinfo -l` reports anything but one POCL platform;
- masks `/dev/dri` with bubblewrap by default, so even a command that drops
  the override cannot reach the vendor driver (`RT_POCL_CONTAIN=0` opts out).

`tools/opencl/verify.sh` applies the same check to `OCL_ICD_VENDORS` and dies
unless `RT_VERIFY_ALLOW_GPU=1` is set for a deliberate hardware run.

Evidence, 2026-11-03:

| Check | Result |
|---|---|
| `tools/opencl/pocl-env.sh --check` | `POCL only: Platform #0: Portable Computing Language` |
| `tools/opencl/pocl-run.sh env -u OCL_ICD_VENDORS clinfo -l` | no platform; the `/dev/dri` mask blocks the vendor runtime |
| `tools/opencl/verify.sh --list-coverage` without the override | exit 1, "refusing the ambient OpenCL vendors" |
| `tools/opencl/pocl-run.sh ./stage-residency-test` with `RT_OPENCL_ALLOW_CPU=1 RT_REQUIRE_GPU=1` | PASS (1 skipped), 0 FAIL |
