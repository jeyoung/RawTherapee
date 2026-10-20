# Preview latency baseline

Preview timings are emitted when `RT_PREVIEW_PROFILE=1` is set. Each completed
render prints:

```text
preview update: <wall-ms> ms todo=0x<mask>
```

The timer starts after a stale queued request has been rejected, so dropped
requests are not included. Queued requests are coalesced by the preview
generation counter.

## Procedure

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

Warm fit-zoom p50 = 54 ms; p95 = 152 ms. The 100% sequence produced the same
timings and `todo=0x1009f`; the exposure path did not trigger a distinct
high-detail render in this harness, so this is **not yet a validated 100%
baseline**. A Tone Curve or crop edit at 100% should be used for that row.

The timer starts after stale-request rejection; cold renders include initial
image loading and are reported separately.
