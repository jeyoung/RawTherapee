> **DEPRECATED — historical record.**
> Do not read for current state. Current record: ../BASELINE_PARITY.md.
> The uncorrected-baseline and AMaZE exception/KNOWN narrative below is
> historical only; the current record is all-exact.

# Baseline parity against `dev`

This record answers the requirement to compare every workstream change against
the current RawTherapee baseline in `dev`, not only against the same commit.

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
| Candidate commit | `17a5995c5` at the time of the run |
| Candidate build | `build/cpu-perf-graph` |
| Data | `rtdata` bound to both `/usr/local/share/rawtherapee` and `/usr/local/share/rawtherapee-graph` |
| Threads | `OMP_NUM_THREADS=1` |
| Device | `--gpu=off` |
| Settings/cache | separate directories per binary |

The binaries were run under `bwrap` with both data paths bound to the same
`rtdata`, so a data-directory difference cannot explain a pixel difference.

The cherry-pick of `2ebf08b0f` conflicted on `.github/workflows/perf-graph.yml`
(absent from `dev`) and `rtengine/CMakeLists.txt`. Only the two source files
above were taken from that commit; the workflow, the CMake test-target wiring
and `rtengine/tests/amaze_tile_parity_test.cc` were not applied. The corrected
baseline therefore builds the same engine change without the graph-workstream
test target.

## Exact results

All exports are uncompressed 16-bit TIFF, compared with
`tools/opencl/compare_tiff.py --bits 16`.

| Image | Profile | Maximum LSB16 | Differing samples | Result |
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

The suite sidecar row with `VNG4 + sharpen-usm` also reports
`sidecar export AE=0`.

## The AMaZE tile-scratch fix

The default neutral profile uses AMaZE. Against the uncorrected `dev` baseline
the row differed by 56 LSB16 / 160 samples on `colorspace_flowers.pef` and
1 LSB16 / 1 sample on `nikon-z7-45mp.nef`. Those differences came from the
AMaZE tile-scratch fix, commit `2ebf08b0f` (`perf: #0: reset AMaZE scratch
before every tile`), which is present on `perf-graph` and absent from `dev` at
`9c3426339`.

Once the corrected baseline includes that fix, both images are pixel-exact:
**0 LSB16, 0 differing samples** on the default AMaZE row. The earlier
isolation evidence remains consistent with this result:

- switching only the demosaic method to VNG4 or MONO made the same images
  pixel-exact before the baseline was corrected;
- every other profile tested on top of VNG4 was also exact;
- the AMaZE fix commit is not reachable from `dev` at the baseline commit;
- the candidate's own thread matrix is AE=0 at 1/2/4/8 threads, so the
  difference is not run-to-run nondeterminism on this host.

The AMaZE row is therefore no longer an exception and carries no relaxed
tolerance. It is an ordinary exact row in the automated gate.

## Coverage that is not a `dev` CLI comparison

- **Thumbnail:** `dev` has no `thumbnail-stage-test` target. The candidate
  harness still checks graph versus the legacy path inside the candidate
  (`embedded` and raw fallback byte-identical). A direct `dev` baseline
  thumbnail comparison is therefore not available.
- **Crop:** the candidate harness checks graph versus legacy crop PPMs for fit,
  100%, zoomed and the staged Lab/CIE-CAM/gamut/local-adjustment cases. The
  VNG4 `crop.pp3` export above additionally matches `dev` exactly.
- **Preview:** the headless preview-latency row passes (5 samples, p50 307 ms,
  p95 308 ms). A GUI screenshot comparison against the *uncorrected* `dev`
  baseline with `tools/perf/test-preview-parity.sh` was `AE=0` for `fit`, while
  `100`, `zoom`, `pan` and `fit2` differed. That run predates the corrected
  baseline and has not been repeated; its AMaZE-domain differences are
  expected to disappear, but no exact-preview claim is made here. A VNG4-default
  preview comparison remains the clean way to isolate that scene, and is not
  yet automated.
- **Device:** the device path is compared with the host path, not with `dev`
  device execution, because the baseline build has no graph/device matrix.

## Automated gate

`tools/perf/test-baseline-parity.sh` automates this matrix. It requires
`bwrap`, binds one `rtdata` tree to both compiled data paths, preflights both
bind targets, uses separate settings/cache, and requires the VNG4, MONO,
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
