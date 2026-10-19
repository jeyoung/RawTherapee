# Building RawTherapee locally

This document describes the local build helper used for development and for
producing ordinary release builds. It is deliberately simple: GCC, Release,
generic x86-64, out-of-source builds.

## Quick start

```bash
tools/build-rt.sh cpu                 # normal build
tools/build-rt.sh opencl              # build with the OpenCL backend
tools/build-rt.sh both                # both configurations
```

Build directories are named after the variant and the current branch, for
example `build/cpu-dev/` and `build/opencl-dev/`. Binaries are written to
`<build-dir>/rtgui/rawtherapee` and `<build-dir>/rtgui/rawtherapee-cli`.

Install with:

```bash
tools/build-rt.sh cpu --install
```

The default prefix is `/usr/local`. Override it with `--prefix` or
`RT_INSTALL_PREFIX`. For staging a package, use `RT_DESTDIR`:

```bash
RT_DESTDIR="$PWD/build/deb-stage" tools/build-rt.sh cpu --install
```

## Why the helper exists

- It always uses GCC. On systems where `/usr/bin/c++` is Clang, a plain CMake
  configure silently selects Clang, which disables OpenMP and can change
  output.
- It always sets `PROC_TARGET_NUMBER=1` (`-mtune=generic`), so builds run on
  other machines. Never ship a `PROC_TARGET_NUMBER=2` build.
- It refuses to configure when stale in-source build artifacts are present,
  because quoted includes resolve to the source tree first and shadow the
  generated headers.

## Building a new RawTherapee release

```bash
tools/build-rt.sh --update
git switch -c release-5.14 origin/release-5.14   # or the desired tag
tools/build-rt.sh cpu --install
```

To stay on `dev`:

```bash
git switch dev
git pull --ff-only
tools/build-rt.sh cpu
```

The build directory name includes the branch, so switching branches does not
reuse a stale CMake cache.

## Building with OpenCL

Install the headers and a diagnostic tool:

```bash
sudo apt install opencl-headers clinfo
```

Build:

```bash
tools/build-rt.sh opencl
```

At runtime an OpenCL device is optional. If no ICD is installed, or the user
disables acceleration, RawTherapee runs on the CPU. A vendor ICD is needed to
use a GPU; for example:

```bash
sudo apt install -t trixie-backports mesa-opencl-icd   # AMD/Intel via Mesa
sudo apt install pocl-opencl-icd                       # CPU device for tests
clinfo -l
```

Check the build:

```bash
build/opencl-dev/rtgui/rawtherapee-cli --gpu-info
```

## Requirements

Build dependencies are the normal RawTherapee ones plus `opencl-headers` for
the OpenCL variant. On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libgtkmm-3.0-dev \
    liblcms2-dev liblensfun-dev libexiv2-dev libfftw3-dev libpng-dev \
    libtiff-dev libjpeg-dev libjxl-dev libraw-dev libiptcdata0-dev \
    libcanberra-gtk3-dev librsvg2-dev libexpat1-dev zlib1g-dev
```

Use `tools/build-rt.sh opencl --check-deps` to see what is missing.

## Troubleshooting

- **OpenCL headers not found**: install `opencl-headers`, or set
  `OPENCL_INCLUDE_DIR` to the directory containing `CL/cl.h`.
- **Build picks up old objects / wrong headers**: use `--clean`, and remove any
  in-source `CMakeCache.txt`, `CMakeFiles/`, `rtgui/config.h` and
  `rtgui/version.h`.
- **No OpenCL device**: the UI reports this and keeps the CPU path. Install a
  vendor ICD or select `pocl-opencl-icd` for testing.
- **Clang instead of GCC**: the helper always passes `CMAKE_C_COMPILER=gcc`
  and `CMAKE_CXX_COMPILER=g++`; if a cache already exists, use
  `--clean`.
