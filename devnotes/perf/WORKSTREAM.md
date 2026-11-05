# RawTherapeeGraph `perf-graph` workstream

Committed copy of the development plan so work can continue on a machine
without the local plan file. It records state, decisions, per-unit
evidence and next tasks; architecture and invariants stay in the sibling
records. Update it with each landed unit.

Refreshed 2026-11-03. P0 blocks 1-4.1 are complete; block 4 device coverage is
the remaining large piece and is scoped below. Stage C has landed local
sharpening (base and damped), the Blur & Noise Gaussian and median arms with
the default denoise curve admitted, local exposure's PDE arm, `crop` and
the `lab.sharpening` impulse-denoise/defringe pair; the remaining Stage C
units are multi-session and are ordered below. Each stable, scanned range is
pushed from 2026-10-29 on.

## State

- Repository: `$REPO`
- Worktree: the main checkout, branch `perf-graph`, clean and in sync with
  `origin/perf-graph`; the workstream tip is `83cfe69ab`. All pushed
  ranges were scanned per the publication gate. Block 4 is complete: the
  device gate is green and the portable package is validated on Debian 12 and
  13; see the progress log and the decisions below.
- Stage C landed 2026-10-31: `local.adjustments` local sharpening (undamped
  `cd38028bb..7467f563f`, damped `211c12597..ebbeab951`), the `crop` device
  node (`544f01573`, `0885d952c`), the `lab.sharpening` impulse-denoise +
  defringe node (`a0040d028..b41aa0de7`), the `lab.tools` black-and-white
  cheap branch (`f744f7bc3..43d6f88bd`), the `lab.tools` admission of rgb.proc
  tone methods (`6c78a463a..8ac8bb389`), the `lab.tools` `bwToning` arm
  (`faf01adfd..9fca53a2d`), the `lab.tools` chromaticity slider + C/L/H=f(H)
  flat curves (`8e4c34e1b..eb3aa5b81`) and the `lab.wavelet` chroma `link`
  arm (`6bb3101da..c1aedc15d`, plus the balance-chroma refusal fix
  `72d7c5f55..bb074e317`). At `bb074e317`, `verify.sh --quick` is
  **43 PASS, 0 FAIL, 0 SKIP**, and `stage-residency-test` is 0 FAIL.
- Contributor: an external contributor accepted write access and self-assigns issues.
  PR #28 (F5 allocation) merged at `7781f6800`. PR #33 (host CI, #31) merged
  as rebased `8831ba35a..775aa41e4`; the workflow is green (CPU and OpenCL,
  15 host tests each, 14m24s) and those two job contexts are now **required
  status checks** on `perf-graph`. Issue #31 is closed and unassigned.
  PR #29 (introduction) merged as rebased `8fff36399..becd88011`, with a
  maintainer addendum recording F5 merged. PR #34 (denoise parity harness,
  #30) is **draft**: one-column/limit4/order0 differs by 182 of 3,538,944
  samples at 1 LSB. Issue #35 (same-binary vs base/candidate determinism) is
  assigned to the contributor; #30 stays assigned until the harness acceptance
  is green. PR #37 (AMaZE scratch reset, test and CI rows) merged as a
  fast-forward at `6e0fd158e`: both builds, the new tile-parity test and
  1-vs-8-thread PEF exports are exact; the old one-thread output differed at
  104 pixels (max 56 LSB16), as documented. PR #36 (SL notes) merged at
  `41d6b5507`.
- Local shadows/highlights is **done and pushed** (`509f90731..5f674e5ba`):
  worst 0.053711 LSB16, full POCL gate 28 PASS, 0 SKIP. Issue #32 is closed
  and unassigned.
- `raw.base` now has two partial device sub-steps: the white-balance multiply
  and optional clamp of `getImage()` (`raw.base.wb`), and the
  camera-to-working-space matrix multiply of `colorSpaceConversion_()` when no
  input or DCP profile is supplied (`raw.base`). PEF and 45.7 MP NEF are AE=0
  with both sub-steps on the device, the `labtools-vibrance.pp3` canary is 0
  differing with the stage nodes alone, and the rest of `raw.base` stays host.
  `verify.sh` sets `RT_OPENCL_RGBPROC` only on the `rgb.proc` row, so the
  classic RGB block no longer perturbs every other row's parity (three clipped
  samples, up to 110 LSB16).
- `perf-graph` is protected (no direct or force pushes by collaborators,
  linear history, one approving review, admins bypass). The claiming rule is
  in `devnotes/perf/CONTRIBUTING.md` and the PR template.
- Check `gh pr list --base perf-graph`, forks, invitations, CI runs and issue
  assignments at session start, before each push and after each block.
- Main checkout stays on `dev` at `4b4cb53be`; `dev` and `perf` must not be
  merged into this branch until the workstream closes.
- Build trees (worktree-relative): `build/cpu-perf-graph`,
  `build/opencl-perf-graph`. Both GCC, Release, generic target, `-j 4`.
- Test targets are **not** part of the default build; build them by name
  (`cmake --build build/opencl-perf-graph --target stage-residency-test`).

### Validated artefact

```
build/portable/rawtherapee-graph-5.13-732-g6bc636307_5.13+git13753.6bc636307-1_amd64.deb
sha256 315a9ea0f7f867a75f95981a77d517fbe90a6fc6e4ec3f546c8cb192670380e4
```

Built 2026-11-05 from `6bc636307` (the P1 closure; the later commits are
documentation) with `tools/build-rt.sh portable`. Validated the same day in
fresh Debian 12 and Debian 13 containers, both PASS: install,
`--list-formats` (35 entries, including PEF/NEF/CR2/ARW, TIFF and JPEG XL),
neutral 16-bit CPU export against the host CLI with the packaged data
directory (PAE 0 LSB16, 0 of 30,419,712 samples, both distributions) and a
GUI smoke test under Xvfb (20 s timeout, exit 124). `--gpu-test` and
`--gpu-test-monitor` were not run: the containers have no GPU. The packaged
binary run on the host also matches the host build with the same data
(AE=0). The previous packages from `3babc4b48` and `2dccc3ee5` passed the
same rows.

## Progress log — resumed 2026-10-28 (Block 4 completion)

- **Full device gate green (2026-11-05)**: three defects were fixed after the
  first closure run. (1) The local Colour & Light chain was not float-exact:
  the two chroma-slider curves now travel as the host's exact
  `DiagonalCurve` polylines evaluated in double, `transit_shapedetect2()`'s
  reference point is resolved on the host instead of with OpenCL `cos`/`sin`,
  and `calcreducdE()`/the transition factors use the host's `pow`/`pow_F`
  forms. The MUNS row returns from 25 LSB16 to 1 LSB16. (2) The profile
  loader assigned `assignFromKeyfile()`'s key-present flag to `spot.expcie`,
  so any profile writing `Expcie_ false` (the local log profile) turned CIE
  CAM on and the node fell back; the call now uses the same function for its
  side effect and mirrors the enable into `visicie`. (3) The local sharpen
  blend omitted the `scope_base` argument, so a 3113x2089 launch failed with
  `CL_INVALID_KERNEL_ARGS` and fell back to the host. `verify.sh --quick` is
  **84 PASS, 0 FAIL** and `--stage-device` is **73 PASS, 0 FAIL, 0 SKIP**,
  both with `rtdata/` bound at the compiled data path; `--formats` passes the
  public PEF and `--monitor` is 1 byte.

- **Gate tooling and closure run (2026-11-05)**: `compare_tiff.py` gained
  `--info {size,bits,pixels}` and `verify.sh` falls back to it when
  ImageMagick is absent, so the gate runs on a host with only python3 and
  numpy. The `pef/neutral` row now enables `RT_OPENCL_RGBPROC` for its own
  auto run and expects `rgbproc` (the classic block's current log name), and
  the two `raw.base` rows use a new `raw-base.pp3` with
  `InputProfile=(camera)` so the matrix arm is selected even when the
  installed data directory supplies a camera ICC or DCP. The first run
  reached 66 PASS and one FAIL (`local-adjustments-munsell`, 19-25 LSB16) and
  exposed the three defects recorded above.

- **`ciecam` CAM16 (2026-11-05)**: `colorappearance.modelmethod == "16"` is
  admitted under the same constraints as CIECAM02. The model travels in a
  new `RT_CIECAM_REC_MODEL` rec slot; the three matrix helpers branch to
  CAT16, the CAT02 HPE stage becomes the identity, and the inverse CAT16
  matrix uses the SIMD copy's `0.621447744f` (the scalar copy differs in the
  last bit). Nineteen device cases, worst 0.113281 LSB16 (CAM16 `ALL`), each
  algorithm arm and the slider/hue/gamut/curves cases non-vacuous;
  `ciecam16.pp3` exports at 1 LSB16 with `impl=opencl` alongside the CIECAM02
  row. The chroma/saturation/colourfulness and per-hue sliders, EPD,
  `datacie` and `badpixsl` stay refused.
- **FFT residuals reclassified (2026-11-05)**: the soft-light `"reti"` arm
  and local `DeNoise()` were listed as completion-pending FFT/DCT units. They
  are decisively partial: `retinex_pde()` drives FFTW r2r DCT-II plans
  (`iplocallab.cc:6969`, `:7040`), and `DeNoise()`'s tail calls
  `fftw_denoise()` (`:12020`), which builds `FFTW_MEASURE` r2r plans on 32x32
  tiles. A measured plan is machine-chosen and its codelet order decides the
  float rounding, so no device transform can be bit-exact for an arbitrary
  ROI. Local exposure reaches parity only because six `fftw_size[]` extents
  have host-precomputed DCT matrices. `fftwreti` is in the same class. The
  refusals stay locked by the existing residency cases.

- **`raw.base.wb` white-balance sub-step (2026-11-04)**: the `rtot *= rm` and
  optional `CLIP` of `RawImageSource::getImage()` is a `raw.base.wb` device
  node. The host derives `rm`, `gm` and `bm` as floats and passes them
  unchanged; the node is admitted only for a full-resolution planar source
  with no D1X, Fuji SuperCCD or highlight-recovery line pass. The call site
  assembles the unmultiplied samples first and applies the multiply in place,
  so a refusal falls through to a host pass with the same arithmetic. Ten
  device cases (five shapes, clamp off and on) are bit-exact, eight refusals
  leave the image untouched, PEF and 45.7 MP NEF are AE=0 with
  `raw.base.wb` recording `impl=opencl`, the `labtools-vibrance.pp3` canary is
  0 differing, and a forced 16 MB device budget falls back with AE=0.

- **`raw.base` matrix arm (2026-11-04)**: the camera-to-working-space matrix
  multiply in `RawImageSource::colorSpaceConversion_()` is the `raw.base`
  device node. It runs only for the no-input-profile, no-DCP arm, where the
  host computes the matrix in double precision; the kernel repeats the double
  products and additions and rounds to float at the write, so it requires an
  fp64 device. Five device shapes are bit-exact (0 of 5,952 samples worst) and
  seven refusals leave the image untouched. `colorspace_flowers.pef` and the
  45.7 MP NEF are AE=0 (0 of 30,419,712 and 0 of 136,918,080), and the
  `labtools-vibrance.pp3` canary is 0 differing with the stage nodes alone.
  The canary exposed a harness issue, not a node issue: `RT_OPENCL_RGBPROC`
  also routes the RGB block through the classic backend and moves three
  clipped samples by up to 110 LSB16; `verify.sh` now sets it only on the
  `rgb.proc` row.

- **`raw.demosaic` CFA split re-admitted (2026-11-04)**: the earlier
  withdrawal blamed the split for three samples that moved by up to
  110 LSB16 on the `labtools-vibrance.pp3` canary. That profile leaves AMaZE
  in place, so `nodemosaic()` never runs; the three samples are the classic
  `RT_OPENCL_RGBPROC` backend at clipped pixels and are identical with the
  split admitted and refused. The split itself is exact: 0 of 30 419 712
  samples on the PEF and 0 of 136 918 080 on the NEF with a `Method=none`
  variant of the vibrance profile, with and without `RT_OPENCL_RGBPROC`, and
  the residency matrix is PAE 0. The MONO and NONE arms are both admitted
  again; `raw-demosaic-split.pp3` and the `pef/stage-device-raw-demosaic-split`
  row keep the split covered, and the AMaZE canary stays at 0.

