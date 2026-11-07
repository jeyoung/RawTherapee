> **DEPRECATED — historical record.**
> Do not read for current state. Current record: `../opencl/PORTABLE.md`.
> Package lineage and per-package prose below are historical only.

# Portable package history

## Earlier verified artefact

Verified artefact from commit `6ed4b80c2` (FAST preview toggle):

```
build/portable/rawtherapee-5.13-143-g6ed4b80c2_5.13+git13164.6ed4b80c2-1_amd64.deb
sha256 683134642cdaf1266aee0d39eace33786a76d4996858154b0e1592ecdea3d242
```

The Debian 12 and Debian 13 package tests installed the `.deb`, listed formats
(including JPEG XL), exported the 10.2 MP PEF through the CLI and started the
GUI under Xvfb. The test containers have no OpenCL ICD, so `--gpu-test` and
`--gpu-test-monitor` were recorded as not run rather than passed. Interactive
toggle checks used host Xvfb; the package test is a functional install and
startup smoke test.

## Earlier graph package matrix

The graph package had its own matrix: install on Debian 12 and 13 with
`apt-get install ./pkg.deb`, four inputs by two outputs compared against the
host build, Xephyr GUI checks and POCL device parity. It was recorded in
`devnotes/perf/VALIDATION.md`.

## Package validated 2026-11-06

Package `5.13+git13806.4071906f7-1` from `4071906f7` (sha256
`587a5b699cd4adc07c9eeb3e25c3ef46b6e39fd62da799bb1668e946da6d73b4`):
fresh Debian 12 and Debian 13 containers both passed
`apt-get install -y ./...deb`; Debian 13's t64 packages satisfied the
pre-t64 `Depends` through `Provides`, with no workaround. `--list-formats`
exited 0 and listed 36 formats including PEF/NEF/CR2/ARW, TIFF and JPEG XL.
A neutral 16-bit CPU export (`OMP_NUM_THREADS=1`, `--gpu=off`) matched the
host CLI exactly when both used the packaged data directory: PAE 0 LSB16,
0 of 30,419,712 samples, on both distributions. GUI smoke under Xvfb
stayed alive to the 20 s timeout on both (exit 124, no stderr).
`--gpu-test` and `--gpu-test-monitor` were not run: the containers have no
GPU.

## Earlier package lineage

The previous packages passed the same install/formats/export/GUI rows:

- `5.13+git13802.e42d991d2-1` (sha256 `3b1ed54edfc0b8edc4dbc7675397de463fcae77e0cf8721141741440f8b0fe58`)
- `5.13+git13800.ade38b7b2-1` (sha256 `12d8ce6823c93566d245ecd49d7ff0f6cb41693389e092d35eaecee34ebb8647`)
- `5.13+git13794.a78e6509a-1` (sha256 `b4656385486e59f60bf79030e01080fe43e1d7bcd06f71bc3d12c9fccc7fceef`)
- `5.13+git13790.ee73df905-1` (sha256 `7f0ecd576e68da9cda58dd2d0e92e3ede175b5821ff2f63e1d5bd46531bbfe06`)
- `5.13+git13786.f1e0e86a6-1` (sha256 `fa2fb3b5904cdb51d84a1a397c8e5ca7bda87fc2374bc5e476ebbbf7314302fa`)
- `5.13+git13776.a262fab23-1` (sha256 `0939a1789ec36514272010cc6ace22c15776fca895244df2950ba288edf8ba94`)
- `5.13+git13767.596b6fed9-1` (sha256 `17ec3cb6169d9afed3ed9ec9b2a4bc606be7d23afff20022d680a1ac27da3985`)
- `5.13+git13753.6bc636307-1` (sha256 `315a9ea0f7f867a75f95981a77d517fbe90a6fc6e4ec3f546c8cb192670380e4`)
- `5.13+git13739.3babc4b48-1` (sha256 `fb7882c4c75c88fbdcc7682c49d4a63265e280b4217804774eb5b365d3f5a62f`)
- `5.13+git13684.2dccc3ee5-1` (sha256 `bc59ce0721df0262e854656c32807061b37a503e450c0b3fdee5f3093e81f5df`)
- `5.13+git13386.6fd009d74-1` (sha256 `d4337ee48ce86c2ac1e996bf82495c49b99dd7fb17973058afcb27e0639cdb3f`)

## Earlier notes

- The package replaces `/usr/local/bin/rawtherapee` and
  `/usr/local/bin/rawtherapee-cli`, like a normal `/usr/local` build.
- Remove any old self-contained bundle in `/usr/local/lib/rawtherapee` before
  installing; stale bundled GTK libraries there would still win at runtime.
