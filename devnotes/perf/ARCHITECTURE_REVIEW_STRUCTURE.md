# Structural architecture review of `perf-graph`

Reviewed 2026-11-06 at `dd983002b581f084544945ddc1a879224861174f`, the
published `perf-graph` tip. Tracking issue: #38.

This is a read-only source review. It **complements** the companion review in
PR #39 (`ARCHITECTURE_REVIEW_2026-11-06.md`, findings R1–R5) and does not
repeat it. Those findings are referenced where this review extends them. The
earlier F1–F9 findings are recorded as fixed in [WORKSTREAM.md](WORKSTREAM.md)
and are not reopened.

**Line references.** They are pinned to the commit above.

**How the findings were reached.** Every finding below comes from reading the
code. None was reproduced at run time. The status column says how sure each one
is:

- **Confirmed**: the code path was read end to end and the defect is
  unambiguous.
- **Inferred**: the trigger is plausible from the code but needs a reproducer.

## Summary

The workstream has three layers:

- **Data-only contract** (`stage_contract.{h,cc}`). This is the right backbone.
  F4 made it the single vocabulary.
- **Planning layer** (fingerprints → dirty blocks → work order → plan). It is
  well tested in isolation.
- **Device layer** (`stage_opencl_device.cc`). It is opt-in, falls back
  honestly, and is gated by parity.

The structural weaknesses sit at the joins between these layers, not inside
them:

1. **Execution authority is still split.** The plan, the work order and the
   legacy request intents are nested gates inside each body. The bugs fixed so
   far (F1, capture sharpening, Retinex) were all disagreements between these
   gates, fixed one at a time. The root cause is still there (S1).
2. **Parameter identity is maintained by hand in about eight places.** Nothing
   checks that any of them is complete. One of them, the persistent base-cache
   key, already leaves out raw inputs (S2, S3).
3. **The device layer has no launcher abstraction.** Ownership is manual and
   the kernel cache is keyed per entry point. Correctness invariants that hold
   for one launcher are not enforced for the others: R1 also applies to
   `lab.wavelet` and `raw.preprocess`, and two launchers drop their working-set
   reservations (S5–S8).
4. **Plugin resolution.** The stage runtime prefers external `.so` modules over
   built-ins by default. Their search path includes a compiled-in build
   directory, and it is resolved on every call (S4).

Recommended order:

1. S3 and S5: data correctness of opt-in features.
2. S4: default-on, hot path, loading external code.
3. S1 and S2: the root cause of the gate-disagreement bug class.
4. S6–S9: device structure and accounting.
5. S10–S13: hardening.

## Architecture as built

```text
EditorRequest (≈25 intent bools) ──┐
ProcParams ─► stage_param_map ─────┼─► dirty blocks ─► StageGraph::plan ─► string dispatch
               (hand hashers)      │      (preview: hand insertions;          │
RenderStateKey ────────────────────┘       crop: derive_dirty_blocks)         ▼
                                   work order (derive_*_work) ─────► legacy body
                                                                    (re-checks intents)
                                                                          │
                                                       stage::run(name, RtStageArg[])
                                                                          │
                                       ┌──────────── node_device() ───────┴───────────┐
                                       ▼                                              ▼
                         OpenCLStageQueue (one global                StageCompiler: module .so
                         context/queue/residency)                   first, then builtin
                                       │ fallback                    (builtin = trampoline back
                                       └───────────────────────────► into the host block)
```

Observations about the model itself:

