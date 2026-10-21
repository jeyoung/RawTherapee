# Initial image load profiling

Set `RT_LOAD_PROFILE=1` to print wall-clock phases for the initial image load
and the first preview. The switch is off by default; when it is unset every
instrumentation point is a cached environment check.

```bash
RT_LOAD_PROFILE=1 rawtherapee-cli -o /tmp/out.jpg -c image.NEF
RT_LOAD_PROFILE=1 rawtherapee image.NEF
```

Each line has the form:

```text
load-profile: <phase> [<detail>] <duration> ms (t+<elapsed> ms)
```

The `t+` column is the time since the start of the most recent
`InitialImage::load`, so preview phases also show the total latency from load
start. Instantaneous events, such as the first image hand-off, print only
`t+`.

## Phases

- `initial_image.load` — total `InitialImage::load`.
- `raw metadata` — metadata parse in `RawImage::loadRaw`; `[probe]` is the
  metadata-only pass and `[data load]` is the second parse before unpacking.
- `raw unpack` — decoder unpack and pixel copy into the raw image buffers,
  labelled with the decoder.
- `compress_image` — conversion to the compressed per-frame layout.
- `preprocess` — first `RawImageSource::preprocess`.
- `demosaic` — first demosaic after the load, labelled with the method; the
  editor uses FAST at fit zoom.
- `getImage` — first `RawImageSource::getImage`, i.e. the preview base image.
- `preview pipeline` — first `ImProcCoordinator::updatePreviewImage`.
- `preview setImage` / `preview imageReady` — first image handed to the
  preview listener.

Only the first preprocess, demosaic, getImage, pipeline and hand-off after a
load are printed. `RT_PREVIEW_PROFILE` remains the per-request preview timer.

## Baseline

Representative run on an 8-thread Intel i7-3770, CPU build, no OpenCL stages
for these phases. Cold means the input file was evicted from the page cache and
the RawTherapee cache was emptied; the process and shared libraries were warm.
Warm means an immediately repeated run.

`rawtherapee-cli`, default profile (AMaZE demosaic):

| Phase | 10 MP cold | 10 MP warm | 45.7 MP cold | 45.7 MP warm |
|---|---:|---:|---:|---:|
| `initial_image.load` | 669.0 ms | 635.1 ms | 2936.8 ms | 2950.0 ms |
| `raw metadata` (both passes) | 6.7 ms | 1.8 ms | 7.6 ms | 2.6 ms |
| `raw unpack` | 631.3 ms | 602.7 ms | 2800.6 ms | 2816.2 ms |
| `compress_image` | 12.4 ms | 12.2 ms | 51.3 ms | 56.2 ms |
| `preprocess` | 35.4 ms | 31.2 ms | 178.7 ms | 171.0 ms |
| `demosaic` | 160.0 ms | 165.9 ms | 837.4 ms | 756.5 ms |
| `getImage` | 43.3 ms | 41.5 ms | 190.7 ms | 169.9 ms |

GUI under Xvfb, fit zoom (FAST demosaic):

| Phase | 10 MP cold | 10 MP warm | 45.7 MP cold | 45.7 MP warm |
|---|---:|---:|---:|---:|
| `initial_image.load` | 638.3 ms | 629.4 ms | 2872.0 ms | 2946.6 ms |
| `raw unpack` | 609.0 ms | 597.3 ms | 2727.3 ms | 2820.4 ms |
| `compress_image` | 11.6 ms | 13.9 ms | 55.5 ms | 52.8 ms |
| `preprocess` | 39.4 ms | 36.8 ms | 197.8 ms | 194.8 ms |
| `demosaic` [fast] | 39.2 ms | 37.6 ms | 160.8 ms | 175.9 ms |
| `getImage` | 12.1 ms | 14.5 ms | 42.0 ms | 42.3 ms |
| `preview pipeline` | 151.7 ms | 155.7 ms | 566.2 ms | 571.5 ms |
| first `imageReady` (`t+`) | 941.4 ms | 929.4 ms | 3593.2 ms | 3664.1 ms |

The decoder unpack dominates the initial load in both cases. The metadata
probe is cheap, but it shows that the file is parsed twice before unpacking.
