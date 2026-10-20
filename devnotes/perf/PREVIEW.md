# Preview latency baseline

Preview timings are emitted when `RT_PREVIEW_PROFILE=1` is set. Each completed
render prints:

```text
preview update: <wall-ms> ms todo=0x<mask>
```

The timer starts after a stale queued request has been rejected, so dropped
requests are not included. Queued requests are coalesced by the preview
generation counter.

## Harness

```bash
tools/perf/bench-preview.sh /usr/local/bin/rawtherapee \
    /path/to/image.NEF 5
```

It starts a private Xvfb display, opens the Exposure tool, drags Exposure
Compensation, and prints warm p50/p95. See `tools/perf/README.md`.

## Manual procedure

1. Build the normal GUI with GCC and OpenCL enabled.
2. Start RawTherapee with `RT_PREVIEW_PROFILE=1` and open a 45 MP NEF.
3. Record the first fit-zoom render, then five consecutive edits of each kind:
   - a Tone Curve point;
   - Exposure;
   - a Lab luminance-curve point;
   - the output profile.
4. Repeat at 100% zoom.
5. Capture stderr and report p50/p95 and the `todo` masks.

Use the same image and profile for every run. Do not commit images or sidecars.

## Current status

45.7 MP NEF under Xvfb, Exposure Compensation slider, no OpenCL device:

| Run | Fit zoom | 100% zoom |
|---|---:|---:|
| Cold first render | 5323 ms | 5324 ms |
| Warm 1 | 152 ms | 152 ms |
| Warm 2 | 141 ms | 141 ms |
| Warm 3 | 53 ms | 51 ms |
| Warm 4 | 53 ms | 53 ms |
| Warm 5 | 54 ms | 54 ms |

Warm fit-zoom p50 = 54 ms; p95 = 152 ms.

A 100% zoom run with the Shadows/Highlights tool, which exercises Lab-stage
work, produced 63–68 ms per edit (`todo=0x7`). No distinct high-detail render
appeared in the timer output; the high-detail path is already computed/cached
for this image at the time of the edits. Treat the 100% row as an interactive
Lab-edit measurement, not a cold high-detail render.

The timer starts after stale-request rejection; cold renders include initial
image loading and are reported separately.
