#!/usr/bin/env bash
# Standard gate for stage, export and preview work.
#
# Usage:
#   tools/perf/run-suite.sh --build-dir build/cpu-perf-stage-runtime \
#       --image "$RT_TEST_IMAGE_45MP" [--pp3 FILE] [--baseline-dir DIR] \
#       [--mode smoke|gate] [--report DIR] [--skip-preview] [--skip-stage-tests]

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
source "$ROOT/tools/perf/lib/common.sh"

build_dir=""
baseline_dir="$ROOT/../../baseline-dev"
image=""
pp3=""
mode="smoke"
report=""
skip_preview=0
skip_stage_tests=0
threads_list="1 2 4 8"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --baseline-dir) baseline_dir="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --pp3) pp3="$2"; shift 2 ;;
        --mode) mode="$2"; shift 2 ;;
        --report) report="$2"; shift 2 ;;
        --skip-preview) skip_preview=1; shift ;;
        --skip-stage-tests) skip_stage_tests=1; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$build_dir" ] || die "--build-dir is required"
[ -n "$image" ] || image="$(test_image)"
[ -f "$image" ] || die "image not found: $image"
[ -n "$report" ] || report="$RUN/report"

mkdir -p "$report"
export RT_TEST_IMAGE_45MP="$image"
export RT_TEST_PP3_45MP="$pp3"
export RT_PREVIEW_DISPLAY="${RT_PREVIEW_DISPLAY:-:98}"

cli="$(cli_for_build "$build_dir")"
baseline_cli=""
if [ -x "$baseline_dir/rtgui/rawtherapee-cli" ]; then
    baseline_cli="$baseline_dir/rtgui/rawtherapee-cli"
fi

failures=0
pass() { printf 'PASS\t%s\n' "$*"; }
fail() { printf 'FAIL\t%s\n' "$*"; failures=$((failures + 1)); }
skip() { printf 'SKIP\t%s\n' "$*"; }

# ---------------------------------------------------------------- stage tests
info "stage tests"
if [ "$skip_stage_tests" -eq 0 ]; then
    for target in stage-abi-test stage-parity-test stage-cache-test stage-graph-test stage-indexer-test; do
        if cmake --build "$build_dir" --target "$target" -j 6 >"$report/$target.build.log" 2>&1 \
            && "$build_dir/rtengine/$target" >"$report/$target.log" 2>&1; then
            pass "$target"
        else
            fail "$target (see $report/$target.log)"
        fi
    done
else
    skip "stage tests (--skip-stage-tests)"
fi

# ------------------------------------------------------------------- exports
info "export parity"
neutral="$report/neutral.tif"
sidecar="$report/sidecar.tif"
base_neutral="$report/base-neutral.tif"
base_sidecar="$report/base-sidecar.tif"

"$ROOT/tools/perf/export-one.sh" --build-dir "$build_dir" --image "$image" \
    --out "$neutral" --gpu off --threads 1

if [ -n "$baseline_cli" ]; then
    "$ROOT/tools/perf/export-one.sh" --build-dir "$baseline_dir" --image "$image" \
        --out "$base_neutral" --gpu off --threads 1
    if result=$(python3 "$ROOT/tools/opencl/compare_tiff.py" "$base_neutral" "$neutral" --bits 16); then
        set -- $result
        if [ "$3" = "0" ]; then pass "neutral export AE=0"; else fail "neutral export differing=$3"; fi
    else
        fail "neutral export comparison"
    fi
else
    skip "neutral parity: no baseline CLI"
fi

if [ -n "$pp3" ]; then
    "$ROOT/tools/perf/export-one.sh" --build-dir "$build_dir" --image "$image" \
        --out "$sidecar" --pp3 "$pp3" --gpu off --threads 1
    if [ -n "$baseline_cli" ]; then
        "$ROOT/tools/perf/export-one.sh" --build-dir "$baseline_dir" --image "$image" \
            --out "$base_sidecar" --pp3 "$pp3" --gpu off --threads 1
        if result=$(python3 "$ROOT/tools/opencl/compare_tiff.py" "$base_sidecar" "$sidecar" --bits 16); then
            set -- $result
            if [ "$3" = "0" ]; then pass "sidecar export AE=0"; else fail "sidecar export differing=$3"; fi
        else
            fail "sidecar export comparison"
        fi
    fi
