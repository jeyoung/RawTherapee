# OpenCL decisions

Short architecture decision records. Append new decisions; do not rewrite
history.

## D001 — Single binary with a runtime toggle

Date: 2026-10-19

Decision: ship one binary built with `WITH_OPENCL=ON`; the user enables or
disables acceleration in Preferences. `WITH_OPENCL=OFF` remains available for
baseline builds and CI.

Alternatives: a second `rawtherapee-opencl` executable, or two packages. Both
add packaging and maintenance cost without adding capability.

## D002 — Runtime loading, no link dependency

Date: 2026-10-19

Decision: open the ICD loader with `Glib::Module`, resolve entry points, and
fall back to the CPU when anything is missing.

Consequences: no `libOpenCL` dependency in packages; the application starts on
systems without OpenCL; a missing device is a normal condition, not an error.

## D003 — OpenCL C 1.2

Date: 2026-10-19

Decision: kernels target OpenCL 1.2. Avoid vendor extensions and 2.0+ features.

Consequences: broad device coverage, including Mesa Clover/Rusticl, POCL and
older NVIDIA drivers. Some optimisations (subgroups, fine-grain SVM) are not
available.

## D004 — Stateless stage adapters in v1

Date: 2026-10-19

Decision: each accelerated stage uploads its inputs, runs, and downloads its
output. No device-resident pipeline in v1.

Consequences: simple correctness and fallback semantics. Transfer overhead is
accepted for the first stages because they are compute-heavy. Residency is a
later optimisation, only if profiling justifies it.

## D005 — CPU build must remain byte-identical

Date: 2026-10-19

Decision: the CPU path and `--gpu=off` must produce byte-identical output to a
`WITH_OPENCL=OFF` build of the same commit.

Consequences: no refactoring of CPU algorithms while porting them; GPU paths
are additive and gated by tests.

## D006 — Cache compiled program binaries

Date: 2026-10-20

Decision: cache OpenCL program binaries under the configured cache directory,
keyed by kernel source, build options, device name, driver and device version.
A corrupt or unreadable cache is ignored and the program is compiled again.

Consequences: first use on a machine or after a driver update pays the
compilation cost; later runs are fast. The cache is an optimisation only and
must never fail the application.

## D007 — Pool device buffers per size

Date: 2026-10-20

Decision: keep a small per-device pool of `cl_mem` buffers keyed by byte size,
reused across stage calls. The pool is capped at 512 MiB or one eighth of
device memory, and is cleared when it would push a stage over the 80 % budget.

Consequences: fewer allocation calls and less fragmentation, without hiding
device memory from the stage budget. The pool is not a substitute for keeping
frames resident between stages.

## D008 — Detect the Rusticl driver at runtime

Date: 2026-10-20

Decision: before loading the ICD on Linux, set `RUSTICL_ENABLE` from the GPU
vendor when the environment does not already define it. AMD systems use
`radeonsi`, Intel systems use `iris`, and hybrid systems use
`radeonsi,iris`. A new Preferences field stores an explicit driver override;
empty means auto-detection.

Consequences: installing the package is enough for Mesa Rusticl to expose the
GPU; no per-user shell or desktop-file environment is needed. An existing
`RUSTICL_ENABLE`, for example one used for a POCL comparison, still wins. The
setting is Linux-specific because it relies on `/sys/class/drm`. Mesa 23.1
or newer is needed for Rusticl; Debian 12's Mesa 22.3 uses Clover instead.

## D009 — Chunk GPU capture sharpening and skip CPU OpenCL devices

Date: 2026-10-20

Decision: launch capture-sharpening tiles in work-budgeted row chunks and finish
each chunk before starting the next. Remove the old 12 MP rejection. Auto mode
selects GPU devices; a CPU OpenCL device is used only when explicitly selected
or when `RT_OPENCL_ALLOW_CPU=1` is set for testing.

Consequences: 45.7 MP capture runs on the RX 550 without a context-loss reset.
High radius/iteration settings that exceed the chunk budget fall back to CPU
instead of risking a watchdog. POCL remains available for parity tests but does
not slow normal exports on POCL-only systems.

## D010 — Drop stale queued previews before rendering

Date: 2026-10-20

Decision: attach a generation counter to preview requests and discard a queued
request when a newer one has arrived before rendering starts. Requeue the full
change mask, adding `M_MONITOR` for panning changes so the final preview is
still produced.

Consequences: obsolete queued renders are avoided without aborting an active
render or changing ownership of intermediate buffers. Mid-render cancellation
was rejected during review because it interacted badly with preview teardown.
A stage cache and measured latency harness are the next preview steps.

