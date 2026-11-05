# Performance tools

## Preview base-cache regression

```bash
tools/perf/test-preview-base-cache.sh build/cpu
```

This builds the optional `preview-base-cache-test` target and checks key
equality, source-generation invalidation, store/restore, destination rejection
and the row-padded memory bound.

## Export determinism

```bash
RT_BUILD_DIR=build/cpu-perf-graph \
    tools/perf/test-export-determinism.sh /path/to/image.NEF [profile.pp3]
```

Exports 16-bit TIFF at 1, 4 and 8 threads and once more at 8 threads, then
compares each against the single-threaded run. The gate is the project's
1 LSB16 tolerance; the differing-pixel count is printed so a rare last-bit
residual is visible rather than hidden. Settings and cache are isolated per
run.

## Preview baseline

```bash
RT_BUILD_DIR=build/cpu-perf-graph \
    tools/perf/bench-preview.sh /path/to/image.NEF 5
```

The script builds and runs `preview-work-order-sweep` with
`OMP_NUM_THREADS=1`, applies the exposure edit sequence, and prints the sample
count with the p50 and p95 of the settled edit latencies. The TIFFs and the
full `TIMING` log stay under `build/test-scratch/bench-preview`. No Xvfb,
xdotool or GUI binary is needed, so the number no longer depends on a window
layout. The image and its sidecar remain local and are not copied into the
repository.
