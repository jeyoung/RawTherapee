# AMaZE tile scheduling parity

## Problem and scope

The Windows CPU/OpenCL benchmark recorded in PR #36 failed the 1 LSB16
tolerance at eight threads: the maximum difference was 60 LSB16. The retained
one-thread CPU output and eight-thread CPU output already differed by 60
LSB16 in 71 channel samples (37 above 1 LSB16). This showed that GPU execution
was not required to reproduce the defect. The failing production AMaZE
regression below localizes it to raw demosaicing. Fresh diagnostic exports
also reproduced the defect
with GPU gamut enabled; the original GPU repeat agreement was insufficient
evidence of determinism.

This change fixes AMaZE worker scratch reuse. It does not change device
selection, the gamut implementation, F8 device timing or the separate FFTW
denoise determinism work in #35. The tested source base is `da6738464`.

## Cause and change

Each AMaZE worker allocated one zeroed scratch buffer and reused it across
tiles. Several scratch arrays alias that storage between algorithm phases.
The per-tile Nyquist-mask reset did not initialize all locations subsequently
read by the next tile. A tile could therefore inherit scratch values from a
previous tile, making its result depend on the worker's tile history.

Keep one allocation per worker and the existing size/alignment/layout, but
zero the complete buffer at the start of every tile. `malloc` replaces the
initial `calloc` because the tile reset supplies the initialization. No
per-tile allocation, thread restriction or parity-tolerance change is added.
The phase calculations and their existing initialization remain unchanged.

Resetting only partial tiles is insufficient: a synthetic 512 by 512 case
still differs by one float after a full/partial/full tile history. The final
change resets every tile, including edge tiles that emit no output.

The production tile core is extracted into an internal function so the
regression can exercise the real calculation without a RAW decoder or a
downloaded image. `RawImageSource` retains progress setup, measurement and
its existing border interpolation. The public image-source API is unchanged.

At the default tile size 160, the allocation remains 1,448,767 bytes per
worker. Initialization now writes that buffer for every tile; this trades
additional memory traffic for deterministic scratch state. No speedup or
resident-memory reduction is claimed.

## Synthetic regression

`amaze-tile-parity-test` uses immutable synthetic CFA data and checks all
three output planes for finite, written samples and bitwise float equality
against a one-thread reference. It also checks input immutability and that
the requested OpenMP team is available.

- Four Bayer patterns, two fixed seeds and three image sizes: 440 by 440,
  440 by 568 and 512 by 512.
- One, two and eight threads; tile chunks one and three; two repeats.
- 24 fixtures, each with one reference and 12 comparisons: 312 core calls.
- No OpenMP is a skip (exit 77), not a passing regression.

With the core extraction and original scratch initialization, the regression
exits 1: the first fixture already differs by 59 float samples at two threads
with chunk one. The complete final regression passes on native Windows with
OpenCL enabled and disabled, and in both Linux builds. The CI host-test list
includes this target in both build configurations and rejects skips.

Build the normal applications with `tools/build-rt.sh`, then run the excluded
unit target from that same build tree:

```bash
cmake --build "$RT_BUILD_DIR" --target amaze-tile-parity-test -j 2
OMP_THREAD_LIMIT=8 "$RT_BUILD_DIR/rtengine/amaze-tile-parity-test"
```

## Native Windows export evidence

Hardware: Intel Core i7-10700 and NVIDIA GeForce RTX 3070. Toolchain: MSYS2
UCRT64 GCC 16.2.0, CMake 4.4.5, LCMS 2.19, Release generic target, with
floating-point contraction disabled. Runs use isolated settings/cache,
dynamic OpenMP teams disabled and either one or eight threads.

The native validation mirror requires three pre-existing local Windows
adaptations: C++17 aligned allocation, LCMS tone-curve segment compatibility
and loading `OpenCL.dll`. These are excluded from this contribution; a
clean unmodified Windows build is not claimed. Linux builds use the
canonical source without those adaptations.

Input is the public `colorspace_flowers.pef` in
`devnotes/opencl/testdata.manifest`, SHA-256
`ddefa07a8b48fd157b48354488b265be48a47c204d00d20d54ead8dfddd9f151`.
Exports use `devnotes/opencl/pp3/gamut.pp3`, AMaZE, uncompressed 16-bit TIFF
and the existing CLI flags `--gpu=off` or `--gpu=auto`. Rendered dimensions
are 3888 by 2608, or 30,419,712 RGB channel samples.

The final candidate produced one CPU reference at one thread followed by
three interleaved CPU/GPU pairs at eight threads:

