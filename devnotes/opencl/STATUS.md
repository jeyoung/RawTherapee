# OpenCL acceleration — status

## Handover block

- Branch: `opencl` (based on `dev` at `b2ee504c4`, 32 commits after tag 5.13)
- Milestone: M3 — transform (complete); next M4 — capture sharpening
- Last verified: 2026-10-20, downscale and upscale parity on POCL and AMD
- Next task: M4 — capture sharpening
- Known limitations: single-stage acceleration is roughly neutral end-to-end;
  transform is limited to rotation/scale/crop without LCP, CA, distortion,
  perspective or vignetting, and falls back to CPU for those

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

- Fresh POCL cache: `--gpu-test` reports `(compiled)`; cache file created.
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
- `--gpu-test` on POCL: PASS.
- `--gpu-test` on AMD Radeon RX 550 (Rusticl): PASS.
- Cross-build CPU vs OpenCL `--gpu=off`, single-threaded:
  `colorspace_flowers.pef`, PAE 0.
- `verify.sh --quick`: SKIP on the CPU build, PASS on POCL and Rusticl.
- Multi-threaded CPU output is not bit-reproducible (up to 5 LSB); see
  `PARITY.md`.

## Runtime notes

- On AMD/Mesa, set `RUSTICL_ENABLE=radeonsi` before starting RawTherapee.
- POCL and Rusticl installed together can crash inside the ICD loader. The
  backend hides POCL when a loadable GPU ICD is present. If the GPU runtime
  reports no device, POCL is also hidden; select it explicitly with
  `OCL_ICD_VENDORS` pointing at a directory containing only `pocl.icd`.
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
| M4 | pending | capture sharpening |
| M6 | pending | portable package, CI, docs |

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
