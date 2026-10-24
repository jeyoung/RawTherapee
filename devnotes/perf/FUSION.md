# Export buffer-traffic fusion

This record covers the phase 9 review of buffer traffic and redundant OpenCL
transfers in the export path (`rtengine/simpleprocess.cc` and
`rtengine/opencl/openclengine.*`). It states what was fused, what was rejected
and why, and the evidence for each acceptance criterion. The behaviour change
is commit `47c50ee77`.

## Problem space

Two exporters run the same `ImageProcessor` stages:

- **Default export** loads and demosaics the full frame, converts to the
  working space, processes in Lab, then crops and resizes at the end of
  `stage_finish()`. Enabled by `-p profile`.
- **Fast export** (`-f`) runs `stage_early_resize()`: it converts the frame to
  Lab, applies the crop, resizes in Lab and converts back to RGB before
  `stage_finish()`.

A 45.7 MP frame is 545 MB as RGB float and 545 MB as Lab float (12 bytes per
pixel), so every full-frame pass costs about 1.1 GB of reads and writes. The
question for this phase was which of those passes are redundant.

Constraints:

- 16-bit CPU export must be pixel-identical to the current branch (`AE=0`)
  for the neutral, sidecar and fast-export paths.
- OpenCL must stay within `max_pae <= 1.5259e-05` (1 LSB at 16-bit).
- No maths changes: only per-pixel-identical transformations or removed
  copies.
- The OpenCL stage split stays as it is; no asynchronous overlap.

## Candidates and verdicts

| Candidate | Location | Verdict |
|---|---|---|
| Full-frame RGB→Lab conversion before the early-resize crop | `stage_early_resize()` | **Taken.** Only the crop rectangle survives, so convert that rectangle and drop the copy out of the full-frame Lab image. |
| Lab crop copy at the end of the default export | `stage_finish()` | **Rejected.** `ImProcFunctions::Lanczos` has no region overload and `LabImage` owns its row pointers, so removing the copy needs changes in `improcfun.*`/`labimage.*`, which are outside this phase. |
| Working-TRC `Lab→RGB→Lab` round trip | `stage_finish()` | **Rejected.** `lab2rgb`/`rgb2lab` are not bit-exact inverses (TRC polynomials), so removing the round trip changes pixels. |
| Pre-TRC reference copy for `apsatur()` | `stage_finish()` | **Taken.** A full-image copy (about 545 MB at 45.7 MP) was made even when the saturation pass was disabled. It is now allocated and copied only when `wapsat > 0`. |
| Transform output buffer | `stage_transform()` | **Rejected.** The warp reads a neighbourhood; the destination cannot alias the source. |
| OpenCL immutable buffers (LUTs, matrices, gamma tables) | `openclengine.cc` | **Rejected — measured neutral.** See below. |

## Change

`stage_early_resize()` converts the kept rectangle directly:

- before: `rgb2lab` over the whole frame (read 545 MB, write 545 MB) then a
  Lab crop copy (read 455 MB, write 455 MB) — 2000 MB of traffic and a 1000 MB
  pair of Lab buffers;
- after: `rgb2lab` over the crop rectangle (read 455 MB, write 455 MB) —
  910 MB and one 455 MB buffer.

`rgbToLabRegion()` is the per-pixel body of
`ImProcFunctions::rgb2lab(Imagefloat&, LabImage&, workingSpace)` restricted to
a rectangle: the same `ICCStore::workingSpaceMatrix()` value and the same
`Color::rgbxyz()`/`Color::XYZ2Lab()` calls in the same order per pixel. There
is no neighbourhood or histogram dependency, so the result is bit-identical to
converting the whole frame and copying the rectangle. A comment at the
definition points at the canonical implementation to keep the two in step.

`stage_finish()` now allocates the `apsatur()` reference image only when
`params.icm.wapsat > 0`; the copy keeps the same pre-TRC content and position
in the sequence.

`stage_early_resize()` also gained an `export.early_resize` profile section.
The fast pipeline had no outer stage timings, which made this work
unmeasurable.

## Acceptance criteria and evidence

Image: 45.7 MP NEF (8256×5504 frame, 8213×4620 sidecar crop, 1920×1080 output).
Hardware: Intel Core i7-3770 (4 cores / 8 threads), AMD RX 550, Debian 13.
Host load averaged 11–20 from unrelated work during the runs, so absolute
times are inflated; A/B arms were interleaved run by run.

### AC1 — CPU 16-bit export is pixel-identical (`AE=0`) — PASS

`compare_tiff.py` against the pre-change binary, `OMP_NUM_THREADS=8`,
`--gpu=off`, 16-bit TIFF:

| Profile | Samples compared | `max_abs` | Differing | Result |
|---|---:|---:|---:|---|
| neutral | 136,918,080 | 0 | 0 | PASS |
| sidecar (default export) | 6,220,800 | 0 | 0 | PASS |
| sidecar (`-f`, exercises the region conversion) | 6,220,800 | 0 | 0 | PASS |
| sidecar (`-f`), 1 thread vs 1 thread baseline | 6,220,800 | 0 | 0 | PASS |
| resize 0.5x with `-f` (no crop, whole-frame branch) | 34,229,520 | 0 | 0 | PASS |
| `WorkingTRC=22`, `TrcExp=true`, `WorkingTRCsat=0.5` (copy still needed) | 34,229,520 | 0 | 0 | PASS |
| `WorkingTRC=22`, `TrcExp=true`, `WorkingTRCsat=0.0` (copy eliminated) | 34,229,520 | 0 | 0 | PASS |

The two `WorkingTRC` profiles are local scratch profiles (a 0.5x resize plus
the colour-management keys); they are not committed. They exercise
`apsatur()` with the reference copy present and absent.

