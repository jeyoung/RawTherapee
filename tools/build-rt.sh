#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  Build helper for local RawTherapee builds.
#
#  It always uses GCC and a generic x86-64 target, because the default
#  "c++" on many systems is Clang and -march=native builds are not portable.
#
#  Usage:
#    tools/build-rt.sh cpu      [options]
#    tools/build-rt.sh opencl   [options]
#    tools/build-rt.sh both     [options]
#    tools/build-rt.sh portable [options]
#
#  Options:
#    -j, --jobs N        Parallel build jobs (default: nproc)
#    --install           Install after building
#    --clean             Remove the build directory first
#    --reconfigure       Re-run CMake even if the build directory exists
#    --check-deps        Check build dependencies and exit
#    --update            Fetch upstream tags and report how far behind we are
#    --build-root DIR    Where build directories are created
#    --prefix PATH       CMAKE_INSTALL_PREFIX (default: /usr/local)
#    -h, --help          Show this help
#
#  Environment:
#    RT_BUILD_ROOT      Same as --build-root
#    RT_INSTALL_PREFIX  Same as --prefix
#    RT_DESTDIR         Passed as DESTDIR to cmake --install
#    OPENCL_INCLUDE_DIR OpenCL header directory, if not in a standard place
#
#  Examples:
#    tools/build-rt.sh cpu --install
#    tools/build-rt.sh opencl -j 4
#    tools/build-rt.sh both --clean
#
set -euo pipefail

readonly SCRIPT_NAME="${0##*/}"

usage() {
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

info() {
    printf '==> %s\n' "$*"
}

repo_root() {
    local dir
    dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    git -C "$dir" rev-parse --show-toplevel 2>/dev/null \
        || die "not inside a git repository"
}

sanitise_ref() {
    local ref="$1"
    printf '%s' "$ref" | sed 's#[^A-Za-z0-9_.-]#-#g'
}

check_in_source_artifacts() {
    local root="$1"
    local found=0
    local path
    for path in CMakeCache.txt CMakeFiles rtgui/config.h rtgui/version.h; do
        if [ -e "$root/$path" ]; then
            printf 'error: stale in-source build artifact: %s\n' "$root/$path" >&2
            found=1
        fi
    done
    if [ "$found" -ne 0 ]; then
        cat >&2 <<'EOF'
In-source build artifacts shadow generated headers. Remove them first:

  rm -rf CMakeCache.txt CMakeFiles rtgui/config.h rtgui/version.h

EOF
        exit 1
    fi
}

check_command() {
    local name="$1"
    local hint="$2"
    if ! command -v "$name" >/dev/null 2>&1; then
        printf 'missing: %-12s %s\n' "$name" "$hint" >&2
        return 1
    fi
    return 0
}

check_opencl_headers() {
    local include_dir="${OPENCL_INCLUDE_DIR:-}"
    if [ -n "$include_dir" ] && [ -f "$include_dir/CL/cl.h" ]; then
        return 0
    fi
    for include_dir in /usr/include /usr/local/include /usr/include/x86_64-linux-gnu; do
        if [ -f "$include_dir/CL/cl.h" ]; then
            return 0
        fi
    done
    cat >&2 <<'EOF'
error: OpenCL headers not found (CL/cl.h).

Install the headers, for example:

  sudo apt install opencl-headers

Alternatively set OPENCL_INCLUDE_DIR to the directory containing CL/cl.h.
EOF
    return 1
}

pkg_status() {
    local module="$1"
    local label="$2"
    if pkg-config --exists "$module" 2>/dev/null; then
        printf 'ok       %-28s %s\n' "$label" "$(pkg-config --modversion "$module")"
    else
        printf 'MISSING  %-28s %s\n' "$label" "$module"
        return 1
    fi
}

check_deps() {
    local variant="$1"
    local failed=0

    info "toolchain"
    check_command cmake "build system (need >= 3.15)" || failed=1
    check_command gcc "GCC C compiler" || failed=1
    check_command g++ "GCC C++ compiler" || failed=1
    check_command make "build tool" || failed=1
    check_command git "version information" || failed=1
    check_command pkg-config "dependency discovery" || failed=1

    info "libraries"
    local module
    for module in gtkmm-3.0 glibmm-2.4 lcms2 lensfun exiv2 fftw3f libpng libtiff-4; do
        pkg_status "$module" "$module" || failed=1
    done

    if [ "$variant" = "opencl" ]; then
        info "OpenCL"
        if check_opencl_headers; then
            printf 'ok       %-28s %s\n' "OpenCL headers" "CL/cl.h"
        else
            failed=1
        fi
    fi

    if [ "$failed" -ne 0 ]; then
        cat >&2 <<'EOF'

One or more dependencies are missing. On Debian/Ubuntu the build dependencies
are (adjust for your distribution):

  sudo apt install build-essential cmake pkg-config libgtkmm-3.0-dev \
      liblcms2-dev liblensfun-dev libexiv2-dev libfftw3-dev libpng-dev \
      libtiff-dev libjpeg-dev libjxl-dev libraw-dev libiptcdata0-dev \
      libcanberra-gtk3-dev librsvg2-dev libexpat1-dev zlib1g-dev
  sudo apt install opencl-headers            # OpenCL variant only

EOF
        return 1
    fi

    info "all checked dependencies are present"
}

variant_cmake_args() {
    local variant="$1"
    case "$variant" in
        cpu)    printf '%s\n' "-DWITH_OPENCL=OFF" ;;
        opencl) printf '%s\n' "-DWITH_OPENCL=ON" ;;
        *) die "unknown variant '$variant'" ;;
    esac
}

