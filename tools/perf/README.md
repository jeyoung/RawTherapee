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
tools/perf/bench-preview.sh /usr/local/bin/rawtherapee \
    /path/to/image.NEF 5
```

The script starts a private Xvfb display, opens the image, expands the Exposure
tool, and drags Exposure Compensation several times. It prints the p50 and p95
of the `preview update` timings emitted by `RT_PREVIEW_PROFILE`.

Requirements: Xvfb, xdotool, and a GTK-capable RawTherapee binary. The image and
its sidecar remain local and are not copied into the repository.
