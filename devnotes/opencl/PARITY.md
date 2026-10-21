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
| 2026-10-20 | f6cc5e236 | AMD RX 550 | d850.nef / rotate 5 deg | off vs auto | 1 LSB | PASS |
| 2026-10-20 | f6cc5e236 | AMD RX 550 | d850.nef / resize 0.5x | off vs auto | 0 | PASS |
| 2026-10-20 | c652e3505 | CPU build vs OpenCL `--gpu=off` | colorspace_flowers.pef / resize 0.5x | cross-build | 0 | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 0.75, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | uncommitted | POCL (`RT_OPENCL_ALLOW_CPU=1`) | colorspace_flowers.pef / capture 0.75, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 1.5, 10 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 2.0, 10 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | colorspace_flowers.pef / capture 0.75, no early stop | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | AMD RX 550 | 2026-10-04-173409.NEF / capture 0.62, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-20 | 83919dd98 | POCL (`RT_OPENCL_ALLOW_CPU=1`) | 2026-10-04-173409.NEF / capture 0.62, 20 it | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | colorspace_flowers.pef / neutral | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | d850.nef / neutral | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | jpg, tif, png manifest entries | off vs auto | 1 LSB | PASS |
| 2026-10-21 | uncommitted | AMD RX 550 | random Lab / RTv4_sRGB, RTv2_sRGB, sRGB, sRGB2014, RTv4_Medium monitor | GPU vs lcms | 1 byte | PASS |

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