configure_build() {
    local root="$1"
    local dir="$2"
    local variant="$3"
    local prefix="$4"

    local -a args=(
        -S "$root"
        -B "$dir"
        -G "Unix Makefiles"
        -DCMAKE_BUILD_TYPE=Release
        -DPROC_TARGET_NUMBER=1
        -DCMAKE_C_COMPILER=gcc
        -DCMAKE_CXX_COMPILER=g++
        -DCMAKE_INSTALL_PREFIX="$prefix"
    )
    # shellcheck disable=SC2207
    args+=($(variant_cmake_args "$variant"))

    if [ -n "${OPENCL_INCLUDE_DIR:-}" ]; then
        args+=("-DOPENCL_INCLUDE_DIR=$OPENCL_INCLUDE_DIR")
    fi

    info "configuring $variant in $dir"
    cmake "${args[@]}"
}

build_portable() {
    local root="$1"
    local build_root="$2"

    command -v docker >/dev/null 2>&1 || die "docker is required for portable builds"

    local tag="rt-portable-debian12"
    info "building container image $tag"
    docker build --platform linux/amd64 \
        -t "$tag" -f "$root/tools/portable/Dockerfile.debian12" "$root/tools/portable"

    local output="$build_root/portable"
    mkdir -p "$output"

    info "building Debian package"
    docker run --rm \
        --platform linux/amd64 \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -v "$root:/src:ro" \
        -v "$output:/out" \
        "$tag" \
        /src/tools/portable/build-debian12.sh
}

build_variant() {
    local root="$1"
    local variant="$2"
    local build_root="$3"
    local prefix="$4"
    local jobs="$5"
    local do_clean="$6"
    local do_reconfigure="$7"
    local do_install="$8"

    local branch dir
    branch="$(git -C "$root" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
    if [ -z "$branch" ] || [ "$branch" = "HEAD" ]; then
        branch="$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo detached)"
    fi
    dir="$build_root/$variant-$(sanitise_ref "$branch")"

    if [ "$do_clean" -eq 1 ]; then
        info "removing $dir"
        rm -rf "$dir"
    fi

    mkdir -p "$dir"

    if [ "$do_reconfigure" -eq 1 ] || [ ! -f "$dir/CMakeCache.txt" ]; then
        configure_build "$root" "$dir" "$variant" "$prefix"
    else
        info "reusing CMake cache in $dir (use --reconfigure to refresh)"
    fi

    info "building $variant with $jobs job(s)"
    cmake --build "$dir" --parallel "$jobs"

    local bin_dir="$dir/rtgui"
    printf '\nbuilt:\n'
    printf '  %s\n' "$bin_dir/rawtherapee"
    printf '  %s\n' "$bin_dir/rawtherapee-cli"

    if [ "$do_install" -eq 1 ]; then
        if [ -n "${RT_DESTDIR:-}" ]; then
            info "installing to DESTDIR=$RT_DESTDIR (prefix $prefix)"
            DESTDIR="$RT_DESTDIR" cmake --install "$dir"
        else
            info "installing to $prefix"
            cmake --install "$dir"
        fi
    fi
}

