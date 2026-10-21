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
- `file open` — `gfopen`, i.e. `mmap` or a full read depending on the build.
- `raw metadata` — metadata parse in `RawImage::loadRaw`; `[probe]` is the
  metadata-only pass and `[data load]` is the first parse in the data pass.
- `decoder setup` — common setup before the decoder branch.
- `decoder open_buffer` — the second LibRaw `open_buffer` in the data pass.
- `decoder unpack` — LibRaw `unpack`, i.e. decompression into `raw_image`.
- `decoder raw2image` — LibRaw `raw2image` fallback, i.e. the 4-channel
  expansion. The detail is the fallback reason.
- `decoder float conversion` — RawTherapee's float/float3 copy for float DNGs.
- `decoder post-unpack` — camera matrices, embedded profile and black-stat
  fixups after image construction.
- `decoder finish` — baseline exposure and progress reporting.
- `decoder dcraw` — dcraw allocation plus `load_raw`, coarse fallback timing.
- `raw unpack` — total decoder bucket, wrapping all of the above.
- `direct_plane` — direct copy from LibRaw's single-channel `raw_image` into
  the compressed frame layout, inside `compress_image`; this replaces
  `raw2image` on the guarded path.
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

`file open` times only the mapping call and is effectively zero with
`MYFILE_MMAP`; the actual file reads are lazy page faults inside
`open_buffer`/`unpack` and cannot be separated without touching the mapped
pages.

## Direct raw-plane path

`RawImageSource::load` enables `allowDirectRawPlane` only for single-frame
loads. `RawImage::loadRaw` then accepts the direct path only when the decoder
is LibRaw, a single-channel `rawdata.raw_image` exists, the image is Bayer or
X-Trans with matching `filters`/`xtrans` metadata, and there is no float image,
Foveon data, Fuji layout, pixel-shift/multi-frame data, shrink, rotation,
camera raw crop, special decoder, invalid pitch/bounds, or dimension mismatch.
`compress_image` copies from `raw_image` using LibRaw's `raw_pitch` and
`top_margin`/`left_margin`; `libraw->recycle()` still runs only after the copy.

Every rejected case calls `raw2image` as before. `RT_LOAD_PROFILE` prints the
reason on the `decoder raw2image` line, for example `[not requested]`,
`[float image]`, `[foveon]`, `[cfa metadata]`, `[raw crop]` or
`[invalid pitch]`.

## Baseline

Representative run on an 8-thread Intel i7-3770, CPU build, no OpenCL stages
for these phases. Cold means the input file was evicted from the page cache and
the RawTherapee cache was emptied; the process and shared libraries were warm.
Warm means an immediately repeated run.

`rawtherapee-cli`, default profile (AMaZE demosaic), after the direct path,
warm values are medians of three runs:

| Phase | 10 MP cold | 10 MP warm | 45.7 MP cold | 45.7 MP warm |
|---|---:|---:|---:|---:|
| `initial_image.load` | 514.8 ms | 508.8 ms | 2356.5 ms | 2367.4 ms |
| `decoder unpack` | 482.0 ms | 480.7 ms | 2237.6 ms | 2255.6 ms |
| `decoder raw2image` | — | — | — | — |
| `direct_plane` | 7.6 ms | 7.8 ms | 36.0 ms | 35.7 ms |
| `compress_image` | 8.0 ms | 8.5 ms | 36.8 ms | 36.4 ms |
| `preprocess` | 32.2 ms | 27.0 ms | 126.5 ms | 134.1 ms |
| `demosaic` | 158.2 ms | 161.9 ms | 683.2 ms | 693.5 ms |
| `getImage` | 47.5 ms | 44.6 ms | 167.4 ms | 172.6 ms |

GUI under Xvfb, fit zoom (FAST demosaic), after the direct path, warm values
are medians of two runs:

| Phase | 10 MP cold | 10 MP warm | 45.7 MP cold | 45.7 MP warm |
|---|---:|---:|---:|---:|
| `initial_image.load` | 503.5 ms | 536.4 ms | 2290.3 ms | 2362.4 ms |
| `decoder unpack` | 475.3 ms | 506.2 ms | 2176.8 ms | 2248.0 ms |
| `direct_plane` | 7.6 ms | 9.0 ms | 35.5 ms | 35.9 ms |
| `compress_image` | 7.7 ms | 9.3 ms | 36.5 ms | 36.6 ms |
| `preprocess` | 40.1 ms | 39.4 ms | 171.5 ms | 183.5 ms |
| `demosaic` [fast] | 39.0 ms | 39.5 ms | 178.2 ms | 187.5 ms |
| `getImage` | 11.0 ms | 13.3 ms | 45.0 ms | 49.0 ms |
| `preview pipeline` | 154.0 ms | 164.8 ms | 555.6 ms | 590.4 ms |
| first `imageReady` (`t+`) | 803.0 ms | 852.3 ms | 2999.6 ms | 3108.3 ms |

Before/after summary (cold load and first preview):

| Workload | Load before | Load after | First preview before | First preview after |
|---|---:|---:|---:|---:|
| CLI 10 MP PEF | 636.5 ms | 514.8 ms | — | — |
| CLI 45.7 MP NEF | 2900.6 ms | 2356.5 ms | — | — |
| GUI 10 MP PEF | 633.0 ms | 503.5 ms | 939.0 ms | 803.0 ms |
| GUI 45.7 MP NEF | 2863.9 ms | 2290.3 ms | 3529.8 ms | 2999.6 ms |

The direct path removes `raw2image` completely for the two test files. Its cost
moves into `direct_plane`, which is much cheaper than the previous
`raw2image` plus `compress_image` combination. LibRaw `unpack` is now the
remaining decoder cost: about 95% of the load for the 45.7 MP NEF.

Metadata parsing and `open_buffer` remain negligible. File reading is hidden
in the unpack page faults: cold `unpack` is only about 0–30 ms slower than warm.

For comparison, setting `EnableLibRaw=false` in the settings file selects the
dcraw fallback:

| Image | LibRaw unpack | LibRaw `raw2image` | dcraw `load_raw` |
|---|---:|---:|---:|
| 10 MP PEF | 471 ms | 128 ms | 176 ms |
| 45.7 MP NEF | 2187 ms | 581 ms | 609 ms |

The resulting JPEGs were pixel-identical (`compare -metric AE` returned 0 for
both images). This is a decoder-path difference, not a profiling artefact:
LibRaw's NEF decoder is about 4.5 times slower than dcraw on the 45.7 MP file.