- **Cleanup incident (2026-11-04)**: the build-tree purge also removed
  `build/portable`, which is the package directory and must be retained. The
  directory is restored; the deleted `.deb` is reproducible with
  `tools/build-rt.sh portable` from `3871b7d0c`. `clean-scratch.sh` and the
  AGENTS checklist now name `build/portable` explicitly.

- **Push (2026-10-29)**: `origin/perf-graph` advanced `192760b7c..03c94581c`
  (166 commits) after the publication gate; identities, range diff, messages,
  tags and manifests are clean. From now on each stable, scanned range is
  pushed.
- **C1 `lab.tools` S0+S1 (partial)**: node plumbing, host context/builtin,
  LabGrid and shadows/highlights in Lab and RGB modes. SH Lab mode is
  bit-exact; RGB mode measures ≤0.229 LSB16 (single precision, no `cl_khr_fp64`
  on POCL) with a fail-closed steep-step refusal above the UI range. The
  guided filter (`subsampling=4`, boxblur order, bilinear rescale/readout) is
  ported; tone tables, matrices and Lab caches are host-built. Residuals:
  Lab curves, local contrast, EPD, vibrance, regions, B&W, non-LabGrid toning.
  `e7c2ce0c0`, `a9b1b299e`, `b59990a76`, `76f12ac26` (CPU-only test skip),
  `c90413e41` (evidence). Pushed `03c94581c..c90413e41`.
- **C2 `lab.tools` Munsell + vibrance (partial)**: the 197 host Munsell tables
  are exported flat with per-table lengths; `MunsellLch`/`AllMunsellLch` and
  `vibrance` are line-faithful ports. Vibrance is bit-exact on CPU OpenCL
  devices (POCL) and **fail-closed on GPUs**: the rusticl `xatan2f` quotient
  can cross a Munsell zone limit at a one-ulp boundary, so a GPU submission
  records `impl=host` and is byte-identical. The gate row is POCL-only and
  says so; GPU is a SKIP (FAIL under `RT_REQUIRE_GPU=1`). Remaining lab.tools
  tools: Lab curves, local contrast, EPD (permanent residual), Lab regions.
  `382d5ea64`, `658ceeaf7`, `7e7257fc4`, `86fa6a189`, `f6a3abc01`,
  `999afb480`, `942eea6ab`, `d5a520eeb`. Pushed `c90413e41..d5a520eeb`.
- P2 added: GPU vibrance zone-boundary residual; the classic backend
  `opencl::initLocked` crash without `RT_OPENCL_RGBPROC=1` (pre-existing).
- **`lab.tools` Lab curves (partial)**: six host-built LUTs, L/a/b,
  C=f(L), C=f(C), L=f(C), red/skin, MUN Munsell and `lcredsk` are on the
  device. CPU-device/fp64 only; 21 cases, 2,347,344 samples bit-exact after
  matching the host's double-sqrt tail. Residuals: `contrast != 0`,
  chromaticity, non-identity C/L/H=f(H), LAB/XYZ Munsell, B&W, unsupported
  red/skin values. `0aff5d7de`, `be9daa44a`, `b978e446d`, `734e27d54`,
  `e2e7011e5`. Pushed `d5a520eeb..e2e7011e5`.
- **`lab.tools` local contrast (partial)**: both host Gaussian paths are
  reproduced exactly — float SSE lanes with double tails below sigma 25,
  double recurrence at and above 25; 23 cases, 2,461,164 samples bit-exact.
  CPU-device/fp64 only; host without SSE2 is a residual. A review found the
  tail deviation was double instead of float; fixed and covered by wide
  non-integer sigma-24 fixtures. `87a9575d1`, `61a0f41ab`, `f3066fe73`,
  `70af1b7b5`, `7b2f1133b`, `e41b62b16`. Pushed `e2e7011e5..e41b62b16`.
- **`lab.tools` Lab regions (partial)**: bounded set ≤4 regions, no mask
  display, samplable mask curves, CPU-device/fp64. Masks are built from a
  snapshot taken at the start of the regions pass (after the preceding tools);
  a review caught the stale-input bug (pre-fix PAE up to 464, post-fix ≤0.5).
  27 cases, 2,741,040 samples, worst 0.5 LSB16. The shared LUT/compute_f
  fidelity fix was measured on the RGB SH path (improvement, no regression).
  `79e042fe6`, `8ee88e357`, `467f330c3`, `1d22c4609`, `11a8908b9`,
  `01a927b49`. Pushed `e41b62b16..01a927b49`.
- Full Stage C ports chosen: `lab.wavelet` (directional equaliser first, then
  the multi-level wavelet) and `local.adjustments` (per-spot tools). Both span
  several sessions.
- **`lab.wavelet` node + directional equaliser (partial)**: host block
  extracted; equaliser ported with the host's SSE group/lane neighbour quirk
  for scale > 1; gamutlab/skin pre-pass on a private copy. 14 cases,
  3,574,560 samples, worst 0.115 LSB16; `wavelet.enabled`, `bef`,
  out-of-range, neutral pyramid, CIE-CAM gate and GPU are host residuals.
  `f745a0420`, `30f4db10b`, `b4a219bb1`. Pushed `01a927b49..b4a219bb1`.
- **`lab.wavelet` contrast path (partial)**: level-0 decimated 8-tap
  Daubechies, undecimated Haar above, single ascending double statistics,
  `calceffect`/`lbeta`/alpha/HS/strength and the final clamp. 14 cases,
  2,068,233 samples, bit-exact; transform-only bit-exact. A review found
  `Thrend` was ignored and could change the level count (pre-fix PAE 70.4);
  fixed and tested. Remaining residuals: denoise, chroma, edges, residual/
  final curves, toning, noise, clarity/unsharp, soft-radius guided filter.
  `65c55b9ec`, `8b412e8c5`, `837e4bfec`, `03daa9925`, `c3b2a11ac`,
  `bdcb5ddcf`. Pushed `b4a219bb1..bdcb5ddcf`.
- **`lab.wavelet` edge panel, plain arm and Medianlev Lipschitz control
  (partial)**: `ContAllL()`'s `EDmet == 2` block. The host builds the edge
  `mea`, the per-level mean/sigma/MaxP and the 501-point `ccwcurve` table
  (the default `ccwcurve` is non-identity, so the curve arm is the normal
  path); the kernel reproduces the strength, radius, repartition,
  `more`/`none`/`less` reinforce, ten-step `beta` and `kmul`/`kinterm`
  arithmetic and touches only the `borderL == 1` interior. Because `edgeena`
  disables the block's a/b level trim, the node also runs the a/b
  decomposition and reconstruction (the round trip is not an identity) and
  keeps the chroma multiply behind its own flags. 17 plain cases, 777,753
  samples, worst 0.058594 LSB16; 12 refusals at host PAE 0.
  `ed4e71f16`, `c5ed40c3c`.
  `Medianlev` adds `calckoe()` and the Lipschitz control. The raw pass runs
  the 3x3 or 5x5 convolution, the `thr` clamp and the `|tmC / temp|` ratio
  for the level's three directions; the running maximum `calckoe()` folds
  into each coefficient is a raster-order prefix maximum, replayed through a
  rowmax, carry and resolve pass rather than approximated by the global
  maximum. The control pass rebuilds the direction-combination `interm`
  with the influence sliders' `eddlipinfl`/`eddlipampl`, `aamp` and the
  threshold clamp. `NPMethod` low/high with `Lipst` replays the in-place
  28/40 or 38/50 neighbour smoothing with one work item per level, because
  the host scan reads its already-updated neighbours. Fifteen device cases
  (3x3 thresholds 20/40/65, 5x5 85/95, Lipst influence, NPMethod low/high
  and low at threshold 0, EDmethod SL, contrast, denoise, final curves, the
  combined arms and a padded 179x49 image) match the host block with a
  worst 0.023438 LSB16 (5x5 threshold 95). Against Medianlev off the host
  result changes by 8,654-114,223 samples, so the parity is not vacuous;
  NPMethod none and Lipst off move it too. `EDmethod` SL is byte-identical
  to CU under the hard-coded `EDmet == 2`, so the `EDmet == 1` threshold
  adjuster is unreachable. Fourteen refusals cover unknown
  `NPmethod`/`EDmethod`, the five detection sliders out of range, the plain
  slider ranges, an unknown `Medgreinf`, zero strength and L skin
  protection. The five export profiles record `impl=opencl` at 1 LSB16.
  `ed9f2a1e7`, `f53d06d4c`. Residuals: a zero-strength edge panel, edge with
  L skin protection, and the global `edghi`/`edglo`/`limrad` options are read
  at build time and are not part of the pp3. Two measured-matrix limits: the
  host's fast `xlogf()` versus the kernel's `log()` (below 0.0586 LSB16), and
  a degenerate `mean + sigma == MaxP` level whose curve exponent is singular
  on both sides and is not covered.
- **`lab.wavelet` final curves**: `Expfinal`'s curve arm.
  `finalContAllL()` runs on the statistics `Evaluate2()` recomputes after
  the contrast and edge passes, with the host-sampled 501-point
  `opacityCurveWL` and `sigmafin`; the pixel-domain `wavclCurve` runs after
  reconstruction and the a/b pass, before the block's `L <= 0 -> 1` clamp.
  `Expfinal` keeps the a/b round trip alive without `Expchroma`/`Expedge`.
  Thirteen device cases (diagonal only, opacity only, both, sigmafin
  0.5/2.0, default curves, with contrast, with chroma, with edge, 179x49
  padded, 3 and 7 levels, inert balance controls) match the host block with
  a worst 0.015625 LSB16; thirteen residual configurations (softrad,
  `BAmethod` sli, `tmr`, `expclari`, sigmafin below/above range,
  avoid, median, expbl, expresid, expnoise, `CLmethod` one and
  skin protection) run the host builtin byte for byte. The export gate
  `labwavelet-final.pp3` records `impl=opencl` at 1 LSB16. The remaining
  `softradend` residual is the clarity combination; the final panel's
  balance-chroma arm is a separate residual. `6a54860e1`, `873000037`.
- **`lab.wavelet` toning (zero-protection)**: `ExpToning`'s opacity-curve
  multiply on the a/b bands. The host builds the nine `mulopaRG`/`mulopaBY`
  samples from the 501-point curves; `computeMea()` fills the per-level
  `calceffect()` thresholds with `sigmaton`; the kernel runs after the chroma
  arm. Twelve device cases (a only, b only, both, sigmaton 0.5/2.0, with
  contrast, with chroma, with edge, with final curves, 179x49 padded, 3 and 7
  levels) match the host block with a worst 0.011719 LSB16; the panel-off
  comparison changes 17,112-57,280 samples. Ten residuals (protab above zero,
  sigmaton below/above range, no opacity multiplier, expnoise, expbl,
  expresid, expclari, `CLmethod` one and skin protection) run the host
  builtin byte for byte. `labwavelet-toning.pp3` is exact on the export TIFF.
  The protection range (`protab > 0`) remains a host residual. `0abd76894`,
  `5781f1c38`.
- **`lab.tools` Lab contrast**: non-zero `labCurve.contrast` is now a device
  pass. The host builds the `hist16` refill from the input L before the node
  and `complexLCurve()` builds the luma table the existing kernel applies;
  the histogram is a host prefix because it is a later node's input. Three
  cases (contrast 5, contrast 15 with L curve and brightness, contrast 30
  with all six tables) are exact; `labtools-labcurve-contrast.pp3` is exact
  on the export TIFF. `89e0a844b`, `47b9980fb`.
- **`lab.wavelet` residual arms**: noise balance (the `balchrom` scaling of
  the forced chroma variance), the denoise hue curve (per-pixel
  `noisevarlum` modulation through the exported polyline) and the
  zero-strength edge no-op are device cases; the matrix reaches 0.015625
  LSB16 (hue falling) and the zero-strength edge cases are exact. The
  `labwavelet-denoise-balance`, `-hue` and `-edge-zero` profiles are wired
  into `verify.sh`.
- **`local.adjustments` Retinex**: the spot Retinex arm runs on the device
  (`Lab_Local()`'s decompositions, mean/stdv reductions and the senstype-4
  blend), with the chroma Retinex slider and a non-identity luminance mask
  left as refusals. The matrix reaches 0.357422 LSB16 and
  `localadjustments-retinex.pp3` records 1 LSB16 with `impl=opencl`.
