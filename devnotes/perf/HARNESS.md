# Performance and parity harness

The harness is the single source of truth for performance and parity claims on
this branch. Raw files and sidecars stay outside the repository.

## Environment

```bash
export RT_TEST_IMAGE_45MP=/path/to/45mp.NEF
export RT_TEST_PP3_45MP=/path/to/profile.pp3
export RT_PREVIEW_DISPLAY=:98
```

`run-suite.sh` creates an isolated `RT_SETTINGS`, `RT_CACHE` and `TMPDIR`
under `build/test-scratch/harness/<timestamp>`.

## Commands

```bash
# Full gate (parity, thread matrix, timing, preview if available)
tools/perf/run-suite.sh \
    --build-dir build/cpu-perf-stage-runtime \
    --baseline-dir /path/to/baseline-build \
    --image "$RT_TEST_IMAGE_45MP" \
    --pp3 devnotes/opencl/pp3/resize.pp3 \
    --graph \
    --report build/test-scratch/reports/run1

# Optional denoise thread matrix (expects a local profile)
tools/perf/run-suite.sh ... --denoise-pp3 /path/to/denoise.pp3

# Preview state controls
RT_PREVIEW_GRAPH=1       # opt-in graph decision layer
RT_PREVIEW_DUMP=/path    # exact preview/work/crop TIFF dumps
RT_STAGE_STORE=1         # persistent preview base cache
RT_STAGE_STORE_MAX_MB=512
RT_TILE_ORDER=1          # opt-in tile ordering, when merged

# One export
tools/perf/export-one.sh --build-dir build/cpu-perf-stage-runtime \
    --image "$RT_TEST_IMAGE_45MP" --out out.tif --threads 8 --gpu off

# Stage tests only
cmake --build build/cpu-perf-stage-runtime \
    --target stage-abi-test stage-parity-test stage-cache-test \
             stage-graph-test stage-indexer-test -j 6

# Build KPI
tools/perf/kpi-build.sh --build-dir build/cpu-perf-stage-runtime

# Interleaved baseline/candidate timing
tools/perf/compare-timing.sh \
    --baseline-dir /path/to/baseline-build \
    --build-dir build/cpu-perf-stage-runtime \
    --image "$RT_TEST_IMAGE_45MP" --pp3 devnotes/opencl/pp3/resize.pp3 \
    --threads 8 --runs 3
```

## Tolerances

| Path | Requirement |
|---|---|
| Export, exact | `AE=0`, `max_abs=0` against baseline |
| Preview viewport, exact | `AE=0` against baseline |
| OpenCL | `max_pae <= 1.5259e-05` (1 LSB at 16-bit) |
| Thread matrix | `AE=0` at 1, 2, 4 and 8 threads |
| Stage built-in vs module | zero differing pixels |

`SKIP` is recorded as `SKIP`, never as `PASS`. A missing DCP profile, a
missing GPU or an unavailable Xvfb display is a skip, not evidence.

## Report

`run-suite.sh` writes `report.json` and leaves the exported TIFFs, logs and
timing table in the report directory. Record the commit, build directory,
image SHA-256, profile SHA-256, thread count, GPU mode and tolerance with
every published figure.

## Known constraints

- The 45.7 MP corpus is private. Never commit the raw files, sidecars,
  reference TIFFs or screenshots. Report image IDs and hashes only.
- Image A with `(cameraICC)` is the portable parity sample. B–H reference
  DCP profiles that are not in the repository; use them for local profile
  regression only.
- `bench-preview.sh` uses a fixed 1600×1000 Xvfb layout and dismisses the
  first-run notice. Set `RT_PREVIEW_DISPLAY` to a free display.
- `test-preview-parity.sh` compares screenshots under isolated settings. It
  needs a working Xvfb display and the first-run notice dismissed.
