#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  Remove regenerable verification artefacts from build/test-scratch.
#
#  Usage:
#    tools/opencl/clean-scratch.sh            # list what would be removed
#    tools/opencl/clean-scratch.sh --apply    # remove it
#
#  The gate and export directories hold multi-hundred-megabyte TIFFs whose
#  measured figures are already recorded in devnotes/opencl/PARITY.md, so they
#  are disposable once a unit is recorded.
#
#  Never removed:
#    build/bench, build/*-rocm-bench, build/test-scratch/rocm-bench
#      benchmark inputs and outputs, owned by the benchmark workstream;
#    build/test-scratch/pocl-cache
#      the POCL kernel cache, which avoids recompiling every kernel;
#    build/ccache and build/rt-testdata
#      the compiler cache and the shared corpus.
#
set -euo pipefail

_rt_clean_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_rt_clean_scratch="$(cd "$_rt_clean_here/../.." && pwd)/build/test-scratch"

apply=0
case "${1:-}" in
    "") ;;
    --apply) apply=1 ;;
    *)
        printf 'usage: %s [--apply]\n' "$0" >&2
        exit 2
        ;;
esac

[ -d "$_rt_clean_scratch" ] || {
    printf 'nothing to clean: %s does not exist\n' "$_rt_clean_scratch"
    exit 0
}

targets=(
    "$_rt_clean_scratch/gate"
    "$_rt_clean_scratch/export"
    "$_rt_clean_scratch/export-lab"
    "$_rt_clean_scratch/export-xyz"
    "$_rt_clean_scratch/lwgate"
    "$_rt_clean_scratch/denoise-cur"
    "$_rt_clean_scratch/auto.tif"
)

printf 'scratch: %s\n' "$_rt_clean_scratch"
for target in "${targets[@]}"; do
    [ -e "$target" ] || continue
    if [ "$apply" -eq 1 ]; then
        rm -rf "$target"
        printf 'removed %s\n' "$target"
    else
        printf 'would remove %s\n' "$target"
    fi
done

while IFS= read -r artefact; do
    [ -n "$artefact" ] || continue
    if [ "$apply" -eq 1 ]; then
        rm -f "$artefact"
        printf 'removed %s\n' "$artefact"
    else
        printf 'would remove %s\n' "$artefact"
    fi
done < <(find "$_rt_clean_scratch" -maxdepth 1 \
    \( -name '*.log' -o -name '*.tif' -o -name '*.tiff' \) -type f)

if [ "$apply" -eq 0 ]; then
    printf 'dry run; pass --apply to remove the listed artefacts\n'
fi
