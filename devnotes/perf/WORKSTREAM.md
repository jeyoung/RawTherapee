# RawTherapeeGraph `perf-graph` workstream

Current-state record. It carries only the state, decisions, package, test
summary and open gaps that apply now. Architecture and invariants stay in the
sibling records: `STAGE_RUNTIME.md`, `STAGE_PIPELINE.md`, `VALIDATION.md`,
`BASELINE_PARITY.md`, `TEST_COVERAGE.md` and `HARNESS.md`. The historical
progress log is in
[`deprecated/WORKSTREAM-PROGRESS-2026-11.md`](deprecated/WORKSTREAM-PROGRESS-2026-11.md)
(deprecated; historical only).

## Current state

| Item | Value |
|---|---|
| Repository | `$REPO` |
| Branch | `perf-graph`, tip `4c690a041` |
| Worktree | main checkout, clean, 23 commits ahead of `origin/perf-graph` |
| Latest code change | `4c690a041` (StagePlan, contract coverage, typed refusals); later commits are documentation and package records |
| Build trees | `build/cpu-perf-graph`, `build/opencl-perf-graph` (GCC, Release, generic target) |
| Baseline | `dev` `9c3426339` plus the AMaZE source patch from `2ebf08b0f`, branch `dev-amaze`, build `build/cpu-dev-amaze` |
| Test corpus | public 10.2 MP PEF; 45.7 MP NEF; TIFF and JPEG generated from the PEF |
| Device | POCL CPU only; no GPU run |

Test targets are not part of the default build; build them by name or through
`tools/perf/run-suite.sh`. Device tests must run through `tools/opencl/pocl-run.sh`
or the POCL environment wrapper.

## Current package

```
build/portable/rawtherapee-graph-5.13-799-g4c690a041_5.13+git13820.4c690a041-1_amd64.deb
sha256 0c92105cd6f8a5f2a7361f4074a35bb9a805fe681ab449bcbd976c88ee7413c3
```

Built 2026-11-07 from `4c690a041` with `tools/build-rt.sh portable`. Validated
in fresh Debian 12 and Debian 13 containers: install, `--list-formats` (36
entries), a neutral 16-bit export identical to the host CLI (AE=0, 0 of
30,419,712 samples), and a GUI smoke test under Xvfb (20 s timeout, exit 124).
`--gpu-test` and `--gpu-test-monitor` were not run: the containers have no GPU.
See `VALIDATION.md` for the matrix and `devnotes/opencl/PORTABLE.md` for the
package guide.

## Current scope

Block 4 (device node coverage) and the P1 workstream are closed. Stage C is
complete: local sharpening, crop, `lab.sharpening`, `lab.tools`, `lab.wavelet`
and `local.adjustments` have their device nodes. The raw nodes cover
`raw.preprocess`, `raw.demosaic`, `raw.base` and `raw.base.wb`. No defect work
remains.

The workstream remains opt-in and measured:

- Stage OpenCL stays opt-in (`RT_STAGE_OPENCL=1`; kill switches
  `RT_STAGE_OPENCL=0` and `RT_STAGE_OPENCL_DISABLE=1`). Every device figure is
  POCL CPU, which is slower than the host path, so the default cannot be
  promoted without a real GPU run.
- The node cache stays opt-in (`RT_NODE_CACHE` unset means off). It restores
  correct bytes, but no workload yet puts a repeated base rebuild on the
  settled-latency critical path. `RT_NODE_CACHE_MB` sets the budget and
  `RT_NODE_CACHE_STATS` prints counters.
- Residency is fail-closed: a node runs on the device only when the preference,
  the environment opt-in, the device requirements and the budget all admit it.
  A refusal runs the host builtin byte for byte and records `impl=host`,
  `source=device-fallback`; it is never a silent skip.
- Tile ordering and fusion stay opt-in performance features. The viewport fast
  path is complete and default-on.
- `RT_WORK_SHADOW` is a diagnostic marker with no engine effect.
- `dev` and `perf` are not merged into `perf-graph` before the workstream
  closes. Stable, scanned ranges are pushed to `origin/perf-graph`.

Environment switches: `RT_WORK_ORDER` (default on), `RT_NODE_CACHE` (default
off), `RT_NODE_CACHE_MB`, `RT_NODE_CACHE_STATS`, `RT_STAGE_OPENCL` (default
off), `RT_STAGE_DEVICE_BUDGET_MB`, `RT_WORK_SHADOW`, `RT_OPENCL_ALLOW_CPU`,
`RT_REQUIRE_GPU`, `RT_TESTDATA`.

