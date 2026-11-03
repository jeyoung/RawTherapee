#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  POCL-only OpenCL test environment.
#
#  Device tests must never reach the ambient GPU ICD by accident: a faulty
#  kernel that POCL turns into a process crash becomes a GPU ring reset under
#  the vendor driver, losing VRAM and the desktop session. This helper selects
#  POCL's CPU device and fails closed if any other platform is visible.
#
#  Usage:
#    source tools/opencl/pocl-env.sh     # exports the environment
#    tools/opencl/pocl-env.sh --check    # exports, then asserts and exits
#
#  Environment:
#    RT_POCL_DIR      Directory holding a pocl-only pocl.icd
#                     (default: build/test-scratch/pocl-icd)
#    RT_POCL_SYSROOT  Directory holding libpocl and the loader dependencies
#                     (default: build/third_party/sysroot/usr/lib/x86_64-linux-gnu)
#    POCL_DEVICES     POCL device list (default: cpu)
#    RT_POCL_THREADS  Optional POCL_MAX_PTHREAD_COUNT cap
#    OMP_NUM_THREADS  Left untouched when set; otherwise pinned to 1, because
#                     several host reference blocks are thread-order
#                     dependent (the wavelet denoise noise variance, for one)
#
#  The helper is sourceable and exports only RT_POCL_* variables, the OpenCL
#  loader variables and a prefixed LD_LIBRARY_PATH entry.
#

_rt_pocl_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_rt_pocl_root="$(cd "$_rt_pocl_here/../.." && pwd)"

RT_POCL_DIR="${RT_POCL_DIR:-$_rt_pocl_root/build/test-scratch/pocl-icd}"
RT_POCL_SYSROOT="${RT_POCL_SYSROOT:-$_rt_pocl_root/build/third_party/sysroot/usr/lib/x86_64-linux-gnu}"

export OCL_ICD_VENDORS="$RT_POCL_DIR"
export POCL_DEVICES="${POCL_DEVICES:-cpu}"
if [ -n "${RT_POCL_THREADS:-}" ]; then
    export POCL_MAX_PTHREAD_COUNT="$RT_POCL_THREADS"
fi
if [ -z "${OMP_NUM_THREADS:-}" ]; then
    export OMP_NUM_THREADS=1
fi

case ":${LD_LIBRARY_PATH:-}:" in
    *":$RT_POCL_SYSROOT:"*) ;;
    *) export LD_LIBRARY_PATH="$RT_POCL_SYSROOT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac

# Report why the environment is unsafe, or nothing when it is safe.
rt_pocl_problem() {
    if [ ! -d "$OCL_ICD_VENDORS" ]; then
        printf 'POCL ICD directory missing: %s\n' "$OCL_ICD_VENDORS"
        return 0
    fi

    local unwanted
    unwanted="$(find "$OCL_ICD_VENDORS" -maxdepth 1 -name '*.icd' \
        ! -name 'pocl.icd' -print 2>/dev/null | head -n 1)"
    if [ -n "$unwanted" ]; then
        printf 'a non-POCL ICD is visible: %s\n' "$unwanted"
        return 0
    fi

    if ! command -v clinfo >/dev/null 2>&1; then
        printf 'clinfo is not installed; cannot prove the platform selection\n'
        return 0
    fi

    local listing
    listing="$(clinfo -l 2>/dev/null || true)"
    if ! printf '%s\n' "$listing" | grep -q 'Portable Computing Language'; then
        printf 'POCL is not the selected platform\n'
        return 0
    fi
    if printf '%s\n' "$listing" | grep -q '^Platform #1'; then
        printf 'more than one OpenCL platform is visible\n'
        return 0
    fi

    return 1
}

# Assert the environment, printing the reason and returning non-zero on
# failure.
rt_pocl_assert() {
    local problem
    if ! problem="$(rt_pocl_problem)"; then
        return 0
    fi
    printf 'error: %s\n' "${problem:-unknown platform selection failure}" >&2
    return 1
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    if [ "${1:-}" != "--check" ]; then
        printf 'usage: source %s, or run %s --check\n' \
            "${BASH_SOURCE[0]}" "${BASH_SOURCE[0]}" >&2
        exit 2
    fi
    if rt_pocl_assert; then
        printf 'POCL only: %s\n' "$(clinfo -l 2>/dev/null | head -n 1)"
        exit 0
    fi
    exit 1
fi
