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