- **`local.adjustments` CIE CAM**: the spot "com" block runs
  `ciecamloc_02float()`'s call==0 CAM16 transform, the `repar` recovery
  blend and the final gain/gamut round trip on the device, then
  `transit_shapedetect2()`'s senstype-31 blend. The Jz pre-processing, the
  CIE and per-hue curves, the contrast sliders, local contrast, the
  gradient, the final gain and gamut compression and every mask stay
  refusals. The matrix reaches 0.399902 LSB16 and
  `localadjustments-cie.pp3` records 1 LSB16 (AE=1) with `impl=opencl`.
  `Expcie_` now loads into the tool enable as well as the UI panel flag, so
  a CIE profile round-trips.
- **`local.adjustments` common mask**: the log-encoding and CIE CAM arms
  admit `enaLMask`/`enacieMask` with the luminance mask curve. The host
  samples the 501-point `LocLLmaskCurve`; the device reproduces
  `maskcalccol()`'s `32768 * LIM01(1 - curve[(500 / 32768) * L])`, the
  1/sk IIR mask blur, `blendmask()`'s copy/clip and, when the recovery
  threshold is below one, `maskrecov()`'s ramps, `1 - pow(k, decay)` and the
  host's three-pass `boxblur()` recurrence. `transit_shapedetect2()` then
  uses the blurred mask as its deltaE reference. Fifteen device cases
  (log/CIE bands, narrow and inverted bands, decay 2 and 4, colour, blur and
  a no-mask control) are within 0.106689 LSB16; the masks move 1,352 of
  45,696 (log, PAE 7.537) and 156,000 of 156,000 (CIE, PAE 249.451) samples
  against mask-off, and `localadjustments-mask.pp3` exports `AE=0`.
  Residuals: chroma/hue mask curves, structure, contrast, mask gamma/slope,
  the laplacian, the standalone `enamask` tool, the recovery threshold's
  upper half (1..2), a CIE mask combined with tone mapping, and the deltaE
  mask. POCL gate PASS (1 skipped), CPU suite PASS (34 skipped).
- **`lab.wavelet` clarity soft radius**: `guidedFilter()`'s box means, `a`/`b`
  reduction and bilinear upsample run on the device over the block's
  `provradius` guide, with the subsampling, radius and four scales derived on
  the host. The second pass reproduces `ip_wavelet()`'s final tile-copy clamp
  (`L <= 0 -> 1`) and the merge uses the soft-filtered L unclamped, which can
  be negative; the earlier `inf` failures were the merge kernel's own clamp,
  now removed. Nine cases (softrad 0.5/2/5/100, zero merge, chroma, denoise,
  703x67 subsampled and 8192x64) are exact except the denoise pair at 0.011719
  LSB16, and `labwavelet-softrad.pp3` is exact with `impl=opencl`.
- **`lab.wavelet` final soft radius**: `softradend`'s portable sub-case runs
  `guidedFilterLog()` on the device. The host derives the blend
  `LIM01(strend / 100)`, `epsilon = 0.001 * 2^-detend`, the radius
  `max(int(softradend), 1)` and the subsampled box grid and four bilinear
  scales; the prepare kernel writes `guide = xlin2log(L / 32768)`,
  `src = xlin2log(L)` and the pre-filter L, the clarity arm's guided-filter
  chain runs unchanged, and the finish kernel applies `xlog2lin(q)` and
  `intp(blend, q, iL)`. It runs after the final pixel curve in both the
  denoise and the plain branch. Eighteen cases (softradend 1..1000, every
  blend and detail-adjust extreme, final curves, contrast, denoise, odd,
  subsampled and smooth grids) match the host block with a worst 0.765625
  LSB16 (curves); the plain, odd, subsampled and smooth cases are exact. Ten
  refusals (Thrend, non-identity `wavguidcurve`, missing `Expfinal`,
  out-of-range sliders, clarity arm and clarity soft radius) run the host
  builtin byte for byte. `labwavelet-softradend.pp3` is exact with
  `impl=opencl`. The coefficient-domain second block (`Thrend > 0`) and the
  hue modulation over a non-identity `wavguidcurve` are device cases as well:
  the host exports the `wavguid` flat-curve polyline, both decompositions and
  `Evaluate2` aggregates, and the kernels reproduce the per-coefficient
  `intp`; the hue arm applies the block's `huelab_to_huehsv2` lookup. The
  added cases reach 0.964844 LSB16 (hue 20 with curves) and both gates are
  1 LSB16 with `impl=opencl`. Because the filter works in the log
  domain, it amplifies a one-LSB16 input difference by up to about five
  LSB16, so the export gate uses identity final curves; a non-identity final
  curve can leave a sub-LSB difference that the radius amplifies.
  `4f476722d`, `aaa41f842`.
- **`lab.wavelet` standard denoise**: `Expnoise`'s standard-L arm together
  with the chroma denoise `ip_wavelet()` forces at six levels. The block
  raises the L decomposition to seven levels, sets `cp.chromfi = 0.01` and
  runs `WaveletDenoiseAllAB()` on the a/b bands with the post-contrast L
  coefficients; the device keeps a second band set for those coefficients.
  The host builds `vari[0..5]`, `ballum` and one squared MAD entry per plane,
  level and direction; the MAD median comes from a 32768-bin device histogram
  whose kernel truncates and clamps exactly as `Mad()` does. The
  `noisevarlum`/`noisevarchrom` last write uses the clamped `(2y+1, 2x+1)`
  sample, and the `ShrinkAllL`/`ShrinkAllAB` kernels reuse the lab.tools box
  blur. Seventeen device cases (levels 3/5/6, ballum 10/7/5/0/-2, high-level
  sliders only, a light slider, odd width, odd height, odd both, with
  contrast, edge, final curves, toning, all panels and all panels padded)
  match the host block with a worst 0.015625 LSB16; the panel-off comparison
  changes 85,918-114,240 samples. The a and b planes keep the base six levels
  that `levwava`/`levwavb` clamp to, while L keeps seven, so no plane carries
  an extra round trip the host does not perform; the final tail reuses the
  pre-denoise statistics unless `cp.val`, `ref`, `contr` or `cp.diagcurv`
  asks for a refresh (`679f5b4d8`). Ten residuals (an unknown `quaMethod`,
  balchrom, `Wavhuecurve`, `CBenab`, `sliMethod` cur, Ballum range, level
  range, all sliders zero and the 128-pixel window) run the host builtin byte
  for byte. The `sliMethod == "sli"` Thrden refinement is a device case: the
  host exports the block's `wavlow` flat-curve polyline and the kernel walks
  it exactly (new accessors on `FlatCurve`), with four cases at worst
  0.050781 LSB16 and `labwavelet-thrden.pp3` at 1 LSB16 with `impl=opencl`. A user chroma strength is admitted and inert: the
  forced arm's `variC` is dead at `local == 1` and `cp.chromco` never reaches
  its `> 30` ramp test, so the host output is byte-identical with the sliders
  zeroed. `labwavelet-denoise.pp3` records
  `impl=opencl` at 1 LSB16. Every comparison pins `OMP_NUM_THREADS=1`: the
  host's `noisevarlum` loop races under OpenMP and is not a deterministic
  reference. Noise balance remains a host residual.
  `66613d0a9`, `6db958afe`, `679f5b4d8`.
- **`lab.wavelet` denoise BiShrink (`quaMethod=agre`)**: the pair
  `ip_wavelet()` runs before the standard arm. On L it is
  `WaveletDenoiseAll_BiShrinkL()` and then `WaveletDenoiseAllL()`; at the
  export call site's `edge == 6` the pair's per-level formula is the same
  `ShrinkAllL()` arithmetic with the same `madL` and `vari`, and its coarsest
  level calls `ShrinkAllL()` outright, so the L arm is the standard tail twice
  over one MAD build (removing the second pass moves the device result by
  8,052 LSB16). On chroma it is `WaveletDenoiseAll_BiShrinkAB()` and then
  `WaveletDenoiseAllAB()`: the pair multiplies levels 0..4 by
  `SQR(1 - exp(-(mag_ab / (nvc * 100 * madab)) - mag_L / (9 * madL)))` with no
  box blur and runs `ShrinkAllAB()` on level 5 with the pre-denoise squared
  MAD, and the standard pass rebuilds `Mad()` from the post-pair bands, so the
  device measures the histogram twice for that plane. The block's hard-coded
  `noisevarab_r = 100.f` lives in `RT_LAB_WAVELET_DENOISE_NOISEVAR_AB`.
  Thirteen device cases (levels 3/5/6, ballum 10/7/5/0/-2, odd 179x161, odd
  height 180x161, contrast, edge, final curves, toning, all panels padded and
  Thrden 5) match the host with worst 0.037109 LSB16, and each differs from
  `quaMethod=cons` by 15,956-60,007 LSB16, so the pair is not vacuous. Four
  refusals (an unknown `quaMethod`, `agre` with Ballum or a level slider out
  of range, `agre` at the 128-pixel window) run the host byte for byte.
  `labwavelet-denoise-agre.pp3` is 1 LSB16 with `impl=opencl` (30,024 of
  30,419,712 samples). `d8b09ee47`.
- **`lab.wavelet` denoise curve (`sliMethod=cur`)**: the post-denoise
  refinement. The host samples the block's `WavCurve` over `wavdenoise` into
  501 entries, detects `cp.denoicurv` exactly as the block does, keeps the
  pre-denoise L coefficients and runs `Evaluate2()` before and after the
  denoise. The device carries both statistics, computes the `mixmet` weight,
  the `intp()` mixes and the per-level `logmax`/`rap`/`asig`/`bsig`/`amean`/
  `k`/`maxVal` on the host's doubles, and the kernel ports
  `xlogf`/`xexpf`/`pow_F` and the block's repeated four-band write. Ten cases
  (mixmet 0-3, an identity curve, contrast, edge, final curves, an odd size
  and all panels) match the host with worst 0.043945 LSB16; two residuals
  (`sigm` range and an out-of-range `Levelsigm`/`Limden`/`Wavdenoiseh`) stay
  on the host. `labwavelet-denoise-cur.pp3` records `impl=opencl` at 1 LSB16
  (21,068 of 30,419,712 samples). `ba5f8f1d3`, `dca455499`.
- **`lab.wavelet` denoise expert pairing (`complexMethod=expert`)**: the same
  refinement at `levref == 6`. `Levelsigm`'s top value is `siglh` for levels
  0..3 and its bottom for 4..5; every level iteration rewrites bands 0..3 with
  the curve factor, the first four add the `wavmed` factor when `Limden` is
  positive, and all six rewrite bands 4..5 with `wavhigh` from
  `Leveldenoise`'s lower value. The host exports `wavmed`/`wavhigh` as the
  same faceted polylines the Thrden arm already uses, the kernel takes six
  post/pre bands and an `expert` flag while the standard path keeps its exact
  writes, and the launcher re-checks the level count, the two extra knot
  counts and the flag. Eight cases (both `sliMethod` arms, `Levelsigm` ends at
  0.05/0.05, 3/3, 0.05/3 and 3/0.05, `Limden` 0, 0.5, 0.75 and 1, contrast,
  edge, final curves, odd 179x161) match the host with worst 0.158691 LSB16,
  and each differs from `complexMethod=normal` (PAE 0.25 to 32,124 LSB16), so
  the pairing is not vacuous. Four refusals (`Levelsigm` top and bottom,
  `Limden` above range, a non-identity `Wavdenoiseh`) run the host byte for
  byte. `labwavelet-denoise-expert.pp3` records `impl=opencl` at 1 LSB16
  (AE=1 on 15,637 of 30,419,712 samples) and differs from the same profile
  with `complexMethod=normal` on 29,442,428 samples (PAE 1084 LSB16).
- **`local.adjustments` groundwork (pushed)**: host block extracted
  byte-for-byte, per-spot context/ABI (`LAJC`), exact block-level predicate,
  refusal path, 25 residual mutations and four multi-spot host cases. No
  device colour subset is claimed; `Device` stays `no`. `987cf7d32`,
  `9f083a44e`, `4f15f01fc`. Pushed `bdcb5ddcf..4f15f01fc`.
- **`local.adjustments` single colour spot (partial)**: one active
  `norm`/`ELI` spot, `expcolor` only, no curves/MUNS/mask/soft radius/merge,
  `reparcol == 100`, ROI >= 5 px, CPU-device/fp64. Prefilter + ROI Gaussian +
  colour core + ELI transition/deltaE + tail clamp. 16 cases, 2,219,001
  samples, worst 0.060547 LSB16. A review found `reparcol` and the 4-px ROI
  were admitted but not applied (pre-fix 3,903 LSB16 on 11.6M samples); both
  now fail closed. The inactive-block round trip that broke `working.trc` is
  fixed and guarded. Full POCL gate 22 PASS + 1 expected SKIP.
  `612fde217`, `c568ed796`, `e6f02f167`, `fc088fe75`, `33b0e85a1`,
  `acfda825b`, `fc9b18c48`, `62079455f`, `a8be69a6a`. Pushed
  `4f15f01fc..a8be69a6a`.
