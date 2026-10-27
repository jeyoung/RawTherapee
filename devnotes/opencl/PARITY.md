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
| 2026-10-27 | 3a9d73c5f | POCL (CPU) | colorspace_flowers.pef / resize.pp3, `lanczos3` node | stage switch off vs on | 0 | PASS |
| 2026-10-27 | 3a9d73c5f | POCL (CPU) | colorspace_flowers.pef / resize.pp3, 16-bit TIFF | stage switch off vs on | AE=0 | PASS |
| 2026-10-27 | 3a9d73c5f | POCL (CPU) | colorspace_flowers.pef / preview sweep, 16 states | stage switch off vs on | 0 (32 images AE=0) | PASS |
| 2026-10-27 | 3a9d73c5f | POCL (CPU) | colorspace_flowers.pef / crop harness, 3 states + sequences | stage switch off vs on | AE=0 (30 PPMs) | PASS |

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

`3a9d73c5f` adds the graph device path and wires the `lanczos3` resize node
through it. Device runs use `RT_OPENCL_ALLOW_CPU=1` and an ICD directory that
contains only `pocl.icd` (`OCL_ICD_VENDORS`), so the selected device is POCL's
CPU device (`cpu-ivybridge-Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz`). These are
CPU-device results and must not be read as GPU parity.

The parity gate remains 1 LSB at 16-bit (1/65535 = 1.53e-5 normalised). The
measured peak absolute error is zero in every case:

- The node itself, on a synthetic 128x96 three-plane F32 image at scales 0.5
  and 1.5, differs from the host implementation in 0 of 9216 and 0 of 82944
  samples.
- A 16-bit TIFF export with the resize profile is identical (`AE=0`, PAE 0 of
  7604928 samples, same stage output hash) and the profile record shows the
  node ran with `impl=opencl` when the switch is on and `impl=module` when it
  is off.
- The preview sweep and the crop harness produce identical images with the
  switch on and off, but neither graph runs a stage-ABI node, so those runs
  are CPU-path regression checks with zero device calls, not device parity.

Timing on POCL CPU, `OMP_NUM_THREADS=1`, same image and profile: the resize
node takes 161.6 ms on the device against 84.2 ms on the host. A CPU OpenCL
device is not an acceleration target; the figure records that the device path
works and is not a performance claim. The preview sweep is unaffected by the
switch: 318.89 ms mean with the switch off and 318.70 ms with it on (18
samples per side), against 318.72 ms at the parent commit `9e8df3493`.