main() {
    local variant=""
    local jobs
    local build_root
    local prefix
    local do_install=0
    local do_clean=0
    local do_reconfigure=0
    local do_check=0
    local do_update=0

    jobs="$(nproc 2>/dev/null || echo 4)"
    build_root="${RT_BUILD_ROOT:-}"
    prefix="${RT_INSTALL_PREFIX:-/usr/local}"

    if [ "$#" -eq 0 ]; then
        usage
        exit 1
    fi

    while [ "$#" -gt 0 ]; do
        case "$1" in
            cpu|opencl|both|portable)
                [ -z "$variant" ] || die "variant already set to '$variant'"
                variant="$1"
                ;;
            -j|--jobs)
                [ "$#" -ge 2 ] || die "$1 needs an argument"
                jobs="$2"
                shift
                ;;
            --jobs=*)
                jobs="${1#*=}"
                ;;
            --install)
                do_install=1
                ;;
            --clean)
                do_clean=1
                ;;
            --reconfigure)
                do_reconfigure=1
                ;;
            --check-deps)
                do_check=1
                ;;
            --update)
                do_update=1
                ;;
            --build-root)
                [ "$#" -ge 2 ] || die "$1 needs an argument"
                build_root="$2"
                shift
                ;;
            --build-root=*)
                build_root="${1#*=}"
                ;;
            --prefix)
                [ "$#" -ge 2 ] || die "$1 needs an argument"
                prefix="$2"
                shift
                ;;
            --prefix=*)
                prefix="${1#*=}"
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "unknown option '$1' (try --help)"
                ;;
        esac
        shift
    done

    [ -n "$variant" ] || die "missing variant: cpu, opencl or both"

    case "$jobs" in
        ''|*[!0-9]*) die "invalid job count '$jobs'" ;;
    esac

    local root
    root="$(repo_root)"
    [ -n "$build_root" ] || build_root="$root/build"

    if [ -f "$root/.git/shallow" ]; then
        printf 'warning: this is a shallow clone; the reported version may name an older release.\n' >&2
        printf '         run "git fetch --unshallow origin" to restore the full history.\n' >&2
    fi

    check_in_source_artifacts "$root"

    if [ "$do_update" -eq 1 ]; then
        info "fetching upstream"
        git -C "$root" fetch origin --tags
        if upstream="$(git -C "$root" rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null)"; then
            behind="$(git -C "$root" rev-list --count "HEAD..$upstream")"
            printf 'branch %s is %s commit(s) behind %s\n' \
                "$(git -C "$root" rev-parse --abbrev-ref HEAD)" "$behind" "$upstream"
        else
            printf 'branch %s has no upstream configured\n' \
                "$(git -C "$root" rev-parse --abbrev-ref HEAD)"
        fi
    fi

    if [ "$do_check" -eq 1 ]; then
        if [ "$variant" = "portable" ]; then
            command -v docker >/dev/null 2>&1 || die "docker is required for portable builds"
            docker info >/dev/null 2>&1 || die "cannot talk to the Docker daemon"
        elif [ "$variant" = "both" ]; then
            check_deps opencl
        else
            check_deps "$variant"
        fi
        exit $?
    fi

    case "$variant" in
        portable)
            build_portable "$root" "$build_root"
            ;;
        cpu|opencl)
            if [ "$variant" = "opencl" ]; then
                check_opencl_headers
            fi
            build_variant "$root" "$variant" "$build_root" "$prefix" \
                "$jobs" "$do_clean" "$do_reconfigure" "$do_install"
            ;;
        both)
            check_opencl_headers
            build_variant "$root" cpu "$build_root" "$prefix" \
                "$jobs" "$do_clean" "$do_reconfigure" "$do_install"
            build_variant "$root" opencl "$build_root" "$prefix" \
                "$jobs" "$do_clean" "$do_reconfigure" "$do_install"
            ;;
    esac
}

main "$@"