- **`local.adjustments` multi-spot (partial)**: any number of `norm` spots
  in order, inactive clamp-only spots, ELI/RECT, the seven L/C/H curves and
  both chroma tables, `avoidgamutMethod=NONE`; per-spot `calc_ref` on the
  previous read-back. 16 scalar + 17 complex cases, worst 0.156 LSB16. A
  review found a clamp-arm mismatch for inactive spots with GHS leftovers
  (pre-fix 1,296 LSB16) and a residual-parity overclaim; both fixed. MUNS is
  refused: 136 LSB16 because clamp last bits flip Munsell zones; the kernel
  exists but is unused. Full POCL gate 25 PASS. `f7e2f2fa1`, `782bf4c26`,
  `6c4297932`, `03768d98b`, `5c453d077`, `771579b7a`. Pushed
  `a8be69a6a..771579b7a`.
- **`local.adjustments` MUNS enabled**: the clamp now reproduces the host
  `lab2rgb` SIMD/scalar cubic order at `width % 4`, and the MUNS sine/cosine
  split matches too. Clamp-only is bit-exact (1,751,721 samples); the MUNS
  gate row is 1 LSB16; full POCL gate 25 PASS, 0 SKIP. Residuals: LAB/XYZ
  gamut, exposure, SH, mask, soft radius, merge, etc. `65429c86a`,
  `8286c191f`, `2569e8bf0`, `33814c82c`. Pushed `771579b7a..33814c82c`.
- **`local.adjustments` vibrance + local contrast (partial)**: reused the
  lab.tools kernels and Munsell/skin tables; order is vibrance -> local
  contrast -> colour; the local contrast `lightness` quirk and `mSPwav` skip
  are reproduced. A review found an empty `localcontMethod` was admitted
  (pre-fix 10,216 LSB16); now only `"loc"` is device-supported. 41 complex
  cases, new worst 0.053711 LSB16; full gate 27 PASS. `9530114e5`,
  `f646e7860`, `0d8b1e557`, `dad0f4652`, `0a5d266ce`, `c5ce2407f`. Pushed
  `33814c82c..c5ce2407f`; contribution checklist `97d109b25`.
- **`local.adjustments` shadows/highlights (partial)**: Lab-mode SH inserted
  after vibrance and before local contrast, with host-built tone tables, the
  reused guided filter and the `senshs` blend. Worst 0.053711 LSB16; full
  POCL gate 28 PASS; issue #32 closed. `509f90731`, `95e839203`,
  `351b66b5b`, `e8fcef224`, `5f674e5ba`. Pushed `7781f6800..5f674e5ba`.
- **`local.adjustments` soft light (partial)**: per-pixel Lab->RGB->soft
  light->Lab after SH and before local contrast, with host matrices/caches
  and the strength-driven Laplacian arm. Worst 0.053711 LSB16; full POCL gate
  29 PASS, 0 SKIP; `reti`, masks and soft radius are host residuals.
  `52a7a99df`, `78b8b57b5`, `ac041ecfd`, `de5ca2825`. Pushed
  `becd88011..de5ca2825`; CI green on both jobs (3m35s).
- **`local.adjustments` CBDL (partial)**: reuses the wavelet equaliser
  kernels, runs before vibrance, reproduces the hidden level mutations, the
  `bfw/bfh > 65` no-op and the chroma normaliser. Worst 0.068359 LSB16.
  Review found two chroma defects (all-ones trim skipped the blend; wrong
  deltaE reference); fixed, with pre-fix 16,178/12,771 LSB16 regressions.
  Full POCL gate 30 PASS. `9a7944719`, `c79ea2579`, `9ac9f9f2c`,
  `b5f52dd99`, `4129076f6`, `7544aa258`. Pushed
  `de5ca2825..7544aa258`.
- **`local.adjustments` dehaze (partial)**: ROI Lab→RGB, normalize, dark
  channel, three guided filters, recovery, restore, RGB→Lab and the senstype-30
  blend, before CBDL. Two exact host reductions: normalize max and the ambient
  percentile estimate. 17 cases, worst 0.052734 LSB16; 31 PASS gate; the
  rescale path is covered at 1024x1793. `149fb9e06`, `56af4d07b`, `ccca72646`,
  `d24d57035`, `8121dad9b`. Pushed `7544aa258..8121dad9b`.
- **`local.adjustments` log encoding (partial)**: per-pixel `apply`/`norm2`
  with saturation and `repar`, first in the CPU order; `find_gray` and the
  scalars are host-built; `comprlog` bool quirk reproduced. 21 cases, worst
  0.876953 LSB16 (log+dehaze); 32 PASS gate; CPU-unset AE 0.
  `9be52549c`, `ee60f7dde`, `77fc14fd3`, `4972df7be`, `da6738464`. Pushed
  `8121dad9b..da6738464`.
- **Common mask attempt**: the cancelled subagent left a 3,067-line partial
  change; its kernel failed to build (`read-only variable is not assignable`)
  and the test suite failed/hung. The partial change was fully reverted; the
  tree is clean at the last pushed tip. Common mask remains a host residual.
- **`local.adjustments` sharpening (partial)**: the RL arm runs on a fresh
  zero-padded ROI plane after local contrast and before Colour & Light,
  reusing the output.sharpen core and kernels for the undamped RLD path. 21
  cases, worst 0.925781 LSB16. The damped arm adds the host `"bef"` fixup
  (zero -> one) without mutating params, `dcdamping()` and the 5x5/7x7
  multiply blur; 19 damped cases, worst 0.871094 LSB16, 13 runtime residuals
  host at PAE 0, and negative damping predicate-refused only. A damped radius
  below 0.6 remains a host residual. `cd38028bb`,
  `476dc9f24`, `db99dc054`, `7467f563f`, `211c12597`, `2ce2a7aff`,
  `ebbeab951`, `6fe238ede`. Pushed `41d6b5507..6fe238ede`; full gate
  **35 PASS, 0 FAIL, 0 gate SKIP**; CI green.
- **`local.adjustments` capture sharpening**: the `"cap"` arm runs on the
  clipped ROI. The device copies the ROI to working RGB, samples `RGB2L()`
  for the manual contrast mask, seeds the Rec2020 luminance plane, runs
  `CaptureDeconvSharpening_SE()`'s tiled division/multiplication recurrence
  with the sigma-selected 3x3..13x13 kernels and the 3/5/7/8 border, scales
  each RGB channel by the new/old luminance ratio, returns through
  `rgb2lab()` and blends with `transit_shapedetect2()`'s senstype-99
  (`lp.senssha`) scope. The tile geometry, `itercheck` early exit and 0.01
  skipped-tile floor are the host's. The end-of-row/column tiles overlap
  their predecessor and the host's `intp()` blends with the already-written
  value; the device reproduces that last-writer chain in four ordered
  phases. Twelve cases (all five kernels, 1..40 iterations, early stop on
  and off, recovery 50, an ROI clipped at the corner, and combined with
  blur, tone map and CIE) are within 0.091553 LSB16; the pure capture cases
  are exact, and every case changes 10,736..40,323 of 270,000 samples
  against tool-off. `localadjustments-capture.pp3` records 1 LSB16
  (AE=25,669 of 30,419,712) with `impl=opencl`; the same profile at Loc
  100/200, sigma 0.55/1.00/2.00, 1/5/10/40 iterations, early stop off and
  contrast 3/200 is also 1 LSB16. The automatic contrast, gamma wrapper,
  corner boost and mask display stay fail-closed; nine residual cases are
  host at PAE 0.
- **`local.adjustments` blur (partial)**: Blur & Noise runs after log
  encoding and before dehaze in two arms. The Gaussian arm reuses the
  lab.tools IIR line kernel with the context's host-built coefficients at
  `lp.rad / (sk * 1.4)`; the median arm reproduces `Median_Denoise()`'s
  sorted-window interior, border copy and pass ping-pong. Both blend with
  `BlurNoise_Local()`'s senstype-0 scope (`mindE` base four, the full-image
  `3/sk` reference). 14 Gaussian device cases (radii 2/5/10/18/28,
  luminance/chroma/all planes, sensitivity 0/100, no scope, a strict
  sub-rectangle ROI, RECT, colour, log, soft light, MUNS and 179x49), worst
  0.023438 LSB16; blur alone moves 16,634 samples at PAE 7,162.726562 LSB16;
  `localadjustments-blur.pp3` records `impl=opencl` at 1 LSB16. 15 median and
  denoise-curve cases (3x3/5x5/7x7/9x9, 1/3/4 passes, chroma, ROI edges,
  colour, log, 179x49, blur-then-median, the default curve and flat curves
  0.2/0.9), worst 0.031250 LSB16; median 5x5 moves 8,010 samples at PAE
  26,979.698730 LSB16; `localadjustments-blur-denoise.pp3` records
  `impl=opencl` at 1 LSB16. The wavelet denoise curve is admitted only when
  `DeNoise()` is idle, which the GUI default's Quality method "Off" makes
  true: the default curve exports identically to an empty curve (AE=0). The
  wavelet denoise itself, noise, grain, bilateral and non-local means remain
  host residuals, as do a non-zero curve with an active Quality method and
  `qualityMethod "enhden"`. The predicate refuses a Gaussian radius at or
  below 1.6 and any radius above the 30 FFT gate.
- **`local.adjustments` Tone Mapping**: `EPDToneMaplocal()` runs on the
  device, including the `equiltm` normalisation the GUI enables by default.
  The edge-preserving decomposition's steps are order-sensitive, so one work
  item reproduces the host's arithmetic: the ROI min/max reduction, the
  edge-stopping weights, the five-diagonal assembly, the incomplete-Cholesky
  column recurrence with its single-prefix fill map, the matrix-vector
  product's three summation regions, the double-precision two-accumulator dot
  products and both `xexpf()` forms (the SSE weight and compression loops take
  the vector grouping below `4 * (w1 / 4)` and `4 * (n / 4)`). 16 device
  cases (strengths 0.5/1.2/-0.4/0.8, gamma 1/2, edge stopping 0.5/1.4/3.5,
  scale 1/4, reweight 2, the equalise-off arm alone and at sensitivity 100,
  sensitivity 0/100, colour, blur, an offset ROI, a zero-strength no-op and a
  ROI below the mDEN gate) are within 0.023438 LSB16; the tone map alone moves 155,998 of 156,000
  samples at PAE 350.9 LSB16, so the parity is not vacuous.
  `localadjustments-tonemap.pp3` records `impl=opencl` and its 16-bit TIFF
  pair differs on 3 of 30,419,712 samples at 1 LSB16 (the neutral export
  differs from it by PAE 50 codes on 1,929,902 samples). Ten residual
  configurations are refused by the predicate. A ROI below the block's
  128 px mDEN gate is its own no-op; the arm needs eighteen ROI-sized scratch
  planes, so a ROI beyond the device budget (a full-frame spot on a 10 MP
  image) falls back to the host. POCL gate PASS (1 skipped), CPU suite PASS
  (34 skipped).
- **`local.adjustments` exposure (PDE arm)**: the tone plane is
  `exlabLocal()`'s host-built highlight/shadow/tone LUT sequence and the
  Laplacian input `pow_F(LIM01(L/32768), gamm)*32768` is bit-identical to the
  host (`memcmp` 0, all admitted sizes). The device then runs a direct double
  REDFT10/REDFT01 DCT pair, the host cosine/Poisson tables,
  `normalize_mean_dt()`'s two double reductions and the senstype-1 blend.
  Admitted ROIs are the `fftw_size[]` entries 64, 65, 80, 96, 100 and 128 on
  each axis, so `optfft()` is the identity; outside that set, the gamma
  wrapper, black point, shadow recovery, chroma, gradient, structure, fat,
  masks, soft radius or noise median fall back to the host. Fifteen device
  cases measure worst 0.030518 LSB16; the 0.7 case moves 7,607 of 49,152
  samples against tool off; `localadjustments-exposure.pp3` records
  `impl=opencl` and differs on 62 of 30,419,712 samples at 1 LSB16. CPU suite
  PASS (35 skipped), POCL suite PASS (1 skipped).
