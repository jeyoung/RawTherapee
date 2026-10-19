# OpenCL acceleration — status

## Handover block

- Branch: `opencl` (based on `dev`)
- Milestone: M1 — program cache (complete); next M2 — Lanczos resize
- Last verified: 2026-10-20, cache hit/miss and invalid-cache recovery
- Next task: M2 — first accelerated stage (Lanczos)
- Known limitations: no stage offload yet; OpenCL only enumerates devices

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
- `verify.sh --quick`: PASS on the CPU build, POCL and Rusticl.
- Multi-threaded CPU output is not bit-reproducible (up to 5 LSB); see
  `PARITY.md`.

## Runtime notes

- On AMD/Mesa, set `RUSTICL_ENABLE=radeonsi` before starting RawTherapee.
- POCL and Rusticl installed together can crash inside the ICD loader. The
  backend hides POCL when a GPU ICD is present; for explicit tests set
  `OCL_ICD_VENDORS` to a directory containing only the wanted `.icd`.
- OpenCL is initialised lazily, so `--gpu=off` and CPU-only use never load a
  vendor driver.

## Scope

Optional OpenCL acceleration for selected RawTherapee stages, with a CPU
fallback and a runtime toggle. The OpenCL build is a single binary; users
choose at runtime whether to use a device.

## Milestones

| M | State | Notes |
|---|---|---|
| M0 | in progress | build helper, CMake option, options/CLI/preferences, stub backend |
| M1 | pending | loader, device discovery, program cache, self-test |
| M2 | pending | Lanczos resize |
| M3 | pending | transform |
| M4 | pending | capture sharpening |
| M6 | pending | portable package, CI, docs |

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
