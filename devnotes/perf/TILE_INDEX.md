# Deterministic tiled overlap merge and opt-in tile ordering

Status: the tiled denoise and wavelet overlap merges are deterministic. Tiled
exports are `AE=0` at one and eight threads, with `RT_TILE_ORDER=0` and `1`, and
match the previous serial reference.

## Problem

Both tools split the image into overlapping tiles and merge each tile with
`dsttmp += factor * value` at absolute coordinates under
`#pragma omp for schedule(dynamic)`. Two tiles share every overlap strip and
four share every tile corner, so the merge had two defects:

- the floating-point association of the four corner terms followed the tile
  visitation order;
- adjacent tiles performed unsynchronised read-modify-write updates of the same
  destination pixels.

Repeated runs at eight threads therefore differed by hundreds of samples, and
reordering the tiles changed 995 (denoise) and 511 (wavelet) samples by one
16-bit step.

## Design

Each tile now writes its weighted contribution into a private tile-sized
buffer at tile-relative coordinates. Every covered buffer pixel is written once
and only covered pixels are read back, so the buffers need no initialisation.
The buffers are merged into `dsttmp` in ascending tile-row and then ascending
tile-column order, which is the historical row-major association. Weights
(`Vmask[i1] * Hmask[j1]`), tile geometry, overlap and per-tile arithmetic are
unchanged; the merge only adds what the existing code already computed.

Merge unit:

