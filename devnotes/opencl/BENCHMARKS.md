# OpenCL benchmarks

Measurements are indicative; record the command and hardware with every
result. Use `OMP_NUM_THREADS=1` for parity runs because the multi-threaded
CPU path is not reproducible.

## ROCm on gfx1200 (Radeon RX 9060 XT)

Measured 2026-11-02 on `dev` at `61ac74691`, `WITH_OPENCL=ON`, GCC 14.2.0.

| Component | Value |
|---|---|
| CPU | AMD Ryzen 7 5700X, 8 cores / 16 threads, 62 GB RAM |
| GPU | `gfx1200` (Radeon RX 9060 XT), 16 compute units, 2700 MHz |
| Driver | ROCm 7.2.4, `amdocl` 3581.0, OpenCL 2.0 |
| Device limits | max work-group 256, 64 KiB local memory, 8144 MB reported |
| OS | Debian 13 trixie, kernel 6.12.113 |

Method: one binary, `--gpu=off` versus `--gpu=auto`, `OMP_NUM_THREADS=8`,
16-bit TIFF, six interleaved runs per cell after a discarded warm-up.
Timing outputs were written to tmpfs: the encrypted home volume's writeback
added 2–8 s of variance to a 274 MB TIFF, whereas identical runs on tmpfs
repeated within 1.3 % and most cells spread by less than 0.2 %. Short
10.2 MP runs quantise to about 50 ms, so treat their small deltas as
indicative. Inputs were warmed in the page cache before measuring.
Absolute values are not comparable with the Rusticl figures below because
both the driver and the host load differ.

Medians, with peak RSS:

| Image | Profile | CPU | GPU | Speed-up | CPU RSS | GPU RSS |
|---|---|---:|---:|---:|---:|---:|
| 10.2 MP PEF | neutral (default) | 1.618 s | 1.617 s | 1.00× | 526 MB | 664 MB |
| 10.2 MP PEF | resize 0.5× | 0.665 s | 0.765 s | 0.87× | 487 MB | 542 MB |
| 10.2 MP PEF | rotate 5° | 0.865 s | 0.915 s | 0.95× | 516 MB | 898 MB |
| 10.2 MP PEF | capture 0.75/20 | 0.966 s | 0.990 s | 0.98× | 526 MB | 660 MB |
| 10.2 MP PEF | rgbProc exposure | 0.815 s | 0.815 s | 1.00× | 516 MB | 660 MB |
| 45.7 MP NEF | neutral (default) | 3.876 s | 3.475 s | 1.12× | 2146 MB | 2146 MB |
| 45.7 MP NEF | resize 0.5× | 1.268 s | 1.268 s | 1.00× | 1969 MB | 1969 MB |
| 45.7 MP NEF | rotate 5° | 2.271 s | 2.020 s | 1.12× | 1972 MB | 3328 MB |
| 45.7 MP NEF | capture 0.75/20 | 2.522 s | 2.071 s | 1.22× | 2144 MB | 2144 MB |
| 45.7 MP NEF | rgbProc exposure | 2.020 s | 1.468 s | 1.38× | 1971 MB | 2113 MB |

The GPU arm is neutral at 10.2 MP and pays off at 45.7 MP, where transfer and
setup are amortised. The transform path costs 1.36 GB of extra peak RSS at
45.7 MP, consistent with the log image and transform map being held alongside
the image planes.
The `rgbProc` row is opt-in with `RT_OPENCL_RGBPROC=1` and includes the
accelerated output conversion.

GPU stage medians:

| Stage | 10.2 MP PEF | 45.7 MP NEF |
|---|---:|---:|
| `resize` | 23 ms | 66 ms |
| `transform` | 76 ms | 300 ms |
| `rgbproc` | 35 ms | 110 ms |
| `lab2srgb` (neutral) | 32 ms | 107 ms |
| capture, CPU fallback | 85 ms | 141 ms |

The stage figure covers the OpenCL pass only; the total includes transfers,
buffer allocation and the remaining CPU pipeline.

GPU capture sharpening is unavailable on this driver. The tiled kernel needs
1024 work-items, `gfx1200` on `amdocl` reports a maximum work-group size of
256, and `openclengine` rejects the path when the limit is below 1024 or the
local memory is below 36 KiB. With `--gpu-capture=on`, both images logged only
`opencl capture sharpening cpu`, so both capture arms above run the CPU
deconvolution.

Cold kernel cache: clearing `$RT_CACHE` before each run changed no total by
more than about one 50 ms timing quantum (worst case PEF rotate, 51 ms;
45.7 MP neutral 3.473 s cold versus 3.475 s warm), so `amdocl` kernel
compilation is not a material cost for this pipeline.

Parity: all twelve CPU/GPU pairs match at 1 LSB with
`OMP_NUM_THREADS=1`; see `PARITY.md`. At eight threads the CPU path itself
differs from its single-threaded output by up to 19 LSB for resize and 63 LSB
for `rgbProc`, so only the single-threaded comparison is meaningful.

`verify.sh` could not run on this host: ImageMagick is not installed and the
helper exits when neither `magick` nor `compare` is available. Parity was
measured with the same run-pair method and the repository's TIFF reader
(`compare_tiff.TiffReader`) on uncompressed 16-bit output.

Reproduction:

