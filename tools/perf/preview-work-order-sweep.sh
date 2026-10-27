#!/usr/bin/env bash
#
# Deterministic parity sweep of the preview raw chain.
#
# Usage:
#   tools/perf/preview-work-order-sweep.sh <raw> [out-dir]
#   tools/perf/preview-work-order-sweep.sh <raw> [out-dir] --timing=N [--clamp]
#
# Environment:
#   RT_BUILD_DIR   configured build directory (default build/cpu)
#
# Parity mode runs the same image and state sequence twice: once with the
# conservative fallback (RT_WORK_ORDER=off) and once with the exact typed order
# at its default (RT_WORK_ORDER unset). It then compares every settled state's
# preview.tif and work.tif with ImageMagick and requires pixel-exact equality,
# checks that no legacy raw/core compare site remains, and probes the kill
# switch. A state directory that is missing on either side fails the run: the
# state list comes from the run log, not from whichever directories happen to
# exist.
#
# Timing mode passes the trailing arguments to the harness once per side and
# writes <out-dir>/off.log and <out-dir>/on.log with one `TIMING` line per
# sample. It compares nothing. The commands it runs are:
#
#   OMP_NUM_THREADS=1 env -u RT_WORK_SHADOW RT_WORK_ORDER=off \
#       <build>/rtengine/preview-work-order-sweep <raw> <out>/off --timing=10
#   OMP_NUM_THREADS=1 env -u RT_WORK_ORDER -u RT_WORK_SHADOW \
#       <build>/rtengine/preview-work-order-sweep <raw> <out>/on  --timing=10
#
# Raw files and dumps stay under build/test-scratch.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${RT_BUILD_DIR:-$root/build/cpu}"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <raw> [out-dir] [--timing=N [--clamp]]" >&2
    exit 2
fi

raw="$1"
shift
out="$root/build/test-scratch/work-order-sweep"
if [ "$#" -gt 0 ] && [ "${1#--}" = "$1" ]; then
    out="$1"
    shift
fi

timing=0
for arg in "$@"; do
    case "$arg" in
        --timing=*) timing=1 ;;
        --clamp) ;;
        *)
            echo "unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "not a configured build directory: $build_dir" >&2
    echo "configure one with: tools/build-rt.sh cpu" >&2
    exit 2
fi

cmake --build "$build_dir" --target preview-work-order-sweep --parallel 6
bin="$build_dir/rtengine/preview-work-order-sweep"

if [ ! -x "$bin" ]; then
    echo "missing harness: $bin" >&2
    exit 2
fi

switch() {
    # Print the effective switch for one explicit environment value. An
    # ambient RT_WORK_SHADOW must not leak into the probe: it is unset here,
    # so the expected `shadow=0` is a property of this run, not of the caller.
    env -u RT_WORK_SHADOW "$@" "$bin" --print-switch
}

check_switch() {
    local expected="$1"
    shift
    local line
    line="$(switch "$@")"
    if [ "$line" != "SWITCH work-order=$expected shadow=0" ]; then
        echo "kill switch FAIL: expected work-order=$expected, got: $line" >&2
        exit 1
    fi
    echo "kill switch: $line"
}

echo "== kill switch =="
check_switch 1 env -u RT_WORK_ORDER
check_switch 1 env RT_WORK_ORDER=1
check_switch 1 env RT_WORK_ORDER=
check_switch 1 env RT_WORK_ORDER=yes
check_switch 0 env RT_WORK_ORDER=0
check_switch 0 env RT_WORK_ORDER=off
check_switch 0 env RT_WORK_ORDER=OFF
# An ambient RT_WORK_SHADOW must not reach the probe.
RT_WORK_SHADOW=1 check_switch 1 env -u RT_WORK_ORDER

rm -rf "$out/off" "$out/on"
mkdir -p "$out/off" "$out/on"

if [ "$timing" -eq 1 ]; then
    echo "== timing: conservative fallback (RT_WORK_ORDER=off) =="
    OMP_NUM_THREADS=1 env -u RT_WORK_SHADOW RT_WORK_ORDER=off \
        stdbuf -oL -eL "$bin" "$raw" "$out/off" "$@" >"$out/off.log" 2>&1
    echo "== timing: exact typed order (default on) =="
    OMP_NUM_THREADS=1 env -u RT_WORK_ORDER -u RT_WORK_SHADOW \
        stdbuf -oL -eL "$bin" "$raw" "$out/on" "$@" >"$out/on.log" 2>&1
    grep -q "^TIMING " "$out/off.log" \
        || { echo "timing run produced no samples: $out/off.log" >&2; exit 1; }
    grep -q "^TIMING " "$out/on.log" \
        || { echo "timing run produced no samples: $out/on.log" >&2; exit 1; }
    echo "timing: samples in $out/off.log and $out/on.log"
    exit 0
fi

echo "== sweep: conservative fallback (RT_WORK_ORDER=off) =="
OMP_NUM_THREADS=1 env -u RT_WORK_SHADOW RT_WORK_ORDER=off \
    stdbuf -oL -eL "$bin" "$raw" "$out/off" >"$out/off.log" 2>&1
echo "== sweep: exact typed order (default on) =="
OMP_NUM_THREADS=1 env -u RT_WORK_ORDER -u RT_WORK_SHADOW \
    stdbuf -oL -eL "$bin" "$raw" "$out/on" >"$out/on.log" 2>&1

grep -q "SWITCH work-order=0 shadow=0" "$out/off.log" \
    || { echo "off run did not report the conservative switch" >&2; exit 1; }
grep -q "SWITCH work-order=1 shadow=0" "$out/on.log" \
    || { echo "on run did not report the exact switch" >&2; exit 1; }

# The raw/core legacy gates are retired: no site emits a comparison line.
if grep -q "work-shadow" "$out/off.log" "$out/on.log"; then
    echo "retirement FAIL: a work-shadow comparison is still emitted" >&2
    exit 1
fi

states=$(grep -c "^STATE " "$out/on.log" || true)
off_states=$(grep -c "^STATE " "$out/off.log" || true)
if [ "$states" -eq 0 ] || [ "$states" -ne "$off_states" ]; then
    echo "sweep FAIL: $states states reported by the exact run, $off_states by the conservative run" >&2
    exit 1
fi
echo "retirement: no legacy raw/core compare site remains ($states states)"

echo "== per-state parity =="
failures=0
while read -r tag index name _; do
    [ "$tag" = "STATE" ] || continue
    dir=$(printf '%02u-%s' "$index" "$name")
    for side in off on; do
        if [ ! -d "$out/$side/$dir" ]; then
            echo "$dir $side MISSING" >&2
            failures=$((failures + 1))
        fi
    done
    for image in preview work; do
        legacy="$out/off/$dir/$image.tif"
        typed="$out/on/$dir/$image.tif"
        if [ ! -f "$legacy" ] || [ ! -f "$typed" ]; then
            echo "$dir $image MISSING" >&2
            failures=$((failures + 1))
            continue
        fi
        ae="$(magick compare -metric AE "$legacy" "$typed" null: 2>&1 || true)"
        printf '%-28s %-7s AE=%s\n' "$dir" "$image" "$ae"
        if [ "$ae" != "0" ]; then
            failures=$((failures + 1))
        fi
    done
done <"$out/on.log"

if [ "$failures" -ne 0 ]; then
    echo "parity FAIL: $failures comparison(s) differ or are missing" >&2
    exit 1
fi

echo "preview work-order sweep: PASS ($states states)"
