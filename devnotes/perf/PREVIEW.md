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

Instrumentation only. The initial baseline has not yet been recorded; the next
step is to run this procedure on the RX 550 host and store aggregate results in
this file.