- **Remaining Stage C**: the Blur & Noise wavelet denoise
  and remaining denoise additions; then the wavelet
  denoise/chroma/edges/final/clarity/soft-radius panels. Each is a
  separate multi-session unit. Gate environment: set `TMPDIR` to a
  project-local directory (`/dev/shm` was 100% full and the second 60 MB TIFF
  in each stage row failed).
- Contributor PRs merged: #37 AMaZE scratch reset at `6e0fd158e`, #36 SL notes
  at `41d6b5507`; #34 stays draft, #35 assigned. Branch tip `6fe238ede`,
  pushed, CI green on both required jobs.

- **Wave 0**: device coverage moved into the `Device` column of the
  `STAGE_PIPELINE.md` catalogue with a `yes`/`partial`/`no` legend; the stale
  "everything except resize is CPU-only" list is gone. `a1c7c17fb`.
- **A1 `thumbnail.scale`**: device node for both call sites (`thumbnail.scale`
  F32 destination, `thumbnail.scale.u8` packed 8-bit), reproducing the three
  `resizeImgTo` arithmetic variants exactly; padded `Image16` view added. 26
  device cases exact on POCL, thumbnail PPMs byte-identical, 4th-ulp gap closed
  by a padded-width test. Commits `e07aa5f4f`, `0f7da2e8a`, `39c7c3dcd`,
  `da6a81b47`.
- **A2 `thumbnail.rgb`**: the block now owns a `thumbnail.rgb` device node that
  reuses the shared `rgb.proc` kernel and is offered under the stage policy,
  without `RT_OPENCL_RGBPROC`. The classic backend stays the fallback and the
  export `rgb.proc` gate is unchanged. The device test covers 8-bit and 16-bit
  source grids with the tone curve, shadow curve, RGB curves and channel
  mixer, all exact against an independent reference; black-and-white is
  refused and leaves no device record. End-to-end PPMs are byte-identical with
  the exposure profile; the mixer moves 1 of 114 240 raw samples at 1 LSB8.
  The earlier composite work also exposed and fixed a real `rgb.proc` defect:
  the shared kernel indexed compactly while stage buffers are row-padded, so
  padded widths sheared and width 179 crashed; regression test at widths
  238/179 against compact-width references. Commits `36fae8d35`, `2a8381228`,
  `c7c3387d9`, `96feabcb0`, `065d3ca53`, `186988ff7`.
- **A3 `thumbnail.lab`**: the block now owns a `thumbnail.lab` device node that
  reuses the `lab.tools` kernel with the reduced path's skip-16 curve tables
  and is offered under the stage policy. The admitted set is LabGrid, the Lab
  curves (with black-and-white output) and vibrance; shadows/highlights, local
  contrast, Lab regions, EPD, soft light, CIE CAM, the working TRC and the
  gamut pass that depends on it keep the whole block on the host, which runs
  the unchanged code on a copy so a refused launch cannot leave it half
  processed. Five device cases cover 8-bit and 16-bit Lab grids and all four
  admitted sub-tools, exact against the production host sequence; end-to-end
  PPMs for LabGrid, Lab curves, vibrance and all three are byte-identical.
  Commits `perf:`, `test:`, `docs:` in this unit.
- Kernel pitch audit: every other stage kernel already takes pitches; only
  `rgb.proc` was compact. Padded-width tests added for wb/permute/scale are
  listed in `STAGE_RUNTIME.md` for the next sweep.
- **B1 `framing`**: device fill+copy, bit-exact, CLI gate, one-axis offset
  case added after review. `3f09c70e7`, `a4d672fa4`, `a6e110725`,
  `b9a3ccb92`, `3ee70b76f`.
- **B2 `gamut`**: in-place Lab kernel for Lab→linear RGB, gain, end-of-process
  ACES compression for four gamuts, and back to Lab; host-built matrices;
  worst 0.06 LSB16; 9x1 table contract tightened after review.
  `98e6ef4d5`, `19b1c20b3`, `463220271`, `256752fd0`, `88dfa5772`,
  `fc60b6167`.
- **B3 `output.sharpen`**: extracted the CPU core (`sharpen_core.h`) and added
  a host builtin so residual requests record `impl=host`; device USM (mask +
  sigma-2 IIR + 3x3/IIR blur + threshold) and undamped RLD (3x3/5x5/7x7
  div/mult, exact border and 7x7 quirk) run on the device. Worst USM 0.82
  LSB16, worst RLD 0.47 LSB16. Default post-resize RLD profile is now device
  covered; damping, radius >= 1.15, blurradius >= 0.25, edges-only, halo and
  show-mask are host residuals. `3514abef7`, `f9acb1a63`, `3e4438d8e`,
  `5169ff98f`, `d1be37d17`, `a191c95a7`, `8f03ad842`, `75cc905ed`,
  `e729d1680`, `0ff404360`, `f8a9ca788`, `6c784a4e5`.
- **`lab.sharpening`** recorded partial through the shared node (`016c5ebfd`).
- **Exceptions** recorded: `source.select`, `source.embedded`, `analysis`,
  `output.icc`, `thumbnail.decode` and `crop` are `n/a` with reasons
  (`588301647`). `crop` is a row copy deferred to residency.
- Remaining CPU-only pixel nodes: `working.trc`, `lab.tools`,
  `lab.wavelet`, `local.adjustments`, `thumbnail.lab`'s working-TRC/CIE-CAM/
  gamut remainder, and the raw nodes (`raw.base` on preview/thumbnail; the
  export names `raw.base.source`, `raw.denoise` and `raw.color`;
  `source.*`/`analysis` are exceptions). `raw.preprocess` is partial through
  its `scaleColors()` Bayer arm and `raw.demosaic` through the MONO/NONE arms
  of `nodemosaic()`.
- **`raw.demosaic` (partial)**: the MONO and NONE arms of `nodemosaic()` run
  on the device; every interpolating method and capture sharpening stay on the
  host. MONO is a three-plane copy and NONE routes each pixel through
  `RawImage::FC()`, so both are bit-exact by construction. The fast arm cannot
  be ported exactly because its `INVGRADV` weights use `_mm_rcp_ps`; the
  remaining arms are full multi-pass algorithms. Thirteen residency shapes
  (mono and split, four filter words, one row, one column, one pixel, a mixed
  word over the eight-row period) are exact, eight launcher refusals leave the
  planes untouched, and the PEF/NEF `Method=mono` exports are AE=0 with
  `impl=opencl`. `perf:`, `test:`, `docs:` in this unit.
- **`working.trc` (partial)**: W1 exposed the probe means, the lcms-built
  profile and the probe primaries matrix via setup-only out-params
  (`78c314f00`, `92ec1641b`). W2 runs a host prefix (`lab2rgb` + fixed sRGB
  inverse) so the probe, profile and second-pass input are byte-identical to
  the CPU block, then a device second pass (toxyz, optional gamut map,
  matrix + inverse type-5 TRC, `apsatur`, RGB→Lab). 1 LSB16 on POCL and the
  AMD RX 550, probe means exact. Type-5 profiles only; a type -5 profile
  (CUSTOM gamma < 1) is a host residual, as are unknown working profiles,
  non-relative intents, local contrast and B&W.
  `e5f142741`, `d408ca0df`, `2b2fe1efc`, `67a37700c`, `897c25fd3`,
  `0f1a819cc`, `b07462094`, `8fdcb2fb3`.
- **`working.trc` soft light**: `ImProcFunctions::softLight()` now runs as a
  device pre-pass before the host prefix, with the host's float working
  matrices and the `gammatab_srgb`/`igammatab_srgb`/`cachef`/`cachefy`
  tables. The launcher owns `prepare()`/`build()`, so the kernel readback is
  frozen on a private host copy and a refusal leaves the caller's Lab
  untouched; the ABI is four arguments plus the `prepare`/`images` callbacks.
  The kernel is bit-exact against the host on the production 3888x2608 Lab
  after reproducing the vector `Lab2XYZ` cube-before-scale association and
  the LUT operator's last interval; the earlier association differed by
  0.03 LSB16 in Lab and the second-pass inverse amplified it to four output
  codes. Thirty-one residency cases (soft light 3/30/50/60/100, five TRCs,
  gamut and saturation combinations, a bright ramp, the off/zero no-ops and
  the 101/-1 refusals) are within 0.099609 LSB16; strength 50 changes all
  17,136 samples at PAE 4,282.638672 against the tool off.
  `working-trc-residual.pp3` is 1 LSB16 with `impl=opencl`.
  `2b3fcd5a9..4a8b1ae85`. Residuals: local contrast,
  B&W, a soft-light strength outside 1..100, type -5 profiles and
  out-of-gamut first passes.
- **`working.trc` tone equalisers**: the block's two
  `ImProcFunctions::toneEqualizer()` calls now run on the device. The host
  derives the block's own `ToneEqualizerParams` (the midtone `wmidtcie` and
  white-distribution `wsmoothciesli` derivations are shared with the host
  block), builds the 65536-entry correction table and the fifteen band factors
  through the shared `toneEqualizerTable()`, and exports the double working
  luminance row. The device computes `Color::rgbLuminance()` in fp64, ports
  `guidedFilterLog()` exactly - `calculate_subsampling()`, `boxblur()`'s
  running sums and the bilinear rescale/readout - and applies the table, with
  `process_pixel()` above 1. The host prefix is split so the midtone arm runs
  between `lab2rgb()` and the fixed sRGB inverse; the white-distribution arm
  runs in place on the post-TRC image. Twelve residency cases at 238x24 and
  179x17 are within 0.083984 LSB16, both arms are non-vacuous (PAE 2 634.8 and
  1 351.8 LSB16 against the tool off), and `working-trc-tone.pp3` (midtone
  -30) and `working-trc-tone-smooth.pp3` (smooth 0.5) are 1 LSB16 with
  `impl=opencl` on the full 3888x2608 PEF, which exercises the subsampling-5
  path. Midtone 101, smooth 1.6 and smooth -0.1 are predicate refusals;
  midtone 100 on the synthetic source and midtone 40 on the full image leave
  the working gamut, so the setup refuses them and they run `impl=host`.
  `perf:`, `test:`, `docs:` in this unit.
- **`output.sharpen` residual switches**: the fallback arms are now device
  kernels with host-derived scalars: edges-only (`bilateral2.h`'s twenty-one
  sigma buckets, upper-triangle integer weights, `0x20000`-entry range table
  and all border rules), halo control (`sharpenHaloCtrl()`'s sliding three-row
  maximum/minimum as one work item per row), the blur-radius pre-pass (plain
  Gaussian, mask merge before and after) and damped RLD (`dcdamping()` with
  the sleef `xlogf()` port and its SIMD/scalar split, on the 3x3 FIR and IIR
  forward blurs). Up to five scratch planes are sized from the switches, so
  plain USM still allocates three. Thirty-eight USM and twenty-four RLD cases
  are within 0.633 LSB16 (edges only on a smooth plane with a flat mask; USM
  0.541, RLD 0.523, damped 0.313); halo, blur radius, edges and damping each
  change the device result against the plain USM arm. `sharpen-usm.pp3`,
  `sharpen-rld.pp3`, `sharpen-rld-damped.pp3` and `sharpen-usm-halo.pp3` are
  1 LSB16 with `impl=opencl`. Show-mask, a non-positive edge tolerance,
  `blurradius >= 25` and the separable RLD radius stay host residuals. The
  edges-only export measures 4 LSB16 because the node is bit-exact on a given
  input and the upstream device pipeline's L plane is amplified by the
  bilateral's piecewise-constant range table; that profile is not gated.
- **`raw.preprocess` scale arm**: the Bayer arm of `scaleColors()` is a device
  node, one work item per row. The host keeps the prelude (black-offset
  selection, `c_white`, `calculate_scale_mul()`, clip levels); the kernel does
  `max(0, pixel - cblacksom[c4]) * scale_mul[c4]` and writes a one-row buffer
  of channel maxima that the host reduces into `chmax`, so no atomics and no
  second full-image scan. The call site enters it only for a three-colour
  Bayer mosaic at a non-empty rect; X-Trans, monochrome, four-colour filters
  and every other preprocess step stay on the host and a refusal rewrites the
  mosaic with the unchanged CPU loop. Nine residency cases are bit-exact with
  exact `chmax`; both corpus images are `AE=0` (0 of 30 419 712 and 0 of
  136 918 080 samples) with `impl=opencl`. `verify.sh` runs the neutral
  profile as `pef/stage-device-raw-preprocess`. Not pushed.
