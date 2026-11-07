#!/usr/bin/env bash
# Compare a baseline build against a candidate build on one raw image.
#
# Usage:
#   tools/perf/test-baseline-parity.sh \
#       --baseline-dir build/cpu-dev-amaze \
#       --candidate-dir build/cpu-perf-graph \
#       --image build/rt-testdata/colorspace_flowers.pef \
#       [--report DIR] [--rtdata DIR]
#
# The VNG4, MONO, composed-profile and default neutral AMaZE rows must all be
# pixel-exact (AE=0). Any difference is FAIL, and a FAIL makes the script exit
# non-zero.
#
# Both CLIs run with OMP_NUM_THREADS=1, --gpu=off and separate settings/cache.
# When bwrap is available, the same rtdata directory is bound to both
# /usr/local/share/rawtherapee and /usr/local/share/rawtherapee-graph so a data
# difference cannot explain a pixel difference. Without bwrap the CLIs run
# directly and the reduced isolation is reported.
#
# The default report directory is build/test-scratch/baseline-parity.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"

usage_error() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

baseline_dir=""
candidate_dir=""
image=""
report=""
rtdata=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --baseline-dir) baseline_dir="$2"; shift 2 ;;
        --candidate-dir) candidate_dir="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --report) report="$2"; shift 2 ;;
        --rtdata) rtdata="$2"; shift 2 ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) usage_error "unknown option: $1" ;;
    esac
done

[ -n "$baseline_dir" ] || usage_error "--baseline-dir is required"
[ -n "$candidate_dir" ] || usage_error "--candidate-dir is required"
[ -n "$image" ] || usage_error "--image is required"
[ -d "$baseline_dir" ] || usage_error "baseline build directory not found: $baseline_dir"
[ -d "$candidate_dir" ] || usage_error "candidate build directory not found: $candidate_dir"
[ -f "$image" ] || usage_error "image not found: $image"

report="${report:-$ROOT/build/test-scratch/baseline-parity}"
mkdir -p "$report"
report="$(realpath "$report")"

# Keep the harness scratch, settings, cache and logs inside the report.
export RT_RUN_DIR="$report/run"
# shellcheck source=lib/common.sh
source "$ROOT/tools/perf/lib/common.sh"

baseline_dir="$(realpath "$baseline_dir")"
candidate_dir="$(realpath "$candidate_dir")"
image="$(realpath "$image")"

if [ -z "$rtdata" ]; then
    if [ -f "$ROOT/rtdata/dcraw.json" ]; then
        rtdata="$ROOT/rtdata"
    else
        rtdata="/usr/local/share/rawtherapee"
    fi
fi
[ -d "$rtdata" ] || usage_error "rtdata directory not found: $rtdata"
rtdata="$(realpath "$rtdata")"
[ -f "$rtdata/dcraw.json" ] || \
    usage_error "not a RawTherapee rtdata directory (no dcraw.json): $rtdata"

baseline_cli="$(cli_for_build "$baseline_dir")"
candidate_cli="$(cli_for_build "$candidate_dir")"
require python3

warn() { printf 'warning: %s\n' "$*" >&2; }

compare_tiff="$ROOT/tools/opencl/compare_tiff.py"
export_one="$ROOT/tools/perf/export-one.sh"
[ -x "$export_one" ] || die "export-one.sh is not executable: $export_one"
[ -f "$compare_tiff" ] || die "compare_tiff.py not found: $compare_tiff"

profile_source="$ROOT/devnotes/opencl/pp3"
for profile in sharpen-usm rotate crop; do
    [ -f "$profile_source/$profile.pp3" ] || \
        die "profile not found: $profile_source/$profile.pp3"
done

mkdir -p "$report"/{logs,profiles,tiff,settings/baseline,settings/candidate,cache/baseline,cache/candidate}

