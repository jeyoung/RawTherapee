#!/usr/bin/env bash
#
# Export determinism guard: compare thread counts and a repeat run.
#
# Usage:
#   tools/perf/test-export-determinism.sh <raw> [pp3]
#
# Environment:
#   RT_BUILD_DIR   configured build directory (default: build/cpu-perf-graph)
#   RT_OUT         scratch directory (default: build/test-scratch/determinism)
#
# The export is 16-bit TIFF with `--gpu=off` and an isolated settings/cache
# directory, so application defaults cannot change between runs. The gate is
# the project's 1 LSB16 parity tolerance: the gross thread race this guard was
# written for moved pixels by up to 77/361 codes, while the residual 1-code
# difference is reported as a count. A run that exceeds 1 LSB fails.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image="${1:-}"
pp3="${2:-}"
build_dir="${RT_BUILD_DIR:-$root/build/cpu-perf-graph}"
out="${RT_OUT:-$root/build/test-scratch/determinism}"

if [ -z "$image" ]; then
    echo "usage: $0 <raw> [pp3]" >&2
    exit 2
fi
[ -f "$image" ] || { echo "not a file: $image" >&2; exit 2; }
[ -z "$pp3" ] || [ -f "$pp3" ] || { echo "missing profile: $pp3" >&2; exit 2; }

cli="$build_dir/rtgui/rawtherapee-graph-cli"
[ -x "$cli" ] || { echo "missing CLI: $cli" >&2; exit 2; }

rm -rf "$out"
mkdir -p "$out/settings" "$out/cache"

# A fresh RT_SETTINGS has no extension filter, so the CLI would skip the
# image. Seed only the extension this run needs; every pixel-affecting value
# otherwise stays at the application default.
ext="$(printf '%s' "${image##*.}" | tr '[:upper:]' '[:lower:]')"
cat >"$out/settings/options" <<EOF
[General]
MultiUser=true

[File Browser]
ParseExtensions=$ext;
ParseExtensionsEnabled=1;
EOF

profile_args=()
[ -n "$pp3" ] && profile_args=(-p "$pp3")

export_one() {
    local threads="$1"
    local name="$2"
    mkdir -p "$out/cache/$name"
    OMP_NUM_THREADS="$threads" RT_SETTINGS="$out/settings" \
        RT_CACHE="$out/cache/$name" "$cli" --gpu=off "${profile_args[@]}" \
        -o "$out/$name.tif" -t -b16 -Y -c "$image" >/dev/null 2>&1
}

export_one 1 t1
for threads in 4 8 8b; do
    case "$threads" in
    8b) export_one 8 t8b ;;
    *) export_one "$threads" "t$threads" ;;
    esac
done

fail=0
printf 'compare\tresult\tdiffering\tmax_abs\tpae\n'
for pair in "t1 t4" "t1 t8" "t8 t8b"; do
    set -- $pair
    metric="$(python3 "$root/tools/opencl/compare_tiff.py" \
        "$out/$1.tif" "$out/$2.tif" --bits 16)" || true
    if [ -z "$metric" ]; then
        printf '%s-vs-%s\tFAIL (comparison error)\n' "$1" "$2"
        fail=1
        continue
    fi
    read -r max_abs pae differing total <<<"$metric"
    if [ "${max_abs:-999999}" -gt 1 ]; then
        result="FAIL"
        fail=1
    else
        result="PASS"
    fi
    printf '%s-vs-%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$result" "$differing" \
        "$max_abs" "$pae"
done

exit "$fail"
