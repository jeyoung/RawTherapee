# Baseline parity against `dev`

Current record: every workstream change is compared against the current
RawTherapee baseline in `dev`, not only against the same commit. Earlier
uncorrected-baseline and AMaZE exception narrative is in
[`deprecated/BASELINE-PARITY-HISTORY.md`](deprecated/BASELINE-PARITY-HISTORY.md)
(deprecated; historical only).

## Baseline

| Item | Value |
|---|---|
| Baseline branch | `dev-amaze` (created from `dev`) |
| Baseline commit | `9c3426339` (`opencl: #0: benchmark ROCm gfx1200 against the CPU path`) |
| Baseline patch | `2ebf08b0f` (`perf: #0: reset AMaZE scratch before every tile`), applied to `rtengine/amaze_demosaic.h` (new) and `rtengine/amaze_demosaic_RT.cc` only |
| Baseline source | `.worktrees/dev-amaze` |
| Baseline build | `build/cpu-dev-amaze` |
| Baseline CLI | `build/cpu-dev-amaze/rtgui/rawtherapee-cli` (the corrected baseline has no graph CLI) |
| Candidate branch | `perf-graph` |
| Candidate commit | `f7f3d2848` |
| Candidate build | `build/cpu-perf-graph` |
| Data | one `rtdata` tree bound to both `/usr/local/share/rawtherapee` and `/usr/local/share/rawtherapee-graph` |
| Threads | `OMP_NUM_THREADS=1` |
| Device | `--gpu=off` |
| Settings/cache | separate directories per binary |

Both binaries run under `bwrap` with the same data tree, so a data-directory
difference cannot explain a pixel difference. The cherry-pick of `2ebf08b0f`
conflicted on the workflow and CMake files, which are absent from `dev`; only
the two source files above were taken. The corrected baseline therefore builds
the same engine change without the graph workstream test target. The recorded
parity runs were made at `17a5995c5`; the later two commits add tests only and
do not change engine code.

## Exact results

All exports are uncompressed 16-bit TIFF, compared with
`tools/opencl/compare_tiff.py --bits 16`. Values are maximum LSB16 / differing
samples.

| Image / format | Row | Maximum LSB16 | Differing samples | Result |
|---|---|---:|---:|---|
| `colorspace_flowers.pef` | VNG4 neutral | 0 | 0 | PASS |
| `colorspace_flowers.pef` | MONO neutral | 0 | 0 | PASS |
| `colorspace_flowers.pef` | VNG4 + `sharpen-usm.pp3` | 0 | 0 | PASS |
| `colorspace_flowers.pef` | VNG4 + `rotate.pp3` | 0 | 0 | PASS |
| `colorspace_flowers.pef` | VNG4 + `crop.pp3` | 0 | 0 | PASS |
| `colorspace_flowers.pef` | default AMaZE neutral | 0 | 0 | PASS |
| `nikon-z7-45mp.nef` | VNG4 neutral | 0 | 0 | PASS |
| `nikon-z7-45mp.nef` | MONO neutral | 0 | 0 | PASS |
| `nikon-z7-45mp.nef` | VNG4 + `sharpen-usm.pp3` | 0 | 0 | PASS |
| `nikon-z7-45mp.nef` | VNG4 + `rotate.pp3` | 0 | 0 | PASS |
| `nikon-z7-45mp.nef` | VNG4 + `crop.pp3` | 0 | 0 | PASS |
| `nikon-z7-45mp.nef` | default AMaZE neutral | 0 | 0 | PASS |
| TIFF generated from PEF | VNG4 neutral | 0 | 0 | PASS |
| TIFF generated from PEF | MONO neutral | 0 | 0 | PASS |
| TIFF generated from PEF | VNG4 + `sharpen-usm.pp3` | 0 | 0 | PASS |
| TIFF generated from PEF | VNG4 + `rotate.pp3` | 0 | 0 | PASS |
| TIFF generated from PEF | VNG4 + `crop.pp3` | 0 | 0 | PASS |
| TIFF generated from PEF | default AMaZE neutral | 0 | 0 | PASS |
| JPEG generated from PEF | VNG4 neutral | 0 | 0 | PASS |
| JPEG generated from PEF | MONO neutral | 0 | 0 | PASS |
| JPEG generated from PEF | VNG4 + `sharpen-usm.pp3` | 0 | 0 | PASS |
| JPEG generated from PEF | VNG4 + `rotate.pp3` | 0 | 0 | PASS |
| JPEG generated from PEF | VNG4 + `crop.pp3` | 0 | 0 | PASS |
| JPEG generated from PEF | default AMaZE neutral | 0 | 0 | PASS |

## No AMaZE exception

The default neutral profile uses AMaZE. The corrected baseline includes
`2ebf08b0f`, so the default AMaZE row is pixel-exact on every image and format:
**0 LSB16, 0 differing samples**. There is no relaxed tolerance and no
exception row. Any difference is `FAIL`. Earlier uncorrected-baseline findings
are in the deprecated record and do not apply here.

## Preview baseline

The preview was compared under isolated settings with
`tools/perf/test-preview-parity.sh`:

| Baseline | fit | 100% | zoom | pan | fit2 |
|---|---|---|---|---|---|
| `perf-graph` pre-consolidation `a1c3e84d2` | AE=0 | AE=0 | AE=0 | AE=0 | AE=0 |
| `dev` `9c3426339` | AE=0 | differs | differs | differs | differs |

The pre-consolidation `perf-graph` comparison is exact and is the clean
handler-registry evidence. The current `dev` preview differs at 100%, zoom, pan
and fit2 because `dev` carries preview changes that are not on `perf-graph`.
That comparison is confounded: it does not isolate this branch and is not a
handler-registry regression. A VNG4-default preview comparison remains the
clean way to isolate a scene against `dev`, and is not yet automated.

## Coverage that is not a `dev` CLI comparison

- **Thumbnail:** `dev` has no `thumbnail-stage-test` target. The candidate
  harness checks graph against the legacy path inside the candidate, with
  embedded and raw fallback byte-identical. A direct `dev` baseline thumbnail
  comparison is not available.
- **Crop:** the candidate harness checks graph against legacy crop PPMs for fit,
  100%, zoomed and the staged Lab/CIE-CAM/gamut/local-adjustment cases. The
  VNG4 `crop.pp3` export above additionally matches `dev` exactly.
- **Preview:** the headless preview-latency row passes (5 samples, p50 305 ms,
  p95 307 ms). The GUI comparison is recorded above.
- **Device:** the device path is compared with the host path, not with `dev`
  device execution, because the baseline build has no graph/device matrix.

## Automated gate

`tools/perf/test-baseline-parity.sh` automates this matrix. It requires `bwrap`,
binds one `rtdata` tree to both compiled data paths, preflights both bind
targets, uses separate settings/cache, and requires the VNG4, MONO,
composed-profile and default AMaZE rows to be pixel-exact (AE=0). Any
difference is `FAIL`.

## Reproduce

Run the baseline and candidate CLIs with `rtdata` bound to both
`/usr/local/share/rawtherapee` and `/usr/local/share/rawtherapee-graph`,
`OMP_NUM_THREADS=1`, `--gpu=off`, and separate settings/cache directories. Use
`export-one.sh --build-dir build/cpu-dev-amaze` for the corrected baseline and
`export-one.sh --build-dir build/cpu-perf-graph` for the candidate, then
`compare_tiff.py --bits 16`. All rows, including default AMaZE, must be exact.
The run artefacts are transient and are not retained in the repository.
