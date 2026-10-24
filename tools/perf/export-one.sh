#!/usr/bin/env bash
# Export one image through the CLI with a canonical, reproducible invocation.
#
# Usage:
#   export-one.sh --build-dir DIR --image FILE --out FILE [options]
#
# Options:
#   --pp3 FILE       Apply a profile with -p
#   --sidecar        Apply the image sidecar with -s
#   --gpu off|auto   GPU mode (default: off)
#   --threads N      OMP_NUM_THREADS (default: 1)
#   --fast           Use the fast-export pipeline (-f)
#
# The image must be the last argument to the CLI (-c must be last).

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
source "$ROOT/tools/perf/lib/common.sh"

build_dir=""
image=""
out=""
pp3=""
sidecar=0
gpu="off"
threads=1
fast=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --pp3) pp3="$2"; shift 2 ;;
        --sidecar) sidecar=1; shift ;;
        --gpu) gpu="$2"; shift 2 ;;
        --threads) threads="$2"; shift 2 ;;
        --fast) fast=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$build_dir" ] || die "--build-dir is required"
[ -n "$image" ] || die "--image is required"
[ -n "$out" ] || die "--out is required"
[ -f "$image" ] || die "image not found: $image"
[ -f "$pp3" ] || [ -z "$pp3" ] || die "profile not found: $pp3"

cli="$(cli_for_build "$build_dir")"
mkdir -p "$(dirname "$out")"
log="${RT_EXPORT_LOG:-$RUN/logs/export-$(basename "$out").log}"

args=(--gpu="$gpu")
if [ "$fast" -eq 1 ]; then
    args+=(-f)
fi
if [ "$sidecar" -eq 1 ]; then
    args+=(-s)
elif [ -n "$pp3" ]; then
    args+=(-p "$pp3")
fi
args+=(-o "$out" -t -b16 -Y -c "$image")

info "export $(basename "$image") -> $out (threads=$threads gpu=$gpu)"
OMP_NUM_THREADS="$threads" "$cli" "${args[@]}" >"$log" 2>&1 || {
    tail -20 "$log" >&2
    die "export failed"
}
[ -s "$out" ] || die "export produced no output: $out"
