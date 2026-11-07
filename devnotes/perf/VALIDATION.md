# Graph validation matrix

Current validation record for the `perf-graph` package and test suite. It
contains only current evidence; earlier packages, commits and matrices are in
[`deprecated/VALIDATION-HISTORY.md`](deprecated/VALIDATION-HISTORY.md)
(deprecated; historical only).

## Artefact

```
rawtherapee-graph-5.13-801-gd6f5ef1f6_5.13+git13822.d6f5ef1f6-1_amd64.deb
sha256 9e1905b398185d0b200dee41dcf4cd83f2550557442bb5ac80d7a89205ee7c13
```

Built 2026-11-07 from `d6f5ef1f6` with `tools/build-rt.sh portable`. The host
reference is the same commit in `build/cpu-perf-graph`; the StagePlan,
coverage and typed-refusal changes preserve valid-path pixel output. The export reference
must use the packaged data directory: the install adds the `options` file and
the generated `camconst.json`, and bare `rtdata` moves the neutral export.

## Environment

| Item | Value |
|---|---|
| Host CPU | AMD Ryzen 7 5700X, 8 cores / 16 threads |
| Host toolchain | GCC, Release, generic target, ccache |
| Host build trees | `build/cpu-perf-graph`, `build/opencl-perf-graph` |
| Container images | `debian:12`, `debian:13` |
| Corpus | `$RT_TESTDATA/colorspace_flowers.pef` (10.2 MP, public), a 45.7 MP NEF, and a 16-bit TIFF and 8-bit JPEG generated from the PEF |
| Device | POCL CPU |

The private corpus stays outside the repository and is named here only as
`nef45`, `tif16` and `jpg8`. Timing was not measured in the containers, so
those runs are functional checks only.

## Determinism protocol

Every comparison uses `OMP_NUM_THREADS=1` and `-j1` for the export. Without
that serialisation, OpenMP scheduling alone changes 16-bit output; two
identical runs can differ by 40 LSB on one pixel, and a host/container pair by
754 LSB on 2,608 pixels. A parity run without `OMP_NUM_THREADS=1` measures
scheduling, not code.

## Package matrix

Both containers installed the package with `apt-get install ./pkg.deb`, which
resolves dependencies; `dpkg -i` was not used. Debian 13 resolves the `t64`
renames from the package dependencies through `Provides`, with no workaround.

| Distro | Install | `--list-formats` | Neutral export vs host | GUI smoke |
|---|---|---|---|---|
| Debian 12 | PASS (exit 0) | 36 formats | PASS, AE=0, 0 of 30,419,712 samples | PASS, Xvfb exit 124 |
| Debian 13 | PASS (exit 0) | 36 formats | PASS, AE=0, 0 of 30,419,712 samples | PASS, Xvfb exit 124 |

The neutral export is 16-bit TIFF, `OMP_NUM_THREADS=1`, `--gpu=off`, compared
with `tools/opencl/compare_tiff.py --bits 16`. The GUI smoke test runs for 20 s
under Xvfb; exit 124 is the timeout, with no stderr or critical line.

`--gpu-test` and `--gpu-test-monitor` were not run: the containers have no GPU.
The package does not ship the test binaries, so container POCL parity is not
available; the host POCL run covers the kernels and the container run covers
installation and export.

## Test suite

The full suite `arch2-suite2` ran at `d6f5ef1f6` on `build/cpu-perf-graph` and
reported `PASS`, `failures: 0`. The StagePlan, contract-coverage and typed-
refusal changes are covered by `stage-plan-test`, `stage-pipeline-test`,
`stage-device-test` and `stage-residency-test`;
their separate runs are recorded below.

### Stage and harness rows

| Row | Result |
|---|---|
| `stage-abi-test` | PASS |
| `stage-parity-test` | PASS |
| `stage-cache-test` | PASS |
| `stage-graph-test` | PASS |
| `stage-indexer-test` | PASS |
| `stage-store-test` | PASS |
| `stage-node-cache-test` | PASS |
| `stage-device-test` | PASS |
| `stage-residency-test` | PASS (host rows); 44 device rows SKIP on the CPU-only build, covered by the separate POCL run below |
| `stage-param-hash-test` | PASS |
| `stage-pipeline-test` | PASS |
| `stage-param-map-test` | PASS |
| `stage-render-state-test` | PASS |
| `stage-dirty-blocks-test` | PASS |
| `stage-work-test` | PASS |
| `stage-handler-test` | PASS |
| `editorrequest-test` | PASS |
| `viewport-admission-test` | PASS |
| `denoise-tile-buffers-test` | PASS |
| `amaze-tile-parity-test` | PASS (4 Bayer patterns, 3 sizes, 2 seeds, threads 1/2/8, chunks 1/3, 2 repeats) |
| `fast-preview-test` | PASS |
| `preview-base-cache-test` | PASS |
| `working-trc-setup-test` | PASS |
| coverage manifest | PASS (36 blocks, 9 work items, 5 paths) |