- Side defects found: `workingtrc` leaks the profile/transform per call
  (`iplab2rgb.cc:2145/2173`, still open); forcing the POCL CPU stage device
  under `--gpu=auto` hits a null `opencl::initLocked` call with
  `gamut.pp3`, avoided by `verify.sh`'s `RT_OPENCL_RGBPROC=1` (pre-existing,
  record in P2).
- **`lab.tools` XYZ/XYZREL gamut correction**: `Color::gamutmap()` is ported
  with its double lam ratios and XYZREL's `0.5` blend, reading the double
  working matrices the launcher already uploads. XYZ 0.317383 LSB16, XYZREL
  0.066406, XYZ neutral 0 and XYZ with the three hue curves 0.753906 (0
  samples over 1); export gate `AE=0` with `impl=opencl`. `206c60df9`,
  `0574346b5`, `fc824e7d5`, follow-up `773c19d07`. Landed on `perf-graph`
  and pushed at `773c19d07`, the current `origin/perf-graph` tip.
- **`lab.tools` LAB gamut correction**: `Color::gamutLchonly()`'s RGB
  overload is ported on the same working-space inverse, so the Lab-curve
  method set is NONE/LAB/XYZ/XYZREL/MUN. LAB all tables 0, exact (45696 of
  45696 samples); LAB neutral 0, exact; LAB with the three hue curves
  0.011719 LSB16 (0 over 1); export gate exact with `impl=opencl`.
  `add8558fe`, `1815ffe8c`, `1f31f106d` and this documentation commit.
  Landed on `perf-graph`; not pushed.
- **`lab.tools` black-and-white with colour toning**: the host's `bwToning`
  is `bwonly` (`improcfun.cc:4602-4606`, `bwq` compiled out), so colour
  toning takes the block past both black-and-white arms and the Lab pass does
  the work it does with black-and-white off. `rgbProc()` owns the
  black-and-white image and the non-LabGrid toning; LabGrid and the regions
  run in the block. The node now exports the block's `bwonly` as its
  black-and-white flag instead of `blackwhite.enabled`, so a combination
  keeps the chroma LabGrid and the regions write. Fourteen matrix cases are
  within 0.75 LSB16 (worst: B&W + Splitlr + Lab curves, 1 sample; the rest
  exact), with B&W off bit-identical, the flag 0/1 as the host decides, and
  `labtools-blackwhite-toning.pp3` pixel-exact with `impl=opencl`.
  `lab.tools` now covers B&W alone, B&W through both arms and the colour-
  toning combination; only a `LabRegions` profile outside the bounded set
  stays a host refusal. Landed on `perf-graph`; not pushed.
- **`lab.tools` dense hue curves**: `FlatCurve::getVal()` is exactly the
  faceted polyline it builds, so the three hue flat curves are now exported as
  one node per polyline knot in double rather than resampled with the chord
  refiner. The double node is what admits a dense curve: a float abscissa in
  [0,1] moves a 40 000-point alternating C=f(H) by 1.2e-4, 240 times the
  5e-7 bound. The slot doubles to 65 536 nodes, the most `FlatCurve`
  subdivides, and a longer hand-built list falls back to the refiner; a curve
  neither can follow (100 000 alternating knots) stays a host refusal. Eight
  dense matrix cases (512/4 096/40 000 steep, 8 000 smooth, slider on, a/b
  and L tables, and a polyline that starts at 0.2 and wraps through the
  period) are bit-exact and carry one node per control point, the
  existing hue matrix is now bit-exact instead of 0.023438 LSB16, and
  `labtools-huecurve-dense.pp3` is pixel-exact with `impl=opencl`. The
  earlier `labtools-huecurve.pp3` wrote two-number points, which `FlatCurve`
  read as the identity; it now carries four per point. Landed on
  `perf-graph`; not pushed.

### Wave B scoping decisions (2026-10-28)

- **`framing`**: done. Full-format F32 in/out, constant fill + offset copy,
  zero residual; CLI gate added.
- **`gamut`**: in progress. One in-place Lab kernel covering Lab→linear
  working RGB, gain, end-of-process ACES compression for REC2020/ADOBE/SRGB/
  DCIP3, and XYZ→Lab; four host-built 3x3 matrices and two scalars in the ABI.
- **`output.sharpen`**: default USM needs five launches (mask gradient +
  sigma-2 IIR H/V + 3x3 FIR + threshold/apply); `radius >= 0.6` replaces the
  FIR with an IIR pair. Named residuals: RLD deconvolution, edges-only
  bilateral, halo control, `showMask`, sigma >= 25. `lab.sharpening` shares
  the kernel but its other sub-tools stay host and make the block atomic.
  To satisfy the `impl=host` rule for a residual, the node needs a host
  builtin that the call site submits when a residual is enabled; otherwise
  the record is absent. Cost: the largest kernel set in Wave B.
- **`working.trc`**: the lcms branch is a matrix-shaper, so matrix + 1-D
  65536-entry inverse TRC (via `buildGpuProfile`) is the faithful device
  model; a 3-D LUT is strictly worse. Two-phase node: a host/device
  dominant-colour probe (whole-region float reduction) then a per-pixel
  kernel. Residuals: non-relative rendering intent, wavelet local contrast,
  both tone equalisers, unknown working profiles (CPU no-ops, must be
  reproduced). The probe's summation order is the main parity risk.
  Side finding: `oprofdef`/`cmsHTRANSFORM` leak once per invocation
  (`iplab2rgb.cc:2145`, `:2173`, `:2260`) — fix separately.
- **`ciecam`**: implemented for the single-pass CIECAM02 and CAM16 surface
  (see Completed this session). The scoping note follows. A faithful port
  needs a host mean/histogram pass, two
  32768-entry brightness LUTs, up to six 65536 user-curve LUTs, CAT02/HPE
  matrices and a verbatim transcription of the sleef `pow_F`/`xlogf` chain.
  A single kernel covers the no-EPD/no-autocielab surface; EPD, autocielab
  sharpening/defringe/denoise, `datacie` and legacy gamut are residuals.
  Estimated 3-5x gamut, 2-4 sessions, and CIE CAM is off by default, so it
  should follow the Lab tools/wavelet work rather than precede it.
- **`lab.tools`**: scoped. The cheapest faithful first stage is LabGrid +
  shadows/highlights in both Lab and RGB modes (SH needs a guided filter with
  subsampling 4 and a host `f` LUT). Default vibrance and the default Lab L
  curve both require the 195-LUT Munsell correction, so that is stage 2.
  EPD, Lab regions and local contrast follow; EPD is not recommended as a
  device target (global reduction + iterated PDE) and stays a residual.
  Stage S0+S1 is ~2.5-3x gamut.
- **Revised priority after `working.trc`**: `lab.tools` S0+S1, then the
  Munsell unit + vibrance/Lab curves, then `lab.wavelet`, then `ciecam`;
  `local.adjustments` and the raw nodes last.

## Completed this session

- **`ciecam`**: the single-pass CIE-CAM path now runs on the device for both
  colour appearance models, CIECAM02 and CAM16. The host context rebuilds the
  histogram pass, the scene/viewing conditions, both `initcam` calls, the two
  brightness tables and the optional working-space inverse; one kernel
  reproduces the CAT02/CAT16 transforms, the CAT02 HPE stage (the identity for
  CAM16), the four algorithm arms, the hue rotation, `skinredfloat()` and the
  Lab gamut control. The model is uniform per run in the rec table, and the
  three matrix helpers branch on it. Twenty-five residency cases cover the
  axes, worst PAE 0.113281 LSB16, nineteen device cases with every one
  changing pixels; the export corpus is 1 LSB16 with `impl=opencl` for both
  `ciecam.pp3` (CIECAM02) and `ciecam16.pp3` (CAM16), and the CAM16 plus
  colourfulness-slider residual is `AE=0` with `impl=host`.
  Chroma/saturation/colourfulness sliders, tone mapping/EPD, an unknown
  `modelmethod` and non-identity colour curves are host residuals; the chroma
  sliders stay out because `skinredfloat()`'s skin-range boundary amplifies a
  last-bit hue difference.
- **Validation matrix** (`devnotes/perf/VALIDATION.md`): Debian 12 and 13
  installed the `.deb` with `apt-get install`, 36 formats, 8/8 exports exit 0
  per distro.
  - TIFF versus the host build: exact for the generated TIFF and JPEG inputs;
    1 LSB16 on one or two pixels for the raw inputs, all three pairs.
  - JPEG: decoded pixels identical everywhere; files differ in 2-3 metadata
    bytes.
  - GUI under Xephyr for NEF, TIFF and JPEG: window 1200x680, preview and crop
    graphs active, pan, 100% and zoom-in each produced a distinct screenshot,
    no error lines.
  - POCL device parity: `stage-residency-test` PASS, PAE 0, 0 differing
    samples, executor CPU, labelled POCL CPU.
- **Mask vocabulary sweep**: the OpenCL and performance records now use the
  typed request vocabulary, with a note that quoted removed code and the
  retirement guard name `M_*` deliberately.
- **`RT_WORK_SHADOW` decision**: kept as a diagnostic marker with no engine
  effect; comments that implied a second authority corrected.
- **Publication scan**: the 83-commit range at scan time (the record commit
  follows it), tree, messages, identities, tracked
  manifest and provider content (24 issues, no PRs, no releases) are clean.
- **Package refresh**: rebuilt at `3a1a55559` and re-ran the matrix. All 24
  host/container TIFF and JPEG comparisons are exact. OpenMP was the earlier
  noise source: without `OMP_NUM_THREADS=1` two identical Debian 12 runs
  differ by 40 LSB and a host-versus-container pair by 754 LSB.
- **Lab-to-RGB device family**: `output.convert` plus the packed 8-bit
  `monitor.output`, `viewport.output` and `thumbnail.output`, sharing one
  arithmetic fragment with the classic kernel. Export parity is 1 LSB on 3.8%
  of pixels; the packed path has a byte-exact reference test.
- **Interactive nodes on the host**: `monitor.output` and `viewport.output`
  run on the device in the GUI (45.7 MP NEF) and the preview image is
  pixel-identical (0 LSB8) to a CPU-only run. Recipe: OpenCL build of the GUI,
  an explicit monitor profile, and the POCL-only ICD directory by absolute
  path.
- **`rgb.proc`**: the RGB tone and Lab conversion now runs on the device, with
  the kernel shared with the classic backend and per-plane uploads. Parity is
  1 LSB on 4% of pixels; the classic path is byte-identical. The node stays
  inside the `RT_OPENCL_RGBPROC` opt-in and its tool exclusions.
- **`thumbnail.transform`**: both the three-plane (`thumbnail.transform`) and
  packed-8-bit (`thumbnail.transform.u8`) coarse transforms run on the device as
  one destination-to-source pass; the call sites allocate the rotated
  destination and swap it in. Sixteen rotation/flip combinations are
  device-tested in both formats. The GUI harness never reached the node (a
  rotated sidecar changed the cache key but produced no record), so the browser
  path is recorded as uncovered.
- **`thumbnail.whitebalance`**: the per-pixel white-balance loop now runs on the
  device with the host-resolved multipliers. The device test found a real bug
  first: the in-place node passed plane 0's compact allocation as the
  destination for all three planes, so planes 1 and 2 wrote out of bounds. The
  whole image is now one allocation; the 45.7 MP thumbnail cache is
  byte-identical to a CPU-only run and the test matches an independent
  reference.
- **`thumbnail.output`**: `rtthumbnail` never set the OpenCL preference on its
  `ImProcFunctions` or the per-thread policies, so the monitor conversion never
  reached a backend. Fixed; the file browser now generates thumbnails on the
  device and the cache is byte-identical to a CPU-only run. The Lab-to-RGB
  family has no uncovered node left.
- **Transform device node**: rotation/scale now runs on the device, sharing one
  map builder and one set of sampling helpers with the classic backend (which
  stayed byte-identical, 0 LSB). Identity is exact on the device; the export is
  1 LSB on 3.8% of pixels. The portable build caught a GCC 12 limitation: a
  multi-line raw string inside a `#define` is rejected there but accepted by the
  host compiler, so the shared fragment is a namespace-scope constant.
