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

For development builds, enable the compiler cache and the runtime stage
modules:

```bash
tools/build-rt.sh cpu -j 6 --ccache --stage-modules
tools/build-rt.sh opencl -j 6 --ccache --stage-modules
```

The cache lives in `build/ccache` by default and is bounded to 10 GiB. The
helper uses `tools/ccache-rt.sh`, so later `cmake --build` invocations keep
using the project cache without extra environment variables. Use
`--ccache-stats` to print the hit rate. Stage modules are built into
`<build-dir>/stages/` and can be rebuilt independently of the GUI and CLI, for
example `cmake --build build/cpu-<branch> --target stage_lanczos`.

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

On AMD/Mesa, the application sets `RUSTICL_ENABLE=radeonsi` automatically
before loading the ICD; Intel systems get `iris`. Leave the
Preferences → Performance → OpenCL → Rusticl driver field empty for
auto-detection, or enter an override. An existing `RUSTICL_ENABLE` environment
variable takes precedence. Rusticl support in Mesa starts with Mesa 23.1;
Debian 12 (Mesa 22.3) normally uses Clover instead.

POCL and Rusticl installed together can crash inside the ICD loader. The
backend hides POCL when a GPU ICD library is present and usable; this can
also hide POCL when the GPU runtime exists but reports no device. Auto mode
ignores CPU OpenCL devices because the native CPU path is faster. To use POCL
for testing, point `OCL_ICD_VENDORS` at a directory containing only
`pocl.icd` and set `RT_OPENCL_ALLOW_CPU=1`, or select the POCL device
explicitly. The allow flag is a fallback when no GPU device is selected; on a
mixed system use the explicit device selection. `RT_OPENCL_VENDOR_FILTER=0` disables the workaround, but the
loader may then crash on broken vendor combinations.

Compiled kernels are cached in the RawTherapee cache directory, under
`opencl/`. The cache key covers the kernel source, build options, device and
driver version. Override the location for tests with
`RT_OPENCL_CACHE_DIR=/path`. Removing that directory is always safe; kernels
are compiled again on demand.

Set `RT_OPENCL_PROFILE=1` to print the time spent in each accelerated stage.

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

## Portable package

Build a self-contained tarball and Debian package inside Ubuntu 22.04:

```bash
tools/build-rt.sh portable --format both
```

Requires Docker. Outputs go to `build/portable/`. The package installs under
`/usr/local` and bundles GTK and the other non-baseline libraries under
`/usr/local/lib/rawtherapee`; OpenCL is still supplied by the host. See `devnotes/opencl/PORTABLE.md`.

## Troubleshooting

- **The version reports an older release than expected**: the clone is
  shallow, so `git describe` cannot see recent tags. Run
  `git fetch --unshallow origin` and rebuild.
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
