# OpenCL acceleration — status

## Handover block

- Branch: `perf` (based on `opencl`, which is based on `dev` at `b2ee504c4`)
- Milestone: M12 — opt-in GPU `rgbProc` core complete; GPU porting paused
- Last verified: 2026-10-21, six-format export, monitor parity and opt-in
  `rgbProc` parity on AMD RX 550
- Next task: none scheduled. RCD demosaic was evaluated and deferred; the next
  candidate, only if requested, is wavelet/denoise
- Known limitations: single-stage acceleration is roughly neutral end-to-end;
  GPU Capture Sharpening is opt-in — the 5×5 path is now faster than the CPU,
  while larger radii remain slower on the RX 550; transform is limited to
  rotation/scale/crop without LCP, CA, distortion,
  perspective or vignetting; capture sharpening is split into watchdog-safe
  chunks and sized by a work budget; output conversion is limited to
  `RTv4_sRGB`, relative colorimetric intent and integer output; monitor
  conversion falls back to lcms for LUT profiles, soft proofing, gamut check,
  non-relative intent, and black-point compensation with a non-zero black
  point; `rgbProc` is opt-in, batch-only because of the tone-curve histogram,
  and slower than the 8-thread CPU on the RX 550

## M12 evidence — opt-in GPU `rgbProc` core

- Scope: channel mixer, highlight tone curve, shadow tone curve, `clampOOG`,
  standard tone curve, normal RGB curves, and RGB→Lab conversion.
- The stage is opt-in with `RT_OPENCL_RGBPROC=1`. Without it, `rgbProc` stays
  on the CPU.
- The fast path requires: no pipette, no B&W, no HSV equaliser, no film
  simulation, no colour toning, no tone equaliser, no custom tone curves,
  saturation 0, no RGB luminosity mode, not ProPhoto, `ClampOOG=true`, no
  tone-curve histogram, and a DCP whose step 2 is identity.
- `verify.sh --rgbproc` asserts that `opencl rgbproc:` actually runs and
  compares neutral and exposure/highlight/black profiles. Both PASS at 1 LSB.
- `RT_OPENCL_RGBPROC=1 verify.sh --formats` PASS on all six manifest entries
  at 1 LSB.
- Performance, 8 threads: 10 MP stage is 319 ms GPU versus 80 ms CPU; 45 MP
  stage is 471 ms versus 385 ms. Total GPU wall time with `rgbProc` is
  2.71–2.80 s versus 2.48 s for output conversion alone at 10 MP, and
  7.14–7.18 s versus 6.5 s at 45 MP. The stage is therefore not enabled by
  default on this hardware.
- Preview does not use the stage because the preview passes a tone-curve
  histogram; the GPU path is batch/thumbnail only for now.
- `WITH_OPENCL=ON` and `WITH_OPENCL=OFF` both build.

## Deferred — RCD GPU demosaic

An RCD full-image pass prototype was implemented and reverted before commit.

- 10 MP GPU stage: 577 ms; CPU RCD single-thread: 202 ms. The 8-thread CPU is
  faster still.
- Parity against the CPU was not achieved (maximum error about 17,972 LSB), due
  to RCD's tile, half-resolution P/Q indexing and border semantics.
- Even with correct parity the stage would not beat the CPU on the RX 550.
- AMaZE is not attempted for the same performance reason.

## M11 evidence — shared Lab→RGB segment

- One `labToRgb()` helper and one kernel source serve 16-bit output and 8-bit
  monitor output. The 16-bit path keeps the profile-exact `RTv4_sRGB` maths.
- The monitor path is used for matrix-shaper profiles with relative
  colorimetric intent, no soft proof or gamut check, and either BPC off or a
  zero destination black point. LUT profiles and unsafe cases keep lcms.
- The matrix and inverse TRC are extracted from the active profile through
  the shared `buildGpuProfile()` helper, then cached per profile/settings key.
  No profile colour values are compiled into the binary. The inverse table is
  built at 16-bit precision and costs about 65 ms on a profile change.
- 1024×768 random full-range Lab, 8 threads: GPU 12 ms versus lcms 26–46 ms.
  Maximum difference 1 byte on `RTv4_sRGB`, `RTv2_sRGB`, system `sRGB`,
  `sRGB2014`, and `RTv4_Medium` (BPC on and off).
- No monitor profile keeps the manual CPU path; no GPU round trip is attempted.
- Monitor parity is a committed self-test: `rawtherapee-cli
  --gpu-test-monitor` compares the GPU monitor output with the lcms CPU path
  and reports the maximum byte difference. `verify.sh --quick` includes it,
  and `verify.sh --monitor` runs it alone; both report PASS at 1 byte.
