# RawTherapee — development process

How work is carried out in this repository. The global
`~/.config/opencode/AGENTS.md` remains in force for general workflow, design
method, code style and security. This file adds the project's process.
Architecture, behaviour and invariants belong in `devnotes/`; coding style and
C++ language constraints are defined in `devnotes/coding-conventions.md` and
`devnotes/cpp.md`.

## Development loop

1. **Analyse** — state the problem, constraints and numbered acceptance
   criteria, and define the evidence that will prove each one.
2. **Design and plan** — choose the smallest solution per criterion and write
   a plan detailed enough for another agent with no prior context. Delegate
   plan review to a subagent before presenting it.
3. **Isolate** — create a branch or git worktree (`issue-<n>-<slug>` for issue
   work). Never mix unrelated work in one branch.
4. **Implement** — one change at a time. Record decisions and evidence in the
   matching `devnotes/` file as you go.
5. **Check** — run static analysis and the relevant tests.
6. **Review** — judge the change against the acceptance criteria. Delegate to
   a subagent and repeat until the review shows high confidence. Report
   PASS/FAIL with evidence per criterion.
7. **Build and test the build** — compile both OpenCL configurations for
   engine changes, and produce and test a package when the change is
   user-visible. Never test in production.
8. **Commit** — only stable, tested, reviewed changes, grouped by concern.

## Workspace and branches

- `origin` is the personal fork, `upstream` is the upstream project. Fetch
  from `upstream`; publish to `origin` only when instructed.
- `dev` is the integration line and `perf` the performance line. Land work on
  a dedicated branch and fast-forward it into `dev` after verification, then
  align `perf` with `dev`. `opencl` is the GPU backend line; merge it through
  `perf` and `dev` as work lands.
- Build release artefacts from a clean commit or clean worktree at the
  intended commit. Do not ship a dirty tree.
- Do not rewrite pushed branches. Do not touch unrelated uncommitted changes;
  if the tree is dirty, work in a separate worktree.
- Always work inside `~/source/github/RawTherapee`. Do not create or use
  worktrees outside it. If isolation is needed, place the worktree under
  `~/source/github/RawTherapee/build/worktrees/<name>`.

## Build process

Use the helper rather than invoking CMake directly:

```bash
tools/build-rt.sh cpu
tools/build-rt.sh opencl
tools/build-rt.sh both
tools/build-rt.sh portable
tools/build-rt.sh cpu --install
```

- Build directories are `build/<variant>-<branch>/` (`build/portable/` for
  packages); binaries are `<build-dir>/rtgui/rawtherapee` and
  `rawtherapee-cli`.
- Always use GCC, the generic target and an out-of-source build. Remove stale
  in-source build artefacts before configuring; the helper refuses to
  configure while they exist. Do not bypass the helper.
- Install the runtime data with the binary (`--install`). The default prefix
  is `/usr/local`; use `--prefix`/`RT_INSTALL_PREFIX` or run as root.
  `RT_DESTDIR` only changes the destination and must be combined with
  `--install` when staging.
- Every engine change must compile with OpenCL enabled and disabled.
- Register new user-visible strings for translation (see
  `devnotes/how-to-add-processing-tool.md`).

### Build-time economy

- Reuse the build tree. Do not pass `--clean` or `--reconfigure` during normal
  iteration; the helper reuses the CMake cache. Reconfigure only after CMake
  inputs change. Build directories are branch-qualified, so switching branches
  does not require cleaning.
- Build the narrowest target. For engine and CLI work use
  `cmake --build build/cpu-<branch> --target rth-cli -j 6`;
  build `rth` only when the change touches GUI code. Run the full helper
  build at the final gate.
- Iterate on one variant: `cpu` for general work, `opencl` only when touching
  OpenCL. Compile the other configuration once before commit.
- Build once, then run every applicable `verify.sh` mode against the same
  `RT_BUILD_DIR`; verification does not rebuild.
- Batch edits to widely included headers, which rebuild most of the tree, and
  prefer forward declarations per `devnotes/coding-conventions.md`.
- Do not use `tools/build-rt.sh portable` as an inner-loop build; it compiles
  from scratch in a Debian container and is for release artefacts only.
- Skip the build for documentation-only changes and run only the checks the
  change needs; the publication gate still applies.
