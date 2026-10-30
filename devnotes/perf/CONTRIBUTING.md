# Contributing to the `perf-graph` workstream

This branch is a self-contained workstream. Changes arrive as pull requests
against `perf-graph`, not as direct pushes.

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
