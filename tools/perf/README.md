# Performance tools

## Preview base-cache regression

```bash
tools/perf/test-preview-base-cache.sh build/cpu
```

This builds the optional `preview-base-cache-test` target and checks key
equality, source-generation invalidation, store/restore, destination rejection
and the row-padded memory bound.

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
