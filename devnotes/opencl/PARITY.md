# Parity results

All comparisons use 16-bit TIFF output and `OMP_NUM_THREADS=1`, because the
multi-threaded CPU path is itself not bit-reproducible (observed up to 5 LSB
difference between two identical runs). The gate is the peak absolute error
(PAE) in normalised units; 1 LSB at 16-bit is 1/65535 = 1.53e-5.

| Date | Commit | Device | Image/profile | Mode | PAE | Result |
|---|---|---|---|---|---|---|
| 2026-10-20 | 300a28c16 | CPU (stub) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | 300a28c16 | POCL (CPU) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | 300a28c16 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | 7d767b314 | POCL (CPU) | colorspace_flowers.pef / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | 7d767b314 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | 7d767b314 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / resize 1.5x | off vs auto | 0 | PASS |
| 2026-10-20 | f6cc5e236 | POCL | colorspace_flowers.pef / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | colorspace_flowers.pef / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | colorspace_flowers.pef / rotate standard | off vs auto | 0 | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | colorspace_flowers.pef / rotate log + 0.8 scale | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | 45.7 MP NEF / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | 45.7 MP NEF / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | c652e3505 | CPU build vs OpenCL `--gpu=off` | colorspace_flowers.pef / resize 0.5x | cross-build | 0 | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 0.75, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | uncommitted | POCL (`RT_OPENCL_ALLOW_CPU=1`) | colorspace_flowers.pef / capture 0.75, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 1.5, 10 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 2.0, 10 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 0.75, no early stop | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | 45.7 MP NEF / capture 0.62, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | POCL (`RT_OPENCL_ALLOW_CPU=1`) | 45.7 MP NEF / capture 0.62, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | colorspace_flowers.pef / neutral | off vs auto | 1 LSB | PASS |
| 2026-10-22 | f416c9fcb | AMD RX 550 | colorspace_flowers.pef / capture 3x3-13x13 | off vs forced GPU | 1 LSB | PASS |
| 2026-10-22 | uncommitted | AMD RX 550 | colorspace_flowers.pef / capture Auto | off vs auto | 1 LSB | PASS |
| 2026-10-22 | uncommitted | AMD RX 550 | 45.7 MP NEF / capture 0.75, 20 it | off vs forced GPU and Auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | 45.7 MP NEF / neutral | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | random Lab / RTv4_sRGB, RTv2_sRGB, sRGB, sRGB2014, RTv4_Medium monitor | GPU vs lcms | 1 byte | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | colorspace_flowers.pef / neutral, `RT_OPENCL_RGBPROC=1` | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | exposure/highlight/black profile, `RT_OPENCL_RGBPROC=1` | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | public PEF baseline, `RT_OPENCL_RGBPROC=1` | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | colorspace_flowers.pef / neutral (`--quick`) | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | colorspace_flowers.pef / bayer14 (`--formats`) | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | 45.7 MP NEF / resize 0.5x | off vs auto | 1 LSB | PASS |
| 2026-10-24 | 47c50ee77 | AMD RX 550 | 45.7 MP NEF / sidecar (crop + resize) | off vs auto | 1 LSB | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / `lanczos3` node via `stage::run` | device vs host, `StageExecution` | 0 | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / resize.pp3, 16-bit TIFF, `--gpu=off` | switch off vs on (host policy) | AE=0 | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / preview sweep, 16 states | switch off vs on | 0 (32 images AE=0) | PASS |
| 2026-10-27 | 8141ddf8d | POCL (CPU) | colorspace_flowers.pef / crop harness, 3 states + sequences | switch off vs on | AE=0 (30 PPMs) | PASS |

`47c50ee77` changes only CPU export buffer handling; the OpenCL rows above
re-run the existing GPU stages against the new CPU neighbour.
`verify.sh --quick` also re-ran the monitor self-test: max 1 byte, 14 of
196608 samples. The exact CPU export parity for that commit (neutral, sidecar,
sidecar `-f`, both `WorkingTRC` profiles, `AE=0`) is recorded in
`devnotes/perf/FUSION.md`.

`verify.sh` now reports `SKIP` instead of `PASS` when no OpenCL device is
selected, so a CPU-only build cannot produce a vacuous parity claim.

Cross-build check: `WITH_OPENCL=OFF` and `WITH_OPENCL=ON --gpu=off` produced
identical pixels (PAE 0) for the baseline image with `OMP_NUM_THREADS=1`.

Notes:

- `--gpu=off` and `--gpu=auto` on the stub build produce identical pixels.
- Multi-threaded runs of the same binary differ by up to 5 LSB, so CPU
  regression tests must either pin `OMP_NUM_THREADS=1` or allow that
  tolerance. This is a pre-existing RawTherapee characteristic, not an OpenCL
  effect.