Thread determinism is unchanged: 8 threads vs 1 thread differs on 13 pixels by
1 LSB both before and after the change. That is a pre-existing fast-pipeline
characteristic, not an effect of this work.

### AC2 — OpenCL stays within 1 LSB — PASS

| Check | Result |
|---|---|
| `verify.sh --quick` (PEF neutral, `lab2srgb` stage forced) | PASS `max_pae=1.5259e-05` |
| `verify.sh --quick` monitor self-test | PASS, max 1 byte, 14/196608 samples |
| `verify.sh --formats` (PEF bayer14) | PASS `max_pae=1.5259e-05` |
| 45.7 MP NEF / resize 0.5x, `--gpu=off` vs `--gpu=auto` | `max_pae=1.5259022e-05` (1 LSB), PASS |
| 45.7 MP NEF / sidecar, `--gpu=off` vs `--gpu=auto` | `max_pae=1.5259022e-05` (1 LSB), PASS |

### AC3 — fast export is measurably faster — PASS

Stage timings (`RT_STAGE_PROFILE=1`, sidecar, `-f`, `OMP_NUM_THREADS=8`),
5 interleaved runs per arm. The baseline arm is the previous pipeline plus
only the new profile line, so both arms measure the same scope:

| Arm | `export.early_resize` (ms), 5 runs | Median |
|---|---|---:|
| before | 2894, 2982, 3957, 3056, 3399 | 3056 |
| after | 1181, 1148, 1180, 1108, 1114 | 1148 |

Every "after" run is below every "before" run: **−1908 ms, −62 %** of that
stage. A confirmation set of three interleaved pairs taken later under heavier
load gave 3116 ms vs 2007 ms medians (7947/2774/3116 against
2007/2817/1592 ms), the same direction with a larger noise spread.

Whole-export wall time (same profile and flags), 8 interleaved pairs:

| Arm | Median | Peak RSS median |
|---|---:|---:|
| before | 46.82 s | 2.87 GB |
| after | 43.81 s | 2.77 GB |

`−3.0 s (−6.4 %)` median wall time, 6 of 8 pairs favouring the change; peak
RSS is 124 MB lower in all 8 pairs, consistent with the removed 545 MB
full-frame Lab allocation. The host noise floor measured with a same-binary
A/B pair is 7–25 %, which is why the isolated stage figure is the primary
evidence.

### AC4 — default export is unaffected — PASS (no change)

The default export does not enter `stage_early_resize()` and the sidecar has
`WorkingTRC=none`, so neither change is executed. Measured on the documented
resize profile (resize 0.5x, no crop), 3 interleaved pairs: 6.10 s vs 6.33 s
medians, spread 5.59–6.38 s — inside the noise floor, as expected for an
unchanged code path. Pixel output is `AE=0` (AC1).

### AC5 — both configurations build — PASS

```bash
tools/build-rt.sh cpu -j 4 --ccache --stage-modules
tools/build-rt.sh opencl -j 4 --ccache --stage-modules
```

Both complete with no new warnings in `simpleprocess.cc`.

## OpenCL immutable-data transfers — measured, not taken

The immutable payload per call is small compared with the image planes:

| Call | Immutable upload | Image traffic per call |
|---|---|---|
| `labToSrgbOut` (default output conversion) | 256 KB gamma LUT + 36 B matrix | ~61 MB (3 Lab planes up, 3 RGB planes down at 1944×1304) |
| `rgbProc` (opt-in, `RT_OPENCL_RGBPROC=1`) | 9 × 256 KB LUTs + 2 matrices (≈ 2.25 MB, one identity LUT included) | ~272 MB (3 RGB planes up, 3 Lab planes down at 11.3 MP) |

Measured stage times for the 45.7 MP resize export with
`RT_OPENCL_PROFILE=1`, `OMP_NUM_THREADS=8`: `opencl resize` 1153 ms,
`opencl lab2srgb` 849 ms.

A content-keyed buffer cache would save at most 256 KB of upload (~50 µs) per
default export and ~2.25 MB (~0.4 ms) per opt-in `rgbProc` call. A content hash
over the same bytes costs the same order as the upload it replaces, and a
sampled fingerprint could serve stale data, which the brief forbids. The
redundant traffic that does matter is the Lab buffer crossing to the host and
back between `rgbProc` and the output conversion; that is a device-resident
segment and belongs to a later step that changes the stage split. No OpenCL
code was changed.

## Reproduce

```bash
export RT_SETTINGS="$PWD/build/test-scratch/phase9/settings"
export RT_CACHE="$PWD/build/test-scratch/phase9/cache"
cli=build/cpu-phase9-fusion-opencl/rtgui/rawtherapee-cli
img=/path/to/45mp.NEF

# parity against a pre-change binary
OMP_NUM_THREADS=8 "$cli" --gpu=off -f -p "$img.pp3" \
    -o cand.tif -t -b16 -Y -c "$img"
python3 tools/opencl/compare_tiff.py base.tif cand.tif --bits 16

# isolated stage timing
RT_STAGE_PROFILE=1 OMP_NUM_THREADS=8 "$cli" --gpu=off -f -p "$img.pp3" \
    -o out.tif -t -b16 -Y -c "$img"
```

## Limitations

- The default export still copies the crop out of the full-frame Lab image
  before Lanczos. Removing that needs a region-aware `Lanczos` or a
  `LabImage` view type; both are outside this phase.
- The fast-export whole-run wall times were taken on a heavily loaded host.
  The stage figure is the reliable number; treat the 6.4 % whole-export delta
  as indicative.
- The OpenCL analysis is arithmetic plus stage timings, not a device-side
  transfer counter: the Rusticl driver does not expose one through this build.
