#!/usr/bin/env bash
#
# Measure settled preview-update latency with the deterministic sweep harness.
#
# Usage:
#   tools/perf/bench-preview.sh <raw> [runs]
#
# Environment:
#   RT_BUILD_DIR     configured build directory (default: build/cpu)
#   RT_PREVIEW_OUT   scratch directory (default: build/test-scratch/bench-preview)
#
# The GUI-driven xdotool driver was retired: its fixed coordinates and sleeps
# made the figures depend on the window layout and on whatever else the host
# was doing. The sweep harness applies the same exposure edit sequence with
# OMP_NUM_THREADS=1, prints one `TIMING <index> <ms>` line per settled sample
# and writes its TIFFs under the scratch directory, so the sample count and
# the log travel with the number.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image="${1:-}"
runs="${2:-5}"

if [ -z "$image" ]; then
    echo "usage: $0 <raw> [runs]" >&2
    exit 2
fi

[ -f "$image" ] || { echo "not a file: $image" >&2; exit 2; }
case "$runs" in
    ''|*[!0-9]*|0)
        echo "runs must be a positive integer, got: $runs" >&2
        exit 2
        ;;
esac

build_dir="${RT_BUILD_DIR:-$root/build/cpu}"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "not a configured build directory: $build_dir" >&2
    echo "configure one with: tools/build-rt.sh cpu" >&2
    exit 2
fi

cmake --build "$build_dir" --target preview-work-order-sweep --parallel 6 >/dev/null

bin="$build_dir/rtengine/preview-work-order-sweep"
[ -x "$bin" ] || { echo "missing harness: $bin" >&2; exit 2; }

out="${RT_PREVIEW_OUT:-$root/build/test-scratch/bench-preview}"
rm -rf "$out"
mkdir -p "$out"

OMP_NUM_THREADS=1 env -u RT_WORK_ORDER -u RT_WORK_SHADOW \
    stdbuf -oL -eL "$bin" "$image" "$out" --timing="$runs" \
    >"$out/timing.log" 2>&1

if ! grep -q '^TIMING ' "$out/timing.log"; then
    echo "no TIMING samples captured; see $out/timing.log" >&2
    exit 1
fi

# Nearest-rank p50/p95 over the settled edit samples. The first render is not
# part of this sequence.
grep '^TIMING ' "$out/timing.log" | awk '{print $3}' | sort -n | awk '
    { value[NR] = $1 }
    END {
        p50 = value[int((NR - 1) * 0.50) + 1]
        p95 = value[int((NR - 1) * 0.95) + 1]
        printf "samples=%d p50=%.0f ms p95=%.0f ms\n", NR, p50, p95
    }
'
echo "log: $out/timing.log"
