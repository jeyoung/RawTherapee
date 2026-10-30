# F5 denoise contribution rows — local handoff

**State:** reviewed; prepared for a pull request against `perf-graph`.
**Branch:** `perf-graph-f5-denoise-rows`
**Base:** `97d109b256fda5a7affc1c15a733fdc8ba055c71`
**Code/test commit:** `2a9de83136b11ba3fc95e3bf039e912c5ca201b8`

## Claim

Single-row tiled denoise reserves only its existing contribution row. The
production allocation uses the tested denoise-only sizing helper. No graph,
work-order, fingerprint, preview-lifecycle or pixel-arithmetic change is included.
The regression failed under the former rule and passes after the fix. CPU and
OpenCL-enabled builds and their allocation/tile-order tests pass. Twelve
before/after CPU image comparisons and six repeats are pixel-exact; disabled
controls prove denoise was active. Details are in `TILE_INDEX.md`.

## Not verified

No GPU execution, visual GUI check, portable package, general performance gain
or resident-memory reduction is established. The largest-count unit case is
an arithmetic check, not support for enormous engine grids. This branch excludes
the separate local F1/F2 fixes.

## Next

The contributor fork is `stephanelee/RawTherapee`; SSH authentication is
verified. The user authorized publication of this F5 branch and a pull request against
`jeyoung/RawTherapee:perf-graph`, refreshing its base and rerunning checks if it
moves. Original pixel baseline: `33814c82c`; publication base is recorded above.
The local patch handoff includes the validation runner and evidence.