- The TIFF files still differ byte-wise because of embedded timestamps; do not
  use SHA-256 of the file as a gate.
- Output conversion is compared only for the shipped `RTv4_sRGB` profile and
  8/16-bit integer output. Other profiles and float output continue to use
  lcms and are pixel-identical between `--gpu=off` and `--gpu=auto`.
- `compare_tiff.py` now normalises through ImageMagick to uncompressed 16-bit
  TIFF and reads the encoded samples. The previous ImageMagick `rgb:` path
  linearised the samples, which amplified a 1 LSB difference into a 26 LSB
  PAE.
- Monitor output is 8-bit, so its parity gate is 1 byte. `verify.sh --quick`
  and `verify.sh --monitor` run `rawtherapee-cli --gpu-test-monitor`, which
  compares the GPU monitor conversion with lcms on a deterministic 256×256
  full-range Lab image.
- `verify.sh --rgbproc` sets `RT_OPENCL_RGBPROC=1`, asserts that
  `opencl rgbproc:` ran, and compares neutral and exposure profiles. The
  stage is batch-only because preview supplies a tone-curve histogram.

## Stage device residency

The graph device path runs the `lanczos3` resize node. Device runs use
`RT_OPENCL_ALLOW_CPU=1` and an ICD directory that contains only `pocl.icd`
(`OCL_ICD_VENDORS`), so the selected device is POCL's CPU device
(`cpu-ivybridge-Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz`). These are
CPU-device results and must not be read as GPU parity. Restricting the ICD set
pins the device identity; it is no longer needed to avoid the rusticl crash
described under "Broken and unsupported ICDs", because a broken ICD is skipped.

The parity gate is 1 LSB at 16-bit (1/65535 = 1.53e-5 normalised). The measured
peak absolute error is zero:

- The node itself, through the same `stage::run()` entry point that
  `ImProcFunctions::Lanczos` uses, on a synthetic 128x96 three-plane F32 image
  at scales 0.5 and 1.5, differs from the host implementation in 0 of 9216 and
  0 of 82944 samples. `StageContext::execute()` reports `StageExecution`
  `device=OPENCL`, `asynchronous=true`, so the parity is attributed to an
  executed device kernel rather than to the selection. If the queue had fallen
  back to the host it would report `device=CPU` and the test would fail.
- An over-budget node runs on the host: `StageExecution` reports `device=CPU`,
  `asynchronous=false`, and its output is exact against the CPU reference.
- A 16-bit TIFF export with the resize profile is identical with the graph
  switch off and on (`AE=0`, PAE 0 of 7604928 samples, same stage output hash).
  With `--gpu=off` the batch policy now disables the graph device, so both runs
  record the host implementation; that run is CPU-path regression evidence, not
  device evidence. With `--gpu=auto` the classic `opencl::resizeLanczos` handles
  the node and no stage `lanczos3` record is emitted, also `AE=0`.
- The preview sweep (16 states, 32 images) and the crop harness (30 PPM files)
  are identical with the switch off and on, but neither graph runs a stage-ABI
  node; they are CPU-path regression checks with zero device calls.

Kill switch: `--gpu=off` with `RT_STAGE_OPENCL=1` records `impl=module` (the
host) and not `impl=opencl`. Before the batch-path wiring it recorded
`impl=opencl`, which was the defect this record corrects.

Timing on POCL CPU, `OMP_NUM_THREADS=1`, same image and profile, interleaved
18 samples per side: the preview sweep is 327.67 ms mean (326.99 median) at
the parent commit `9e8df3493`, 331.48 (328.69) with the switch off and 328.99
(324.72) with it on. The spread is run-to-run noise on this host, whose
samples ranged from 319 to 363 ms. No performance claim is made for the device
node on a CPU OpenCL device: it is slower than the host and exists for parity
and correctness.

## Broken and unsupported ICDs

Device selection must survive whatever the ICD set contains. The stage device
enumerates the platforms the ICD loader reports and validates each one before it
dispatches to it.

Failure found on 2026-10-27 at `3339afc62`: with the distribution vendor set
(`mesa.icd`, `pocl.icd`, `rusticl.icd` in `/etc/OpenCL/vendors`)
`stage-device-test` and `stage-residency-test` died with SIGSEGV inside the ICD
loader, while an `OCL_ICD_VENDORS` directory holding only `pocl.icd` worked. The
fault is the rusticl platform's ICD dispatch chain: ocl-icd 2.3.3 tail-jumps
through a null entry while answering the first platform query. A 20-line C
program calling `clGetPlatformInfo` on the second platform, and `clinfo`,
crash identically, so this is an ICD defect and not a RawTherapee or merge
defect. It is Debian bug #1117930 (`mesa-opencl-icd`, rusticl dispatch table not
initialised). `clGetPlatformIDs` still reports both platforms without an error
and the crash happens before a CL error code exists, so a platform cannot be
validated in-process.