### Export, crop, thumbnail and thread rows

| Row | Result |
|---|---|
| Export graph | PASS, 22 canonical nodes, graph export vs reference `AE=0` |
| Crop graph | PASS, fit/full/zoomed `AE=0`, 6 plans, 0 plan fallbacks, 6 dirty derivations |
| Crop stages (Lab, CIE CAM, gamut, local adjustments) | PASS, all three scenes `AE=0` |
| Thumbnail graph | PASS, embedded 239x160 and raw 238x160, 6 canonical nodes |
| Thread determinism, neutral | PASS, `AE=0` at t=2/4/8 against t=1 |
| Preview latency | PASS, 5 samples, p50 305 ms, p95 307 ms |

Export timings on the 45.7 MP NEF: t=1 1240/1240/1243 ms; t=8 702/701/700 ms.
The neutral exports are pixel-identical across thread counts.

## Baseline parity

Baseline: `dev` `9c3426339` plus the AMaZE source patch from `2ebf08b0f`, on
branch `dev-amaze`, build `build/cpu-dev-amaze`. Candidate: `perf-graph`
`f7f3d2848`, build `build/cpu-perf-graph`. Both binaries run under `bwrap` with
one `rtdata` tree bound to both data paths, `OMP_NUM_THREADS=1` and
`--gpu=off`.

All values are maximum LSB16 / differing samples:

| Image / format | VNG4 | MONO | VNG4+sharpen | VNG4+rotate | VNG4+crop | AMaZE default |
|---|---|---|---|---|---|---|
| `colorspace_flowers.pef` | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS |
| `nikon-z7-45mp.nef` | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS |
| TIFF generated from PEF | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS |
| JPEG generated from PEF | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS | 0 / 0 PASS |

There is no AMaZE exception and no relaxed tolerance; any difference is
`FAIL`. See `BASELINE_PARITY.md` for the reproduction and the preview baseline.

## Preview parity

GUI screenshot comparison under isolated settings, 8-bit RGB:

| Baseline | fit | 100% | zoom | pan | fit2 |
|---|---|---|---|---|---|
| `perf-graph` pre-consolidation `a1c3e84d2` | AE=0 | AE=0 | AE=0 | AE=0 | AE=0 |
| `dev` `9c3426339` | AE=0 | differs | differs | differs | differs |

The pre-consolidation comparison is the clean handler-registry evidence. The
`dev` comparison is confounded: `dev` carries preview changes that are not on
`perf-graph`, so it does not isolate this branch. It is not a
handler-registry regression.

## Device fault injection

Run on the POCL CPU device after `f7f3d2848`:

| Case | Result |
|---|---|
| Injected device wait failure | PASS, note recorded, PAE 0 LSB16, 0 of 1,152 samples differ |
| Injected readback failure, descriptor 2 | PASS, note recorded, PAE 0 LSB16, 0 of 1,953 samples differ |
| Injected readback failure, descriptor 3 | PASS, note recorded, PAE 0 LSB16, 0 of 1,953 samples differ |
| Injected readback without a fallback, descriptors 2 and 3 | status 1, 0 differing samples |
| `stage-residency-test` run wiring | PASS, PAE 0.000732 LSB16, 2,140 of 4,608 samples differ (within 1 LSB16) |
| `stage-device-test`, OpenCL build | PASS |
| `stage-device-test`, CPU fallback build | PASS |

The test uses a private context, so the literal bounded profile category is not
captured; that assertion is code-reviewed, not measured.

## Not run

- **GPU hardware.** Workstream policy forbids a vendor ICD run without explicit
  approval. Every device figure is POCL CPU and is not GPU parity.
- **Package GPU rows.** `--gpu-test` and `--gpu-test-monitor` were not run: no
  GPU in the containers.
- **Container POCL parity.** The package does not ship the test binaries; the
  host POCL run covers the kernels.
- **Raw format breadth.** Parity covers PEF and NEF; TIFF and JPEG are generated
  from the PEF. Other raw formats are not in the matrix.
- **Preview against `dev`.** Confounded by `dev`-only preview changes; only the
  pre-consolidation comparison is used for the handler-registry claim.
- **rusticl.** Skipped on this host; POCL is the device used for every result.

## Publication scan

No publication scan is recorded for the current working-tree change; the
earlier scan of the pushed range is historical and is kept in
[`deprecated/VALIDATION-HISTORY.md`](deprecated/VALIDATION-HISTORY.md)
(deprecated; historical only). Run the publication gate before publishing.
