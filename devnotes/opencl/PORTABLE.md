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

Current verified artefact, built from commit `2dd85e36b`:

```
build/portable/rawtherapee-5.13-196-g2dd85e36b_5.13+git13217.2dd85e36b-1_amd64.deb
sha256 09cb0d5dcc046b9be1cce103779d1a9b0998cb8d681ecf914354cf020ffbf8b7
```

The package was built from a clean clone because a linked worktree exposes a
`.git` file that points outside the container-mounted source directory, which
makes the version resolve as `git0.local`.

Fresh `debian:12` and `debian:13` containers installed the `.deb` and listed
formats including JPEG XL. A single-threaded 16-bit TIFF export with an
explicit PP3 was identical to the single-threaded host export except for the
embedded version string and timestamp. The installed package started under
Xvfb on Debian 12 with a 10.2 MP PEF. An exposure-edited comparison rendered
with `ALLNORAW` in 32.6 ms and preserved zoom/anchor; the following hold
logged `cache hit 0.092 ms` and its viewport was byte-identical to the first
render. The application quit cleanly. The test containers have no OpenCL ICD,
so `--gpu-test` and `--gpu-test-monitor` are recorded as not run. These are
functional checks; no general performance claim is made for the package.

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
