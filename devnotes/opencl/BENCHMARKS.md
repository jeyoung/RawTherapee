# OpenCL benchmarks

Measurements are indicative; record the command and hardware with every
result. Use `OMP_NUM_THREADS=1` for parity runs because the multi-threaded
CPU path is not reproducible.

## Hardware

| Component | Value |
|---|---|
| CPU | Intel Core i7-3770, 4 cores / 8 threads |
| GPU | AMD Radeon RX 550 / 550 Series (Polaris 12), 4 GB |
| Driver | Mesa 26.1.6, Rusticl, `RUSTICL_ENABLE=radeonsi` |
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

The GPU path is about 2% faster end-to-end. Larger images fall back to CPU at
12 MP because the 1024-work-item, 35 KiB-local-memory tile kernel can exceed
the AMD watchdog on older cards.
