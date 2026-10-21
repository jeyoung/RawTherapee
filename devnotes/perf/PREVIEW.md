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

Automated harness, cold Xvfb start: warm fit-zoom p50 = 54 ms; p95 = 152 ms.

## Interactive host baseline

45.7 MP NEF, real interactive session on the host, Exposure and
Shadows/Highlights edits:

| Sequence | Samples | p50 | p95 | Max |
|---|---:|---:|---:|---:|
| Cold initial render | 1 | 7392 ms | — | 7392 ms |
| Fit zoom, Exposure (`0x1009f`) | 17 | 252 ms | 274 ms | 274 ms |
| High-detail transition (`0x40000`) | 1 | 9001 ms | — | 9001 ms |
| 100% zoom, Shadows/Highlights (`0x7`) | 11 | 243 ms | 255 ms | 255 ms |
| 100% zoom, Exposure (`0x1009f`) | 24 | 335 ms | 373 ms | 3196 ms |
| Monitor-only update (`0x1000`) | 2 | 86.5 ms | 89 ms | 89 ms |

The 100% Exposure sequence contains one 3196 ms outlier; excluding it would
lower the p95, but the raw sequence is reported. The current interactive
target p95 <100 ms at fit and <250 ms at 100% is **not met**. The next preview
change should be progressive/lower-resolution rendering rather than another
cache.

The timer starts after stale-request rejection; cold renders include initial
image loading and are reported separately.

## Loading placeholder

Single-editor mode replaces the previous image immediately when a new image is
selected. The placeholder is the selected image's filmstrip preview, which is
derived from the embedded JPEG. An empty placeholder is shown first, then the
preview pixbuf is installed when available, so a slow thumbnail lookup cannot
leave the old image visible.

Editing is disabled while the placeholder is active: tool panels, crop and
image-area input, history, save/export/queue and editing shortcuts are
blocked. The preview-only toggles remain usable: clipped highlights/shadows,
focus mask, RGB/L channel view, background colour and sharp mask. Sharp mask,
soft proof and gamut check are deferred until the new processor exists.

The placeholder clears on the first real preview, on initial-image load
failure, or when processing finishes without producing a preview. Load
failures clear the placeholder without applying deferred preview state to the
old processor.

Manual Xvfb test with a 10 MP PEF followed by a 45.7 MP NEF:

- placeholder visible about 100 ms after the next-image command
- first full preview after about 10 s; the placeholder remained until then
- rotate shortcut was ignored while loading
- the clipped-highlight toggle was accepted during loading and applied to the
  new preview
- editing controls were insensitive during loading and re-enabled afterwards