- Cap parallelism at the physical-core count, up to six jobs: on a 4C/8T
  machine use `-j 4` to `-j 6`, not `-j 8`, because SMT plus limited RAM
  thrashes. Check `lscpu` for cores per socket rather than `nproc`, and pass
  the count explicitly to the helper (`tools/build-rt.sh cpu -j 6`). Lower it
  further if the linker is killed for memory, and do not compile while
  running GUI or performance tests.

### Compiler cache (optional)

- Install `ccache` (`sudo apt install ccache`). It is optional.
- Enable it once per helper build tree, as the only exception to configuring
  through the helper. Existing cache values are retained:

  ```bash
  cmake -S . -B build/cpu-<branch> \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  ```

  Repeat after `--clean`, which deletes the build directory.
- Keep the cache local and bounded:

  ```bash
  export CCACHE_DIR="$PWD/build/ccache"
  export CCACHE_MAXSIZE=5G
  ```

  Check effectiveness with `ccache -s`; a low hit rate means the compiler or
  flags differ between builds.
- It speeds repeat compilation and branch switches. It does not speed
  configuration, linking, the first build, or the portable container build.

## Verification process

`RT_BUILD_DIR` is required: `tools/opencl/verify.sh` does not descend into
`build/<variant>-<branch>/`. Use an absolute path, or run from the repository
root.

```bash
export RT_BUILD_DIR="$PWD/build/cpu-<branch>"
tools/opencl/verify.sh --quick
tools/opencl/verify.sh --formats
tools/opencl/verify.sh --monitor
tools/opencl/verify.sh --rgbproc
tools/opencl/verify.sh --list-coverage
tools/perf/test-preview-base-cache.sh "$RT_BUILD_DIR"
```

- Keep `RT_VERIFY_THREADS=1` for parity runs (see
  `devnotes/opencl/PARITY.md`).
- `SKIP` is not `PASS`. On an OpenCL-enabled build with no selected device,
  `RT_ALLOW_CPU_PARITY=1` permits a CPU-only `--formats` comparison; record
  it as CPU-only, never as GPU parity. `--quick` and `--rgbproc` still
  require an accelerated stage. Use `RT_REQUIRE_GPU=1` when a missing device
  must fail the gate and `RT_OPENCL_ALLOW_CPU=1` to test with POCL.
- Record the measured difference against the tolerance in
  `devnotes/opencl/PARITY.md`: about 1 LSB at 16-bit for standard parity,
  1 byte for monitor parity, and pixel-exact 16-bit TIFF output (`AE=0`) for
  decoder and raw-loading changes (see `devnotes/perf/INITIAL_LOAD.md`).
- Test data lives in `RT_TESTDATA` (default
  `$(PWD)/build/rt-testdata`); the corpus and hashes are in
  `devnotes/opencl/testdata.manifest`. Copy samples in rather
  than symlinking them; never commit or upload raw files, sidecars or
  captures. Use `tools/opencl/fetch-testdata.sh` for shared samples.
- Use the representative corpus: 10.2 MP PEF and 45.7 MP NEF, plus the other
  formats when the path is format-sensitive.
- Preview-cache changes need a GUI check; CLI export parity is not evidence
  for preview correctness (see `devnotes/perf/PREVIEW_BASE_CACHE.md`).

### GUI checks

Run GUI checks under Xvfb or Xephyr and time performance only on the host, not
in a container. For isolated runs, point `RT_SETTINGS` and `RT_CACHE` at
absolute paths in a project-local scratch directory such as
`$PWD/build/test-scratch`.

1. Maximise the window.
2. Dismiss any startup notice.
3. Click the hand tool, then click the image so the image area has focus.
4. Send shortcuts (`z`, `+`/`-`, `alt+f`); without focus they reach the wrong
   widget. Use the mouse for modal dialogs such as Rename.
5. Keep double-click at 100% zoom as an independent fallback.
6. For loading work, use the checks in `devnotes/perf/PREVIEW.md`.

## Packaging process

- `tools/build-rt.sh portable` builds the Debian package in a Debian 12
  container and targets Debian 12 and newer.
- Build from a clean commit, then record the `.deb` filename and SHA-256 in
  the handover or issue.
