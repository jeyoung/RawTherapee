#!/usr/bin/env bash
# Verify the fast-export (`-f`) stage graph.
#
# Usage:
#   tools/perf/test-fast-export-graph.sh --build-dir DIR --image FILE \
#       --pp3 FILE [--baseline FILE] [--report DIR]
#
# The check has two parts. First, a fast export with RT_STAGE_PROFILE enabled
# must emit one record per fast node, each carrying an impl field. Second, when
# --baseline names a pre-change fast-export TIFF, the new output must match it
# with AE=0.
#
# The profile must enable Resize; otherwise the CLI takes the normal pipeline
# and this test has nothing to measure.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
source "$ROOT/tools/perf/lib/common.sh"

build_dir=""
image=""
pp3=""
baseline=""
report=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --pp3) pp3="$2"; shift 2 ;;
        --baseline) baseline="$2"; shift 2 ;;
        --report) report="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$build_dir" ] || die "--build-dir is required"
[ -n "$image" ] || die "--image is required"
[ -n "$pp3" ] || die "--pp3 is required"
[ -f "$image" ] || die "image not found: $image"
[ -f "$pp3" ] || die "profile not found: $pp3"
[ -z "$baseline" ] || [ -f "$baseline" ] || die "baseline not found: $baseline"

[ -n "$report" ] || report="$RUN/fast-export-graph"
mkdir -p "$report"

# common.sh points RT_SETTINGS at an empty scratch directory. The CLI filters
# input extensions from that options file, so seed it with the one extension
# this run needs; otherwise a first run in a clean tree skips the image.
if [ ! -f "$RT_SETTINGS/options" ]; then
    ext="$(printf '%s' "${image##*.}" | tr '[:upper:]' '[:lower:]')"
    mkdir -p "$RT_SETTINGS"
    cat >"$RT_SETTINGS/options" <<EOF
[General]
MultiUser=true

[File Browser]
ParseExtensions=${ext};
ParseExtensionsEnabled=1;
EOF
fi

out="$report/fast-export.tif"
profile="$report/fast-export.jsonl"
log="$report/fast-export.log"

rm -f "$profile"
cli="$(cli_for_build "$build_dir")"
OMP_NUM_THREADS=1 RT_STAGE_PROFILE="$profile" \
    "$cli" --gpu=off -f -a -p "$pp3" -o "$out" -t -b16 -Y -c "$image" \
    >"$log" 2>&1 || {
    tail -20 "$log" >&2
    die "fast export failed"
}
[ -s "$out" ] || die "fast export produced no output: $out"

failures=0
fail() { printf 'FAIL\t%s\n' "$*"; failures=$((failures + 1)); }
pass() { printf 'PASS\t%s\n' "$*"; }

nodes=(
    source.select raw.preprocess raw.demosaic raw.base.source raw.color
    analysis transform early.resize denoise finish
)

for node in "${nodes[@]}"; do
    if grep -q "\"stage\":\"$node\",\"impl\":\"[^\"]*\"" "$profile"; then
        pass "fast node recorded: $node"
    else
        fail "fast node has no impl record: $node"
    fi
done

if grep -q 'stage-graph: fast-export active, 10 nodes' "$log"; then
    pass "fast-export graph executed its ten nodes"
else
    fail "fast-export graph did not report its ten nodes"
fi

if [ -n "$baseline" ]; then
    result="$(python3 "$ROOT/tools/opencl/compare_tiff.py" \
        "$baseline" "$out" --bits 16)"
    set -- $result
    differing="$3"
    if [ "$differing" = "0" ]; then
        pass "fast-export parity AE=0"
    else
        fail "fast-export parity differing=$differing"
    fi
else
    printf 'SKIP\tfast-export parity (no --baseline)\n'
fi

[ "$failures" -eq 0 ]