# Seed the extension filter for the image under test in both settings trees.
# export-one.sh only seeds a missing options file, so a report directory reused
# with a different image extension would otherwise skip the image.
image_ext="$(printf '%s' "${image##*.}" | tr '[:upper:]' '[:lower:]')"
for side in baseline candidate; do
    cat >"$report/settings/$side/options" <<EOF
[General]
MultiUser=true

[File Browser]
ParseExtensions=$image_ext;
ParseExtensionsEnabled=1;
EOF
done

# ------------------------------------------------------------------ profiles
cat >"$report/profiles/vng4-neutral.pp3" <<'EOF'
[RAW Bayer]
Method=vng4
EOF

cat >"$report/profiles/mono-neutral.pp3" <<'EOF'
[RAW Bayer]
Method=mono
EOF

for name in sharpen-usm rotate crop; do
    {
        printf '[RAW Bayer]\nMethod=vng4\n\n'
        cat "$profile_source/$name.pp3"
    } >"$report/profiles/vng4-$name.pp3"
done

# -------------------------------------------------------------------- runner
command -v bwrap >/dev/null 2>&1 || \
    die "bwrap is required to bind the same rtdata to both data paths"
use_bwrap=1

# A data tree under /usr/local/share would be hidden by the tmpfs that pins
# the two compiled data paths. Copy that case to a private staging tree first;
# a repository rtdata directory can be bound directly.
bind_source="$rtdata"
if [ "$use_bwrap" -eq 1 ] && [[ "$rtdata" == /usr/local/share/* ]]; then
    rtdata_stage="$report/run/rtdata-stage"
    rm -rf "$rtdata_stage"
    mkdir -p "$rtdata_stage"
    cp -a "$rtdata/." "$rtdata_stage/"
    bind_source="$rtdata_stage"
fi

invoke() {
    if [ "$use_bwrap" -eq 1 ]; then
        bwrap --dev-bind / / \
            --tmpfs /usr/local/share \
            --dir /usr/local/share/rawtherapee \
            --dir /usr/local/share/rawtherapee-graph \
            --bind "$bind_source" /usr/local/share/rawtherapee \
            --bind "$bind_source" /usr/local/share/rawtherapee-graph \
            -- "$@"
    else
        "$@"
    fi
}

if [ "$use_bwrap" -eq 1 ]; then
    invoke test -f /usr/local/share/rawtherapee/dcraw.json || \
        die "bwrap data bind is empty for the baseline path"
    invoke test -f /usr/local/share/rawtherapee-graph/dcraw.json || \
        die "bwrap data bind is empty for the candidate path"
fi

# export_row SIDE BUILD_DIR PROFILE OUTPUT
export_row() {
    local side="$1" build_dir="$2" profile="$3" out="$4"
    local -a args=(--build-dir "$build_dir" --image "$image" --out "$out"
                   --gpu off --threads 1)
    if [ -n "$profile" ]; then
        args+=(--pp3 "$profile")
    fi
    (
        export RT_SETTINGS="$report/settings/$side"
        export RT_CACHE="$report/cache/$side"
        export RT_EXPORT_LOG="$report/logs/$side-$(basename "$out" .tif).log"
        invoke "$export_one" "${args[@]}"
    )
}

# compare_pair A B LOG
# Prints "max normalized differing total" on success. compare_tiff.py exits 1
# when the maximum exceeds the tolerance, so the caller handles the status.
compare_pair() {
    python3 -B "$compare_tiff" "$1" "$2" --bits 16 2>"$3"
}

# ---------------------------------------------------------------------- rows
rows="$report/rows.tsv"
: >"$rows"
failures=0

# record NAME STATUS MAX DIFFERING
record() {
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >>"$rows"
}

# exact_row NAME PROFILE
exact_row() {
    local name="$1" profile="$2"
    local base="$report/tiff/baseline-$name.tif"
    local cand="$report/tiff/candidate-$name.tif"
    local log="$report/logs/compare-$name.log"

    if ! export_row baseline "$baseline_dir" "$profile" "$base" \
        || ! export_row candidate "$candidate_dir" "$profile" "$cand"; then
        record "$name" FAIL "-" "-"
        failures=$((failures + 1))
        return
    fi

    local result rc
    if result="$(compare_pair "$base" "$cand" "$log")"; then
        rc=0
    else
        rc=$?
    fi
    if [ "$rc" -gt 1 ] || [ -z "$result" ]; then
        record "$name" FAIL "-" "-"
        failures=$((failures + 1))
        return
    fi

    local max differing
    set -- $result
    max="$1"
    differing="$3"

    if [ "$max" -eq 0 ] && [ "$differing" -eq 0 ]; then
        record "$name" PASS "$max" "$differing"
    else
        record "$name" FAIL "$max" "$differing"
        failures=$((failures + 1))
    fi
}

info "baseline parity: $(basename "$image")"
info "baseline  $baseline_cli"
info "candidate $candidate_cli"
info "rtdata    $rtdata (bwrap=$use_bwrap)"

exact_row vng4-neutral "$report/profiles/vng4-neutral.pp3"
exact_row mono-neutral "$report/profiles/mono-neutral.pp3"
exact_row vng4-sharpen-usm "$report/profiles/vng4-sharpen-usm.pp3"
exact_row vng4-rotate "$report/profiles/vng4-rotate.pp3"
exact_row vng4-crop "$report/profiles/vng4-crop.pp3"

# Default neutral profile: AMaZE.
exact_row amaze-default ""

# -------------------------------------------------------------------- report
printf '\n%-22s %-6s %9s %14s\n' "row" "status" "max LSB" "differing"
printf -- '------------------------------------------------------------------------\n'
while IFS=$'\t' read -r row_name status max differing; do
    printf '%-22s %-6s %9s %14s\n' "$row_name" "$status" "$max" "$differing"
done <"$rows"
printf '\nreport: %s\n' "$report"

if [ "$failures" -ne 0 ]; then
    printf 'result: FAIL (%s row(s))\n' "$failures"
    exit 1
fi
printf 'result: PASS\n'
