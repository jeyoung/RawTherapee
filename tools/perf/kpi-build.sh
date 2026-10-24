#!/usr/bin/env bash
# Measure build-time KPI: no-op, one module, one translation unit, ccache.
#
# Usage: tools/perf/kpi-build.sh --build-dir DIR [--report DIR]

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
source "$ROOT/tools/perf/lib/common.sh"

build_dir=""
report=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --report) report="$2"; shift 2 ;;
        *) die "unknown option: $1" ;;
    esac
done
[ -n "$build_dir" ] || die "--build-dir is required"
[ -n "$report" ] || report="$RUN/kpi-build"
mkdir -p "$report"

time_build() {
    local label="$1"; shift
    local start end
    start=$(date +%s%N)
    "$@" >"$report/$label.log" 2>&1
    end=$(date +%s%N)
    printf '%s\t%s\n' "$label" "$(( (end - start) / 1000000 ))"
}

printf 'metric\tms\n' >"$report/kpi.tsv"
time_build noop cmake --build "$build_dir" --target rth-cli -j 6 >>"$report/kpi.tsv"
touch "$ROOT/rtengine/stage/stage_lanczos.cc"
time_build module cmake --build "$build_dir" --target stage_lanczos -j 6 >>"$report/kpi.tsv"
touch "$ROOT/rtengine/ipresize.cc"
time_build one_tu cmake --build "$build_dir" --target rth-cli -j 6 >>"$report/kpi.tsv"

if command -v ccache >/dev/null 2>&1; then
    "$ROOT/tools/ccache-rt.sh" -s >"$report/ccache.txt" 2>&1 || true
fi

cat "$report/kpi.tsv"