- **Stage-device precedence and gate**: the classic backend was intercepting
  the one device node whenever acceleration was on, so the stage device was
  unreachable from a CLI export. Both `ImProcFunctions::Lanczos` overloads now
  run the stage device first when `RT_STAGE_OPENCL` selects the node; the
  default order is untouched. `verify.sh --stage-device` (also in `--quick`)
  asserts an `impl=opencl` record and 1 LSB parity against the CPU reference,
  and the assertion was proved non-vacuous by forcing a device refusal.

## Remaining

### Block 4 — device node coverage: complete

Every node is either a device node or recorded `n/a` with file:line evidence.
The device set covers `transform`, `rgb.proc`, `gamut`, `crop`, `resize`,
`output.convert`, `framing`, the monitor/viewport/thumbnail output family,
`thumbnail.scale`/`rgb`/`lab`/`whitebalance`/`transform`, `lab.tools`,
`lab.sharpening`, `lab.wavelet`, `local.adjustments`, `working.trc`,
`output.sharpen`, `ciecam` (CIECAM02 and CAM16), `raw.preprocess`,
`raw.demosaic` (mono and the Bayer CFA split) and `raw.base` (white balance
and the camera-matrix arm). `source.select`, `source.embedded`, `analysis`
and `thumbnail.decode` are `n/a`: they are I/O, pointer swaps or a
65,536-bin histogram with no portable pixel kernel.

The residuals that remain are recorded as **decisively partial** in
`STAGE_PIPELINE.md` and `PARITY.md`: FFTW-driven transforms (local `"reti"`,
local `DeNoise()`, `fftwreti`, exposure outside the six `fftw_size[]` ROIs),
libc-`rand()` added noise, the host-SIMD `rsqrt` show-mask, the
`skinredfloat()` CIE CAM chroma/hue sliders, the interpolating demosaic
methods, EPD and `datacie`. They are not unstarted work.

Gate evidence: `verify.sh --quick` is 84 PASS / 0 FAIL, `--stage-device` is
73 PASS / 0 FAIL / 0 SKIP, `--formats` passes the public PEF and `--monitor`
is 1 byte, all on POCL CPU with `rtdata/` bound at the compiled data path.

### P1 gaps

- Fast export (`-f`) is in the graph: `StagePath::FastExport` carries its ten
  nodes and the executor runs the same private methods. The `-f` row is
  `AE=0` against the pre-change binary; see `STAGE_RUNTIME.md`.
- Thumbnail `loadFromImage` for non-raw files is now graph-covered:
  `thumbnail-stage-test` accepts any raster extension, runs `processImage()`
  on a 16-bit TIFF exported from the public PEF, and asserts the six-node
  graph line plus byte-identical passes on the host and the POCL device.
  Inspector mode is a deliberate `n/a`: `loadFromImage(..., inspectorMode=true)`
  stops at the full-size `Image8` resize (`rtthumbnail.cc:495-505`) with no
  profile pipeline, and the harness asserts that no `processImage` line
  appears.
- Export raw split (P1-3a): the canonical export names `raw.base.source`,
  `raw.denoise` and `raw.color`, giving 22 nodes and 21 edges. PEF (10.2 MP)
  and NEF (45.7 MP) neutral 16-bit exports are AE=0 against `ffc7e219c` at one
  thread, and `RT_STAGE_PROFILE` records the three nodes. The fast-export spec
  is unchanged.
- Folded nodes: preview `analysis`; crop `raw.preprocess`/`raw.demosaic`; four
  export sub-blocks (framing geometry, Nearest output resize, histogram
  allocation, forced B&W). The three `raw.base` sub-blocks are named export
  nodes since P1-3a; none of the remaining four has a buffer boundary, so each
  is covered by its parent's record.
- Preview dirty residuals are closed by design and tests. A guide-only crop
  change is the `cropGuideFingerprint` field, which is deliberately unmapped:
  it names an editor overlay no stage consumes, and `stage-dirty-blocks-test`
  asserts it reports no aspect, so it cannot schedule a stale repaint.
  `deferredAnalysisRequest` is a coordinator scheduling fact published from an
  advisory snapshot; the coordinator absorbs it before building the key, so it
  cannot order a dirty set. A high-detail transition maps to the `HighDetail`
  aspect and the preview row `source.select`, `raw.preprocess`, `raw.demosaic`,
  `raw.base`, `analysis`, `transform`; the test covers the four
  high-detail fields. The remaining gap is the GUI dump coverage for tone
  curve and crop geometry, which is P2.
- Node-level profiling and cache metrics: `RT_STAGE_PROFILE` writes JSONL and
  `tools/perf/summarize-stage-profile.py` reduces it to `count`, `total_ms`,
  `max_ms`, `impls` and `cache_hits` per node. A `raw.base` tier served by the
  node cache records `impl=host`, `source=node-cache`, `cache_hit=1`; a
  preview sweep with `RT_NODE_CACHE=1` produced two such records and the
  summary reports `raw.base` `cache_hits=2`.
- `bench-preview.sh` now runs `preview-work-order-sweep` with
  `OMP_NUM_THREADS=1` and prints `samples=N p50/p95`; the Xvfb/xdotool driver
  was retired. A three-sample run on the PEF reported `samples=3 p50=306 ms`.

### P2 residuals

- **Thread determinism**: the gross race is resolved. `test-export-determinism`
  on the current tip is `AE=0` for the 45.7 MP NEF neutral and with denoise at
  t=1/t=4/t=8 and on a repeated t=8 run, and for the PEF neutral. The PEF with
  denoise keeps 24 pixels at 1 LSB16 between t=1 and t=8 (and on the repeated
  t=8 pair), inside the parity tolerance but not bit-identical. The guard is
  `tools/perf/test-export-determinism.sh`; the bisect history is in
  `devnotes/perf/DETERMINISM.md`.
- **Tile ordering / fusion**: still opt-in and incomplete. It is a
  performance feature, not a parity defect; the default order is the
  conservative one.
- **`crop-stage-test` stall**: not reproduced on the public PEF at the current
  tip (`--sequences` with `sharpen-usm.pp3`: PASS, 39 plans, 0 fallbacks,
  `AE=0`). The historical flake is recorded but has no acceptance case.
- **GUI dump coverage**: the deterministic sweep already carries the
  `tone-curve` state and the `zoom-100`/`zoom-fit`/pan crop states, so those
  edits have dump pairs there. The *screenshot* harness
  (`test-preview-parity.sh`) still captures only fit/100/zoom/pan, because the
  xdotool panel drive is not reproducible; that screenshot gap is recorded,
  never treated as a PASS.
- **rusticl**: skipped on this host; deterministic device tests run on POCL.
  This is an environment fact, not a code residual.

### Decisions (closed 2026-11-05)

- **Node cache**: stays **opt-in**. `RT_NODE_CACHE` unset means off and
  `RT_NODE_CACHE=0` is the kill switch; `RT_NODE_CACHE_MB` sets the budget
  (default 256 MiB, valid 1-8192) and `RT_NODE_CACHE_STATS` prints the
  counters at exit. `stage-node-cache-test` passes: the key rejects a
  different image generation, profile, geometry, work order, backend, node or
  size; a repeated identical pass is `AE=0` with a hit; the true LRU and
  budget behave; the disabled path allocates nothing; a malformed payload is
  refused. That is the correctness evidence, not a production-performance
  one. A preview sweep with `RT_NODE_CACHE=1` on the PEF (five exposure
  samples) reports `hits=2 misses=3 refused=1` and a mean settled latency of
  307.1 ms against 305.9 ms without the cache: the hit is real, but the win
  is inside the noise. The promotion gate is therefore **not met** and the
  default stays off. A preview-path measurement with a clear win at the
  default budget, plus a user-visible preference, is what would flip it.
- **Stage OpenCL**: stays **opt-in** (`RT_STAGE_OPENCL=1`; the kill switches
  are `RT_STAGE_OPENCL=0` and `RT_STAGE_OPENCL_DISABLE=1`). Coverage is now
  meaningful: `verify.sh --quick` is **84 PASS / 0 FAIL** and
  `--stage-device` is **73 PASS / 0 FAIL / 0 SKIP**, with every node at
  1 LSB16 or better. But every figure is POCL CPU, and the project rule says
  a CPU-device result is not GPU parity. Promotion to default therefore waits
  for the same rows on a real GPU, with the fp64 and `rsqrt` paths exercised.
- **Residency policy (recorded)**: the device path is opt-in and fail-closed.
  A node runs on the device only when the user preference requests OpenCL,
  `RT_STAGE_OPENCL=1` is set, the node's device requirements are met (fp64
  where the kernel uses doubles), and the per-node budget admits the buffers.
  A refusal runs the host builtin byte for byte and is recorded as
  `impl=host`, `source=device-fallback`; it is never a silent skip. Buffers
  come from the context's residency manager, are reused across that
  context's calls and are released with it. `RT_STAGE_DEVICE_BUDGET_MB`
  overrides the default budget; out-of-range values fall back to the default.
- **Push**: authorised 2026-10-29. Each stable range is scanned and pushed to
  `origin/perf-graph`.
- **Merge**: `dev`/`perf` untouched, by instruction.

## Workspace recipes that worked

```bash
export RT_TESTDATA=$REPO/build/rt-testdata
export CCACHE_DIR=$REPO/build/ccache

# Builds (run in the worktree)
tools/build-rt.sh cpu -j 4 --ccache
tools/build-rt.sh opencl -j 4 --ccache
cmake --build build/opencl-perf-graph --target stage-residency-test -j 4

# Tests
./build/cpu-perf-graph/rtengine/stage-work-test
./build/cpu-perf-graph/rtengine/stage-dirty-blocks-test

# Device parity, POCL only
mkdir -p build/test-scratch/pocl-icd
ln -sf /etc/OpenCL/vendors/pocl.icd build/test-scratch/pocl-icd/pocl.icd
RT_OPENCL_ALLOW_CPU=1 RT_REQUIRE_GPU=1 \
  OCL_ICD_VENDORS=$PWD/build/test-scratch/pocl-icd \
  ./build/opencl-perf-graph/rtengine/stage-residency-test
```

Environment traps found this session:

- The desktop session runs Wayland, so the GUI attaches to the Wayland
  compositor and creates no X window. Set `GDK_BACKEND=x11` for Xephyr runs.
- Without a window manager, `xdotool key --window` is ignored. Put the pointer
  over the image with `xdotool mousemove --window` and send keys globally
  (XTEST). `xdotool windowfocus` does not help.
- Docker writes root-owned files into `/dev/shm`; clean with a throwaway
  container (`docker run --rm -v /dev/shm:/shm debian:12 rm -rf /shm/<dir>`),
  because there is no passwordless sudo.
- The GTK app creates a hidden 10x10 utility window titled
  `rawtherapee-graph` before the real toplevel. Pick the widest window and
  re-query the geometry after the image loads.
- No `openbox`; the Xephyr recipe runs unmanaged and relies on XTEST above.

## Reference: switches

`RT_WORK_ORDER` (default on), `RT_NODE_CACHE` (default off), `RT_NODE_CACHE_MB`,
`RT_NODE_CACHE_STATS`, `RT_STAGE_OPENCL` (default off), `RT_STAGE_DEVICE_BUDGET_MB`,
`RT_WORK_SHADOW` (diagnostic only), `RT_OPENCL_ALLOW_CPU`, `RT_REQUIRE_GPU`,
`RT_TESTDATA`.

## Delegation model

- One writer per worktree; up to three agents concurrently; reviewers read-only.
- Bounded briefs: goal, acceptance criteria, exact diff or range, allowlist.
  Report only findings that change PASS/FAIL, with file and line evidence.
- Commit by concern, docs apart from behaviour. **Push every stable, scanned
  range** (authorised 2026-10-29); do not batch unpushed commits beyond review.
- Subagent shells are synchronous: foreground builds with large timeouts.

## Publication rules

- Push each stable range after the publication gate. Scan the range, the tree,
  messages, identities, tags and provider content before publishing, and record
  the result in `devnotes/perf/VALIDATION.md` before the push commit.
- Prefer the project noreply identity. No personal paths, emails, secrets
  or private test data in tracked files; the corpus stays in the gitignored
  `$RT_TESTDATA/local/`.
- Do not rewrite pushed history.

## Defect trace: second image stays pixelated on zoom (fixed)

Fixed after this trace: `CropHandler::close()` disabled the handler and
`newImage()` never re-enabled it, so `setDetailedCrop()` returned early.
Re-enabling on bind restored the render and the zoom. Kept because the
trace shows how the single-tab state was carried across an image switch.

