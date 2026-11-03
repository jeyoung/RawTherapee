# RawTherapeeGraph `perf-graph` workstream

Committed copy of the development plan so work can continue on a machine
without the local plan file. It records state, decisions, per-unit
evidence and next tasks; architecture and invariants stay in the sibling
records. Update it with each landed unit.

Refreshed 2026-11-02. P0 blocks 1-4.1 are complete; block 4 device coverage is
the remaining large piece and is scoped below. Stage C has landed local
sharpening (base and damped), `crop` and the `lab.sharpening`
impulse-denoise/defringe pair; the remaining Stage C units are multi-session
and are ordered below. Each stable, scanned range is pushed from 2026-10-29 on.

## State

- Repository: `$REPO`
- Worktree: the main checkout, branch `perf-graph`, clean and in sync with
  `origin/perf-graph`; the last pushed tip is `26a27a98b`. All pushed ranges
  were scanned per the publication gate.
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
build/portable/rawtherapee-graph-5.13-342-g3871b7d0c_5.13+git13363.3871b7d0c-1_amd64.deb
sha256 c347ed4d42753cbea8bb30848a083ee068f6de920bf40eb474b1cbc480ce5a61
```

Validated at `3871b7d0c`. The branch has advanced since with unit code and
documentation, so the package is evidence for that build only, not for the
current tip.

## Progress log — resumed 2026-10-28 (Block 4 completion)

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
- **`lab.wavelet` edge panel, plain arm (partial)**: `ContAllL()`'s
  `EDmet == 2` block with `cp.detectedge` clear and no `lip3`/`lipp`
  smoothing. The host builds the edge `mea`, the per-level mean/sigma/MaxP
  and the 501-point `ccwcurve` table (the default `ccwcurve` is non-identity,
  so the curve arm is the normal path); the kernel reproduces the strength,
  radius, repartition, `more`/`none`/`less` reinforce, ten-step `beta` and
  `kmul`/`kinterm` arithmetic and touches only the `borderL == 1` interior.
  Because `edgeena` disables the block's a/b level trim, the node also runs
  the a/b decomposition and reconstruction (the round trip is not an
  identity) and keeps the chroma multiply behind its own flags. 17 cases,
  777,753 samples, worst 0.058594 LSB16; 12 refusals at host PAE 0.
  `ed4e71f16`, `c5ed40c3c`. Residuals: median level detection (`calckoe`),
  `NPMethod` low/high, `Lipst`, `EDmethod` `SL` (the threshold adjuster is
  dead under the hard-coded `EDmet == 2`), a zero-strength edge panel,
  out-of-range sliders, an unknown `Medgreinf`, edge with L skin protection,
  and the global `edghi`/`edglo`/`limrad` options are read at build time and
  are not part of the pp3. Two measured-matrix limits: the host's fast
  `xlogf()` versus the kernel's `log()` (below 0.0586 LSB16), and a
  degenerate `mean + sigma == MaxP` level whose curve exponent is singular on
  both sides and is not covered.
- **`lab.wavelet` final curves**: `Expfinal`'s curve arm.
  `finalContAllL()` runs on the statistics `Evaluate2()` recomputes after
  the contrast and edge passes, with the host-sampled 501-point
  `opacityCurveWL` and `sigmafin`; the pixel-domain `wavclCurve` runs after
  reconstruction and the a/b pass, before the block's `L <= 0 -> 1` clamp.
  `Expfinal` keeps the a/b round trip alive without `Expchroma`/`Expedge`.
  Thirteen device cases (diagonal only, opacity only, both, sigmafin
  0.5/2.0, default curves, with contrast, with chroma, with edge, 179x49
  padded, 3 and 7 levels, inert balance controls) match the host block with
  a worst 0.015625 LSB16; fourteen residual configurations (softradend,
  softrad, `BAmethod` sli, `tmr`, `expclari`, sigmafin below/above range,
  avoid, median, expbl, expresid, expnoise, `CLmethod` one and
  skin protection) run the host builtin byte for byte. The export gate
  `labwavelet-final.pp3` records `impl=opencl` at 1 LSB16. The remaining
  final-panel residuals are the guided soft-radius tone mapping and the
  balance-chroma arms. `6a54860e1`, `873000037`.
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
- **`lab.wavelet` clarity soft radius**: `guidedFilter()`'s box means, `a`/`b`
  reduction and bilinear upsample run on the device over the block's
  `provradius` guide, with the subsampling, radius and four scales derived on
  the host. The second pass reproduces `ip_wavelet()`'s final tile-copy clamp
  (`L <= 0 -> 1`) and the merge uses the soft-filtered L unclamped, which can
  be negative; the earlier `inf` failures were the merge kernel's own clamp,
  now removed. Nine cases (softrad 0.5/2/5/100, zero merge, chroma, denoise,
  703x67 subsampled and 8192x64) are exact except the denoise pair at 0.011719
  LSB16, and `labwavelet-softrad.pp3` is exact with `impl=opencl`. `softradend`
  remains a residual.
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
  asks for a refresh (`679f5b4d8`). Ten residuals (`quaMethod` agre,
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
  reference. The denoise curves, the expert pairing and the BiShrink arms
  remain host residuals. `66613d0a9`, `6db958afe`, `679f5b4d8`.
- **`lab.wavelet` denoise curve (`sliMethod=cur`)**: the post-denoise
  refinement. The host samples the block's `WavCurve` over `wavdenoise` into
  501 entries, detects `cp.denoicurv` exactly as the block does, keeps the
  pre-denoise L coefficients and runs `Evaluate2()` before and after the
  denoise. The device carries both statistics, computes the `mixmet` weight,
  the `intp()` mixes and the per-level `logmax`/`rap`/`asig`/`bsig`/`amean`/
  `k`/`maxVal` on the host's doubles, and the kernel ports
  `xlogf`/`xexpf`/`pow_F` and the block's repeated four-band write. Ten cases
  (mixmet 0-3, an identity curve, contrast, edge, final curves, an odd size
  and all panels) match the host with worst 0.043945 LSB16; four residuals
  (`sigm` range and expert pairing) stay on the host.
  `labwavelet-denoise-cur.pp3` records `impl=opencl` at 1 LSB16 (21,068 of
  30,419,712 samples). `ba5f8f1d3`, `dca455499`.
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
  host at PAE 0, and negative damping predicate-refused only. Damped radius
  below 0.6 and capture sharpening remain host residuals. `cd38028bb`,
  `476dc9f24`, `db99dc054`, `7467f563f`, `211c12597`, `2ce2a7aff`,
  `ebbeab951`, `6fe238ede`. Pushed `41d6b5507..6fe238ede`; full gate
  **35 PASS, 0 FAIL, 0 gate SKIP**; CI green.
- **Remaining Stage C**: exposure (FFT PDE), blur/denoise, tone map, retinex,
  CIE, common mask, local sharpening capture; then the wavelet
  denoise/chroma/edges/final/clarity/soft-radius panels. Each is a separate
  multi-session unit. Gate environment: set `TMPDIR` to a project-local
  directory (`/dev/shm` was 100% full and the second 60 MB TIFF in each stage
  row failed).
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
- **A2 `thumbnail.rgb`**: composite coverage proven (`transform` and
  `rgb.proc` records `impl=opencl`, hash chain); the run exposed a real
  `rgb.proc` defect — the shared kernel indexed compactly while stage buffers
  are row-padded, so padded widths sheared and width 179 crashed. Fixed with
  explicit source/destination pitches in the shared kernel and both backends;
  regression test at widths 238/179 against compact-width references; classic
  export byte-identical at a padded width. Residual 1-4 samples at 1 LSB8.
  Commits `36fae8d35`, `2a8381228`, `c7c3387d9`, `96feabcb0`.
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
- Remaining CPU-only pixel nodes: `working.trc`, `ciecam`, `lab.tools`,
  `lab.wavelet`, `local.adjustments`, `thumbnail.lab`, and the raw four
  (`raw.preprocess`, `raw.demosaic`, `raw.base`; `source.*`/`analysis` are
  exceptions).
- **`working.trc` (partial)**: W1 exposed the probe means, the lcms-built
  profile and the probe primaries matrix via setup-only out-params
  (`78c314f00`, `92ec1641b`). W2 runs a host prefix (`lab2rgb` + fixed sRGB
  inverse) so the probe, profile and second-pass input are byte-identical to
  the CPU block, then a device second pass (toxyz, optional gamut map,
  matrix + inverse type-5 TRC, `apsatur`, RGB→Lab). 1 LSB16 on POCL and the
  AMD RX 550, probe means exact. Type-5 profiles only; a type -5 profile
  (CUSTOM gamma < 1) is a host residual, as are unknown working profiles,
  non-relative intents, soft light, local contrast, both tone equalisers and
  B&W. `e5f142741`, `d408ca0df`, `2b2fe1efc`, `67a37700c`, `897c25fd3`,
  `0f1a819cc`, `b07462094`, `8fdcb2fb3`.
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
- **`ciecam`**: scoped. A faithful port needs a host mean/histogram pass, two
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

### Block 4 — device node coverage (the large piece)

Facts established this session, so the next session does not re-derive them:

- The catalogue has 30 nodes (`rtengine/stage/stage_pipeline.cc`); `resize`
  is the only one with a device implementation.
- Only one runtime call site reaches `stage::run` (`rtengine/ipresize.cc:725`).
  The other blocks are graph and scheduling metadata; their pixels still come
  from `ImProcFunctions`.
- Each node therefore needs four pieces: a stage ABI argument set, a refactor
  of that block's runtime path onto `stage::run`, a kernel (or a wrapper around
  an existing `rtengine/opencl/openclengine.cc` kernel), and a parity test at
  1 LSB.
- `tools/opencl/verify.sh --quick` probes the old backend, so it reports "no
  stage executed". Fix the gate first or every coverage run is a SKIP.

Order (reuse before new kernels, cheapest parity first):

1. ~~Teach `verify.sh` to observe stage-device execution.~~ Done, together
   with the precedence fix that makes the device node reachable at all.
2. ~~`transform` (kernel exists in `openclengine`); apply the same
   device-first precedence rule at its call site.~~ Done: shared map builder
   and sampling helpers, device kernel, identity test, extended gate.
3. ~~`output.convert`, `monitor.output`, `viewport.output` behind one shared
   Lab-to-RGB wrapper (kernel exists).~~ Done, with `thumbnail.output` as well:
   shared arithmetic, sub-rectangle views, node names threaded through
   `lab2monitorRgb()`, export gate, packed-path reference test, and
   `monitor.output`/`viewport.output` demonstrated on the host, and
   `thumbnail.output` reached after fixing the missing device policy in
   `rtthumbnail`.
4. ~~Partial `rgb.proc`: tone curve, RGB curves, channel mixer, Lab conversion
   only.~~ Done: the whole classic kernel runs on the device inside its existing
   opt-in; the tools the classic kernel does not cover stay host-side. The
   Lab-to-RGB nodes still take their profile tables from the gated classic
   block, so a future change that disables the classic backend would strand
   them.
5. New kernels: `gamut`, `ciecam`, `working.trc`, `framing`, `output.sharpen`.
6. Composites: `local.adjustments`, `lab.tools`, `lab.sharpening`,
   `lab.wavelet`, `crop`.
7. Thumbnail nodes. `thumbnail.output`, `thumbnail.whitebalance` and
   `thumbnail.transform` (both formats) are done. `thumbnail.scale`,
   `thumbnail.rgb`, `thumbnail.lab` and `thumbnail.decode` remain. Each new
   kernel needs a device test as well as a call-site gate: the white-balance
   test paid for itself immediately.
8. Destructive raw nodes last: `source.select`, `source.embedded`,
   `raw.preprocess`, `raw.demosaic`, `raw.base`, `analysis`.
9. Device-buffer reuse across consecutive nodes, then a residency policy
   (tiers across passes, node-cache interaction, eviction under pressure).
10. Per-node parity plus `--quick`, `--formats`, `--monitor` on a
    device-enabled build.

### P1 gaps

- Fast export (`-f`) is outside the graph.
- Thumbnail `loadFromImage` for non-raw files and inspector mode are not
  graph-covered. New evidence: the TIFF and JPEG GUI runs produce no
  thumbnail-graph line, while the raw run does.
- Folded nodes: preview `analysis`; crop `raw.preprocess`/`raw.demosaic`; five
  export sub-blocks.
- Preview dirty residuals: guide-only crop changes, high-detail transitions,
  deferred analysis.
- Node-level profiling and cache metrics for the integrated graph.
- `bench-preview.sh` remains unreliable; use the sweep harness and record
  sample counts.

### P2 residuals

- Multi-threaded export is non-deterministic in the baseline; parity runs are
  single-threaded.
- Raw-to-RGB preparation is thread-order dependent; denoise leaves a 6/12-pixel
  residual at 1 LSB.
- Tile ordering is opt-in; fusion/reduced-work modes are incomplete.
- `crop-stage-test` can stall on sharpening-only edits with a warm Lab profile.
- GUI scenario coverage is partial: tone curve and crop geometry lack dump
  pairs.
- rusticl is skipped on this host; deterministic device tests restrict to POCL.

### Decisions outstanding

- **Node cache**: default off, 6.5-8.8% on repeated passes. Needs a promotion
  decision and a preference/kill-switch policy.
- **Stage OpenCL**: default off, one node. Promote only when coverage is
  meaningful.
- **Push**: authorised 2026-10-29. The 166-commit range up to `03c94581c` is
  pushed to `origin/perf-graph`; each later stable range is scanned and pushed
  the same way.
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
LSB16; the pair refuses the CIE-CAM arm, images below 8x8, thresholds outside
0..100, a radius in 0.25..1.15, a device without fp64 and an oversized window.
Residuals: that radius range, the CIE-CAM arm, ML sharpen edges/microcontrast,
and prescaled inputs that leave the admitted range (threshold > 100 at scale
>= 2, radius >= 25 at scale ~7.7).

After that, in order: `lab.tools` residuals. The black-and-white cheap branch
landed at `43d6f88bd`: `chromiLuminanceCurve()`'s early return zeroes a/b when
`labCurve.enabled` and `colorToning.enabled` are both false, and the node now
reproduces that in the Lab-curve slot (B&W-only exact, a/b exactly 0, combos
with SH/LC/vibrance at PAE 0, both other arms refused with host PAE 0).
Remaining: B&W with colour toning (the `bwonly` condition excludes it). The
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
17 cases, worst 0.058594 LSB16, and 12 residuals at PAE 0, the final curves
with 13 cases, worst 0.015625 LSB16, and 14 residuals at PAE 0, toning
with 12 cases, worst 0.011719 LSB16, and 10 residuals at PAE 0, the
standard denoise L arm plus its forced chroma arm with 17 cases, worst
0.015625 LSB16, and 12 residuals at PAE 0, and CLmethod's selection plus
the clarity arm with 20 cases, exact except the denoise pair at
0.015625 LSB16, and 8 residuals at PAE 0),
then `local.adjustments` exposure/retinex/CIE, then the common mask
once the wavelet panels land. EPD stays a permanent residual.

## Next immediate task

The `lab.wavelet` clarity arm landed with `5ff287bda` (engine) and
`dba2cc58a` (test and export gate). `CLmethod`'s `one`/`inf`/`sup` selection
now runs on the device for every admitted panel set, the clarity sequence runs
both `ip_wavelet()` passes with the block's forced-`all` first pass, and the
merge kernel reproduces the `mergeL`/`mergeC` and `Backmethod` rules. The
twenty-case matrix is bit-exact except the denoise pair at 0.015625 LSB16, and
both `labwavelet-clarity.pp3` and `labwavelet-clarity-sup.pp3` export with
`AE=0`. The first pass uses the trimmed level count and skips the a/b planes
when the block's own trim drops them to zero; a profile whose `ch[]`, opacity
curve, `cbenab`, `CHmethod`/`chro` or residual sliders would stop that trim
short of zero stays a host residual.

The clarity arm's guided soft radius (`softrad`) landed: the host exports the
block's `provradius` guide and the subsampling scales, the kernels port
`guidedFilter()`, and the second pass now applies the block's `L <= 0 -> 1`
tile-copy clamp while the merge keeps the soft-filtered L unclamped. Nine
cases are exact except the denoise pair at 0.011719 LSB16, and
`labwavelet-softrad.pp3` is exact. Next is `softradend`, the final panel's
guided tone mapping, which adds `guidedFilterLog`, the `strend`/`detend`/
`thrend` blend and its statistics. The standard denoise arm (and the chroma denoise the block forces
at six levels) landed with the unit above; its denoise curves, expert pairing,
BiShrink arms, user chroma strength, noise balance and hue curve stay
residuals. The final curves and the toning zero-protection arm landed with the
units above. The edge panel's plain arm is landed; its median/Lipschitz
detection (`calckoe`), `NPMethod` low/high smoothing, `Lipst`, and the dead
`EDmethod` `SL` threshold adjuster stay residuals, so do not reopen them
without porting the detector. The balance-chroma arm (`TMr` + `BAmethod`) is
refused fail-closed, so do not reopen it without porting `ContAllAB`'s BA arm.
Keep `OMP_NUM_THREADS=1` in every comparison.
