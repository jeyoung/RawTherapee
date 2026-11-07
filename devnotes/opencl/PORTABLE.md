# Portable package

Builds a RawTherapee `.deb` for Debian 12 (bookworm) and newer, inside a
Debian 12 container. No AppImage and no build dependencies on the target
machine.

## Build
```bash
tools/build-rt.sh portable
```

Output in `build/portable/`:

- `rawtherapee-<git-describe>_<base-version>+git<count>.<hash>-1_amd64.deb`

The first build downloads the Debian 12 image and installs build dependencies.
The container is always based on Debian 12 so the package keeps the bookworm
baseline.

## Current package

```
build/portable/rawtherapee-graph-5.13-801-gd6f5ef1f6_5.13+git13822.d6f5ef1f6-1_amd64.deb
sha256 9e1905b398185d0b200dee41dcf4cd83f2550557442bb5ac80d7a89205ee7c13
```

Built 2026-11-07 from `d6f5ef1f6` with `tools/build-rt.sh portable`. Fresh
Debian 12 and Debian 13 containers both installed it with
`apt-get install -y ./...deb`; Debian 13's t64 packages satisfied the pre-t64
`Depends` through `Provides`. `--list-formats` listed 36 formats including
PEF/NEF/CR2/ARW, TIFF and JPEG XL. A neutral 16-bit CPU export
(`OMP_NUM_THREADS=1`, `--gpu=off`) matched the host CLI exactly with the
packaged data directory: AE=0, 0 of 30,419,712 samples, on both distributions.
GUI smoke under Xvfb stayed alive to the 20 s timeout on both (exit 124, no
stderr). `--gpu-test` and `--gpu-test-monitor` were not run: the containers
have no GPU. The full matrix is in `devnotes/perf/VALIDATION.md`.

## Previous artefacts

These passed the same install/formats/export/GUI rows:

- `5.13+git13820.4c690a041-1` (sha256 `0c92105cd6f8a5f2a7361f4074a35bb9a805fe681ab449bcbd976c88ee7413c3`)
- `5.13+git13816.f7f3d2848-1` (sha256 `41bdcd00a5997299c72d0d9b5e2f6b79c5d49bfc33be4c37a99c0103ccea73e4`)
- `5.13+git13806.4071906f7-1` (sha256 `587a5b699cd4adc07c9eeb3e25c3ef46b6e39fd62da799bb1668e946da6d73b4`)
- `5.13+git13802.e42d991d2-1` (sha256 `3b1ed54edfc0b8edc4dbc7675397de463fcae77e0cf8721141741440f8b0fe58`)
- `5.13+git13800.ade38b7b2-1` (sha256 `12d8ce6823c93566d245ecd49d7ff0f6cb41693389e092d35eaecee34ebb8647`)

Longer package lineage and per-package prose are in
[`../perf/deprecated/PORTABLE-HISTORY.md`](../perf/deprecated/PORTABLE-HISTORY.md)
(deprecated; historical only).

## Layout

```
/usr/local/bin/rawtherapee
/usr/local/bin/rawtherapee-cli
/usr/local/lib/rawtherapee/libexiv2.so.27      bundled (bookworm soname)
/usr/local/lib/rawtherapee/libjxl.so.0.7       bundled (bookworm soname)
/usr/local/lib/rawtherapee/libjxl_threads.so.0.7
/usr/local/share/rawtherapee/                  profiles, ICC, DCP, languages, themes, images
/usr/local/share/applications/rawtherapee.desktop
/usr/local/share/icons/hicolor/...
```

GTK, gtkmm, GLib, lensfun, lcms2, libtiff and the rest of the desktop stack
come from the host. Exiv2 and JPEG XL are bundled because their sonames changed
between Debian 12 and Debian 13 (`libexiv2.so.27`/`.28`,
`libjxl.so.0.7`/`.0.11`); bundling those two keeps one package usable on both.

## OpenCL auto-detection

The installed application detects the Rusticl driver before loading OpenCL:

- AMD PCI vendor `0x1002` → `RUSTICL_ENABLE=radeonsi`
- Intel PCI vendor `0x8086` → `RUSTICL_ENABLE=iris`
- AMD plus Intel → `radeonsi,iris`

`Preferences → Performance → OpenCL → Rusticl driver` overrides the detected
value; leave it empty for automatic detection. An existing `RUSTICL_ENABLE`
environment variable takes precedence. No shell profile or launcher wrapper is
required after installing the package. The package recommends
`mesa-opencl-icd` and `ocl-icd-libopencl1`; install them explicitly when using
`--no-install-recommends`. Rusticl needs Mesa 23.1 or newer; Debian 12's Mesa
22.3 uses Clover instead, so automatic Rusticl setup applies on Debian 13 and
newer.

Binaries use `DT_RUNPATH=$ORIGIN/../lib/rawtherapee`; bundled libraries use
`DT_RUNPATH=$ORIGIN`. OpenCL is loaded from the host at runtime, so the vendor
driver and an ICD loader must be installed separately.

## Portability baseline

- Built on Debian 12: glibc 2.36, GCC 12, `-mtune=generic`.
- Requires Debian 12 or newer. Debian 13 t64 packages are accepted through
  their `Provides` entries.
- The package replaces `/usr/local/bin/rawtherapee` and
  `/usr/local/bin/rawtherapee-cli`, like a normal `/usr/local` build.
- Remove any old self-contained bundle in `/usr/local/lib/rawtherapee` before
  installing; stale bundled GTK libraries there would still win at runtime.

## Limitations

- Host GTK/gtkmm/lensfun/lcms2 must be installed; the `.deb` declares them as
  dependencies and `apt` resolves them. If installing with
  `--no-install-recommends`, also install `liblensfun-data-v1` and
  `librsvg2-common` explicitly for lens correction and SVG icons.
- The bundled exiv2 and JPEG XL copies do not receive distribution security
  updates; rebuild the package to pick up fixes.
- OpenCL support depends on the host ICD and driver; without a GPU device the
  CPU path is used. POCL-only systems also use the CPU path by default; set
  `RT_OPENCL_ALLOW_CPU=1` only for POCL testing.
- `librsvg2-common` is recommended so the SVG icon theme renders correctly.
- GPU package rows were not run; see `devnotes/perf/VALIDATION.md` for the
  recorded NOT RUN items.
