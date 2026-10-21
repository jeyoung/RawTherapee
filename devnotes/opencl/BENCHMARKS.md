# OpenCL benchmarks

Measurements are indicative; record the command and hardware with every
result. Use `OMP_NUM_THREADS=1` for parity runs because the multi-threaded
CPU path is not reproducible.

## Hardware

| Component | Value |
|---|---|
| CPU | Intel Core i7-3770, 4 cores / 8 threads |
| GPU | AMD Radeon RX 550 / 550 Series (Polaris 12), 4 GB |
| Driver | Mesa 26.1.6, Rusticl, auto-detected `RUSTICL_ENABLE=radeonsi` |
| OS | Debian 13 trixie |

## Lanczos resize, 0.5x

Input: `colorspace_flowers.pef`, 3896x2616 -> 1944x1304, 16-bit TIFF.
Command:

```bash
RT_OPENCL_PROFILE=1 OMP_NUM_THREADS=1 rawtherapee-cli \
    --gpu=auto -p devnotes/opencl/pp3/resize.pp3 -t -b16 \
    -o out.tif -c colorspace_flowers.pef
```

| Run | OpenCL resize | Total wall | Peak RSS |
|---|---|---|---|
| CPU (`--gpu=off`) | — | 4.47–4.56 s (median 4.50) | 688 MB |
| GPU, cold cache | 102 ms | 5.21 s | — |
| GPU, warm cache | 102 ms | 4.61–4.72 s (median 4.67) | 697 MB |

Measured with `/usr/bin/time -f '%e %M %P'`, three runs, `OMP_NUM_THREADS=1`.
The GPU total is currently about 4 % slower than the CPU total at this image
size, because the pipeline is dominated by demosaic and colour processing.
The resize stage itself is fast, but transfer and context overhead outweigh
the saved CPU work when only one stage is accelerated.

## Notes

- Cold cache includes compiling and storing the resize program.
- Total time includes loading, demosaic, colour processing, resize and TIFF
  encoding.
- Do not compare timings across different `OMP_NUM_THREADS` settings.

## Transform, 5 degree rotation

PEF, 3896x2616, log transform, three runs:

| Mode | Wall (median) | Peak RSS |
|---|---|---|
| CPU | 11.97 s | 718 MB |
| GPU | 11.61 s | 1.06 GB |

D850 NEF, 8288x5520, log transform:

| Mode | Wall | Peak RSS |
|---|---|---|
| CPU | 46.22 s | 2.22 GB |
| GPU | 47.61 s | 3.55 GB |

Transform stage on PEF is about 1 s; on the 45 MP NEF about 2.4 s. Single-stage
offload remains roughly neutral because map transfer and the extra log image
offset the GPU gain. Reducing map traffic (residency) and accelerating further
stages is required for a clear end-to-end win.

## Capture sharpening, 0.75 radius, 20 iterations

PEF, 3896x2616:

| Mode | Wall | Peak RSS | Stage |
|---|---|---|---|
| CPU | 7.88 s | 730 MB | — |
| GPU | 7.71 s | 942 MB | 1.15 s |

The GPU path is about 2% faster end-to-end. The stage is now split into
watchdog-safe row chunks, so images above 12 MP use the GPU too. On the 45.7 MP
D850 NEF, Rusticl RX 550 capture took 4.77 s; total 36.31 s GPU versus 36.69 s
CPU, with PAE 1 LSB. POCL is test-only and intentionally skipped in auto mode:
the same 45.7 MP case took 16.75 s capture and 50.76 s total on POCL.

## sRGB output conversion

The GPU stage replaces the lcms `Lab -> sRGB` transform for the shipped
`RTv4_sRGB` profile. Profile-exact matrix and tone-curve values are used.
Commands:

```bash
RT_OPENCL_PROFILE=1 OMP_NUM_THREADS=8 rawtherapee-cli \
    --gpu=auto -d -Y -o out.tif -t -b16 -c input
OMP_NUM_THREADS=8 rawtherapee-cli \
    --gpu=off -d -Y -o out.tif -t -b16 -c input
```

| Image | Mode | Wall (median) | Stage |
|---|---|---|---|
| colorspace_flowers.pef, 10 MP | CPU | 2.84 s | — |
| colorspace_flowers.pef, 10 MP | GPU | 2.48 s | 129 ms |
| d850.nef, 45.7 MP | CPU | 8.12 s | — |
| d850.nef, 45.7 MP | GPU | 6.58 s | 439 ms |

Three alternating runs each. The 45 MP GPU total is about 19 % faster. All
six `verify.sh --formats` entries PASS at 1 LSB PAE.

## Monitor conversion

1024×768 random full-range Lab, `OMP_NUM_THREADS=8`, matrix-shaper monitor
profiles. The GPU path uses the profile matrix and inverse TRC; lcms remains
the reference and the fallback.

| Profile | lcms | GPU | Max byte difference |
|---|---:|---:|---:|
| RTv4_sRGB, BPC on | 38.5 ms | 12.0 ms | 1 |
| RTv2_sRGB, BPC on | 38.1 ms | 12.0 ms | 1 |
| system sRGB, BPC on | 26.6 ms | 12.1 ms | 1 |
| sRGB2014, BPC on | 28.0 ms | 11.9 ms | 1 |
| RTv4_Medium, BPC on/off | 39.3 ms | 12.0 ms | 1 |

The profile matrix and TRC setup takes 3–7 ms and is cached per profile and
settings key. `tools/opencl/verify.sh --quick` includes the committed
`--gpu-test-monitor` parity check; `--monitor` runs it alone. The GUI preview
harness could not run because Xvfb is not installed, so interactive preview
latency is not recorded yet.

## Debian package smoke run

`2026-10-04-173409.NEF` (8288x5520) with its `.pp3`, single-threaded,
Debian 12-built package run on the host through a private mount namespace,
with no `RUSTICL_ENABLE` or `OCL_ICD_VENDORS` in the environment:

| Mode | Wall | Peak RSS | Stage |
|---|---|---|---|
| CPU (`--gpu=off`) | 39.05 s | 2.74 GB | — |
| GPU (`--gpu=auto`, Rusticl RX 550, auto-detected) | 37.60 s | 2.74 GB | resize 401 ms |

`compare_tiff.py --bits 16`: maximum absolute error 0, differing samples 0.