- denoise: at most `TILE_MERGE_BAND_ROWS = 2` existing tile rows. One row (10 tiles at the test
  image's tiling) is narrower than eight threads and loses load balance; two
  rows (20 tiles) restore it while keeping the extra memory bounded. The extra
  buffers are 210 MB at 8280 x 5512.
- wavelet: one tile row. The tool already caps this tile size at two main
  threads, and a row holds four tiles, so a row already fills the thread pool.
  The extra buffers are 255 MB at the same image size.

Because the buffers are keyed by grid position, the compute order is irrelevant
to the result: the merge always commits in grid order. `RT_TILE_ORDER=1` can
therefore rank the tiles of a band by the `StageIndexer` cheap score without
touching pixels. Rows and bands stay in ascending order, so a band is the
largest unit that can be reordered.

`RT_TILE_ORDER` semantics are unchanged: unset, empty, `0` and `off` keep the
row-major order, `1` enables score ordering.

## Parity

Input: 45.7 MP (8280 x 5512) raw exported once to a fixed 16-bit TIFF. Denoise
forced tiled with `[Performance] RgbDenoiseThreadLimit=8`; wavelet with
`TilesMethod=big`. `compare_tiff.py --bits 16`, 136918080 samples:

| comparison                              | max abs | differing |
| --------------------------------------- | ------- | --------- |
| denoise t=1, order 0 vs 0 repeat        | 0       | 0         |
| denoise t=1, order 0 vs 1               | 0       | 0         |
| denoise t=8, order 0 vs 0 repeat        | 0       | 0         |
| denoise t=8, order 1 vs 1 repeat        | 0       | 0         |
| denoise t=8, order 0 vs 1               | 0       | 0         |
| denoise t=1 vs t=8                      | 0       | 0         |
| wavelet t=1, order 0 vs 0 repeat        | 0       | 0         |
| wavelet t=1, order 0 vs 1               | 0       | 0         |
| wavelet t=8, order 0 vs 0 repeat        | 0       | 0         |
| wavelet t=8, order 1 vs 1 repeat        | 0       | 0         |
| wavelet t=8, order 0 vs 1               | 0       | 0         |
| wavelet t=1 vs t=8                      | 0       | 0         |

The new default output is also `AE=0` against the pre-merge 16-bit reference
produced by the previous build at one thread, so the default path is unchanged.

## F5: reserve only existing contribution rows (2026-10-30)

Contribution capacity is `min(TILE_MERGE_BAND_ROWS, tile_rows) * tile_cols`.
The allocation and its regression share the denoise-only
`denoiseTileBufferCount()` helper. Band traversal, tile arithmetic, deterministic
merge order and cleanup are unchanged. Verbose output reports the actual grid,
buffer count and tile dimensions when the tiled allocation runs.

The regression fails with the former two-row rule for a one-row panorama and
a single tile, and passes with the fix. It also covers a one-column grid, two
rows, an odd final band, invalid dimensions and multiplication without a signed
`int` intermediate. The large-count case tests the helper, not giant-image support.

Against upstream `33814c82c`, three deterministic noisy 16-bit RGB TIFF fixtures
(4096x512, 512x2304 and 1024x1793) were denoised at one/four threads and with
`RT_TILE_ORDER=0/1`. A positive `RgbDenoiseThreadLimit` forced tiling. All 12
before/after comparisons were pixel-exact, as were six same-build repeats. All
active outputs differed from their disabled-denoise controls. The fixed logs
confirmed grids 1x5, 3x1 and 3x2 and capacities 5, 2 and 4 respectively.

The panorama previously reserved ten contributions and now reserves five,
avoiding 29,122,560 bytes of unpadded RGB float payload. Whole-process peak RSS
did not show a clear reduction; unused allocations may remain physically
uncommitted. Allocation arithmetic is not an RSS or speed claim.

CPU and OpenCL-enabled full builds pass, as do `denoise-tile-buffers-test` and
`stage-indexer-test` in both builds. No GPU execution or release-package test
was performed. The reproducible fixture/profile runner accompanies the local
handoff; the recorded parity evidence is CPU denoise on TIFF input.

## Performance

Host: Intel Core i7-3770, 4 cores / 8 threads, 8 OpenMP threads, GPU off. The
host was shared with another build for the whole session (load average 8-16), so
the wall times vary by more than the effect being measured. CPU time
(`user + sys`) is the stable signal; wall medians from the least loaded window
are given as well. Means are medians over 5-11 runs.

| workload | default before | deterministic default | delta |
| -------- | -------------- | --------------------- | ----- |
| denoise CPU time, final set (5 runs) | 100.8 s | 101.2 s | +0.4 % |
| denoise CPU time, earlier set (11 runs) | 102.7 s | 103.9 s | +1.3 % |
| denoise wall time (6 runs, least loaded window) | 32.0 s | 32.9 s | +2.5 % |
| wavelet CPU time, final set (5 runs) | 51.6 s | 51.7 s | +0.1 % |
| wavelet CPU time, earlier set (5 runs) | 52.9 s | 53.3 s | +0.8 % |
| wavelet wall time (5 runs, least loaded window) | 14.13 s | 14.14 s | +0.1 % |

Peak RSS: denoise 1.99 GB -> 2.20 GB, wavelet 1.97 GB -> 2.24 GB, matching the
band buffers.

Writing each contribution once and skipping buffer initialisation removed about
1.5 GB of extra memory traffic from the denoise export and about 1.5 GB from the
wavelet export.

`RT_TILE_ORDER=1` against the deterministic default, same host:

| workload | delta |
| -------- | ----- |
| denoise CPU time (10 vs 11 runs) | within +-1 % (neutral) |
| wavelet wall time (5 runs each, least loaded window) | -1.6 % whole export, about 8 % of the wavelet stage |

The earlier global ordering measured about 15 % on the wavelet stage; limiting
ordering to one tile row gives up roughly half of that because the heaviest
tiles of later rows cannot start first. The trade keeps the extra memory at one
tile row. On the denoise band the gain is inside the noise, as expected: its
tiles are dominated by fixed-size block transforms.

## Evidence

- `stage-indexer-test`: PASS (real build tree); ordering helper unchanged, all
  origin/uniqueness/tie/flag checks still covered.
- `tools/build-rt.sh cpu` and `tools/build-rt.sh opencl`: both pass, no new
  warnings from the touched files.
- Tiling confirmed by the verbose thread report (8 main threads for denoise, 2
  for the big wavelet tile).

## Limitations

- `FFTW_MEASURE` plan selection is timing-based. During peak host load (load
  average 16) one early denoise t=1 run selected a different plan and differed
  by 1763 samples at 1 LSB; it also differed from the pre-change reference in
  the same way, and every later run, including all 8-thread runs, is `AE=0`.
  This is pre-existing and independent of the merge or the tile order.
- Preview timing was not measured; the merge is shared by preview and export,
  and the preview denoise path is untiled (`kall=0`).
- OpenCL parity was not re-run; the change is CPU scheduling and float
  accumulation order only, no kernel is touched.
