# Architecture review of `perf-graph`

Reviewed 2026-11-06 at `dd983002b581f084544945ddc1a879224861174f`,
the published `perf-graph` tip after a fresh fetch. Tracking issue: #38.
This is a source review of the graph workstream, with focused planner tests;
it is not a complete audit of RawTherapee or a new parity certification.

## Assessment

The direction is sound: a shared stage contract, typed dirty/work derivation,
conservative reuse, and opt-in device execution make the legacy processor
easier to inspect and migrate incrementally. Keep those boundaries. The
contract is not yet sufficient to drive every execution path or buffer
lifetime, and fallback safety must cover a whole node rather than one buffer
descriptor. Resolve R1 before relying on recovery from device readback errors;
resolve R2 and R3 before broadening generic execution or stage extensibility.

The earlier F1-F9 findings are recorded as addressed in
[WORKSTREAM.md](WORKSTREAM.md). This review credits those fixes, including
the single contract and working-set admission. R1 is a remaining transaction
boundary problem beyond F3's individual-descriptor readback fix, not a claim
that the earlier fix was absent.

## Architecture and safeguards

```text
typed editor request + parameters + render/source state
             |
stage contract -> fingerprints -> dirty blocks -> work order
             |                                      |
       path specification ----------------------> path adapter
                                                    |
                                      legacy bodies / stage::run
                                                    |
                             CPU builtin/module or OpenCL queue
                                                    |
                                 residency + scratch reservations
                                                    |
                                      output / host fallback
```

