# Preview base-image cache

The preview base-image cache is enabled by default. Set
`RT_PREVIEW_BASE_CACHE=0` (or `false`, `off`, `no`) to disable it for one
process. The snapshot is taken in
`ImProcCoordinator::updatePreviewImage()` immediately after
`ImageSource::getImage()` and before spot removal, colour-space conversion,
film negative processing, gamut compression, `firstAnalysis()` and the rest of
the pipeline. A hit restores a deep copy into `orig_prev` and then runs the
normal pipeline, so every downstream side effect and listener still runs.

The cache holds one snapshot owned by the coordinator. It never retains a
pointer into `orig_prev` or `oprevi`, and it refuses entries larger than
`256 MiB` (computed with `Imagefloat`'s 128-bit row padding, not just
`pW * pH * 3 * sizeof(float)`). The old snapshot is released before a
replacement is allocated, so a miss does not transiently hold two snapshots.

The implementation is in `rtengine/previewbasecache.h`. Each cache operation
is internally locked and validates its destination dimensions before copying.
The coordinator's `mProcessing` lock remains the contract for access to the
shared `ImageSource`; the cache lock does not replace it.

## Eligibility

The cache is bypassed for the whole request when any of these is active:

- spot removal;
- film negative;
- dehaze;
- Fattal tone mapping;
- Retinex, which modifies the shared raw planes before `getImage()`;
- an entry larger than the memory bound.

Otherwise the key must match: image source, a monotonic source generation,
frame, sensor type, current white balance (temperature, green, equal,
observer), high-detail mode and computed flags, raw demosaic
method/ccSteps/exposure, the highlight-reconstruction subset (`hrenabled`,
method, `hlbl`, `hlth`, `clampOOG`), `isRGBSourceModified()`, coarse transform,
preview scale, full dimensions and preview dimensions.

The source generation is bumped by `ImageSource` mutations, including
`preprocess()`, `demosaic()`, `retinex()`, `captureSharpening()`, colour
highlight recovery inside `getImage()`, `setBorder()` when the border changes,
`setCurrentFrame()` when the frame changes, and `flush()`/`load()`. It covers
source changes made by detail crops and by `saveInputICCReference()` that are
not represented by the preview parameter key. The store key is captured
immediately after `getImage()` so a mutation made by that call is included in
the stored key.

## Invalidation

The snapshot is dropped or marked dirty for:

- image switch (`assign()`), `freeAll()`, destruction;
- `setScale()` when the scale or preview dimensions change;
- a `raw.preprocess` block fingerprint change (`preprocess`);
- a `raw.demosaic` block fingerprint change (`raw`);
- a request carrying the typed white-balance fact (`white-balance`);
- the high-detail request fact (`high-detail-request`);
- a `highDetailNeeded` transition;
- a `preprocess()`, `demosaic()`, `retinex()` or `captureSharpening()` call;
- colour highlight recovery modifying the shared raw planes;
- `setBorder()` and `setCurrentFrame()` when they actually change source state;
- `saveInputICCReference()`, which reprocesses and demosaics the shared
  `ImageSource` at full resolution;
- `flush()` and `load()`.

The request reasons come from `stage::baseCacheInvalidationFor()`, which maps a
narrow typed input set (`raw.preprocess`, `raw.demosaic`, `raw.base` for the
white-balance fact) and the high-detail flag to the same strings the old mask
mapper published. The typed set is not the pass execution set: a spot,
film-negative, Retinex or tone-curve change does not invalidate the snapshot,
matching the old mask behaviour. `stage-render-state-test` pins the decision
table with hand-written block sets and reason strings: 21 of the 22 intents
(all but `NoRequest`) and three representative pairs
(`Demosaic+WhiteBalance`, `DarkFrame+HighQuality`,
`WhiteBalance+HighQuality`). It does not replay requests against the retired
mask oracle.

Crop-only and output/monitor-only changes do not invalidate. A transform-only
change is not an invalidation either because it normally changes only the
post-base transform; the coarse transform (`tr`) is part of the key instead.

## Profiling

With `RT_LOAD_PROFILE=1` or `RT_PREVIEW_PROFILE=1` the cache prints hit, miss,
bypass, store and invalidation lines with reasons. The existing
`preview update:` line also reports `cache=1` on a hit. A summary with counters
and reason maps is printed when the coordinator is destroyed.

## Regression test

The cache has a standalone unit test for key equality, generation mismatch,
store/restore, destination-size rejection, dirty/clear handling and the
row-padded memory bound:

```bash
tools/perf/test-preview-base-cache.sh build/cpu
```

The CLI does **not** instantiate `ImProcCoordinator`, so `rawtherapee-cli`
export parity is not evidence that the preview cache is correct. Preview
correctness must be checked through the GUI. The existing CLI builds and
`tools/opencl/verify.sh` remain useful as general build/regression gates.

For GUI checks:

- maximise the RawTherapee window so the whole right-hand tool panel is
  visible;
- select the hand tool and click the image before using keyboard shortcuts;
- use `z` for 100% or double-click the image as an independent check;
- use a fixed Xvfb/Xephyr screen size and crop the same image region when
  comparing cache-on and cache-off screenshots.

The exposed startup notice in a fresh settings directory can capture the
pointer and keyboard; close it first. Keyboard shortcuts work under Xephyr once
the image has focus; they are not inherently disabled by the virtual display.

## Initial evidence

GUI under Xvfb, CPU build, Contrast by Detail Levels enabled and changed:

| Image | Hit p50 | No-cache p50 | Samples |
|---|---:|---:|---:|
| 10 MP PEF (`OMP_NUM_THREADS=1`) | 247 ms | 269 ms | 7 |
| 45.7 MP NEF | 138 ms | 165 ms | 7 |

The full GUI screenshots for the PEF sequence were pixel-identical with the
cache off and on. The saving is modest because `getImage()` at preview scale is
only a small part of the pipeline. The cache is enabled by default after the
generation/locking hardening; detail crops and export remain outside its
scope. Use the environment opt-out if a workload regresses.

Post-hardening CPU checks (45.7 MP NEF, maximised Xephyr window, cache on and
off): an Exposure edit produced a cache hit and cropped preview screenshots
were pixel-identical (ImageMagick difference bbox empty). Selecting the hand
tool and clicking the image allowed the `z` shortcut to reach the same 100%
state as double-clicking the image.
