## Issue

- Claimed issue: #<number>
- [ ] The issue is assigned to the pull-request author for the duration of
      the work, and will be unassigned if the work is dropped

## Scope

Block or tool:

Base branch: `perf-graph` (rebase onto the published tip before review).

## Evidence

- [ ] POCL `tools/opencl/verify.sh --stage-device` at 1 LSB16, record
      `impl=opencl`
- [ ] Host residual or refusal at `impl=host`, byte-identical output
- [ ] Unit tests pass; the new test fails on the pre-change code
- [ ] Both build configurations compile (`cpu` and `opencl`)
- [ ] Documentation updated in a separate commit
- [ ] Measured difference recorded; `SKIP` is not `PASS`

## Publication gate

- [ ] No credentials, tokens, keys or `.env` content
- [ ] No personal paths, private email addresses, IP/MAC addresses,
      machine identifiers or serials
- [ ] No private test data or captures; the tracked manifest lists public or
      synthetic samples only
- [ ] Commit author and committer use the contributor's own address or a
      provider noreply address

See `devnotes/perf/CONTRIBUTING.md` for the full gate and merge policy.
