# Contributing to the `perf-graph` workstream

This branch is a self-contained workstream. Changes arrive as pull requests
against `perf-graph`, not as direct pushes. The branch is protected: force
pushes and direct pushes by collaborators are blocked, linear history is
required, and one approving review is required.

## Claiming and dropping work

- **Before starting**, comment `claiming #<issue>` and assign the issue to
  yourself. One owner per issue. Do not start work on an issue assigned to
  someone else without an explicit handoff.
- **The pull request must reference the issue** with `Closes #<issue>`.
- **If you drop the issue**, comment why, unassign yourself, and leave the
  current state and evidence on the issue. No stale assignments.
- **On completion**, the merged pull request closes the issue; the assignment
  ends with it.
- A handoff is explicit: unassign yourself, assign the new owner, and state
  what remains.

## Before opening a pull request

1. **Rebase onto the published tip.** Fetch `origin/perf-graph` and rebase
   your branch. A pull request that is behind the tip will be asked to rebase
   before review.
2. **Keep the scope to one block or one tool.** Shared files
   (`rtengine/stage/stage_opencl_device.cc`, `rtengine/simpleprocess.cc`,
   `devnotes/perf/STAGE_RUNTIME.md`) are the usual conflict points; say in the
   pull request which block or tool you are changing.
3. **Follow the commit format.** `<area>: #<ticket>: <summary>`, wrapped at
   78 columns. Areas in use are `perf`, `opencl`, `gui`, `docs`, `tools`,
   `test` and `build`. Use `#0` only when no ticket exists.
4. **Update the record.** Behaviour and its record belong in the same change;
   documentation is committed separately from behaviour.

## Evidence required

Every device-path pull request must carry:

- a POCL `tools/opencl/verify.sh --stage-device` run at 1 LSB16, with the
  node record showing `impl=opencl`;
- a host residual or refusal case showing `impl=host` and byte-identical
  output;
- a full run of the relevant unit tests, including a non-vacuous case: the
  test must fail on the pre-change code;
- both build configurations compiling: `tools/build-rt.sh cpu` and
  `tools/build-rt.sh opencl` (or the equivalent `rth-cli` targets);
- the measured difference, not an assumed one. `SKIP` is not `PASS`.

Do not relax a tolerance to make a pull request pass. If a path cannot hold
1 LSB16, keep it as a named host residual and record the measured divergence.

## Continuous integration

The `perf-graph build and host stage tests` workflow runs on pull requests
targeting `perf-graph` and on pushes to that branch. Two Ubuntu 24.04 jobs use
GCC, the generic target and `tools/build-rt.sh` with `-j 2`, ccache and stage
modules. One job disables OpenCL and the other enables it. Both compile the
applications and run the same bounded host test set:

```text
stage-cache-test              stage-node-cache-test
stage-graph-test              stage-indexer-test
stage-param-hash-test         stage-render-state-test
stage-param-map-test          stage-dirty-blocks-test
stage-work-test               stage-parity-test
stage-pipeline-test           stage-store-test
editorrequest-test            viewport-admission-test
denoise-tile-buffers-test
```

These targets are excluded from the default CMake build, so CI builds them
explicitly and runs their executables; they are not registered with CTest.
`stage-parity-test` compares the built-in and module host implementations of
Lanczos. It does not establish OpenCL device parity. The OpenCL-enabled job
checks compilation and host behavior without an OpenCL device or POCL.

Every selected test must exit successfully within 120 seconds and report no
`SKIP` or `skipped` record. A skip fails this host gate, even if the executable
returns zero. The job summary reports each test result. On failure, build,
dependency and test logs are retained for seven days; image data, build trees
and configuration or environment dumps are not uploaded. Each job has a
60-minute limit and a separate compiler cache capped at 2 GiB. New commits
cancel older runs of the same pull request or branch.

This is a bounded host gate, not the full stage suite. `stage-abi-test` is
currently uncovered because its upstream CMake target lacks the project
support sources needed to link it. CI does not patch that target or count it
as passing. `stage-device-test` and `stage-residency-test` are outside this
gate; their device-dependent coverage must be recorded separately.

Device parity, POCL verification, GUI checks, packaging and performance
measurements remain local gates. A green workflow does not replace the
device-path evidence above, the relevant full unit-test run, or the gates in
`AGENTS.md` and the matching `devnotes/` record. Record unavailable checks as
not run; `SKIP` is not `PASS`. CI needs no repository secrets and uses only
read access to repository contents.

## Publication gate

Before a pull request is merged, the range is scanned for:

- credentials, keys, tokens and `.env` content;
- personal paths, home or account names, private email addresses, IP or MAC
  addresses, machine identifiers and serials;
- private test data, sidecars, captures or screenshots with personal paths;
- author and committer identities other than the contributor's own address
  (prefer a provider noreply address).

The tracked test-data manifest must list only public or synthetic samples.
Private corpus rows stay in the gitignored `$RT_TESTDATA/local/` manifest.

## Review and merge

- The maintainer fetches the pull-request head, runs the gates and reviews the
  diff against the stated criteria.
- Author and committer identities are preserved; history is not rewritten.
- The merge keeps the `perf-graph` history linear where practical (rebase or
  fast-forward). A pull request that needs a merge commit will be asked to
  rebase first.
- `dev` and `perf` are not merged into `perf-graph` before the workstream
  closes.
