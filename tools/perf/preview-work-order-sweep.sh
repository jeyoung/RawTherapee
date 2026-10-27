#!/usr/bin/env bash
#
# Deterministic parity sweep of the preview raw chain.
#
# Usage:
#   tools/perf/preview-work-order-sweep.sh <raw> [out-dir]
#
# Environment:
#   RT_BUILD_DIR   configured build directory (default build/cpu)
#
# The driver runs the same image and state sequence twice: once with the
# conservative fallback (RT_WORK_ORDER=off) and once with the exact typed
# order at its default (RT_WORK_ORDER unset). It then compares every settled
# state's preview.tif and work.tif with ImageMagick and requires pixel-exact
# equality, checks that no legacy raw/core compare site remains, and probes
# the kill switch. Raw files and dumps stay under build/test-scratch.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${RT_BUILD_DIR:-$root/build/cpu}"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <raw> [out-dir]" >&2
    exit 2
fi

raw="$1"
out="${2:-$root/build/test-scratch/work-order-sweep}"

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
    # Print the effective switch for one explicit environment value.
    "$@" "$bin" --print-switch
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

rm -rf "$out/off" "$out/on"
mkdir -p "$out/off" "$out/on"

echo "== sweep: conservative fallback (RT_WORK_ORDER=off) =="
OMP_NUM_THREADS=1 RT_WORK_ORDER=off \
    stdbuf -oL -eL "$bin" "$raw" "$out/off" >"$out/off.log" 2>&1
echo "== sweep: exact typed order (default on) =="
OMP_NUM_THREADS=1 env -u RT_WORK_ORDER \
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

states=$(grep -c "^STATE " "$out/off.log" || true)
if [ "$states" -eq 0 ]; then
    echo "sweep FAIL: no states reported" >&2
    exit 1
fi
echo "retirement: no legacy raw/core compare site remains ($states states)"

echo "== per-state parity =="
failures=0
for dir in "$out/on"/*/; do
    name="$(basename "$dir")"
    case "$name" in
        cache|dump|settings) continue ;;
    esac
    for image in preview work; do
        legacy="$out/off/$name/$image.tif"
        typed="$out/on/$name/$image.tif"
        if [ ! -f "$legacy" ] || [ ! -f "$typed" ]; then
            echo "$name $image MISSING" >&2
            failures=$((failures + 1))
            continue
        fi
        ae="$(magick compare -metric AE "$legacy" "$typed" null: 2>&1 || true)"
        printf '%-28s %-7s AE=%s\n' "$name" "$image" "$ae"
        if [ "$ae" != "0" ]; then
            failures=$((failures + 1))
        fi
    done
done

if [ "$failures" -ne 0 ]; then
    echo "parity FAIL: $failures comparison(s) differ" >&2
    exit 1
fi

echo "preview work-order sweep: PASS ($states states)"
