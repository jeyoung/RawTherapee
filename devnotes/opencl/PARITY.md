# Parity results

All comparisons use 16-bit TIFF output and `OMP_NUM_THREADS=1`, because the
multi-threaded CPU path is itself not bit-reproducible (observed up to 5 LSB
difference between two identical runs). The gate is the peak absolute error
(PAE) in normalised units; 1 LSB at 16-bit is 1/65535 = 1.53e-5.

| Date | Commit | Device | Image/profile | Mode | PAE | Result |
|---|---|---|---|---|---|---|
| 2026-10-20 | faab74912 | CPU (stub) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | faab74912 | POCL (CPU) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |
| 2026-10-20 | faab74912 | AMD RX 550 (Rusticl) | colorspace_flowers.pef / neutral | off vs auto | 0 | PASS |

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
