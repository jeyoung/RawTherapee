# OpenCL acceleration — status

## Handover block

- Branch: `perf` (based on `opencl`, which is based on `dev` at `b2ee504c4`)
- Milestone: M9 — preview latency instrumentation; next preview stage cache
- Last verified: 2026-10-20, downscale and upscale parity on POCL and AMD
- Next task: preview latency benchmark, stage cache and progressive preview
- Known limitations: single-stage acceleration is roughly neutral end-to-end;
  transform is limited to rotation/scale/crop without LCP, CA, distortion,
  perspective or vignetting; capture sharpening is split into watchdog-safe
  chunks and sized by a work budget

## M9 evidence — preview latency instrumentation

- `RT_PREVIEW_PROFILE=1` emits wall time and the `todo` mask after each actual
  preview render, excluding rejected stale requests.
- A Tracy zone covers `preview::base_image`.
- Baseline procedure and results are in `devnotes/perf/PREVIEW.md`.
  Warm fit-zoom Exposure slider: p50 54 ms, p95 152 ms. The 100% row still
  needs a Tone Curve/crop edit to force high-detail processing.

## M8 evidence — preview request coalescing

- Preview changes carry a generation counter, incremented in both
  `startProcessing(int)` and `endUpdateParams(int)`.
- Before a render starts, `updatePreviewImage` discards a stale request and
  requeues its complete mask; panning requests also requeue `M_MONITOR`.
- The crop/post-crop-vignetting transform augmentation is applied before the
  discarding check.
- `WITH_OPENCL=ON` and `WITH_OPENCL=OFF` builds PASS.
- Mid-render cancellation was prototyped but rejected during review because
  it interfered with preview teardown and state ownership. The next increment
  is a stage cache and a measured latency harness.

## M7 evidence — Rusticl auto-detection

- Before loading the ICD, Linux builds read `/sys/class/drm/card*/device/vendor`
  and set `RUSTICL_ENABLE` when the variable is not already present:
  AMD `0x1002` → `radeonsi`, Intel `0x8086` → `iris`.
- Preferences → Performance → OpenCL has a **Rusticl driver** text box. Empty
  means auto; an explicit value overrides detection; an externally set
  `RUSTICL_ENABLE` wins over both.
- `--gpu-info` with no environment variables and POCL installed:
  `AMD Radeon RX 550 ... (OpenCL 3.0)` PASS.
- Explicit `RUSTICL_ENABLE=llvmpipe` is respected PASS.
- Explicit preference `OpenCLRusticlDriver=llvmpipe` is respected PASS.
- CPU-only build (`WITH_OPENCL=OFF`) compiles and runs PASS.
- AMD + Intel detection returns `radeonsi,iris`; Mesa accepts the comma-separated
  list.
- Preferences screenshot under Xvfb shows the Rusticl field with
  `Auto (radeonsi)` and the environment/restart hint.
- Typing `llvmpipe` and pressing OK writes
  `OpenCLRusticlDriver=llvmpipe` to the options file.
- The package recommends `mesa-opencl-icd`, so a normal install has a Rusticl
  ICD on current Debian.

## M6 evidence — portable package

- `tools/build-rt.sh portable` builds a `.deb` in Debian 12 Docker for
  Debian bookworm and newer.
- Installs under `/usr/local`; GTK/gtkmm come from the host, while exiv2 and
  JPEG XL are bundled because their sonames differ on Debian 13.
- Tested on Debian 12 and Debian 13 containers: `apt-get install ./...deb`,
  CPU export, `--list-formats` with JPEG XL, and GUI startup under Xvfb.
- CPU export matches the host single-threaded export (PAE 0).
- No-ICD `--gpu-info` starts and falls back cleanly.
- OpenCL ICD/driver libraries are not bundled; the package suggests common
  vendor ICD packages.

## M5 evidence — buffer pool