**Symptom** (confirmed): single-tab. Load a second image from the file browser,
then double-click the image to zoom. The viewport stays at the first
progressive stage: a small, pixelated image with the grid overlay and a `0%`
zoom indicator.

**Reproduced** with the OpenCL build, a seeded monitor profile and POCL, using
`RT_STAGE_PROFILE` to count passes:

| Step | Device records | Trace |
| --- | --- | --- |
| first image loaded and zoomed | 20 | `setZoom old=200 new=200 enabled=1`, crop `0,0 8280x5512`, `viewport.output` ran |
| second image loaded | 21 | one `monitor.output` at `552x828` (scale 10); no transform, no final pass |
| double-click on second image | 21 | `setZoom old=200 new=500 enabled=0`, crop `0,0 5512x8280`, **no update() call** |

Two findings from the trace:

1. After the image switch the crop handler is `enabled=0`, so
   `CropHandler::setZoom()` never calls `update()` and no crop or preview pass
   is requested on zoom.
2. The fit zoom chosen for the portrait image is `500` (factor 0.02) whereas a
   fit for the 1200x680 window needs roughly `200`; the resulting view is the
   small block in the screenshot.

The first image keeps `enabled=1`, so the difference is state carried across
the image switch in single-tab mode. The next step is to instrument
`CropHandler::setEnabled`, `CropWindow::zoomFit()`/`getFitZoom()` and the
editor load path to see which one resets `enabled` and which device size the
fit uses. Do not patch blindly: the fit value and the disabled flag may share
one stale input.

## Block 4 completion plan (decided 2026-10-28)

The user chose: (1) every remaining node gets a device implementation,
including the raw six; (2) per-stage package and matrix validation; (3) stage
OpenCL is promoted to default once Stages A-C land with coverage and parity.

Stage A (next): `thumbnail.scale`, `thumbnail.rgb`, `thumbnail.lab`, then
device-buffer reuse. Stage B: `framing`, `output.sharpen`, `gamut`,
`working.trc`, `ciecam`. Stage C: `crop`, `lab.tools`, `lab.sharpening`,
`lab.wavelet`, `local.adjustments`. Stage D: the raw six. Stage E: residency
policy, `--formats`/`--monitor` on a device build, promotion, full parity and
closure.

### `thumbnail.scale` analysis (Stage A, step 1)

- `stageScale` calls `resizeTo<Imagefloat>(rwidth, rheight, state.interp,
  thumbImg)`, and `thumbImg` is an `ImageIO*` whose concrete type depends on
  the source: `Image8` for the quick/cached path, `Image16` for raw caches,
  `Imagefloat` for float sources.
- `resizeImgTo` has three paths: an exact-size copy, `TI_Nearest` (integer
  floor mapping), and `TI_Bilinear` (floor seed plus a fractional blend of the
  four neighbours, with the right/bottom edge clamped). `state.interp` is
  `TI_Nearest` for the quick path and `TI_Bilinear` for the full one.
- The destination dimensions depend on `params.coarse.rotate`: 90/270 swap
  them, and the height is derived from the cached thumbnail's aspect.
- The conversions matter: `convertTo` maps U8 to float and U16 to float with
  fixed rules, and a device node must reproduce them exactly. The cleanest
  split is to convert the small source to F32 on the host when it is U8/U16 and
  let the device do the resize, or to pass the source format to the kernel and
  cast. Decide on the first implementation and record the choice.

## User-requested queue (2026-10-28)

1. ~~`thumbnail.transform`~~ Done, including the U8 quick path and both tests.
2. `output.sharpen` — not started. It routes into
   `ImProcFunctions::sharpening` (~400 lines, several methods); a proper port is
   a day and must not be faked with a subset.
3. ~~The upstream crop/zoom defect.~~ Fixed: `CropHandler::close()` disabled the
   handler and `newImage()` never re-enabled it, so `setDetailedCrop()` returned
   early. Re-enabling on bind restores the render and the zoom; the second image
   now adds `viewport.output` and `monitor.output` records and renders sharp.

## Stage C status (2026-10-31)

Done and pushed:

- `local.adjustments` local sharpening, undamped and damped, through the output
  sharpening core's mask/RLD kernels. Undamped worst 0.926 LSB16, damped worst
  0.871 LSB16, residuals host byte-identical, full POCL gate PASS.
- `crop` device node: exact three-plane copy (`AE=0` on six shapes), refusals
  before any transfer, call-site `memcpy` unchanged as the fallback, export
  gate 1 LSB16.

Next unit: `lab.sharpening` impulse denoise + defringe landed 2026-10-31 as a
new `lab.sharpening` node with a three-plane L/A/B ABI (`a0040d028`).
Impulse is bit-exact; defringe corrected 218/218 with a/b within 0.000977
LSB16; the pair refused the CIE-CAM arm, images below 8x8, thresholds outside
0..100, a radius in 0.25..1.15, a device without fp64 and an oversized window.
ML sharpen edges and ML microcontrast landed 2026-11-03 in the same node and
the same atomic block (`b864f6168`). Edges is bit-exact (`AE=0`) for 1..4 passes, the L-only
and L/a/b arms and the CIE-CAM arm; microcontrast is within 0.036 LSB16 because
the host's `buildBlendMask` sigmoid uses the SSE reciprocal square root while
the device evaluates it exactly. Eleven device cases cover the arms, strengths
and the combined impulse+defringe pass, all non-vacuous.
`labsharpening-edges.pp3` is `AE=0` on the export corpus and
`labsharpening-micro.pp3` is 1 LSB16, both recording `impl=opencl`. Refusals: edge passes outside 0..4, edge amount
outside 0..100, microcontrast amount outside 0..100, contrast outside 0..200
and uniformity outside 0..10. The CIE-CAM arm keeps ML sharpen edges on the Lab
image and leaves impulse, defringe and microcontrast to the CIE-CAM pipeline.

The last two `lab.sharpening` residuals closed 2026-11-03 in the same node:
the whole legal defringe radius range and the CIE-CAM arm. `PF_correct_RT()`
uses the default `GAUSS_STANDARD` blur, so the host split is 3x3 below 0.6 and
the separable IIR from 0.6; the device reuses `rt_sharpen_fir3` for the 3x3
arm, which is bit-identical (`PAE 0`) at radii 0.25, 0.5 and 0.59, and the IIR
arm is within 0.000732 LSB16 at 0.6, 1.0 and 1.15. Eleven radius cases include
the edges and microcontrast combinations (worst 0.012695 LSB16), and each is
non-vacuous. The CIE-CAM arm is device for the only pass it owns on the Lab
image: edges at radii 0.5 and 1.0, with the main pass beside it, is `AE=0` and
non-vacuous; the defringe hue table is not built there, nor with defringe off
or below 0.25, so a curve the device table cannot follow never refuses a pass
it cannot reach. `labsharpening-fir3.pp3` (radius 0.5) and
`labsharpening-iir.pp3` (radius 1.0) are 1 LSB16 export gates recording
`impl=opencl`. The prescaled export inputs are device too: the node carries
the export scale, `labSharpeningScaleParams()` is the one
`adjust_procparams()` formula, and the launcher validates the prescaled
threshold and radius against the bounds that formula derives. Ten cases
across scales 0.25, 0.5, 0.75, 1.5 and 2 combine impulse, defringe, both ML
arms and the main pass; the worst is 0.063477 LSB16, all are non-vacuous, and
the out-of-range scale, threshold and radius cases stay host residuals. Above
the 2.0 impulse low-pass clamp the mask is host-derived from `markImpulse()`
and only the correction stays on the device, so the scaled threshold reaches
the host output without a kernel change. The fast-export gates
`labsharpening-scaled.pp3` (scale 0.5, impulse) and
`labsharpening-prescaled.pp3` (scale 1.5, radius 0.5 prescaled to -1, the
newly admitted Gaussian no-op) are 1 LSB16 and record `impl=opencl`.
Remaining: the CIE-CAM pipeline's own `defringecam`, `impulsedenoisecam` and
`MLmicrocontrastcam` on the `CieImage`, which belong to the CPU-only `ciecam`
node.

After that, in order: `lab.tools` residuals. The black-and-white cheap branch
landed at `43d6f88bd`: `chromiLuminanceCurve()`'s early return zeroes a/b when
`labCurve.enabled` and `colorToning.enabled` are both false, and the node now
reproduces that in the Lab-curve slot (B&W-only exact, a/b exactly 0, combos
with SH/LC/vibrance at PAE 0, both other arms refused with host PAE 0).
Remaining: the `lab.tools` residuals are now only a `LabRegions` profile
outside the bounded set, a local-contrast slider outside the ABI range, EPD
and a profile where no device tool would run. Dense hue curves are device
cases now: the host carries each `FlatCurve` polyline as double nodes and the
40 000-point curve that used to be refused is bit-exact. B&W with colour
toning landed
as a device case: the host's `bwonly` excludes it, so the block takes neither
arm and the node exports the block's flag. The
LAB/XYZ/XYZREL Munsell methods (`gamutLchonly`/`gamutmap`) are device cases
now; see the progress log. Non-zero Lab contrast landed with the host `hist16`
prefix. B&W with `labCurve.enabled` landed at `9fca53a2d`: the `bwToning` arm
is faithful, including the non-zero a/b delta when an a/b table is active
(the closing block writes `a=atmp, b=btmp`, not zero) and MUN through the
Munsell tables (not `gamutLchonly`, which is the Lab gamut correction). All
seven variants are PAE 0. The chromaticity slider and C=f(H)/L=f(H)/H=f(H)
landed at `eb3aa5b81` with host-built `FlatCurve` tables, a 5e-7 fidelity
bound, worst 0.023438 LSB16, and a dense curve refused at host PAE 0.
Non-LabGrid tone methods are not a refusal: rgb.proc applies them before the
Lab block, and
Splitco/Splitlr/RGBSliders are admitted with PAE 0 at `6c78a463a`. Then the
`lab.wavelet` panels
(denoise, noise, clarity, soft radius; the chroma `link` arm landed at
`6bb3101da` with 12 cases at PAE 0, the edge plain arm at `c5ed40c3c` with
17 cases, worst 0.058594 LSB16, and 12 residuals at PAE 0, the Medianlev
Lipschitz control at `ed9f2a1e7` with 15 cases, worst 0.023438 LSB16, and 14
residuals at PAE 0, the final curves
with 13 cases, worst 0.015625 LSB16, and 14 residuals at PAE 0, toning
with 12 cases, worst 0.011719 LSB16, and 10 residuals at PAE 0, the
standard denoise L arm plus its forced chroma arm with 17 cases, worst
0.015625 LSB16, and 12 residuals at PAE 0, the BiShrink pair with 13 cases,
worst 0.037109 LSB16, and 4 residuals at PAE 0, and CLmethod's selection plus
the clarity arm with 20 cases, exact except the denoise pair at
0.015625 LSB16, and 8 residuals at PAE 0, balance chroma with 10 cases,
worst 0.052734 LSB16, and 11 residuals at PAE 0),
then `local.adjustments` exposure, then the wavelet panels. The common mask,
Retinex, CIE CAM and local capture sharpening have landed. EPD stays a
permanent residual.

## Next immediate task

Block 4 (device node coverage) and the P1 workstream are closed.

P1 evidence: fast export runs through `StagePath::FastExport` with `AE=0`
against the pre-change binary; non-raw thumbnails are covered by
`thumbnail-stage-test` and inspector mode is a documented `n/a`; the three
`raw.base` sub-blocks are named export nodes (`AE=0` on PEF and NEF); the
remaining four export folds have no buffer boundary and are covered by their
parents; the preview dirty residuals are unmapped-by-design with tests;
`summarize-stage-profile.py` reports per-node time and cache hits; and
`bench-preview.sh` uses the sweep harness with a sample count.

The two promotion gates were measured and **not met**:

1. Node cache: a preview sweep hits twice in five samples but the mean settled
   latency is 307.1 ms against 305.9 ms without the cache, inside the noise.
   The default stays off.
2. Stage OpenCL: the gate is green on POCL CPU, but no GPU was available, so
   the same rows have not been run on the target device. The default stays
   opt-in.

The next workstream is P2: the multi-thread determinism residuals, tile
ordering/fusion, the `crop-stage-test` stall, and the GUI dump pairs for tone
curve and crop geometry. Residuals are not unstarted work; the
decisively-partial list is in `STAGE_PIPELINE.md` and `PARITY.md`.