- **Specification and execution are separate.**
  [stage_contract.h:31-52](../../rtengine/stage/stage_contract.h#L31)
  defines the shared vocabulary and path records. The contract owns no image
  memory and executes no processing. The parameter map binds hashers, dirty
  derivation identifies affected blocks, work orders identify destructive
  jobs, and path adapters execute bodies. This is a useful migration seam,
  provided each adapter's extra ordering and side effects remain explicit.
- **Reuse includes more than image pixels.**
  [stage_cache_integration.cc:271-292](../../rtengine/stage/stage_cache_integration.cc#L271)
  serializes raw-base auxiliary state, including white balance, denoise
  validity and histogram. The preview cache and its conservative admission
  rules are described in [STAGE_PIPELINE.md](STAGE_PIPELINE.md#node-output-cache-of-the-preview-rawbase-tier).
  Retain side-channel restoration and edit-sequence parity checks when
  expanding cache coverage; a matching pixel buffer alone is insufficient.
- **Device use is opt-in and fallback attribution is honest.**
  [stage_runtime.cc:523-550](../../rtengine/stage/stage_runtime.cc#L523)
  selects the device path and records the device that actually executed.
  [stage_device.cc:384-390](../../rtengine/stage/stage_device.cc#L384)
  runs compiled host programs on the host queue. Host fallback is an explicit
  execution policy, rather than an accelerated-success claim.
- **Memory admission and readback have useful local invariants.**
  [stage_opencl_device.cc:2094-2127](../../rtengine/stage/stage_opencl_device.cc#L2094)
  provides RAII scratch reservations sharing the residency budget.
  [The transfer implementation](../../rtengine/stage/stage_opencl_device.cc#L2008)
  stages one descriptor's download before committing its rows, preserving
  surrounding pixels for subrectangle views. R1 explains the remaining
  limitation when one node writes several descriptors.

## Prioritized findings

Priorities describe follow-up urgency: P1 is a conditional correctness risk,
P2 is an architectural or diagnostic gap, and P3 is API hardening. The failure
scenarios below are inferred from source; no driver failure was injected and
no new image-corruption reproducer is claimed.

### R1 — P1: readback is transactional per descriptor, not per node

**Evidence and trigger.**
[run_lab_sharpening](../../rtengine/stage/stage_opencl_device.cc#L15137)
downloads L, a and b through three separate `readback()` calls, checks their
statuses only afterward, and writes directly into the host planes. Each
successful call commits its own descriptor. If L succeeds but a or b fails,
the node returns an error with some host outputs already replaced.
[The queue](../../rtengine/stage/stage_opencl_device.cc#L22301)
then submits host fallback. The
[builtin](../../rtengine/stage/stage_builtins.cc#L399)
passes the same decoded descriptors to the whole host block; that block
[runs the applicable impulse-denoise, defringe, sharpening and microcontrast steps](../../rtengine/stage/lab_sharpening_context.cc#L581).
An already transformed luminance plane can therefore be processed again.

**Existing protection.** Failure within one descriptor leaves that
descriptor unchanged. Unsupported configurations refused before readback do
not encounter this partial-commit scenario. These safeguards do not roll back
a previously successful descriptor.

**Follow-up and acceptance.** Treat the complete writable output set as one
transaction: stage every download and commit only after all succeed, or
restore every modified host input before fallback. Audit other launchers with
multiple writable descriptors. Inject failures on the second and third
Lab-plane transfers and require final output equal to an untouched-input
host-only run, with `impl=host`; also retain subrectangle and normal-success
coverage. The injection must prove failure on the current implementation.

### R2 — P2: crop graph order differs from execution order

**Evidence and consequence.**
[The crop contract](../../rtengine/stage/stage_contract.cc#L306)
chains `raw.base -> analysis`. The
[crop execution enum and comment](../../rtengine/dcrop.cc#L293)
intentionally put analysis before raw-base finishing, because noise is
measured on the freshly loaded region. The adapter
[checks topological order](../../rtengine/dcrop.cc#L487)
but [constructs execution in enum order](../../rtengine/dcrop.cc#L506).
The graph currently selects work; its edges do not fully describe the crop's
actual data dependencies. A generic executor or lifetime planner following
those edges would not reproduce the established interleaving.

**Existing protection.** The adapter deliberately preserves legacy order;
this finding does not demonstrate a current crop pixel defect. Structural
graph validation cannot certify that the runtime order matches its edges.

**Follow-up and acceptance.** Model crop acquisition and base finishing as
separate steps, or express the actual dependency with path-specific nodes and
side-channel edges. Preserve the existing processing order while making the
model accurate. Compare an instrumented executed sequence with the contract
after fold resolution, including auto-denoise cases, then run crop sequence
parity. Do not replace enum order with topological order as a standalone fix.

### R3 — P2: preview validation does not prove a handler exists

**Evidence and trigger.** The planner promises that every node has an
executor, but its
[executor check](../../rtengine/improccoordinator.cc#L4596)
calls [isPreviewNode](../../rtengine/improccoordinator.cc#L1261), which only
checks membership in the same specification being iterated. A new canonical
preview node without a dispatch arm passes this check. The
[unknown dispatch arm](../../rtengine/improccoordinator.cc#L4683)
contains only `assert(false)`, so it does no work in an `NDEBUG` build.

**Existing protection.** Current nodes have dispatch arms; debug builds
assert if dispatch reaches the unknown arm. The risk is silent omission when
the contract grows, not a demonstrated missing current handler.

**Follow-up and acceptance.** Use an adapter-owned handler registry for both
lookup and dispatch, and validate every non-folded node against it before any
body executes. Keep the contract independent of coordinator implementation
types. Add an unhandled node in a negative test and require a rejected plan
with its id in both assertion-enabled and `NDEBUG` builds, with no body run.

### R4 — P2: fallback reasons disappear before profiling

**Evidence and consequence.**
[StageEvent::note](../../rtengine/stage/stage_device.h#L78)
carries refusal details, and the queue sets it on
[Lab sharpening fallback](../../rtengine/stage/stage_opencl_device.cc#L22312).
[StageExecution](../../rtengine/stage/stage_device.h#L314)
and [StageContext::execute](../../rtengine/stage/stage_device.cc#L371)
carry only device and asynchronous state upward. The
[runtime profile](../../rtengine/stage/stage_runtime.cc#L548)
therefore reports generic `device-fallback`. A successful host recovery does
not distinguish unsupported parameters, memory admission or a device error.
That limits diagnosis of correctness incidents and unexpected slow paths.

**Existing protection.** The profile correctly reports `impl=host` and elapsed
time; this is a loss of cause, not false device attribution.

**Follow-up and acceptance.** Carry a stable refusal category and bounded
diagnostic through execution and profile records. Test unsupported parameters,
a forced budget refusal and an injected device error; each must preserve
host-output parity while publishing a distinct category without private paths.

### R5 — P3: the low-level graph plan cannot report a cycle

[StageGraph::plan](../../rtengine/stage/stage_graph.cc#L88)
ignores the boolean result of `topologicalOrder()` and returns a full-sized
vector even when some nodes were not visited. Current preview, crop, export
and thumbnail adapters precheck topology, so this is mitigated for their
existing calls. Before reusing this API in another executor, return an
explicit failed-plan result and test that cyclic input produces no executable
decisions. Keep the existing adapter checks until that invariant is shared.

## Verification coverage and follow-up sequence

The [host CI workflow](../../.github/workflows/perf-graph.yml#L93)
explicitly excludes device parity, POCL, GUI, packaging and performance, and
records the uncovered ABI test linkage gap. The OpenCL matrix job compiles
that configuration and runs host tests; it is not a device execution gate.
The [contribution process](CONTRIBUTING.md#continuous-integration)
already states this accurately. Repair the ABI target and add a reproducible
POCL/fault-injection gate as follow-up work; retain hardware-specific parity
evidence where GPU support differs from POCL.

Recommended sequence: R1's failure injection and node transaction first;
R3's handler check next; R2 before generic scheduling or lifetime inference;
R4 alongside device-coverage expansion; R5 before adding graph API consumers.
Keep each behavior change and its measured parity evidence in its own PR.

Review acceptance evidence:

| Criterion | Result and evidence |
| --- | --- |
| Current architecture and existing safeguards | PASS: source map above; prior F1-F9 fixes credited, not reopened wholesale. |
| Actionable, qualified findings | PASS: R1-R5 include source, trigger, current mitigation and acceptance checks; delegated source reviews checked graph and device boundaries. |
| Focused planner checks | PASS: existing `stage-pipeline-test` and `stage-graph-test`, compiled directly from this worktree with GCC, C++17, `-O2 -Wall -Wextra -Werror`. Both reported PASS. These checks exercise specification/planner behavior, not pixels. |
| Document integrity and publication | PASS: relative file/heading links, pinned source line references, diff whitespace and outgoing-document identity/secret/path checks completed before publication; tracked corpus manifest contains only the public sample. |

No production behavior changes are included. Full CPU/OpenCL application
builds, ABI test linkage repair, device fault injection, RAW/export parity,
GUI, package and performance checks were not run for this documentation PR.
Previous workstream results remain historical evidence, not tests rerun by
this review. In particular, R1's fault scenario still needs reproduction.