- Device buffers are pooled by size and reused across stage calls.
- Pool is capped at 512 MiB or one eighth of device memory, whichever is
  smaller; stale entries are evicted when a stage needs budget.
- Allocation failure clears the pool and retries once, then falls back to CPU.
- PEF resize/rotate/capture parity remains PASS (PAE 0 / 1 LSB).
- Effect is modest: resize 3.98 s CPU vs 4.06 s GPU; rotate 6.20 s vs 6.04 s.

## M4 evidence — capture sharpening

- Scope: `sigmaCornerOffset == 0`, fixed 3/5/7/9/13 kernels, tile size 32,
  per-tile early stop. Other cases fall back to CPU.
- PEF 10 MP: PASS, PAE 1 LSB on AMD RX 550 and POCL.
- 0.75, 1.5 and 2.0 radii, with and without early stop: PASS, PAE 1 LSB.
- 45.7 MP NEF: PASS on Rusticl RX 550 after the 12 MP rejection was removed;
  capture is split into adaptive 16-row (or smaller) launches and each launch
  is finished before the next. Capture 4.77 s; total 36.31 s GPU versus
  36.69 s CPU; PAE 1 LSB. End-to-end remains roughly neutral.
- Kernel uses 1024 work-items and ~35 KiB local memory; devices below that
  fall back. Work per chunk is bounded by `chunkBudget` so high radius/iteration
  settings can fall back to CPU instead of tripping the watchdog.
- POCL remains a functional test target: `RT_OPENCL_ALLOW_CPU=1` permits a CPU
  OpenCL device when no GPU is selected; with both present, select POCL
  explicitly or use `OCL_ICD_VENDORS`. It is intentionally not selected in auto
  mode because the native CPU path is faster (45.7 MP: 16.75 s capture /
  50.76 s total).
- PEF 10 MP: stage 1.15 s; PAE 1 LSB.
- PEF radius 2.0, 100 iterations, early stop off: PASS, 5.73 s, PAE 1 LSB;
  this exercises the one-row chunk limit.
- POCL-only auto mode uses the native CPU path; explicit POCL selection and
  `RT_OPENCL_ALLOW_CPU=1` remain available for tests.

## M3 evidence — transform

- Scope: rotation, common-transform scale and crop offsets, with log or
  standard encoding, cubic and bilinear sampling. LCP, CA, distortion,
  perspective, vignetting, gradient and PC vignette fall back to CPU.
- 5 degree rotation, PEF: POCL and AMD RX 550 PASS, PAE 1 LSB.
- Standard (non-log) profile: PASS, PAE 0.
- Log + 0.8 scale profile: PASS, PAE 1 LSB.
- Nikon D850 NEF, 45.7 MP: rotate PASS, PAE 1 LSB; resize PASS, PAE 0.
- All six local/public format samples load and export; parity PAE 0.
- Coordinate mapping is computed on the host in double and uploaded as a
  float map, so devices without fp64 (Rusticl Polaris) are supported.
- Host map + buffers for 45 MP are checked against the 80% VRAM budget.

## M2 evidence — Lanczos

- `WITH_OPENCL=ON` and `WITH_OPENCL=OFF` both build.
- POCL, 0.5x downscale: `verify.sh --stage resize` PASS, PAE 0.
- AMD RX 550, 0.5x downscale: PASS, PAE 0.
- AMD RX 550, 1.5x upscale: PASS, PAE 0.
- Resize program appears in the program cache; second run uses the cached
  binary.
- Stage timing with `RT_OPENCL_PROFILE=1`: 102 ms for 3896x2616 -> 1944x1304
  on the RX 550. End-to-end export is about 4% slower because only one stage is
  accelerated.
- CPU-only build still builds and runs; `verify.sh --quick` reports SKIP
  because no device is present.

## M1 evidence — program cache

- Fresh POCL cache (`RT_OPENCL_ALLOW_CPU=1`): `--gpu-test` reports
  `(compiled)`; cache file created.
