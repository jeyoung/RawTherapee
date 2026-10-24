#!/usr/bin/env bash
# Interleaved baseline/candidate export timing.
#
# Usage:
#   tools/perf/compare-timing.sh --baseline-dir DIR --build-dir DIR \
#       --image FILE [--pp3 FILE] [--threads 8] [--runs 3] [--out DIR]

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
source "$ROOT/tools/perf/lib/common.sh"

baseline_dir=""
build_dir=""
image=""
pp3=""
threads=8
runs=3
out=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --baseline-dir) baseline_dir="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --pp3) pp3="$2"; shift 2 ;;
        --threads) threads="$2"; shift 2 ;;
        --runs) runs="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$baseline_dir" ] || die "--baseline-dir is required"
[ -n "$build_dir" ] || die "--build-dir is required"
[ -n "$image" ] || image="$(test_image)"
[ -f "$image" ] || die "image not found: $image"
[ -n "$out" ] || out="$RUN/timing"
mkdir -p "$out/out"

printf 'run\tvariant\tms\n' >"$out/timing.tsv"
for run in $(seq 1 "$runs"); do
    for variant in baseline candidate; do
        if [ "$variant" = baseline ]; then
            dir="$baseline_dir"
        else
            dir="$build_dir"
        fi
        target="$out/out/$variant-$run.tif"
        args=(--build-dir "$dir" --image "$image" --out "$target"
              --gpu off --threads "$threads")
        if [ -n "$pp3" ]; then
            args+=(--pp3 "$pp3")
        fi
        start=$(date +%s%N)
        "$ROOT/tools/perf/export-one.sh" "${args[@]}" >/dev/null
        end=$(date +%s%N)
        ms=$(( (end - start) / 1000000 ))
        printf '%s\t%s\t%s\n' "$run" "$variant" "$ms" >>"$out/timing.tsv"
        printf '%s run %s: %s ms\n' "$variant" "$run" "$ms"
    done
done

python3 - "$out/timing.tsv" "$out/summary.json" <<'PY'
import json
import statistics
import sys

rows = []
with open(sys.argv[1], encoding="utf-8") as handle:
    next(handle)
    for line in handle:
        run, variant, ms = line.rstrip("\n").split("\t")
        rows.append((variant, int(ms)))

summary = {}
for variant in ("baseline", "candidate"):
    values = [ms for name, ms in rows if name == variant]
    summary[variant] = {
        "samples": len(values),
        "median_ms": statistics.median(values) if values else None,
        "min_ms": min(values) if values else None,
        "max_ms": max(values) if values else None,
    }

if summary["baseline"]["median_ms"] and summary["candidate"]["median_ms"]:
    summary["speedup"] = (
        summary["baseline"]["median_ms"] / summary["candidate"]["median_ms"]
    )

with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2)
    handle.write("\n")
print(json.dumps(summary, indent=2))
PY