| Comparison with fixed CPU reference | Repeats | Maximum LSB16 | Differing samples | Samples above 1 LSB16 |
| --- | ---: | ---: | ---: | ---: |
| Eight-thread CPU | 3 | 0 | 0 | 0 |
| Eight-thread GPU gamut | 3 | 1 | 826,275 | 0 |

All seven exports exit zero. The repository's `tools/opencl/compare_tiff.py`
with `--bits 16` independently passes all three CPU/GPU pairs, reporting
maximum 1 LSB16 and 826,275 differing samples in each pair.
Each GPU gamut record reports `impl=opencl`
and `source=opencl`; all three input hashes agree. This establishes actual
device execution for this image/profile, not parity for every device stage
or the wider RAW corpus. The F8 `ns=0` timing defect remains observable and
is outside this fix.

Both native configurations build the GUI, CLI, regression and
`stage-device-test` targets; the GUI/CLI runtime bundles are installed.
The regression and device-contract tests pass in
both configurations; CPU fallback in the disabled build is host-contract
coverage, not GPU parity. An eight-thread CPU-only-build export is pixel-exact
against the fixed one-thread reference from the OpenCL-enabled build.

Disabling gamut compression and final gain (`Wwgamut=none`, `Wgamgain=0`)
changes 30,409,099 samples with maximum difference 9,649 LSB16. The active
profile therefore exercises a non-vacuous gamut operation. No input, profile,
export or capture is included in this contribution.

## Serial output and bounded timing

A control CLI at the same `da6738464` base uses the same core extraction,
toolchain, profile and Windows prerequisites, with only the original scratch
initialization restored. Against this control, the fixed one-thread CPU
export changes 165 of 30,419,712 channel samples, maximum 60 LSB16. Serial
workers also inherited scratch from previous tiles, so the old one-thread
output is not a golden reference preserved by this change. The correctness
basis is consistent fresh-worker scratch state for each tile; fixed
one-thread/eight-thread equality is measured separately above.

With all compilers idle, the same two CLIs ran one warm-up pair followed by
three measured pairs at eight threads, alternating execution order. All runs
start a fresh process, use isolated settings/cache/temp paths, the public PEF
and active profile above, GPU disabled and `RT_STAGE_PROFILE` enabled.
OS/file caches are warm. Stage duration covers the `raw.demosaic` block, not
an isolated kernel; wall time includes startup, profiling, all processing and
TIFF writing. These numbers are not directly comparable with the unprofiled
whole-export benchmark in PR #36.

| Variant | Demosaic median (range), ms | Whole-export median (range), s |
| --- | ---: | ---: |
| Original scratch initialization | 120.245 (118.891-126.789) | 5.890 (5.731-6.135) |
| Fresh scratch per tile | 115.228 (107.993-117.943) | 6.012 (5.876-6.101) |

All measured samples are retained:

| Pair | Original demosaic, ms | Fixed demosaic, ms | Original export, s | Fixed export, s |
| --- | ---: | ---: | ---: | ---: |
| 1 | 120.245 | 115.228 | 6.134630 | 6.100717 |
| 2 | 126.789 | 117.943 | 5.890010 | 6.011645 |
| 3 | 118.891 | 107.993 | 5.731225 | 5.876145 |

The whole-export ranges overlap. Three pairs on one image/profile are a
bounded cost check, not evidence of a general speedup or a performance
guarantee. Initialization adds memory traffic even though this sample does
not show a slower demosaic stage.

## Other completed checks

Both Linux full helper builds and runtime-data installs pass, with OpenCL
disabled and enabled. All 16 bounded host tests pass in each configuration,
including the AMaZE regression; the separate CPU `stage-device-test` also
passes without skips. Host fallback is not GPU execution.
Workflow actionlint, shell/target-list verification and the existing CI
failure/skip/timeout fixtures pass with the expanded 16-test list.

The installed Linux CPU and OpenCL CLIs both pass `--list-formats`, exiting
zero with nonempty format output under a 30-second bound and isolated
settings/cache. This checks installed runtime startup, not portable-package
installation or the wider format corpus.

The installed native Windows GUI passes a bounded eight-thread smoke check:
load the public PEF, maximize, render to Ready at 100% and 50%, double-click
fit, render to Ready and close normally. This is app-smoke evidence, not
measured preview parity. No compiler runs during GUI or timing checks.

## Validation limits

The bounded host CI set is not the full stage suite. The existing
`stage-abi-test` target has a linkage gap, and native Windows
`stage-residency-test` uses unavailable POSIX environment APIs. Neither is
counted as passing. CLI parity does not establish GUI preview parity.
The larger NEF corpus, portable Debian package installation and additional
GPU models are not covered by the Windows export results above.