## Architecture changes

- **StagePlan**: `buildStagePlan()` composes the path specification, dirty
  blocks, work order, buffer/working-set requirements and execute order. Crop
  uses it for derivation and execution; preview uses it for the canonical
  bundle and falls back to the coordinator-merged plan when the sets differ.
- **Contract implementation map**: every block now carries `hostBody`,
  `deviceCoverage` and `deviceStages`, projected into the descriptor and
  validated against `stage_opencl_device.cc`.
- **Typed refusals**: `StageRefusalCode` travels from the queue and submission
  into `StageExecution` and the profile; the profile publishes `refusal`,
  `fallback` and `diagnostic`.
- **Buffer plan**: `StagePlan::buffers` describes ownership, working-set class,
  inputs and output for every block. Runtime allocation and residency still
  consume it only descriptively.
- **Shared body layer**: not completed. Preview and crop still own separate
  stage-body orchestration. This is the remaining architecture change and
  requires a dedicated extraction, not a mechanical merge.

## Current verification summary

The full suite `final-suite` (suite tip `17a5995c5`, build `cpu-perf-graph`)
reports `PASS`, `failures: 0`. All of its rows pass:

- stage host tests: `stage-abi`, `stage-parity`, `stage-cache`, `stage-graph`,
  `stage-indexer`, `stage-store`, `stage-node-cache`, `stage-device`,
  `stage-residency`, `stage-param-hash`, `stage-pipeline`, `stage-param-map`,
  `stage-render-state`, `stage-dirty-blocks`, `stage-work`, `stage-handler`,
  `editorrequest`, `viewport-admission`, `denoise-tile-buffers`;
- `amaze-tile-parity`, `fast-preview`, `preview-base-cache`,
  `working-trc-setup`;
- the coverage manifest (36 blocks, 9 work items, 5 paths);
- baseline parity against the corrected `dev` baseline;
- export graph (22 nodes, AE=0), crop and thumbnail parity;
- neutral thread determinism at t=1/2/4/8 (AE=0);
- preview latency (5 samples, p50 305 ms, p95 307 ms).

The suite build is CPU-only, so `stage-residency-test` reports `PASS` with 44
device rows skipped. The separate POCL run (`inject-residency.log`) reports
`PASS` with 1 skipped and covers the device rows. Device fault injection passes
there: wait failure and readback descriptors 2 and 3 fall back with PAE 0, and
the no-fallback cases return status 1 with 0 differing samples.

Baseline parity is exact on every row: PEF, NEF, TIFF and JPEG, each with VNG4,
MONO, VNG4+sharpen, VNG4+rotate, VNG4+crop and default AMaZE, all `0/0 PASS`.
There is no AMaZE exception and no relaxed tolerance. See `BASELINE_PARITY.md`.

Preview parity against the pre-consolidation `perf-graph` baseline `a1c3e84d2`
is `AE=0` for fit, 100%, zoom, pan and fit2. The current `dev` preview differs
at 100%, zoom, pan and fit2 because `dev` carries preview changes that are not
on `perf-graph`; that comparison is confounded and is not a handler-registry
regression.

## Open gaps

- **GPU hardware not run.** Workstream policy forbids a vendor ICD run without
  explicit approval. All device evidence is POCL CPU. The package GPU rows
  (`--gpu-test`, `--gpu-test-monitor`) were not run because the containers have
  no GPU.
- **Raw format breadth.** Baseline parity covers the public PEF and the 45.7 MP
  NEF; TIFF and JPEG coverage uses files generated from the PEF. Other raw
  formats are not in the parity matrix.
- **Preview comparison against `dev` is confounded.** The exact preview result
  is the pre-consolidation `perf-graph` comparison; the `dev` comparison cannot
  isolate this branch.
- **Fault-injection profile category not captured.** The injection test uses a
  private context, so the bounded profile category is code-reviewed rather than
  asserted by the test.
- **POCL only.** rusticl was skipped on this host. This is an environment fact,
  not a code residual.
- **Measured residual:** the PEF with denoise keeps 24 pixels at 1 LSB16
  between t=1 and t=8, inside the parity tolerance but not bit-identical. The
  NEF is exact at all thread counts.

## History

The long progress log, earlier completion plans, earlier package details and
superseded narratives are in
[`deprecated/WORKSTREAM-PROGRESS-2026-11.md`](deprecated/WORKSTREAM-PROGRESS-2026-11.md)
(deprecated; historical only). Do not read it for current state.
