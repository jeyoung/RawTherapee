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

## Exact tiled-denoise parity

`tools/perf/test-tiled-denoise-parity.py` compares two installed graph CLIs
using synthetic inputs only. It requires Python 3, NumPy and ImageMagick 7's
`magick` command; comparison uses the existing `tools/opencl/compare_tiff.py`.
Install each CLI with its matching runtime data using `tools/build-rt.sh cpu
--install --prefix /path/to/prefix` before running:

```bash
python3 tools/perf/test-tiled-denoise-parity.py \
    --base-cli /path/to/base/bin/rawtherapee-graph-cli \
    --candidate-cli /path/to/candidate/bin/rawtherapee-graph-cli \
    --scratch "$PWD/build/test-scratch/denoise-parity"
```

The scratch directory must be new or empty. It contains all generated TIFFs,
profiles, isolated settings/cache/temp directories, CLI/comparator logs and
`report.json`; it is preserved on failure. Nothing is downloaded. Use a new
scratch path for each run. `--timeout` bounds each subprocess (default 180
seconds). Record both source commits alongside the report; binary SHA-256
hashes identify the executables, not their installed runtime data.

The fixture writer uses only the standard library. It writes little-endian,
uncompressed, unsigned RGB16 TIFFs: 4096x512 (one tile row), 512x2304
(multiple rows in one column), and 1024x1793 (odd final band). A fixed-seed
32-bit linear congruential generator adds noise to a bounded RGB gradient;
regeneration is deterministic and hashes are recorded. The embedded active
profile applies Lab denoise with luma/chroma strength 30, luma detail 25,
manual chroma and gamma 1.7; its paired control disables denoise. Profiles
and settings are generated in scratch and hashed in the report.

Every export uses `--gpu=off -t -b16 -Y` and one OpenMP thread. Inherited
`RT_*` and `OMP_*` overrides are removed; the harness pins
`OMP_NUM_THREADS=1`, `OMP_THREAD_LIMIT=1`, `OMP_MAX_ACTIVE_LEVELS=1` and
`OMP_DYNAMIC=FALSE`. It varies `RgbDenoiseThreadLimit` between 1 and 4 and
`RT_TILE_ORDER` between 0 and 1. Limit 4 is a tiling configuration, not a
four-thread run. Active exports must report one main and one nested denoise
thread and must differ from their disabled control. The candidate must also report a multi-tile merge layout matching each
fixture's row/column class, with positive buffer count and tile dimensions;
no particular buffer count is required. An older baseline may predate this
diagnostic. Its layout is validated when reported, but absent baseline
markers leave its tiled execution unproven; exact comparisons and active
controls still apply to both binaries.

A successful run has 30 exports (two binaries, three fixtures, four active
configurations plus one disabled control) and 12 base/candidate comparisons.
All outputs must retain the expected RGB16 geometry. Exact parity requires
`max_abs=0`, normalized maximum zero and zero differing samples. The shared
comparator permits a one-LSB difference at `--bits 16`, so its successful
exit alone is insufficient: this harness checks the metrics independently
and exits nonzero for any differing sample, missing output, CLI/comparator
error, allocation failure or inactive denoise control.

Run the focused regression checks with the same NumPy/magick dependencies:

```bash
python3 -m unittest discover -s tools/perf/tests \
    -p test_tiled_denoise_parity.py -v
```

They include a real TIFF with one sample changed by one LSB: the comparator
exits zero, but the harness parity gate must fail. They also cover malformed
metrics, conflicting environment overrides, deterministic fixtures, paths
with spaces, failed CLIs and preserving nonempty scratch. Dependency-bound
checks explicitly skip if NumPy or magick is absent; skips are not evidence
of a passing integration check.

Using the same current-tip binary for both arguments validates the harness
and its controls. It does not establish parity between different engine
revisions. This gate does not measure performance, RSS or allocation savings,
cover GPU parity or GUI previews, exercise multiple OpenMP threads, or
replace the raw-image corpus and other stage tests.

### Initial current-tip validation limitation

The initial single-thread, same-binary validation at engine commit
`7781f6800` completed all 30 exports but failed exact parity at
`one-column-limit4-order0`: 182 of 3,538,944 samples differed by one LSB.
Both runs reported one main/nested thread and a 3x1 tile layout. The other
seven one-column exports had identical rasters. This is a failed gate, not
an accepted tolerance. The engine's measured FFTW planning is a possible
source of run-to-run numerical variation; the cause has not been proven.
Resolving it requires separate engine investigation rather than changing
this tools-only contribution's exact comparison. Preserve failing reports
and do not retry until green or relax the tolerance to claim acceptance.
