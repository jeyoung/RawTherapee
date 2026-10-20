# Portable package

Builds a self-contained RawTherapee for x86-64 Linux inside an Ubuntu 22.04
container. No AppImage, no FUSE, no build dependencies on the target machine.

## Build

```bash
tools/build-rt.sh portable --format both
```

Outputs in `build/portable/`:

- `rawtherapee-<version>-portable-linux-x86_64.tar.gz`
- `rawtherapee-<version>_<version>_amd64.deb`

Use `--format tarball` or `--format deb` for one artefact. The first build
downloads the Ubuntu 22.04 image and installs build dependencies.

## Layout

```
/usr/local/bin/rawtherapee       (wrapper)
/usr/local/bin/rawtherapee-cli   (wrapper)
/usr/local/lib/rawtherapee/      bundled binaries, GTK/gtkmm/lcms/exiv2/lensfun/...
/usr/local/share/rawtherapee/    profiles, ICC, DCP, languages, themes, images
/usr/local/share/glib-2.0/schemas
/usr/local/share/lensfun
/usr/share/applications/rawtherapee-5.13.desktop
```

Binaries use `RPATH=$ORIGIN/lib`; the wrapper sets only GTK data paths. OpenCL
is still loaded from the host at runtime, so the vendor driver must be
installed separately.

## Portability baseline

- Built on Ubuntu 22.04: glibc 2.35, GCC 11, `-mtune=generic`.
- Bundled libraries are copied from the build container; glibc, the dynamic
  loader, OpenCL, DRM/GL/EGL/Vulkan and vendor driver libraries are not.
- Tested on Debian 12 and Ubuntu 22.04 containers, installed with
  `dpkg -i`, CPU export and `--gpu-info` with no ICD. Extract the tarball at
  `/`; it contains `usr/local/...` and the compiled data path is absolute.
- Requires `libc6 >= 2.35`; not intended for older distributions.

## Limitations

- JPEG XL is disabled because Ubuntu 22.04 has no libjxl development package.
  `--list-formats` reflects the formats actually built.
- The package replaces `/usr/local/bin/rawtherapee` and
  `/usr/local/bin/rawtherapee-cli`, like a normal `/usr/local` build.
- Bundled libraries do not receive distribution security updates.
- OpenCL support depends on the host ICD and driver; without one the CPU path
  is used.