- Test on both Debian 12 and Debian 13: install, `--list-formats`, a CLI
  export compared with a single-threaded host export, and a GUI smoke test
  under Xvfb. Run `--gpu-test` and `--gpu-test-monitor` where hardware is
  available; otherwise record them as not run.
- Remove any stale self-contained bundle before installing (see
  `devnotes/opencl/PORTABLE.md`).
- Report packaging limitations and skipped checks instead of marking the
  package verified.

## Performance-change process

- Establish a baseline before editing: `RT_LOAD_PROFILE=1` for raw loading and
  `RT_PREVIEW_PROFILE=1` for preview timings. Report before/after on the same
  image, profile and zoom.
- The interactive preview must meet the documented parity tolerance; do not
  accept visible approximation.
- Time on the host. Container runs are functional checks only.
- New fast paths start opt-in. Promote to default only with parity and
  stability evidence, and leave an environment kill switch plus a preference
  where the setting is user-visible.
- Document hardware, thread count, image, profile and tolerance with every
  timing or parity figure.

## Documentation process

- Update the matching record with the code. OpenCL records live in
  `devnotes/opencl/` (`STATUS`, `DECISIONS`, `BENCHMARKS`, `PARITY`,
  `PORTABLE`, `FORMATS`, `TESTDATA`, `REVIEWS`); performance records in
  `devnotes/perf/` (`PREVIEW`, `INITIAL_LOAD`, `PREVIEW_BASE_CACHE`,
  `VIEWPORT_FAST_PATH`); and `devnotes/build.md` for build-helper changes.
- Replace stale numbers and decisions instead of appending contradictions.
- Commit documentation separately from behaviour changes.

## Git and tickets

- Commit format: `<area>: #<ticket>: <summary>`, wrapped at 78 characters.
  Use `#0` only when no ticket exists. Areas in use include `perf`, `gui`,
  `opencl`, `docs`, `tools`, `test` and `build`.
- Create or reuse an issue for the work. If the work is already complete, add
  a retrospective issue with commit links and evidence.
- Scan for secrets and personal information before publishing, following the
  publication gate in the global `AGENTS.md`. Push only when asked.
- Merge completed work fast-forward into `dev`; do not rewrite shared history.

### Publication gate

The global gate applies in full, to every ref, tag, issue, release and
workflow log, including documentation-only changes. In this project:

- Scan the working tree, the complete history of every ref being published,
  commit messages and tags before pushing.
- Inspect the author and committer of every commit in the range, not only the
  configured `user.name` and `user.email`. Prefer a project-owned or provider
  noreply address for this public fork.
- New and modified source headers must not add personal contact details such
  as an email address, phone number or postal address; a name alone is
  permitted. Do not rewrite existing headers except to remove a leak.
- The tracked `testdata.manifest` and `TESTDATA.md` must list only public or
  synthetic samples. Keep private `local/` rows in a gitignored manifest under
  `$RT_TESTDATA`. Replace them with public samples where coverage is required
  and record any remaining gap as uncovered, never as PASS.
- Remove the existing private rows from `testdata.manifest` and `TESTDATA.md`
  before the next push.
- Never commit or upload raw files, sidecars, captures, exported previews or
  screenshots that show private paths or file names.
- Model-level hardware detail is permitted for benchmark reproducibility.
  Never publish host names, serial numbers, UUIDs or raw `lscpu`/`dmidecode`
  output.
- Record the scan scope and result as evidence.
- Treat a leak as an incident. Rotate or revoke a secret. For personal
  information, remove it from current content and ask the provider to purge
  it. Do not rewrite pushed history; assume published data stays public.

## Review and delegation

- Delegate exploration, planning and review to subagents so the main session
  keeps context small; run at most three concurrently.
- Give each subagent a self-contained brief with the goal, files, constraints
  and acceptance criteria.
- Do not accept a change until a review confirms each criterion with evidence.

## Pre-commit checklist

- [ ] Acceptance criteria marked PASS with evidence.
- [ ] Static analysis clean; relevant tests run.
- [ ] Both OpenCL configurations compile for engine changes.
- [ ] Parity measured, not assumed; `SKIP` runs recorded as such.
- [ ] Build and package tested when user-visible.
- [ ] Documentation updated in a separate commit.
- [ ] Ticket created or linked.
- [ ] Publication gate passed: no secrets or personal information; identity
      checked; scan recorded.