- `--quick`, `--formats` and the resize/rotate/capture stage tests PASS at
  1 LSB. `WITH_OPENCL=ON` and `WITH_OPENCL=OFF` both build.
- The GUI preview harness was not run in this environment because Xvfb is not
  installed; the committed monitor self-test covers the conversion itself.

## M10 evidence — sRGB output conversion

- Scope: `ImProcFunctions::lab2rgbOut` for the shipped `RTv4_sRGB` profile
  with relative colorimetric intent and integer output. Other profiles and
  float output keep the lcms path.
- The kernel receives the matrix and inverse tone curve extracted at runtime
  from the actual `rtdata/iccprofiles/output/RTv4_sRGB.icc`; no colour values
  are hardcoded. Using generic sRGB constants instead differed from lcms by
  up to 26 LSB; the profile-exact values reduce the error to 1 LSB.
- 10 MP PEF, `OMP_NUM_THREADS=8`, 16-bit TIFF: CPU 2.73–2.84 s versus GPU
  2.48–2.49 s; stage 129–132 ms.
- 45.7 MP NEF, `OMP_NUM_THREADS=8`, 16-bit TIFF: CPU 8.09–8.13 s versus GPU
  6.50–6.58 s; stage 431–449 ms.
- `--formats` PASS on all six manifest entries with maximum PAE 1 LSB.
- 32-bit float TIFF keeps the lcms path; `--gpu=auto` and `--gpu=off` pixel
  data are identical.
- `WITH_OPENCL=ON` and `WITH_OPENCL=OFF` both build.

## M9 evidence — preview latency instrumentation

- `RT_PREVIEW_PROFILE=1` emits wall time and the `todo` mask after each actual
  preview render, excluding rejected stale requests.
- A Tracy zone covers `preview::base_image`.
- Baseline procedure and results are in `devnotes/perf/PREVIEW.md`.
  Interactive host run: fit Exposure p50 252 ms / p95 274 ms; 100%
  Shadows/Highlights p50 243 ms / p95 255 ms; 100% Exposure p50 335 ms /
  p95 373 ms. Targets (100 ms fit, 250 ms at 100%) are not met; progressive
  preview is the next increment.

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
- GPU capture is opt-in as of 2026-10-22 (`--gpu-capture=on` or Preferences).
  Default auto mode keeps capture on the CPU. The 5×5 path is now faster than
  the CPU after specialisation; the 7×7, 9×9 and 13×13 paths remain slower.
- PEF 10.2 MP, `capture.pp3`, OMP=8: CPU total 3.13 s; GPU default 2.88 s with
  no capture stage; GPU with `--gpu-capture=on` 2.88 s and a 0.25 s capture
  stage. Parity at OMP=1 is 1 LSB. The 5×5 path is specialised; other radii
  still use the generic kernel and remain slower than the CPU.
- PEF 10 MP: PASS, PAE 1 LSB on AMD RX 550 and POCL.
- 0.75, 1.5 and 2.0 radii, with and without early stop: PASS, PAE 1 LSB.
- 45.7 MP NEF: PASS on Rusticl RX 550 after the 12 MP rejection was removed;
  capture is split into adaptive 16-row (or smaller) launches and each launch
  is finished before the next. After the 5×5 specialisation the opt-in stage
  takes 1.71 s at OMP=8 (17.98 s total). The earlier automatic-selection result
  was 4.77 s capture and 36.31 s GPU versus 36.69 s CPU; PAE 1 LSB.
- Kernel uses 1024 work-items and ~35 KiB local memory; devices below that
  fall back. Work per chunk is bounded by `chunkBudget` so high radius/iteration
  settings can fall back to CPU instead of tripping the watchdog.
- POCL remains a functional test target: `RT_OPENCL_ALLOW_CPU=1` permits a CPU
  OpenCL device when no GPU is selected; with both present, select POCL
  explicitly or use `OCL_ICD_VENDORS`. It is intentionally not selected in auto
  mode because the native CPU path is faster (45.7 MP: 16.75 s capture /
  50.76 s total).
- Historical PEF 10 MP generic-kernel stage: 1.15 s; PAE 1 LSB.
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
| M10 | complete | sRGB output conversion with profile-exact parity |
| M11 | complete | shared Lab→RGB segment for output and monitor |
| M12 | complete | opt-in GPU `rgbProc` core; slower than CPU on RX 550 |

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
