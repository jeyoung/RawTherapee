# Notes — stephanelee / SL

> **Maintainer note (2026-10-30):** F5 (#28) was merged at `7781f6800` after
> review. The introduction below records the state before that merge.

Second, AI-assisted contributor on `perf-graph`, working through a fork and
pull requests against `jeyoung/RawTherapee:perf-graph`.

## About me

- GitHub: [stephanelee](https://github.com/stephanelee)
- Initials / note owner: SL. I own this note; other contributors retain theirs.
- Timezone: America/Toronto. Communication is asynchronous; no fixed response
  schedule is promised.
- Fork: [stephanelee/RawTherapee](https://github.com/stephanelee/RawTherapee)
- Environment: Ubuntu 26.04 under WSL, GCC 15.2, CMake 4.2.3, C++17.
- Both CPU and OpenCL dependency checks pass at introduction base `97d109b25`.
  OpenCL headers/build support do not establish device execution.
- I can build and run the CPU application and compile the OpenCL-enabled
  application. F5's rebased head `7781f6800` passed both full builds and the
  allocation/stage-indexer tests. Its TIFF matrix is pixel-exact against the
  original `33814c82c` baseline in 12 comparisons, with six exact repeat checks.
- Earlier local validation at `0ce68a242` ran `stage-work-test`,
  `stage-graph-test`, and the production preview parity sweep. Those are
  historical results, not a rerun of every test on the introduction base.
- No OpenCL runtime/device is available locally. GPU parity, visual GUI
  checks and portable-package validation have not been performed. A known
  ABI-test linkage failure was recorded in earlier validation, not hidden.

### Reading and remaining onboarding

Read for the completed work: `AGENTS.md`, `CONTRIBUTING.md`, `cpp.md`,
`coding-conventions.md`, `TILE_INDEX.md`, `DETERMINISM.md`, and the F1–F9
architecture review. I also reviewed relevant sections of `STAGE_PIPELINE.md`,
`STAGE_RUNTIME.md`, `VIEWPORT_FAST_PATH.md` and `VALIDATION.md`.

Before accepting further work I will complete the briefing's remaining
onboarding: the full architecture records above, `FUSION.md`,
`PREVIEW_BASE_CACHE.md`, `FAST_PREVIEW_TOGGLE.md`, OpenCL `DECISIONS.md`
(particularly D004/D005) and `REVIEWS.md`. Internal device reviews are not a
substitute for CPU scheduling or lifecycle evidence.

## State

F5 is already open as [PR #28](https://github.com/jeyoung/RawTherapee/pull/28).
It was opened before this introduction, so the briefing's intended order was
missed. This note records that actual sequence; it does not imply prior
maintainer acceptance of the task. The introduction contains no F5 code.

## Proposal

Please confirm or reassign the scope of F5. If accepted, I propose finishing
that review first, then taking F9 (parity helper), followed by F8 (synchronous
device timings), one finding per branch and PR. These later tasks are proposals,
not claims or work already started in this contribution stream.

I will not edit `stage_graph.*`, `stage_work.*`, `stage_param_map.*` or
`improccoordinator.cc` without an explicit handoff. F2, F3 and F6 require an
explicit handoff as well. I will wait for introduction acceptance and explicit
maintainer scope confirmation before starting another substantive task;
silence will not be treated as consent.

## Notes from here on

### 2026-10-30 — SL — introduction and existing F5 proposal

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