```bash
OMP_NUM_THREADS=8 rawtherapee-cli --gpu=off -p <profile> -t -b16 -Y \
    -o out.tif -c <image>
```

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

## Capture sharpening (opt-in)

GPU Capture Sharpening has three modes:

- **Off** — always CPU.
- **On** — GPU when the stage is supported.
- **Auto** — compare the real CPU and GPU deconvolution on a 2.3 MP sample,
  require a 10 % GPU win, and cache the decision per radius, iteration count
  and CPU thread count. On is available through `--gpu-capture=on`; Auto uses
  `--gpu-capture=auto`.

Off remains the default because a wrong automatic choice would regress export
time. The first Auto use per workload pays a small calibration cost.

PEF, 3896x2616, 10.2 MP, `capture.pp3`, `OMP_NUM_THREADS=8`, 16-bit TIFF:

```bash
RT_OPENCL_PROFILE=1 OMP_NUM_THREADS=8 rawtherapee-cli \
    --gpu=auto --gpu-capture=on -p devnotes/opencl/pp3/capture.pp3 \
    -t -b16 -Y -o out.tif -c colorspace_flowers.pef
```

| Mode | Wall | Peak RSS | Capture stage |
|---|---|---|---|
| CPU (`--gpu=off`) | 3.13 s | 751 MB | — |
| GPU default | 2.88 s | 841 MB | — |
| GPU forced (`--gpu-capture=on`) | 2.88 s | — | **0.25 s** |

The default GPU path is faster overall because of output conversion. After the
kernel specialisations, forced GPU capture is faster than the CPU for 3×3, 5×5
and 9×9 at 8 threads; 7×7 and 13×13 are still slower. Parity at
`OMP_NUM_THREADS=1` is 1 LSB. The earlier table below was recorded when auto
mode always selected GPU capture; its absolute totals are not comparable with
the current runs.

Capture stage at `OMP_NUM_THREADS=8`, approximate CPU stage derived from
capture-on minus capture-off totals:

| Kernel | GPU stage | CPU stage | Forced winner | Auto at 8 threads |
|---|---:|---:|---|---|
| 3×3 | 0.32 s | 0.46 s | GPU | CPU |
| 5×5 | 0.26 s | 0.31 s | GPU | CPU |
| 7×7 | 0.69 s | 0.54 s | CPU | CPU |
| 9×9 | 0.98 s | 1.20 s | GPU | GPU |
| 13×13 | 1.78 s | 1.48 s | CPU | CPU |

The mixed result is why automatic selection uses calibration. Auto is
conservative at 8 threads: the 2.3 MP sample keeps 3×3 and 5×5 on the CPU even
though a full-image forced GPU run is slightly faster. At one thread the same
sample selects the GPU for 5×5. Forced On remains available when the user has
measured a win.

Historical PEF measurement:

| Mode | Wall | Peak RSS | Stage |
|---|---|---|---|
| CPU | 7.88 s | 730 MB | — |
| GPU | 7.71 s | 942 MB | 1.15 s |

On the 45.7 MP D850 NEF, the opt-in 5×5 capture stage took 1.71 s at OMP=8
after the specialisation, with a 17.98 s total in the earlier run. Final
Auto checks on the same image: at OMP=1 the CPU stage was 5.74 s, forced GPU
1.63 s and Auto 1.35 s, all at 1 LSB; at OMP=8 the CPU stage was 1.29 s, forced
GPU 1.32 s and Auto correctly kept the CPU. Before the specialisation the
stage took 4.77 s when selected automatically. POCL is test-only and
intentionally skipped in auto mode: the same 45.7 MP case took 16.75 s capture
and 50.76 s total on POCL.

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
| 45.7 MP NEF | CPU | 8.12 s | — |
| 45.7 MP NEF | GPU | 6.58 s | 439 ms |

Three alternating runs each. The 45 MP GPU total is about 19 % faster. The
public PEF baseline passes `verify.sh --formats`; private local format samples
are outside the public corpus and are recorded as uncovered.

## Opt-in RGB processing

`RT_OPENCL_RGBPROC=1` enables the GPU `rgbProc` core. `OMP_NUM_THREADS=8`,
dynamic default profile, 16-bit TIFF:

| Image | CPU `rgbProc` | GPU stage | GPU total with stage | GPU output-only total |
|---|---:|---:|---:|---:|
| colorspace_flowers.pef, 10 MP | 80 ms | 319 ms | 2.71–2.80 s | 2.48 s |
| 45.7 MP NEF | 385 ms | 471 ms | 7.14–7.18 s | 6.5 s |

The GPU stage is slower than the multi-threaded CPU, so it is opt-in. The
overall GPU path remains faster than the CPU because it also accelerates
output conversion.

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

A private 45.7 MP NEF (8288x5520) with its `.pp3`, single-threaded,
Debian 12-built package run on the host through a private mount namespace,
with no `RUSTICL_ENABLE` or `OCL_ICD_VENDORS` in the environment:

| Mode | Wall | Peak RSS | Stage |
|---|---|---|---|
| CPU (`--gpu=off`) | 39.05 s | 2.74 GB | — |
| GPU (`--gpu=auto`, Rusticl RX 550, auto-detected) | 37.60 s | 2.74 GB | resize 401 ms |

`compare_tiff.py --bits 16`: maximum absolute error 0, differing samples 0.