- Second POCL run: reports `(cached)`.
- Fresh AMD RX 550 cache: `(compiled)`, second run `(cached)`.
- Corrupt cache file: ignored, kernel recompiled and cache rewritten.
- Unwritable cache directory (`/proc/...`): kernel still compiles, no crash.
- Cache key includes source, options, device and driver/version, so driver
  updates invalidate it automatically.

## M0 evidence

- `tools/build-rt.sh cpu`: PASS (GCC 14.2.0, Release, generic x86-64).
- `tools/build-rt.sh opencl`: PASS; runtime loader via `Glib::Module`, no
  link dependency.
- `--list-formats`: 35 extensions, matching `rtengine/formats.{h,cc}`.
- `--gpu-info` with no ICD: prints "No OpenCL platform found" and exits 0.
- `--gpu-test` with no ICD: exits 2, no crash.
- `--gpu-test` on POCL with `RT_OPENCL_ALLOW_CPU=1`: PASS.
- `--gpu-test` on AMD Radeon RX 550 (Rusticl): PASS.
- Cross-build CPU vs OpenCL `--gpu=off`, single-threaded:
  `colorspace_flowers.pef`, PAE 0.
- `verify.sh --quick`: SKIP on the CPU build, PASS on POCL with
  `RT_OPENCL_ALLOW_CPU=1`, PASS on Rusticl.
- Multi-threaded CPU output is not bit-reproducible (up to 5 LSB); see
  `PARITY.md`.

## Runtime notes

- On AMD/Mesa, `RUSTICL_ENABLE` is set automatically to `radeonsi` when it is
  not already present; Intel systems get `iris`.
- Preferences → Performance → OpenCL → Rusticl driver overrides the detected
  value. Leave it empty for auto-detection. An existing environment variable
  takes precedence over both.
- POCL and Rusticl installed together can crash inside the ICD loader. The
  backend hides POCL when a loadable GPU ICD is present. If the GPU runtime
  reports no device, POCL is also hidden; select it explicitly with
  `OCL_ICD_VENDORS` pointing at a directory containing only `pocl.icd` and
  `RT_OPENCL_ALLOW_CPU=1`.
- Auto mode ignores CPU OpenCL devices (POCL); use an explicit device selection
  or `RT_OPENCL_ALLOW_CPU=1` for POCL tests.
- OpenCL is initialised lazily, so `--gpu=off` and CPU-only use never load a
  vendor driver.

## Scope

Optional OpenCL acceleration for selected RawTherapee stages, with a CPU
fallback and a runtime toggle. The OpenCL build is a single binary; users
choose at runtime whether to use a device.

## Milestones

| M | State | Notes |
|---|---|---|
| M0 | complete | build helper, CMake option, options/CLI/preferences, runtime loader |
| M1 | complete | device discovery, program cache, self-test |
| M2 | complete | Lanczos resize |
| M3 | complete | rotation/scale/crop transform |
| M4 | complete | capture sharpening, work-chunked at all sizes |
| M6 | complete | portable `/usr/local` `.deb` for Debian 12+ |
| M7 | complete | Rusticl driver auto-detection and preference |
| M8 | incremental | preview request coalescing; cache/benchmark pending |
| M9 | incremental | preview timing instrumentation; baseline pending |

## Review

Two independent reviews are recorded in `REVIEWS.md`. The second pass found
five medium items; all were addressed except the pre-existing cross-thread
`Options` copy race, which is documented there.

## Verification

See `PARITY.md` and `FORMATS.md` once the first stage is ported. Record the
command and the key numbers for every gate; do not record a PASS without
evidence.

## Decisions

See `DECISIONS.md`. The most important ones so far:

- stateless stage adapters in v1 (upload, run, download);
- OpenCL 1.2 kernels only;
- runtime loading via `Glib::Module`, so there is no link dependency;
- one binary with a runtime toggle, no separate OpenCL executable.
