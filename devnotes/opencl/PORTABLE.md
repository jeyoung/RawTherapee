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

Verified artefact from commit `32821115a` (dev tip):

```
build/portable/rawtherapee-5.13-169-g32821115a_5.13+git13190.32821115a-1_amd64.deb
sha256 64dda18a6e0efe4481bb7a12e9311ae840c9399f3e4f6d67a3e22c48c8a8e674
```

The Debian 12 and Debian 13 package tests installed the `.deb`, listed formats
(including JPEG XL), exported the 10.2 MP PEF through the CLI and started the
GUI under Xvfb. Both releases produced pixel-identical 16-bit TIFF output
(AE=0 over 10,139,904 pixels), and a Debian 13 source build of the same commit
matched both exactly. That reference was built in a Debian 13 container
because the build host has no C++ build dependencies installed; the package
itself is still built in the documented Debian 12 container. The test
containers have no OpenCL ICD, so `--gpu-test` and `--gpu-test-monitor`
reported "No OpenCL device found" (exit 2) and are recorded as not run rather
than passed. The package test is a functional install and startup smoke test;
no performance is claimed.

Container images used for the tests:

- `debian:12`:
  `sha256:763cbdcdd70f724557c36ad2f9ff506a702c206412d5f7fc81342698b0e0f0c1`
- `debian:13`:
  `sha256:e94e0ef014004824e778a450941b2d74896bd1be7ebd5b31f0465134797c38c8`

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
- Tested on Debian 12 and Debian 13 containers with `apt-get install ./...deb`,
  CPU export, `--list-formats` (including JPEG XL) and a GUI smoke test under
  Xvfb.
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
