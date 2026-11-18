# Architecture review of `perf-graph`

Reviewed 2026-11-17 at [`8a61197d4`](https://github.com/jeyoung/RawTherapee/commit/8a61197d4585e3829043cab5942e254be38cc81e).
The latest engine change at that tip is [`437575c18`](https://github.com/jeyoung/RawTherapee/commit/437575c18).
This review follows [#39](https://github.com/jeyoung/RawTherapee/pull/39)
and [#40](https://github.com/jeyoung/RawTherapee/pull/40) for [#38](https://github.com/jeyoung/RawTherapee/issues/38).

## Assessment

Keep the canonical stage contract, per-path handler registries and shared CPU
bodies. Fix preview cache ownership and wavelet scratch lifetime before
carrying those components forward. Resolve the device reservation and refusal
lifetimes before expanding OpenCL use. The buffer plan needs explicit pixel
bindings before it can drive allocation or residency.

Typed request and parameter fingerprints drive dirty-block derivation. The
canonical plan combines that result with the path graph and the destructive
work order for preview and crop. Crop executes the plan directly. Preview
adds coordinator-owned producer, deferred and intent facts and can run clean
in-place bodies, so it also resolves a merged order. Export, fast export and
thumbnail still plan in their adapters. Preview, crop and export share host
bodies where their call options permit. The device queue and CPU fallback
remain separate from those planning decisions. [Stage-plan builder](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_plan.cc#L84-L178),
[preview scheduling](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/improccoordinator.cc#L4558-L4647),
[workstream record](WORKSTREAM.md).

The five findings in #39 have corresponding fixes at this tip. Multi-output
readback stages the whole transaction, the crop graph puts analysis before
base finishing, preview validates against its handler registry, the profile
has a typed refusal field, and graph planning explicitly rejects cycles
among connected nodes. The edge builder still discards self-edges. The
refusal field has a concurrency problem described below. [Node readback](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L2241-L2285),
[crop order](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_contract.cc#L365-L381),
[preview handler validation](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/improccoordinator.cc#L4085-L4093),
[typed refusals](https://github.com/jeyoung/RawTherapee/commit/b44dee85b),
[cycle validation](https://github.com/jeyoung/RawTherapee/commit/e42d991d2),
[self-edge handling](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_graph.cc#L18-L26).

## Findings

### P1 — A preview cache hit replaces its own snapshot

`PreviewBaseCache::tryRestore()` swaps the cached image with the caller's
destination and returns a hit while retaining the original key and valid bit.
The next matching lookup can therefore return whatever was in the old
destination. The preview cache is enabled by default. The coordinator
snapshots the result of `getImage()`, restores it into `orig_prev` on a hit,
and later converts `orig_prev` in place. If an eligible same-key rebuild
restores twice while the destination holds converted pixels, it can feed
those pixels back into color conversion. [Cache defaults](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/previewbasecache.h#L231-L246),
[restore](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/previewbasecache.h#L347-L397),
[snapshot point](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/improccoordinator.cc#L2154-L2178),
[conversion](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/improccoordinator.cc#L2244-L2263).

Reproduced at the cache API. The existing 8×8 fixture passed. In a scratch
copy, filling the destination with a sentinel before the first hit and
checking the pixels after the second hit exposed the stale payload. See the
validation detail below for the two-hit probe. Preserve the snapshot across
hits, or explicitly consume it and recompute after a miss. Verify each
successful hit returns the stored RGB samples and compare repeated eligible
preview rebuilds with cache disabled. For a GUI check, make two manual
Exposure Compensation edits at fixed zoom and confirm both produce
cache-hit records before comparing the rendered pixels. [Regression fixture](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/tools/perf/preview-base-cache-test.cc#L110-L130),
[documented Exposure hit](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/devnotes/perf/PREVIEW_BASE_CACHE.md#L134-L138).

### P1 — The shared wavelet body leaks a full Lab scratch image

With Wavelet enabled and `softrad > 0`, the host body allocates a copy of all
three Lab planes. It frees the copy only in the clarity branch, whose
predicate also requires `expclari`, a matching `ushamethod` and
`CLmethod != "all"`. A profile with a positive saved radius and clarity
disabled leaks roughly 12 bytes per processed pixel each time the body
runs. The allocation pattern predates the shared body. Preview, crop and
export now have one place to fix it. [Allocation](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/lab_wavelet_context.cc#L1367-L1406),
[conditional cleanup](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/lab_wavelet_context.cc#L1490-L1542),
[plane storage](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/labimage.cc#L87-L94).

Allocate the snapshot only when the clarity branch will read it, with
scope-bound ownership spanning both passes. Render repeatedly with a
positive radius and clarity disabled under a leak checker, then compare
the active clarity case pixel-for-pixel against the current host output.
The existing radius profile is a starting point. [Wavelet profile](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/devnotes/opencl/pp3/labwavelet-softrad.pp3#L10-L31).

### P2 — Persistent preview cache hits can undo preprocessing invalidation

With `RT_STAGE_STORE=1`, a dirty or mismatched in-memory entry can trigger
a disk lookup before the miss is returned. The disk key omits the source
generation and has no complete fingerprint for the preprocessing that
produced the pixels. A Bayer black-point edit invalidates the preview cache
and changes preprocessing, but the persisted lookup can still match and
clear the dirty state. The cache-level behavior was reproduced by changing
the generation and marking the entry dirty for preprocessing. This confirms
the persistent identity finding in #40. [Persistent key](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/previewbasecache.h#L129-L157),
[disk lookup before miss](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/previewbasecache.h#L315-L344),
[disk restore](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/previewbasecache.h#L629-L642),
[black-point fingerprint](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_param_map.cc#L2108-L2109),
[preprocess invalidation](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/improccoordinator.cc#L1450-L1462).

Use stable source identity and all pixel-producing state in the persisted
key. Version and validate the payload schema so entries from incompatible
implementations miss. Test that an unchanged source/profile hits across
sessions and a preprocessor change misses, then compare a RAW black-point
edit with the store disabled.

### P2 — Asynchronous device scratch outlives its budget reservation

The Lanczos and transform launchers reserve device memory before creating
tap buffers or the transform map. Both transfer the scratch buffers into
`OpenCLSubmission` while omitting its optional reservation argument. The
guard releases the charge at launcher return while the submission still
owns the allocation. An overlapping submission can pass admission against
memory already committed to the first request. The transform map alone is
16 bytes per destination pixel. This confirms the reservation-lifetime
finding in #40 at the current tip. [Reservation guard](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L2142-L2160),
[submission lifetime](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L2291-L2319),
[Lanczos submission](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L2592-L2694),
[transform submission](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L2798-L2885).

Make the submission own the reservation through command retirement,
including error and unwaited-event paths. Keep one submission live and
assert that its exact scratch charge remains visible, that a second
over-budget request refuses before allocation, and that the charge is
released when the command and scratch retire.

### P2 — Device refusal codes can be attributed to the wrong request

`stage::run()` shares a process-wide OpenCL context. A queue submission resets
a refusal field on the shared residency object. On synchronous refusal the
fallback lambda runs the CPU implementation before reading that field into
its result. If a second submission resets or overwrites it during CPU
fallback, the first request can lose a `Memory` refusal or report the second
request's category. The individual accesses are locked. The lifetime of the
value still spans requests. [Shared context](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_runtime.cc#L219-L236),
[category selection](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L22171-L22188),
[fallback and reset](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_opencl_device.cc#L22274-L22288).

Carry the refusal on the request or launch result from the point of failure.
Pause a memory-refused request in CPU fallback while a second request
submits on the same context. Check both reported categories independently
and compare each output with its CPU reference.

### P2 — The buffer plan treats ordering edges as image bindings

`populateInputs()` copies every graph predecessor into the descriptor.
`buildStagePlan()` then copies those predecessors into its buffer requirements
and gives each non-consumer its own output ID. In the preview graph that
makes `analysis` the image input to `transform`. Analysis is an effect folded
into `raw.base`; the transform actually takes the coordinator's RGB image.
The projection is currently descriptive. Driving allocation or residency
directly from it would bind the wrong producer. [Input projection](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_pipeline.cc#L101-L114),
[buffer projection](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_plan.cc#L152-L177),
[analysis contract and fold](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_contract.cc#L123-L150),
[preview order](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/stage_contract.cc#L352-L362),
[transform call site](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/improccoordinator.cc#L2360-L2384).

Specify pixel inputs, outputs and in-place aliases separately from ordering
and effects. Assert the preview transform takes the RGB base, including
any path-owned spot correction, with analysis represented as an effect.
Pin crop and export bindings independently and rerun cross-path parity
before switching allocation to the plan.

### P2 — Hosted CI omits the planner and handler regressions

The hosted CPU and OpenCL jobs compile and execute an explicit list that
omits `stage-plan-test` and `stage-handler-test`. Both run in the full local
suite. A PR can pass hosted CI while breaking the unified plan's
merged-order check or the registry's malformed-entry checks. Add both
host-only targets to the workflow matrix and verify both appear as passes
in each job's summary. Add the repeated-restore check to the hosted gate
when the cache ownership fix lands. [Hosted test list](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/.github/workflows/perf-graph.yml#L80-L92),
[local gate](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/tools/perf/run-suite.sh#L92-L109),
[merged-order regression](https://github.com/jeyoung/RawTherapee/blob/8a61197d4585e3829043cab5942e254be38cc81e/rtengine/stage/tests/stage_plan_test.cc#L405-L461).

## Validation

- PASS — `stage-graph-test`, `stage-pipeline-test` and
  `stage-handler-test`, built from the pinned source with GCC C++17,
  `-O2 -Wall -Wextra -Werror`.
- REPRODUCED — The pinned preview-base-cache test passed against the local
  CPU build's support library. The probe adds
  `fill(destination, 99.0f)` immediately before the first successful
  restore. The first hit returned the stored RGB values (10, 11, 12).
  The second reported a hit and returned the old destination values
  (99, 100, 101) exactly.
  The pinned test and cache header were compiled with GCC C++17. The
  existing support library was reused for unchanged image and store code.
- REPRODUCED — A second scratch probe used an empty temporary disk store
  and wrote the fixture at one source generation. After preprocess
  invalidation, a lookup with the next generation reported a hit and
  returned the earlier generation's fixture pixels.
- Source review covers the wavelet leak, device reservation lifetime,
  concurrent refusal attribution and buffer bindings. Application builds,
  GUI reproduction, leak instrumentation, POCL/GPU tests and new parity
  measurements were outside this documentation review. The branch's
  existing results are recorded in [WORKSTREAM.md](WORKSTREAM.md).