## D011 — Defer the base-image cache

Date: 2026-10-20

Decision: do not land the first preview base-image cache. The base stage only
runs for `M_INIT`, `M_LINDENOISE` and `M_HDR`, so ordinary RGB/Lab slider edits
never read the cache. The review also found that the saved image was taken
after colour conversion, which would double-apply that conversion on a hit.

A useful next cache must sit at a stage that is actually recomputed for slider
edits, most likely around the Lab processing boundary, and must be keyed by the
RGB/Lab input flags. Keep preview output byte-identical when it lands.

## D012 — Progressive preview before another cache

Date: 2026-10-20

Decision: the interactive baseline shows p95 latency of 274 ms at fit zoom and
373 ms at 100%, so the next preview optimisation is progressive rendering:
use a reduced-resolution preview while the user is adjusting, then request a
full-resolution render when the interaction settles.

Consequences: the preview cache remains deferred. The final frame must remain
the full-quality render, and crop/detail windows must not show stale geometry.

## D013 — Use profile-exact sRGB maths for output conversion

Date: 2026-10-21

Decision: the GPU output stage is limited to the shipped `RTv4_sRGB` profile
with relative colorimetric intent and integer output. It uses the profile's
exact XYZ matrix and parametric tone curve, not the generic `sRGB_xyz`
constants.

Alternatives: the generic sRGB constants are shorter to write but differ from
lcms by up to 26 LSB at 16-bit. A 3D LUT would cover arbitrary profiles but
needs interpolation-error testing and a larger cache.

Consequences: output conversion is 1 LSB parity on the six-format corpus.
`RTv2_sRGB`, other output profiles and 32-bit float output keep the lcms path.
`NoICMString` is not offloaded because it already uses the faster manual CPU
sRGB path; adding a GPU round trip would be slower. The profile values are
duplicated in `openclengine.cc` and must be updated if the shipped profile
changes.

## D014 — Offload matrix-shaper monitor conversion

Date: 2026-10-21

Decision: `lab2monitorRgb()` uses the GPU when the monitor profile is a
matrix shaper with same R/G/B TRCs, the intent is relative colorimetric, there
is no soft proof or gamut check, and BPC is either off or has a zero
destination black point. LUT-based profiles and unsafe cases keep lcms. The
inverse TRC is built from the lcms estimated table and cached per profile and
settings key.

Alternatives: the manual CPU sRGB path is faster than the GPU for the
no-profile case (about 2 ms versus 12 ms at 1 MP), so it is kept. A generic
sRGB matrix for all monitor profiles differed by up to 2 bytes from lcms, so
the profile data is used instead.

Consequences: matrix-shaper monitor conversion is about 2–3× faster than lcms
at 1 MP (12 ms versus 26–46 ms) with a maximum 1-byte difference. The GPU path
is only built when OpenCL is enabled; thumbnail and CPU-only paths do not pay
the LUT construction cost. The LUT inversion assumes the estimated table is
monotonic and the three TRCs are identical.

## D015 — Keep the GPU `rgbProc` core opt-in

Date: 2026-10-21

Decision: ship the basic GPU `rgbProc` core behind `RT_OPENCL_RGBPROC=1` and
keep the CPU path as the default. The fast path covers channel mixer,
highlight/shadow tone curves, `clampOOG`, standard tone curve, normal RGB
curves and RGB→Lab conversion, and falls back for all other tools.

Alternatives: enabling it by default would add 0.2–0.7 s to the GPU export
path on the RX 550 because the stage is slower than the 8-thread CPU. Dropping
the implementation would lose the groundwork for a future fused, resident
segment on stronger GPUs.

Consequences: parity is 1 LSB on neutral, exposure and all six formats. The
stage is batch-only until the preview tone-curve histogram is handled. The
next decision is whether fusion with the Lab→RGB segment can offset the
standalone stage cost; otherwise the next milestone is the guided-filter
family.

## D016 — Defer RCD and AMaZE GPU demosaic

Date: 2026-10-21

Decision: do not port RCD or AMaZE demosaicing to the GPU on the current
target hardware. The RCD full-image pass prototype was reverted before commit.

Alternatives: a tile-per-workgroup RCD port could fix the indexing and border
issues, but it would still be slower than the 8-thread CPU.

Consequences: 10 MP RCD measured 577 ms on the RX 550 versus 202 ms on one CPU
thread; the multi-threaded CPU is faster still. The prototype also failed
parity (maximum error about 17,972 LSB). GPU work stops at output conversion,
monitor conversion and the opt-in `rgbProc` core. Wavelet/denoise is the only
remaining compute-heavy candidate, and only if requested.
