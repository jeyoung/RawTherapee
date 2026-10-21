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
- `decoder raw2image` — LibRaw `raw2image`, i.e. the 4-channel expansion.
- `decoder float conversion` — RawTherapee's float/float3 copy for float DNGs.
- `decoder post-unpack` — camera matrices, embedded profile and black-stat
  fixups after image construction.
- `decoder finish` — baseline exposure and progress reporting.
- `decoder dcraw` — dcraw allocation plus `load_raw`, coarse fallback timing.
- `raw unpack` — total decoder bucket, wrapping all of the above.
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

## Baseline

Representative run on an 8-thread Intel i7-3770, CPU build, no OpenCL stages
for these phases. Cold means the input file was evicted from the page cache and
the RawTherapee cache was emptied; the process and shared libraries were warm.
Warm means an immediately repeated run.

`rawtherapee-cli`, default profile (AMaZE demosaic), warm values are medians of
three runs:

| Phase | 10 MP cold | 10 MP warm | 45.7 MP cold | 45.7 MP warm |
|---|---:|---:|---:|---:|
| `initial_image.load` | 636.5 ms | 637.7 ms | 2900.6 ms | 2867.4 ms |
| `raw metadata` (both passes) | 4.6 ms | 1.7 ms | 7.6 ms | 1.6 ms |
| `file open` [mmap] | 0.0 ms | 0.0 ms | 0.0 ms | 0.0 ms |
| `decoder open_buffer` | 0.3 ms | 0.3 ms | 0.3 ms | 0.3 ms |
| `decoder unpack` | 471.1 ms | 477.3 ms | 2187.3 ms | 2161.5 ms |
| `decoder raw2image` | 128.3 ms | 128.5 ms | 580.5 ms | 580.2 ms |
| `decoder setup`/`post-unpack`/`finish` | 0.0 ms | 0.0 ms | 0.0 ms | 0.0 ms |
| `raw unpack` (total) | 599.8 ms | 606.2 ms | 2768.2 ms | 2740.8 ms |
| `compress_image` | 13.8 ms | 12.4 ms | 51.1 ms | 52.9 ms |
| `preprocess` | 31.0 ms | 28.9 ms | 131.0 ms | 123.9 ms |
| `demosaic` | 161.5 ms | 160.8 ms | 682.6 ms | 711.2 ms |
| `getImage` | 43.2 ms | 42.6 ms | 177.1 ms | 170.8 ms |

GUI under Xvfb, fit zoom (FAST demosaic), warm values are medians of two runs:

| Phase | 10 MP cold | 10 MP warm | 45.7 MP cold | 45.7 MP warm |
|---|---:|---:|---:|---:|
| `initial_image.load` | 633.0 ms | 652.8 ms | 2863.9 ms | 2912.4 ms |
| `decoder unpack` | 476.3 ms | 492.1 ms | 2166.1 ms | 2203.0 ms |
| `decoder raw2image` | 126.8 ms | 129.1 ms | 573.1 ms | 581.7 ms |
| `raw unpack` (total) | 603.4 ms | 621.5 ms | 2739.5 ms | 2785.1 ms |
| `compress_image` | 11.4 ms | 12.1 ms | 51.4 ms | 53.0 ms |
| `preprocess` | 38.5 ms | 37.6 ms | 157.4 ms | 168.1 ms |
| `demosaic` [fast] | 41.2 ms | 38.2 ms | 163.4 ms | 167.4 ms |
| `getImage` | 13.7 ms | 14.0 ms | 42.5 ms | 43.4 ms |
| `preview pipeline` | 161.6 ms | 153.8 ms | 518.8 ms | 526.1 ms |
| first `imageReady` (`t+`) | 939.0 ms | 953.7 ms | 3529.8 ms | 3588.9 ms |

Within the decoder bucket, `unpack` accounts for about 79% and `raw2image` for
about 20%. The remaining phases are effectively zero. Metadata parsing and
`open_buffer` are negligible; enabling LibRaw's `raw2image` step and then
copying the result again in `compress_image` is the largest removable cost.
File reading is hidden in the unpack page faults: cold `unpack` is only about
0–30 ms slower than warm `unpack`.

For comparison, setting `EnableLibRaw=false` in the settings file selects the
dcraw fallback:

| Image | LibRaw unpack | LibRaw `raw2image` | dcraw `load_raw` |
|---|---:|---:|---:|
| 10 MP PEF | 471 ms | 128 ms | 176 ms |
| 45.7 MP NEF | 2187 ms | 581 ms | 609 ms |

The resulting JPEGs were pixel-identical (`compare -metric AE` returned 0 for
both images). This is a decoder-path difference, not a profiling artefact:
LibRaw's NEF decoder is about 4.5 times slower than dcraw on the 45.7 MP file.
