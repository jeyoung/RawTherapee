# Preview base-image cache experiment

`RT_PREVIEW_BASE_CACHE=1` enables an experimental, default-off cache of the
display preview's base image. The snapshot is taken in
`ImProcCoordinator::updatePreviewImage()` immediately after
`ImageSource::getImage()` and before spot removal, colour-space conversion,
film negative processing, gamut compression, `firstAnalysis()` and the rest of
the pipeline. A hit restores a deep copy into `orig_prev` and then runs the
normal pipeline, so every downstream side effect and listener still runs.

The cache holds one snapshot owned by the coordinator. It never retains a
pointer into `orig_prev` or `oprevi`, and it refuses entries larger than
`256 MiB` (nominal `pW * pH * 3 * sizeof(float)`).

## Eligibility

The cache is bypassed for the whole request when any of these is active:

- spot removal;
- film negative;
- dehaze;
- Fattal tone mapping;
- Retinex, which modifies the shared raw planes before `getImage()`;
- an entry larger than the memory bound.

Otherwise the key must match: image source and frame, current white balance
(temperature, green, equal, observer), high-detail mode and computed flags, raw
demosaic method/ccSteps/exposure, the highlight-reconstruction subset
(`hrenabled`, method, `hlbl`, `hlth`, `clampOOG`), `isRGBSourceModified()`,
coarse transform, preview scale, full dimensions and preview dimensions.

## Invalidation

The snapshot is dropped or marked dirty for:

- image switch (`assign()`), `freeAll()`, destruction;
- `setScale()` when the scale or preview dimensions change;
- `M_PREPROC`, `M_RAW`, `M_WB` and `M_HIGHQUAL` request bits;
- a `highDetailNeeded` transition;
- every `preprocess()`, `demosaic()` and `captureSharpening()` call;
- `saveInputICCReference()`, which reprocesses and demosaics the shared
  `ImageSource` at full resolution.

Crop-only (`M_CROP`) and output/monitor-only (`M_MONITOR`) changes do not
invalidate. `M_TRANSFORM` is not used as an invalidation bit by itself because
it is part of `ALLNORAW` and normally changes only the post-base transform; the
coarse transform (`tr`) is part of the key instead.

## Profiling

With `RT_LOAD_PROFILE=1` or `RT_PREVIEW_PROFILE=1` the cache prints hit, miss,
bypass, store and invalidation lines with reasons. The existing
`preview update:` line also reports `cache=1` on a hit. A summary with counters
and reason maps is printed when the coordinator is destroyed.

## Initial evidence

Built the CPU and OpenCL variants (OpenCL CLI tested with `--gpu=off`). 16-bit
TIFF CLI output with the cache off and on was pixel-identical for the 10 MP PEF
and the 45.7 MP NEF (`tiffcmp` exit 0; ImageMagick `AE` 0 for both PEF runs).

GUI under Xvfb, CPU build, Contrast by Detail Levels enabled and changed:

| Image | Hit p50 | No-cache p50 | Samples |
|---|---:|---:|---:|
| 10 MP PEF (`OMP_NUM_THREADS=1`) | 247 ms | 269 ms | 7 |
| 45.7 MP NEF | 138 ms | 165 ms | 7 |

The full GUI screenshots for the PEF sequence were pixel-identical with the
cache off and on. The saving is modest because `getImage()` at preview scale is
only a small part of the pipeline; this experiment validates the boundary and
invalidation policy before considering a larger cache.
