# Denoise export thread determinism

Status: the denoise-stage merge is now deterministic and committed
(`perf: #0: order denoise block merge`). The export as a whole is still not
bit-identical across thread counts because the raw-to-RGB path is
thread-order dependent (see *Blocker*).

## Symptom

45.7 MP NEF, directional-pyramid denoise enabled (Luma 30, Ldetail 25,
Chroma 30, manual chroma), 16-bit TIFF export:

| compare              | differing | max abs |
| -------------------- | --------- | ------- |
| t=1 vs t=4           | 13102     | 77      |
| t=1 vs t=8           | 13942     | 77      |
| t=8 vs t=8 (repeat)  | 14669     | 361     |

The last row is the important one: two runs at the *same* thread count differ
as well, so this is a race, not only an order-dependent reduction.

## Method

Profiles are derived from the image sidecar into a scratch directory and are
never committed; raw files and sidecars stay out of the tree.

```bash
tools/perf/export-one.sh --build-dir "$BUILD" --image "$RT_TEST_IMAGE_45MP" \
    --pp3 "$SCRATCH/denoise.pp3" --out "$SCRATCH/denoise-t$N.tif" \
    --gpu off --threads "$N"
python3 tools/opencl/compare_tiff.py "$REF" "$OUT" --bits 16
```

`export-one.sh` runs the CLI with its own empty `RT_SETTINGS`/`RT_CACHE`, so
every comparison starts from identical application defaults. Reproducing the
defect is only meaningful with those defaults: a profile that differs in any
other section (for example `[HLRecovery]`) changes the result independently of
threading.

For the stage-level tests the same export was first written to 16-bit TIFF and
then used as input, so that no raw decoding, demosaicing or highlight recovery
runs and only the denoise stage is exercised.

## Bisect

1. **Tile overlap merge is not involved in the CLI export.** `simpleprocess.cc`
   calls `RGB_denoise` with `kall = 2`, but `RGB_denoise` passes `kall = 0` to
   `Tile_calc` on the first attempt when `rgbDenoiseThreadLimit == 0` (its
   default), which yields a single tile: `dsttmp == dst` and the
   overlap-masked accumulation in `dsttmp` never runs. Instrumentation printed
   `numtiles=1 tilewidth=8280 tileheight=5512` for the test image.

2. **The DCT detail-recovery block loop is order dependent.** In
   `rtengine/FTblockDN.cc` the loop over block rows (`vblk`) is a
   `#pragma omp for`, while adjacent block rows overlap: `RGBoutput_tile_row`
   accumulates into `Ldetail` for rows `[top, top + TS)` with `TS = 64` and
   `offset = 25`, and the block fill loop accumulates `totwt` the same way.
   Both are unsynchronised read-modify-write reductions whose order follows the
   OpenMP partition (`totwt` also races outright).

3. **Stage-level check on deterministic input.** With a full-frame NEF export
   used as a TIFF input, the unpatched build gives 215 (`t=4`) and 414 (`t=8`)
   differing pixels at 1 LSB. The committed change reduces that to 6 (`t=4`)
   and 12 (`t=8`) pixels at 1 LSB on the full-frame 45.7 MP TIFF. The
   single-thread export is bit-identical before and after the change
   (`AE=0`), i.e. the change removes only thread-order dependence.

4. **The whole export is still not deterministic.** Hashing the image at
   `RGB_denoise` entry shows the input already differs between `t=1` and `t=8`
   and between repeated `t=8` runs. Hashing the demosaiced source planes
   immediately before `RawImageSource::HLRecovery_inpaint` gives the same
   result, so the divergence exists before any denoise code runs.

5. **Denoise amplifies it.** The same NEF with denoise disabled differs by 43
   pixels at 1 LSB between `t=1` and `t=8`; with denoise enabled the same
   input-level differences fan out to 13-14 k pixels because the wavelet and
   DCT stages threshold on them.

## Blocker

The raw-to-RGB preparation (`RawImageSource::getImage`: demosaic, preprocess,
highlight recovery) is thread-count dependent and is not stable even at a fixed
thread count. This is outside the file scope of this tranche, so the acceptance
criterion "t=1/t=4/t=8 `AE=0` for the raw export" cannot be met by changing
denoise or wavelet code.

Ruled out or noted during the bisect:

- `hilite_recon.cc:481` (`reduction(+:hipass_sum,hipass_norm)`, a `double`
  sum used as a threshold). Serialising it does not fix determinism.
- `FTblockDN.cc:2172` (`Noise_residualAB` reduction) feeds only the reported
  noise estimate (`nresi`, `highresi`), not exported pixels.
- `ipwavelet.cc` `Aver`/`Sigma` (`reduction(+:)` over `double`) and
  `WaveletcontAllL` (`reduction(+:avedbl)`) are order dependent but are not
  reached by the default wavelet tool settings; a wavelet-only export from a
  deterministic TIFF input is `AE=0` at `t=1`, `t=4` and `t=8`.

## Committed change

A patch touching only `rtengine/FTblockDN.cc`:

- block weights (`totwt`) are computed in a separate pass with one thread per
  destination row, keeping the serial order (block row ascending, then block
  column ascending);
- the block loop uses `#pragma omp for schedule(dynamic, 1) ordered`, and
  `RGBoutput_tile_row` runs in an `omp ordered` region, so overlapping block
  rows merge in ascending block-row order while their compute stays parallel.

Evidence: single-thread output unchanged (`AE=0`); full-frame TIFF input
reduced from 215/414 differing pixels to 6/12 at 1 LSB for t=4/t=8; NEF export
at 8 threads, three runs each, median 24.4 s with the change and 24.6 s
without (run-to-run spread 23-31 s on a loaded 4C/8T host), i.e. no measurable
regression. The residual 6/12 pixels are not attributed to the block merge and
remain under investigation.

## Uncovered

- Making the raw-to-RGB path thread-deterministic. Until that is done the NEF
  export cannot be `AE=0`, with or without denoise.
- The same check on other raw formats and on the POCL/OpenCL backend.
