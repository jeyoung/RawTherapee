# Notes — stephanelee / SL

> **Maintainer note (2026-10-30):** F5 (#28) was merged at `7781f6800` after
> review. The introduction below records the state before that merge.

Current capabilities and work status are updated below; the dated
introduction entry is retained as history.

Second, AI-assisted contributor on `perf-graph`, working through a fork and
pull requests against `jeyoung/RawTherapee:perf-graph`.

## About me

- GitHub: [stephanelee](https://github.com/stephanelee)
- Initials / note owner: SL. I own this note; other contributors retain theirs.
- Timezone: America/Toronto. Communication is asynchronous; no fixed response
  schedule is promised.
- Fork: [stephanelee/RawTherapee](https://github.com/stephanelee/RawTherapee)
- Environments: Ubuntu 26.04 under WSL (GCC 15.2 and CMake 4.2.3 at the
  introduction), and native Windows x86-64 with MSYS2 UCRT64, GCC 16.2.0,
  CMake 4.4.5 and C++17.
- Native NVIDIA OpenCL is now available on an RTX 3070. The dated validation
  below records the tested base, local prerequisites, passes and failures;
  this does not establish pristine upstream or unrestricted device parity.
- I can build and run the CPU application and compile the OpenCL-enabled
  application. F5's rebased head `7781f6800` passed both full builds and the
  allocation/stage-indexer tests. Its TIFF matrix is pixel-exact against the
  original `33814c82c` baseline in 12 comparisons, with six exact repeat checks.
- Earlier local validation at `0ce68a242` ran `stage-work-test`,
  `stage-graph-test`, and the production preview parity sweep. Those are
  historical results, not a rerun of every test on the introduction base.
- Native Windows GPU exports and a bounded GUI smoke test have now run.
  Preview parity and portable-package validation remain unverified. The known
  ABI-test linkage failure belongs to earlier validation and has not been
  counted as passing here.

### Reading and remaining onboarding

Read for the completed work: `AGENTS.md`, `CONTRIBUTING.md`, `cpp.md`,
`coding-conventions.md`, `TILE_INDEX.md`, `DETERMINISM.md`, and the F1–F9
architecture review. I also reviewed relevant sections of `STAGE_PIPELINE.md`,
`STAGE_RUNTIME.md`, `VIEWPORT_FAST_PATH.md` and `VALIDATION.md`.

Remaining onboarding from the introduction includes the full architecture
records above, `FUSION.md`,
`PREVIEW_BASE_CACHE.md`, `FAST_PREVIEW_TOGGLE.md`, OpenCL `DECISIONS.md`
(particularly D004/D005) and `REVIEWS.md`. Internal device reviews are not a
substitute for CPU scheduling or lifecycle evidence.

## State

- F5 ([PR #28](https://github.com/jeyoung/RawTherapee/pull/28)) was integrated
  at `7781f6800`. It preceded the introduction; that sequence remains recorded
  in the historical entry below.
- The introduction in [PR #29](https://github.com/jeyoung/RawTherapee/pull/29)
  was accepted and integrated by the maintainer as rebased commits
  `8fff36399..becd88011`.
- The host CI contribution for issue #31, from
  [PR #33](https://github.com/jeyoung/RawTherapee/pull/33), was integrated by
  the maintainer as `8831ba35a..775aa41e4`. This is host build/test coverage,
  not device parity.
- The issue #30 tiled-denoise harness is in draft
  [PR #34](https://github.com/jeyoung/RawTherapee/pull/34). Exact-parity
  acceptance remains unresolved; passing focused tests and diagnostic repeats
  do not erase the failed engine acceptance run.
- F8 is reproduced on native NVIDIA OpenCL at the tested base plus local
  prerequisites below. Device access is available for this bounded path;
  synchronous timing implementation still needs agreed scope. The separate
  eight-thread output divergence is unresolved.

## Coordination and scope

Keep findings on separate branches and PRs, with an issue claim and explicit
maintainer coordination before substantive implementation. The native
portability prerequisites and benchmark are local validation work, not an F8
fix or a claim that those source changes have been accepted upstream.

I will not edit `stage_graph.*`, `stage_work.*`, `stage_param_map.*` or
`improccoordinator.cc` without an explicit handoff. F2, F3 and F6 require an
explicit handoff as well. Introduction acceptance does not grant ownership of
those areas or imply acceptance of later proposals; silence is not consent.

## Notes from here on

### 2026-10-30 — SL — introduction and existing F5 proposal (historical)

This entry records the pre-acceptance state; use the current State above for
subsequent integration and validation status.

**State:** awaiting introduction acceptance and maintainer coordination; F5
PR #28 is open.
**Branch:** `perf-graph-intro-sl`, based on `97d109b25`.
**Claim:** this one-file introduction establishes my note and proposed scope.
F5's implementation and measured evidence remain in its separate PR and handoff.
**Not verified:** no GPU execution or resident-memory reduction is claimed;
the remaining onboarding reading is listed above.
**Next:** respond to the maintainer and keep findings on separate branches.
**Needs from main developer:** confirm or reassign F5, and indicate whether
F9 then F8 are suitable follow-up areas.

### 2026-10-30 — SL — native Windows validation and CPU/OpenCL benchmark

**State:** native NVIDIA device execution verified for the tested gamut path;
F8 remains unfixed. Eight-thread output parity failed and remains unresolved.
**Tested base:** `775aa41e4`, with three uncommitted local prerequisites below.
This is a development build, not a pristine upstream build, a validation of
the latest `perf-graph` tip, or a release package. This note update publishes
none of those source changes.

#### Environment and prerequisites

Native Windows x86-64; MSYS2 UCRT64 GCC 16.2.0, CMake 4.4.5, generic target,
C++17 and LCMS 2.19. Hardware: Intel Core i7-10700 (8 cores / 16 logical
processors), NVIDIA GeForce RTX 3070, driver 610.60, OpenCL 3.0 CUDA.
Application execution for these checks was native Windows, not WSL.

The three local build/device prerequisites were:

| File | Local prerequisite |
|---|---|
| `rtengine/stage/stage_device.cc` | C++17 aligned allocation and matching deallocation for MinGW |
| `rtengine/stage/working_trc_context.cc` | Tone-curve segment access for LCMS 2.16 and newer |
| `rtengine/stage/stage_opencl_device.cc` | Load `OpenCL.dll` on Windows |

They do not change F8 timing logic. The native CLI used for the benchmark has
SHA-256 `4eb98bc489ad153434f8825dd47ac0aff305b3704fbc8ba1fae8dad114ce6077`.
The separate prerequisite patches and full local logs are retained locally;
no private paths, images, screenshots or environment dumps accompany this note.

#### Bounded validation

- **PASS:** both native CPU and OpenCL full builds and runtime-data installs;
  `stage-device-test` and allocator diagnostics in both configurations. The
  CPU-only configuration's expected OpenCL-disabled fallback is not GPU proof.
- **PASS:** native device descriptor identifies RTX 3070 with `cpu=false`;
  CLI GPU self-test and all three planar rectangular writes/readbacks succeed.
- **PASS:** full-size gamut export records `impl=opencl`. With one OpenMP
  thread, the GPU/host maximum error is 1 LSB16, within the standard tolerance.
  Disabling gamut materially changes the output, so the check is non-vacuous.
- **PASS:** the CPU-only build's single-thread output is pixel-exact against
  the OpenCL-enabled build with GPU disabled.
- **PASS, smoke only:** the GUI loads the public PEF, renders at 100% and 50%
  zoom, handles double-click fit, and closes normally with isolated settings.
  This does not establish preview parity or general GUI coverage.
- **F8 reproduced, not fixed:** the native GPU gamut record has `ns=0` at
  3888x2608. A separate affine probe produces 1,048,576 exact samples and a
  positive GPU event duration of 27,552 ns. That probe is not the gamut kernel.
- **NOT RUN because compilation failed:** the canonical native
  `stage-residency-test` uses POSIX `setenv`/`unsetenv`, unavailable in this
  Windows build. It is not counted as passing; no full-suite claim is made.

#### Whole-export benchmark

Workload: public `colorspace_flowers.pef`, rendered at 3888x2608 (10.1 MP),
unchanged `devnotes/opencl/pp3/gamut.pp3`, 16-bit RGB TIFF. The input SHA-256
matches the public manifest:
`ddefa07a8b48fd157b48354488b265be48a47c204d00d20d54ead8dfddd9f151`.

Both arms use the same native OpenCL-enabled executable. CPU forces
`--gpu=off` and `RT_STAGE_OPENCL=0`; OpenCL uses `--gpu=auto` and
`RT_STAGE_OPENCL=1`, selecting NVIDIA. Match `OMP_NUM_THREADS` and
`OMP_THREAD_LIMIT` at 1 or 8; set `OMP_MAX_ACTIVE_LEVELS=1` and
`OMP_DYNAMIC=FALSE`. Settings, cache and temporary storage are isolated per
arm. One untimed warm-up/attribution export precedes five measured pairs at
each thread count; pair order alternates CPU/OpenCL and OpenCL/CPU.

Each export starts a fresh process with warm OS/driver caches. Timed runs
disable profiling and verbose output. Wall time includes startup, loading,
processing, transfers, TIFF encoding/writing and exit. Warm-up device records
identify `gamut`, `working.trc` and `output.convert` as OpenCL implementations;
the CPU warm-up records its active gamut duration with GPU disabled. These are
whole-export mode comparisons across multiple stages, not gamut/kernel-only
speedups. F8's invalid zero timing is not used to calculate them.

| OpenMP threads | CPU median (range), s | OpenCL median (range), s | CPU/OpenCL ratio |
|---|---:|---:|---:|
| 1 | 15.687 (15.458–18.515) | 6.117 (5.789–6.346) | 2.564x |
| 8 | 6.786 (6.608–7.135) | 4.279 (4.246–4.458) | 1.586x, timing only; parity failed |

All 20 measured exports exited successfully. Individual measured seconds,
ordered by pair within each arm:

| Threads | Mode | Pair 1 | Pair 2 | Pair 3 | Pair 4 | Pair 5 |
|---|---|---:|---:|---:|---:|---:|
| 1 | CPU | 15.596206 | 15.687176 | 15.457880 | 15.709878 | 18.515290 |
| 1 | OpenCL | 6.117326 | 6.345541 | 6.140131 | 6.013062 | 5.789267 |
| 8 | CPU | 7.134804 | 6.785949 | 6.835399 | 6.607693 | 6.778860 |
| 8 | OpenCL | 4.310896 | 4.257773 | 4.246142 | 4.278614 | 4.457621 |

The 18.515 s CPU sample is retained. Other desktop applications remained open;
GPU utilization snapshots before/after timed work were 9%/10%. Five warm pairs
on one image/profile are local indicative measurements, not a general hardware
performance claim or a cold-start benchmark.

#### Output parity and unresolved work

Comparisons use the existing `tools/opencl/compare_tiff.py` on the retained
final output of each arm, not on every timed export. There are 30,419,712
channel samples per image. The standard acceptance bound is 1 LSB16.

- **One thread: PASS.** Maximum error 1 LSB16; 826,270 samples differ.
- **Eight threads: FAIL.** Maximum error 60 LSB16; 826,340 samples differ;
  comparator exit 1. The 1.586x timing ratio does not establish equivalent
  output performance, and the tolerance is unchanged.
- One prespecified untimed CPU-only eight-thread repeat is pixel-exact against
  the original CPU output (maximum 0, zero differing samples). Comparing that
  repeat with the retained GPU output still gives 60 LSB16. This does not
  establish a cause, and does not erase the original failed comparison.

An initial setup attempt expected JSON from the direct CPU path, which emits
duration text instead. It stopped before any measured sample; that setup
failure and all benchmark/diagnostic outputs are retained locally. It is not
an excluded timing outlier or a successful parity retry.

**Next:** coordinate the F8 timing scope with the maintainer, and agree a
separate investigation of the eight-thread output divergence before claiming
equivalent-output performance at that setting. Preview parity, the Windows
residency suite and portable-package validation remain uncovered. The native
portability prerequisites need their own agreed contribution scope.