else
    skip "sidecar parity: no profile supplied"
fi

# -------------------------------------------------------------- thread matrix
info "thread determinism"
ref="$neutral"
for threads in $threads_list; do
    [ "$threads" = "1" ] && continue
    out="$report/neutral-t$threads.tif"
    "$ROOT/tools/perf/export-one.sh" --build-dir "$build_dir" --image "$image" \
        --out "$out" --gpu off --threads "$threads"
    if result=$(python3 "$ROOT/tools/opencl/compare_tiff.py" "$ref" "$out" --bits 16); then
        set -- $result
        if [ "$3" = "0" ]; then pass "thread determinism t=$threads"; else fail "thread determinism t=$threads differing=$3"; fi
    else
        fail "thread determinism comparison t=$threads"
    fi
done

# ------------------------------------------------------------------- timing
info "export timing"
timing_file="$report/timing.tsv"
printf 'threads\trun\tms\n' >"$timing_file"
for threads in 1 8; do
    for run in 1 2 3; do
        out="$report/timing-t$threads-$run.tif"
        start=$(date +%s%N)
        "$ROOT/tools/perf/export-one.sh" --build-dir "$build_dir" --image "$image" \
            --out "$out" --gpu off --threads "$threads" >/dev/null
        end=$(date +%s%N)
        printf '%s\t%s\t%s\n' "$threads" "$run" "$(( (end - start) / 1000000 ))" >>"$timing_file"
    done
done

# ------------------------------------------------------------------- preview
if [ "$skip_preview" -eq 0 ] && [ -x "$build_dir/rtgui/rawtherapee" ] \
    && command -v Xvfb >/dev/null 2>&1 && command -v xdotool >/dev/null 2>&1; then
    info "preview latency"
    if RT_PREVIEW_DELAY=20 RT_PREVIEW_WAIT=6 \
        "$ROOT/tools/perf/bench-preview.sh" "$build_dir/rtgui/rawtherapee" "$image" 5 \
        >"$report/preview.log" 2>&1; then
        pass "preview latency run"
    else
        fail "preview latency run (see $report/preview.log)"
    fi
else
    skip "preview latency: GUI/Xvfb not available or --skip-preview"
fi

# -------------------------------------------------------------------- report
commit="$(git -C "$ROOT" rev-parse --short HEAD)"
image_sha="$(sha256_file "$image")"
build_name="$(basename "$build_dir")"
export REPORT="$report" COMMIT="$commit" IMAGE_SHA="$image_sha" BUILD_NAME="$build_name" \
       FAILURES="$failures" MODE="$mode" THREADS="$threads_list"
python3 - <<'PY'
import json, os
from pathlib import Path

report = Path(os.environ["REPORT"])
timing = []
path = report / "timing.tsv"
if path.exists():
    lines = path.read_text().strip().splitlines()[1:]
    for line in lines:
        threads, run, ms = line.split("\t")
        timing.append({"threads": int(threads), "run": int(run), "ms": int(ms)})

data = {
    "schema": 1,
    "commit": os.environ["COMMIT"],
    "build": os.environ["BUILD_NAME"],
    "mode": os.environ["MODE"],
    "image_sha256": os.environ["IMAGE_SHA"],
    "threads": os.environ["THREADS"],
    "timing": timing,
    "failures": int(os.environ["FAILURES"]),
    "result": "PASS" if int(os.environ["FAILURES"]) == 0 else "FAIL",
}
(report / "report.json").write_text(json.dumps(data, indent=2) + "\n")
print(json.dumps(data, indent=2))
PY

if [ "$failures" -ne 0 ]; then
    die "$failures gate(s) failed"
fi

info "suite PASS; report in $report"