The trigger is order dependent: rusticl second, after pocl, is broken, while
rusticl first is fine for the same pair (`mesa.icd` before `rusticl.icd` is also
fine). The distribution file order (`pocl.icd`, then `rusticl.icd`) is the
broken one, which is why the ambient set reproduced it.

Fix: `probe_platforms()` in `stage_opencl_device.cc` asks every platform the
questions the selector will ask (platform name, device ids, first device name,
type and global memory) in a forked child, one child per platform, with
`RLIMIT_CORE=0` and a 30 s alarm. The parent dispatches only to platforms whose
child answered; a platform whose child dies or times out is skipped, with a
diagnostic such as:

    no GPU OpenCL device; set RT_OPENCL_ALLOW_CPU=1 to use a CPU device;
    1 of 2 OpenCL platform(s) skipped: platform 1 crashed the ICD query;
    check the installed ICDs (/etc/OpenCL/vendors, or OCL_ICD_VENDORS)

and the node falls back to the host. `StageOpenCLDeviceInfo::note` carries the
same text when a device was still selected, so a run that loses a vendor is
visible without failing. One child per platform is deliberate: a broken
platform does not hide the platforms behind it. The probe is a one-off cost of
about 30 to 40 ms per process on this host, measured on the two device tests.

Limitations:

- rusticl is unsupported on this host: its platform kills the query when it is
  loaded after pocl, so its devices are unusable here even though the ICD is
  installed. Only POCL is selected. The probe cannot say which vendor is at
  fault beyond reporting that the platform at that index crashed.
- The probe covers enumeration only. A vendor that fails or crashes later, in
  context, queue, kernel or a transfer, is still reported through the existing
  fallback paths; the crash path is not additionally protected.
- The probe needs `fork()`, so it runs on POSIX systems only. Elsewhere the
  platforms are queried directly as before, because the defect is specific to
  the ocl-icd loader. If `fork()` or `pipe()` fails the affected platforms are
  skipped and the device path stays on the host rather than querying an
  unvalidated platform. Forking a process that other threads are using can
  inherit a loader or allocator lock; that only delays the child, which the
  alarm then kills, so the platform is skipped instead of hanging RawTherapee.

Evidence, 2026-10-27, `build/opencl-perf-graph-final`, GCC, ambient ICD set
unless stated. "Ambient" is the `/etc/OpenCL/vendors` set, `OCL_ICD_VENDORS`
unset.

| Check | Result |
|---|---|
| `stage-device-test`, ambient ICD set | PASS, exit 0; host fallback with the diagnostic above, no crash |
| `stage-residency-test`, ambient ICD set | PASS, exit 0, 5 skipped; no crash |
| `stage-residency-test`, ambient ICD set, `RT_OPENCL_ALLOW_CPU=1` | PASS, 0 skipped; POCL CPU device, platform 1 skipped; PAE 0.000000 LSB16 at scales 0.5 and 1.5 (0 of 9216 and 0 of 82944 samples) and 0 of 4608 through `run()`; `StageExecution` reports `device=OPENCL`, `asynchronous=true` |
| `stage-residency-test`, `OCL_ICD_VENDORS` = `pocl.icd` only, `RT_OPENCL_ALLOW_CPU=1 RT_REQUIRE_GPU=1`, `OMP_NUM_THREADS=1` | PASS, 0 skipped, no platform skipped; PAE 0.000000 LSB16 with the same sample counts. This is the device-parity row |
| `stage-residency-test`, `OCL_ICD_VENDORS` with `rusticl.icd` before `pocl.icd` | PASS, 0 skipped; no platform skipped and POCL selected, so the order dependence is covered from both sides |
| `OCL_ICD_VENDORS` = empty directory, `RT_REQUIRE_GPU=1` | FAIL, exit 1, "no OpenCL platform" and the host fallback; a missing device still cannot pass as a skip |
| CPU build (`WITH_OPENCL=OFF`) and OpenCL build, 16 stage test executables plus `editorrequest-test` | both compile without warnings, 17 of 17 executables PASS in both trees (34 runs) |
| kill switch, `--gpu=off` with `RT_STAGE_OPENCL=1`, resize profile | the stage profile records one `lanczos3` node with `impl=module`; no `impl=opencl` record |
