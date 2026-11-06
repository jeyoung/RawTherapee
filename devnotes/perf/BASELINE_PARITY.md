# Baseline parity against `dev`

This record answers the requirement to compare every workstream change against
the current RawTherapee baseline in `dev`, not only against the same commit.

## Baseline

| Item | Value |
|---|---|
| Baseline branch | `dev` |
| Baseline commit | `9c3426339` (`opencl: #0: benchmark ROCm gfx1200 against the CPU path`) |
| Baseline source | `.worktrees/dev-baseline` |
| Baseline build | `build/cpu-dev` |
| Baseline CLI | `build/cpu-dev/rtgui/rawtherapee-cli` (`dev` has no graph CLI) |
| Candidate branch | `perf-graph` |
| Candidate commit | `1098f658f` at the time of the run |
| Candidate build | `build/cpu-perf-graph` |
| Data | `rtdata` bound to both `/usr/local/share/rawtherapee` and `/usr/local/share/rawtherapee-graph` |
| Threads | `OMP_NUM_THREADS=1` |
| Device | `--gpu=off` |
| Settings/cache | separate directories per binary |

The binaries were run under `bwrap` with both data paths bound to the same
`rtdata`, so a data-directory difference cannot explain a pixel difference.

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
| `nikon-z7-45mp.nef` | VNG4 neutral | 0 | 0 | PASS |

The suite sidecar row with `VNG4 + sharpen-usm` also reports
`sidecar export AE=0`.

## The AMaZE exception

The default neutral profile uses AMaZE and does not match `dev`:

| Image | Profile | Maximum LSB16 | Differing samples |
|---|---|---:|---:|
| `colorspace_flowers.pef` | default AMaZE neutral | 56 | 160 of 30,419,712 |
| `nikon-z7-45mp.nef` | default AMaZE neutral | 1 | 1 of 136,918,080 |

This is the **known AMaZE tile-scratch fix**, commit `2ebf08b0f`
(`perf: #0: reset AMaZE scratch before every tile`), which is present on
`perf-graph` and absent from `dev` at `9c3426339`. The fix zeroes each
worker's scratch at the start of every tile; `dev` can reuse values from the
previous tile. The differing coordinates are isolated to tile boundaries.

The isolation evidence is decisive:

- switching only the demosaic method to VNG4 or MONO makes the same images
  pixel-exact (0 LSB16, 0 samples);
- every other profile tested on top of VNG4 is also exact;
- the AMaZE fix commit is not reachable from `dev` at the baseline commit;
- the candidate's own thread matrix is AE=0 at 1/2/4/8 threads, so the
  difference is not run-to-run nondeterminism on this host.

This is an intentional correctness fix, not a regression from the contract or
handler-registry changes. It is recorded here rather than hidden by a relaxed
tolerance.

## Coverage that is not a `dev` CLI comparison

- **Thumbnail:** `dev` has no `thumbnail-stage-test` target. The candidate
  harness still checks graph versus the legacy path inside the candidate
  (`embedded` and raw fallback byte-identical). A direct `dev` baseline
  thumbnail comparison is therefore not available.
- **Crop:** the candidate harness checks graph versus legacy crop PPMs for fit,
  100%, zoomed and the staged Lab/CIE-CAM/gamut/local-adjustment cases. The
  VNG4 `crop.pp3` export above additionally matches `dev` exactly.
- **Preview:** the headless preview-latency row passes (5 samples, p50 307 ms,
  p95 308 ms). A GUI screenshot comparison against `dev` was run with
  `tools/perf/test-preview-parity.sh`: `fit` is AE=0; `100`, `zoom`, `pan` and
  `fit2` differ. The diff is image-domain and matches the same AMaZE tile-fix
  exception as the neutral export; it is not a UI or handler-registry
  difference. A VNG4-default preview comparison remains the clean way to
  isolate that scene, and is not yet automated.
- **Device:** the device path is compared with the host path, not with `dev`
  device execution, because the baseline build has no graph/device matrix.

## Automated gate

`tools/perf/test-baseline-parity.sh` automates this matrix. It requires
`bwrap`, binds one `rtdata` tree to both compiled data paths, preflights both
bind targets, uses separate settings/cache, and applies the AMaZE bound of
64 LSB16 and 1024 differing samples within the 16 px halo of the 128 px tile
stride. Any VNG4/MONO/profile difference, or an AMaZE difference outside those
bounds, is `FAIL`.

## Reproduce

Run the baseline and candidate CLIs with `rtdata` bound to both
`/usr/local/share/rawtherapee` and `/usr/local/share/rawtherapee-graph`,
`OMP_NUM_THREADS=1`, `--gpu=off`, and separate settings/cache directories. Use
`export-one.sh --build-dir build/cpu-dev` for the baseline and
`export-one.sh --build-dir build/cpu-perf-graph` for the candidate, then
`compare_tiff.py --bits 16`. The default AMaZE row fails the suite's neutral
row by design; the VNG4/MONO isolation rows are the parity evidence for the
current architecture changes. The run artefacts are transient and are not
retained in the repository.