- **The graph is a chain.** Every path except the thumbnail is a linear chain
  (`chain(nodes)`, [stage_contract.cc:260](../../rtengine/stage/stage_contract.cc#L260)).
  Side channels such as analysis, the spot buffer and the raw planes are
  modelled as `StageEffect` flags rather than edges.
  - `StageGraph::plan()` can therefore reuse output only on the clean prefix
    before the first dirty node. That is the same expressiveness as the
    upstream refresh mask.
  - The graph currently adds three things: a validated inventory, a log
    vocabulary and a cache-key boundary. It does not add scheduling freedom.
    This is acceptable, but documents and code comments should not imply
    generic DAG execution. R2 covers the crop order mismatch.
- **Five hand-written planner copies.** Each one builds the spec, builds a
  `StageGraph`, marks it dirty, validates and dispatches by string:
  - preview, [improccoordinator.cc:4540-4650](../../rtengine/improccoordinator.cc#L4540);
  - crop, [dcrop.cc:409-519](../../rtengine/dcrop.cc#L409);
  - export and fast export, [simpleprocess.cc:416-484](../../rtengine/simpleprocess.cc#L416)
    and [554-635](../../rtengine/simpleprocess.cc#L554);
  - thumbnail, [rtthumbnail.cc:299-388](../../rtengine/rtthumbnail.cc#L299).

  R3 shows one consequence: the executor check cannot fail.
- **A stage node is a trampoline.** The C-ABI builtin receives a call-site
  context that carries a `run_host` function pointer back into the legacy block
  ([stage_builtins.cc:78-98](../../rtengine/stage/stage_builtins.cc#L78)).
  - Each device node therefore needs a second, flattened C representation of
    its tool parameters, plus a hand port of the algorithm to OpenCL. Examples:
    `local_adjustments_context.h` is 1,164 lines and `lab_wavelet_context.h` is
    1,124.
  - Device kernels total about 18,900 lines under `algorithms/*_cl.h`.
  - This triple representation (ProcParams, context, kernel) is the dominant
    maintenance cost of the workstream. Parity gates are the only thing keeping
    the three in step.

## Findings

| ID | Sev. | Status | Area | One line |
| --- | --- | --- | --- | --- |
| S1 | High | Confirmed / Inferred | Planning | Plan, work order and request intents are nested, independent gates |
| S2 | High | Confirmed | Fingerprints | No completeness check of the hand-written ProcParams hashers |
| S3 | High (opt-in) | Inferred | Cache | Persistent base-cache key leaves out preprocess and capture-sharpening inputs |
| S4 | High | Confirmed | Runtime | `stage::run` prefers external modules by default and resolves them on every call |
| S5 | High | Confirmed | Device | R1's partial commit also affects `lab.wavelet` and `raw.preprocess` |
| S6 | Med-High | Confirmed | Device | Lanczos and transform drop their working-set reservation |
| S7 | Medium | Confirmed | Device | A full program build per kernel entry point, no on-disk cache, no failure memo |
| S8 | Medium | Confirmed | Device | No launcher abstraction; one 22.6k-line TU; manual `cl_mem` ownership |
| S9 | Medium | Confirmed | Device | Global queue lock held across host work and waits; residency accounting can under-count |
| S10 | Medium | Confirmed | Integration | About eight hand-maintained parameter-equality lists overlap |
| S11 | Med-Low | Inferred | Concurrency | Lock order `mProcessing → updaterThreadStart` on the immediate-flush path |
| S12 | Low | Confirmed | Policy | Detail-crop threads never set the per-thread stage OpenCL switch |
| S13 | Low | Confirmed | Robustness | Export plan rejection calls `std::abort()` inside the GUI process |

### S1 — High: execution authority is split across three nested gates

**Evidence.** Each preview and crop body re-checks gates after the plan has
selected it. `stageRawDemosaic` is one example:

1. It returns unless the work order schedules `rawDemosaic`
   ([improccoordinator.cc:1465](../../rtengine/improccoordinator.cc#L1465)).
2. It then runs the actual demosaic only when `request.rawIntent` or a
   highlight-recovery predicate is true
   ([:1470-1475](../../rtengine/improccoordinator.cc#L1470)).

The Lab nodes in the dirty set do nothing unless `labChainIntent` is set:

- preview, [:3476-3477](../../rtengine/improccoordinator.cc#L3476);
- crop, [dcrop.cc:2285](../../rtengine/dcrop.cc#L2285) and five more sites.

The work log records three fixes for disagreements of exactly this kind (F1,
capture sharpening, Retinex; [WORKSTREAM.md](WORKSTREAM.md)). Each one moved a
single gate.

**Inferred trigger.** `rawDemosaic` is scheduled from the sensor fingerprint,
but the demosaic still requires `rawIntent`. A parameter-only or coalesced
request that changes `raw.bayersensor.method` without the raw intent bit would
therefore rerun capture sharpening but keep the old demosaic. This is not
reproduced.

**Recommendation.**

- Make the plan plus the work order the only execution gate.
- Turn each remaining intent check into one of two things:
  - an input to the dirty or work derivation, produced in `make_request`; or
  - a debug assertion that the intent agrees with the plan.
- Add a check, in both release and debug builds, that every scheduled work item
  is implemented by a block that is in the plan. `workItemBlocks()` already
  exists for this.

**Acceptance.** A `stage-work-test` or `crop-stage-test` case changes the
demosaic method through a parameter-only request and proves the pixels change.
The case must fail on the current tip.

### S2 — High: no completeness check of the ProcParams hashers

**Evidence.**
[stage_param_map.cc](../../rtengine/stage/stage_param_map.cc) contains about 55
hand-written `addField` overloads, with 1,568 member lines in total.
[stage_param_map_test.cc:414-617](../../rtengine/stage/tests/stage_param_map_test.cc#L414)
applies one representative mutation per family. Nothing compares the hashed
members with the members of `ProcParams`:

- there is no `sizeof` tripwire;
- there is no cross-check against `operator==`;
- there is no cross-check against the keyfile keys.

**Current state.** The coverage is complete today for the structs sampled
(`ToneCurve`, `DirPyrDenoise`, `ColorManagement`, `Crop` and `Resize` match
their declared member counts). `Sharpening` deliberately omits five
uninitialised members, which is documented at
[:1718-1726](../../rtengine/stage/stage_param_map.cc#L1718).

**Risk.** A member added to a `ProcParams` sub-struct and not to the hasher
produces no dirty block, no work item and an unchanged node-cache key. That is
a silently stale preview.

**Release-build gap.** `buildPath()` drops a contract block that has no hasher
under `NDEBUG`. The guard is an `assert` only
([:2576](../../rtengine/stage/stage_param_map.cc#L2576)).

**Recommendation.** Add a test that walks every key `ProcParams::save()` writes,
mutates it, reloads the profile and asserts that at least one fingerprint
changes, with an explicit allow-list for non-processing keys. This needs no
reflection and catches every new serialised member. Also make the
missing-hasher case fail closed in release builds.

### S3 — High (opt-in `RT_STAGE_STORE`): persistent base-cache key is incomplete

**Evidence.** The persistent key drops the generation counter on purpose
([previewbasecache.h:110-166](../../rtengine/previewbasecache.h#L110)).

- **What it keeps:** file identity, white balance, demosaic method, `ccSteps`,
  `raw.expos`, the highlight-recovery settings and geometry.
- **What it leaves out:** dark frame, flat field, black levels, hot and dead
  pixels, raw CA, line noise, dual-demosaic contrast and `pdsharpening`.

`tryRestore()` consults the store whenever the in-memory entry is not valid
([:324-329](../../rtengine/previewbasecache.h#L324)), and `markDirty()` makes it
not valid.

**Inferred trigger.**

1. Turn on the store.
2. Change capture sharpening or the dark frame.
3. The in-memory entry is invalidated.
4. The persistent entry written for the previous settings has an identical key
   and is restored.

The result is stale base pixels. The in-memory path is safe only because
`sourceGeneration` is part of its key. This is not reproduced.

**Recommendation.** Build the persistent key from the `raw.preprocess` and
`raw.demosaic` fingerprints, which already exist and are tested, instead of a
hand-picked field list. Until then, skip the persistent lookup after any
`markDirty()` in the session.

**Acceptance.** With the store on, a change to capture sharpening produces
`AE>0` against the previous render and `AE=0` against an uncached render.

### S4 — High: `stage::run` prefers external modules and resolves them on every call

**Evidence.** On the host path, every `stage::run()` call does the following
([stage_runtime.cc:408-512](../../rtengine/stage/stage_runtime.cc#L408),
[:515-576](../../rtengine/stage/stage_runtime.cc#L515)):

1. It builds a `StageRequest`, copying the argument vector.
2. It reads `RT_STAGE_<NAME>` and `RT_STAGE_IMPL`.
3. It takes the global compiler mutex.
4. For each search directory it `stat()`s up to three candidates and can
   `dlopen` one.

Only after that does it fall back to the builtin.

- **Search directories.** They are `RT_STAGE_PATH` plus the compiled-in
  `RT_STAGE_BUILD_DIR="${CMAKE_BINARY_DIR}/stages"`
  ([rtengine/CMakeLists.txt:196](../../rtengine/CMakeLists.txt#L196)).
- **Default resolution.** The default `impl` is `"auto"`, which means "module
  first".

Consequences:

- **Packages.** A shipped package therefore embeds, and probes at run time, the
  absolute path of the machine that built it. Any `.so` placed there, or on
  `RT_STAGE_PATH`, replaces a built-in processing stage without any signature or
  version check beyond the ABI integer.
- **Bare-name candidate.** One candidate is the bare `request.name`. Whether
  `Glib::Module` then resolves it relative to the working directory has not
  been verified and should be checked.
- **Cost.** The same work runs once per call: `rgb.proc`, `lab→rgb` and
  thumbnail calls happen per image and, for some paths, per tile.
- **ABI.** Modules are documented as stateless, but the trampoline contexts
  (see above) carry host function pointers. A module build of those nodes
  cannot be meaningful, so the plugin ABI promises more than the nodes can
  deliver.

**Recommendation.**

- Default to builtin-only.
- Allow modules only behind an explicit opt-in such as `RT_STAGE_IMPL=module`
  or a build option that is off for packages.
- Drop `RT_STAGE_BUILD_DIR` from release configurations.
- Resolve once per name and memoise, including negative results.
- Remove the bare-name candidate.

### S5 — High: R1 also affects `lab.wavelet` and `raw.preprocess`

R1 (PR #39) describes `lab.sharpening`. The same per-descriptor commit followed
by host fallback occurs in two more launchers:

- **`lab.wavelet`, in place.** L is committed at
  [stage_opencl_device.cc:11521](../../rtengine/stage/stage_opencl_device.cc#L11521).
  A chroma readback failure at
  [:11527-11533](../../rtengine/stage/stage_opencl_device.cc#L11527) then
  returns `RT_STAGE_FALLBACK`, so the host reruns the wavelet on an
  already-processed L.
- **`raw.preprocess`, in-place `scaleColors`.** The image is committed at
  [:12743](../../rtengine/stage/stage_opencl_device.cc#L12743). A row-max
  readback failure at
  [:12746](../../rtengine/stage/stage_opencl_device.cc#L12746) leads to a
  second host scaling of the mosaic.

`local.adjustments` (one staging buffer,
[:21973](../../rtengine/stage/stage_opencl_device.cc#L21973)) and the
out-of-place `rgb.proc` are safe.

[WORKSTREAM.md:143](WORKSTREAM.md) states "commit … after every plane
succeeds". That is true per `readback()` call, not per node, and should be
corrected.

**Recommendation.** Fix this with R1 through one node-level helper: stage every
output, then commit all of them. Then audit every launcher that has more than
one writable descriptor.

### S6 — Medium-High: Lanczos and transform drop their working-set reservation

The F6 admission creates a `WorkingSetReservation` in `launch_lanczos`
([:2412](../../rtengine/stage/stage_opencl_device.cc#L2412)) and in
`launch_transform`
([:2611](../../rtengine/stage/stage_opencl_device.cc#L2611)). The comments
say the guard "travels with the submission".

Both launchers then build `OpenCLSubmission` without the optional seventh
`reservation` argument
([:2501](../../rtengine/stage/stage_opencl_device.cc#L2501),
[:2692](../../rtengine/stage/stage_opencl_device.cc#L2692); constructor at
[:2133](../../rtengine/stage/stage_opencl_device.cc#L2133)). The reservation is
therefore released when the launcher returns, while the scratch stays alive
until the asynchronous submission completes.

At 45.7 MP the transform map (four floats per pixel, about 730 MB) leaves the
budget during the kernel. That defeats the admission F6 was written to provide.

**Fix.** Pass `working` to the constructor. Add a residency test asserting
`used_bytes` while a submission is pending.

### S7 — Medium: kernel program cache is per entry point

The kernel cache key is `def->name + ':' + entry`
([:2261](../../rtengine/stage/stage_opencl_device.cc#L2261)). Each miss calls
`CreateProgramWithSource` and `BuildProgram` on the whole program source and
then releases the program
([:2266-2311](../../rtengine/stage/stage_opencl_device.cc#L2266)).

- `local.adjustments` concatenates about 13k lines of OpenCL and resolves about
  67 entry points, so the first use compiles that source about 67 times.
- There is no on-disk binary cache, although `rtengine/opencl/openclcache.{h,cc}`
  exists and the classic engine uses it.
- A failed build is not remembered, so it is retried on every call.

**Recommendation.**

- Cache one `cl_program` per `KernelDef` plus build options, and create kernels
  from it.
- Reuse `openclcache`, keyed by source hash, options, device and driver
  version.
- Memoise failures.
- Measure first-use latency before and after on the host.

### S8 — Medium: no launcher abstraction; one 22.6k-line translation unit

**Size.** `stage_opencl_device.cc` is 22,603 lines and about 1 MB. The largest
functions are:

- `run_local_adjustments`: about 6,800 lines (15174–21983);
- `run_lab_wavelet`: about 4,400 lines;
- `run_lab_tools`: about 2,400 lines.

**Repeated plumbing.** Every launcher is hand-rolled:

- 26 local `set_arg` lambdas;
- 193 `residency->acquire` calls, 75 uploads and 40 readbacks;
- scratch held in raw `std::vector<cl_mem>` and released by hand-called
  `release_scratch()` lambdas, about 20 in `run_sharpen` alone.

Nothing is exception-safe, so a `std::bad_alloc` between create and release
leaks device memory.

**Dispatch.** The queue dispatch is 23 copies of the same "run, OK → complete,
else fallback" block
([:22036-22370](../../rtengine/stage/stage_opencl_device.cc#L22036)).

**Error codes.** `RT_STAGE_FALLBACK` and `RT_STAGE_ERROR` (450 and 81 returns)
are treated identically by the queue, so the distinction carries no meaning.
R4 covers the related loss of the fallback reason.

**Recommendation.**

1. Introduce RAII `cl_mem`/`cl_event` handles and a small `KernelLauncher`:
   typed argument binding, upload, enqueue, wait, and a transactional
   multi-readback (which also fixes S5).
2. Move to a table-driven dispatch.
3. Split per node into `stage_opencl_<node>.cc` so that a change to one node
   no longer rebuilds 1 MB.

Do this incrementally, one launcher per PR, each with `--stage-device`
parity.

### S9 — Medium: global queue lock and residency accounting

**One global device.** There is one process-wide `OpenCLEnv`, with one context,
one in-order queue and one residency. All host use is serialised by the
recursive `queue_mutex`.

That lock is held across:

- kernel waits ([:2166](../../rtengine/stage/stage_opencl_device.cc#L2166));
- `rgb.proc` readbacks;
- in `run_local_adjustments`, everything from
  [:15695](../../rtengine/stage/stage_opencl_device.cc#L15695) to the end,
  including the host-side context build for each spot.

As a result, preview, detail-crop and export work serialise on one lock even
while they are doing host work.

**Accounting can under-count.** `release()`
([:1940](../../rtengine/stage/stage_opencl_device.cc#L1940)), `clear()`
([:1951](../../rtengine/stage/stage_opencl_device.cc#L1951), which also wipes
live reservations) and the stale-shape rebind
([:1829-1833](../../rtengine/stage/stage_opencl_device.cc#L1829)) erase entries
without checking `use_count()`. `used_bytes` can therefore fall while the memory
is still held.

**Budget default.** The default budget is a fixed 1 GiB and ignores
`CL_DEVICE_GLOBAL_MEM_SIZE`. Its comment says it "fits a pair of" 45 MP RGB F32
planes ([:112](../../rtengine/stage/stage_opencl_device.cc#L112)), but two such
planes are about 1,046 MiB.

**Recommendation.**

- Build host contexts outside the lock, and hold it only for argument binding
  plus enqueue.
- Account orphaned live buffers until their last reference drops, for example
  with a deleter that decrements.
- Derive the default budget from the device's memory size.

### S10 — Medium: about eight overlapping parameter-equality lists

Apart from the fingerprints, "did the inputs change" is decided by hand-written
lists:

- the `EditorRequest` intent table
  ([editorrequest.cc:120-300](../../rtengine/editorrequest.cc#L120));
- the preview dirty insertions
  ([improccoordinator.cc:4413-4459](../../rtengine/improccoordinator.cc#L4413));
- `baseCacheDirtyBlocks` ([:94-140](../../rtengine/improccoordinator.cc#L94));
- the `PreviewBaseCache::Key` fields (S3);
- `sameBaseInputs` ([:551-578](../../rtengine/improccoordinator.cc#L551));
- viewport admission
  ([editorrequest.cc:850-934](../../rtengine/editorrequest.cc#L850));
- `panningRelatedChange`, a 40-term `!=` chain
  ([improccoordinator.cc:5996-6036](../../rtengine/improccoordinator.cc#L5996)).

Crop and preview also use different dirty derivations: crop uses
`derive_dirty_blocks` and never reads `request.dirty`.

**Recommendation.** Replace each list with a comparison of contract
fingerprints for the relevant block set. Keep the lists only where a GUI-only
input is involved, and document it there. This is the structural fix that
complements S1 and S2.

### S11 — Medium-Low: lock order on the immediate-flush path (inferred)

With `RT_VIEWPORT_DEFER_MS=0` (not the default), the path is:

1. `updatePreviewImage` holds `mProcessing`.
2. It calls `queueViewportDeferredAnalysis`
   ([improccoordinator.cc:4847](../../rtengine/improccoordinator.cc#L4847)).
3. That calls `flushDeferredAnalysis()`, which calls `startProcessing()`, which
   locks `updaterThreadStart`
   ([:983-987](../../rtengine/improccoordinator.cc#L983),
   [:5943](../../rtengine/improccoordinator.cc#L5943)).

Meanwhile, `stopProcessing()` holds `updaterThreadStart` and joins the updater
thread ([:5912-5931](../../rtengine/improccoordinator.cc#L5912)). The
`deferredWatcherPaused` check in `flushDeferredAnalysis` is taken and released
before `startProcessing()`. A stop that lands in that window would therefore
deadlock.

**Fix.** Defer the flush until `mProcessing` is released, or post it to the
watcher.

### S12 — Low: detail-crop threads ignore the per-thread stage switch

Two per-thread switches exist:

- `stage::set_opencl_stage_allowed` is `thread_local` and defaults to `true`
  ([stage_runtime.cc:100](../../rtengine/stage/stage_runtime.cc#L100));
- the classic switch defaults to `false`.

The preview, export and thumbnail paths set the stage switch from the user
preference. `dcrop.cc` never does. With the `RT_STAGE_OPENCL=1` opt-in, the
detail window therefore runs device nodes even when the user's OpenCL
preference is off.

**Fix.** Set the switch in `Crop::update`, or default it to `false` and require
every pipeline to opt in explicitly.

Related: the builtins toggle the switch with manual save and restore
(`set_opencl_stage_allowed(false)` … restore,
[stage_builtins.cc:98](../../rtengine/stage/stage_builtins.cc#L98) and six other
sites). A throwing host block leaves the thread disabled. A scoped guard would
fix that.

### S13 — Low: export plan rejection aborts the process

`abortStageGraph()` calls `std::abort()`
([simpleprocess.cc:206](../../rtengine/simpleprocess.cc#L206); nine call
sites). The export path also runs inside the GUI for the batch queue, so a
contract or executor mismatch kills the editor and any unsaved work.

The conditions are static per build: node count, acyclicity, and an executor
for each id. They belong in a unit test that fails the build, plus a recoverable
export error at run time.

## Further notes (no action ranked)

- **Environment control plane.** About 35 `RT_*` environment variables steer
  behaviour, caches and device selection. Several duplicate user preferences.
  Before promotion to default, list them in one place, and state which are
  supported, which are test-only, and how each interacts with the preference.
- **Documentation volume.** `STAGE_RUNTIME.md` (3,900 lines),
  `STAGE_PIPELINE.md` (2,750) and `WORKSTREAM.md` (1,500) mix the current
  architecture with history. A short current-state architecture page, with the
  history kept in the work log, would make the invariants easier to review.
  `stage_work.h` is about 60% prose and has already drifted ("nine inputs"
  against "eight").
- **Two OpenCL stacks.** The classic engine (`rtengine/opencl`) and the stage
  layer each have their own loader, device selection, program cache and memory
  pool, with no shared budget. Converge them before both run in one session by
  default.
- **Portability.** Module resolution hard-codes `.so` and `:` separators, and
  the Windows and macOS workflows do not run on `perf-graph`. Record the
  workstream as Linux-only until that is tested.
- **Fork probe.** The ICD probe forks after GUI threads exist
  ([stage_opencl_device.cc:370-395](../../rtengine/stage/stage_opencl_device.cc#L370)).
  A lock held across `fork` hangs the child until its alarm, and the platform
  is then wrongly marked timed-out. Probe at start-up or in a helper process.

## Strengths to keep

- A single data-only contract, read by all four planning authorities (F4).
- Conservative reuse. Device execution is opt-in. Fallback attribution is
  honest: `impl=host` is recorded when the host ran.
- Parity discipline: `FP_CONTRACT OFF`, no fast-math, and correctly rounded
  divide/sqrt where needed, with a documented tolerance per gate.
- Transactional single-descriptor readback (F3) and RAII scratch reservations
  (F6). S5 and S6 extend these to every launcher; they do not replace them.
- Strong unit coverage of the planning layer: dirty blocks, work order, param
  map, store and residency.

## Review acceptance evidence

| Criterion | Result and evidence |
| --- | --- |
| Covers architecture beyond PR #39 | PASS: S1–S13 do not duplicate R1–R5; S5 explicitly extends R1. |
| Each finding is actionable and evidenced | PASS: pinned file:line evidence, trigger, status (Confirmed/Inferred) and fix or acceptance criterion. |
| Key claims checked against source | PASS: S3 key fields, S4 resolution order and build-dir define, S5 wavelet readbacks, S6 constructor calls, S7 cache key, S11 lock path and S2 hasher member counts (sampled structs) were each re-read directly. |
| No behaviour change | PASS: documentation only. No build was needed; no build, parity, GUI or package check was run. |
| Publication gate | PASS: no secrets, personal data, host names or private sample names; relative links only. |

Not run: application builds, unit tests, device fault injection, parity, GUI
and performance. S1, S3 and S11 still need reproducers before their fixes are
accepted.
