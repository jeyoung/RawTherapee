#!/usr/bin/env bash
# Compare a baseline build against a candidate build on one raw image.
#
# Usage:
#   tools/perf/test-baseline-parity.sh \
#       --baseline-dir build/cpu-dev \
#       --candidate-dir build/cpu-perf-graph \
#       --image build/rt-testdata/colorspace_flowers.pef \
#       [--report DIR] [--rtdata DIR]
#
# The VNG4, MONO and composed-profile rows must be pixel-exact (AE=0). The
# default neutral AMaZE row is KNOWN only when both VNG4 and MONO are exact and
# every differing sample lies within the AMaZE tile halo of a tile boundary or
# the image edge. Any other result is FAIL, and a FAIL makes the script exit
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
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
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
    if [ -f /usr/local/share/rawtherapee/dcraw.json ]; then
        rtdata="/usr/local/share/rawtherapee"
    else
        rtdata="$ROOT/rtdata"
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
# Bind target for the data tree; used only inside the bwrap namespace.
rtdata_stage="$report/run/rtdata-stage"
mkdir -p "$rtdata_stage"

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

# AMaZE runs in square tiles; the scratch reuse fix changes samples near the
# tile edges. AMAZETS is defined in rtengine/amaze_demosaic_RT.cc and the halo
# is the 16-pixel border each tile adds on every side. The stride is the tile
# advance. A differing sample is accepted only within the halo of a stride
# boundary or the right/bottom image edge.
amaze_ts="$(sed -n 's/^#define AMAZETS \([0-9][0-9]*\)$/\1/p' \
    "$ROOT/rtengine/amaze_demosaic_RT.cc" | head -n 1)"
[ -n "$amaze_ts" ] || die "cannot read AMAZETS from rtengine/amaze_demosaic_RT.cc"
amaze_halo=16
amaze_stride=$((amaze_ts - 2 * amaze_halo))
# A tile-boundary set must also be small: at most 0.01% of channel samples.
amaze_max_sample_fraction="1e-4"

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
use_bwrap=0
if command -v bwrap >/dev/null 2>&1; then
    use_bwrap=1
else
    warn "bwrap not found: running the CLIs directly; the data directories are"
    warn "not pinned, so install both share directories or provide bwrap"
fi

invoke() {
    if [ "$use_bwrap" -eq 1 ]; then
        # Stage the data outside /usr/local/share first: an --rtdata path under
        # the installed tree would otherwise disappear when the tmpfs masks its
        # parent. The second and third binds then expose the same tree at both
        # compiled data paths.
        bwrap --dev-bind / / \
            --bind "$rtdata" "$rtdata_stage" \
            --tmpfs /usr/local/share \
            --dir /usr/local/share/rawtherapee \
            --dir /usr/local/share/rawtherapee-graph \
            --bind "$rtdata_stage" /usr/local/share/rawtherapee \
            --bind "$rtdata_stage" /usr/local/share/rawtherapee-graph \
            -- "$@"
    else
        "$@"
    fi
}

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

# classify_amaze A B MAX DIFFERING TOTAL
# Prints: pixels samples outside allowed
classify_amaze() {
    python3 -B - "$ROOT" "$1" "$2" "$amaze_stride" "$amaze_halo" \
        "$amaze_max_sample_fraction" "$3" "$4" "$5" <<'PY'
import os
import sys

import numpy as np

root, a_path, b_path, stride, halo, fraction, max_lsb, differing, total = \
    sys.argv[1:]
sys.path.insert(0, os.path.join(root, "tools", "opencl"))
from compare_tiff import TiffReader  # noqa: E402

stride = int(stride)
halo = int(halo)
fraction = float(fraction)
reported_max = int(max_lsb)
reported_differing = int(differing)
reported_total = int(total)

a = TiffReader(a_path)
b = TiffReader(b_path)
try:
    pixels = 0
    samples = 0
    outside = 0
    observed_max = 0
    for y in range(0, a.height, 256):
        count = min(256, a.height - y)
        left = a.rows(y, count).astype(np.int32).reshape(
            count, a.width, a.samples)
        right = b.rows(y, count).astype(np.int32).reshape(
            count, a.width, a.samples)
        difference = np.abs(left - right)
        observed_max = max(observed_max, int(difference.max()))
        samples += int((difference > 0).sum())
        ys, xs = np.nonzero(difference.max(axis=2))
        for yy, xx in zip(ys, xs):
            yy = int(y + yy)
            xx = int(xx)
            pixels += 1
            dx = min(xx % stride, stride - (xx % stride), a.width - 1 - xx)
            dy = min(yy % stride, stride - (yy % stride), a.height - 1 - yy)
            if min(dx, dy) > halo:
                outside += 1
finally:
    a.close()
    b.close()

consistent = (observed_max == reported_max
              and samples == reported_differing
              and a.width * a.height * a.samples == reported_total)
limit = reported_total * fraction
allowed = consistent and outside == 0 and samples <= limit
print(pixels, samples, outside, int(allowed))
PY
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

    local max differing total
    set -- $result
    max="$1"
    differing="$3"
    total="$4"

    if [ "$max" -eq 0 ] && [ "$differing" -eq 0 ]; then
        record "$name" PASS "$max" "$differing"
    else
        record "$name" FAIL "$max" "$differing"
        failures=$((failures + 1))
    fi
    # Expose the exact counts to the AMaZE classification below.
    if [ "$name" = "vng4-neutral" ]; then vng4_differing="$differing"; fi
    if [ "$name" = "mono-neutral" ]; then mono_differing="$differing"; fi
}

vng4_differing=""
mono_differing=""

info "baseline parity: $(basename "$image")"
info "baseline  $baseline_cli"
info "candidate $candidate_cli"
info "rtdata    $rtdata (bwrap=$use_bwrap)"

exact_row vng4-neutral "$report/profiles/vng4-neutral.pp3"
exact_row mono-neutral "$report/profiles/mono-neutral.pp3"
exact_row vng4-sharpen-usm "$report/profiles/vng4-sharpen-usm.pp3"
exact_row vng4-rotate "$report/profiles/vng4-rotate.pp3"
exact_row vng4-crop "$report/profiles/vng4-crop.pp3"

# Default neutral profile: AMaZE. The known scratch-initialization difference
# is accepted only on the isolation evidence above and at tile boundaries.
name="amaze-default"
base="$report/tiff/baseline-$name.tif"
cand="$report/tiff/candidate-$name.tif"
log="$report/logs/compare-$name.log"
amaze_note=""
if ! export_row baseline "$baseline_dir" "" "$base" \
    || ! export_row candidate "$candidate_dir" "" "$cand"; then
    record "$name" FAIL "-" "-"
    failures=$((failures + 1))
else
    result=""
    rc=0
    if result="$(compare_pair "$base" "$cand" "$log")"; then
        rc=0
    else
        rc=$?
    fi
    if [ "$rc" -gt 1 ] || [ -z "$result" ]; then
        record "$name" FAIL "-" "-"
        failures=$((failures + 1))
    else
        set -- $result
        max="$1"
        differing="$3"
        total="$4"
        if [ "$max" -eq 0 ] && [ "$differing" -eq 0 ]; then
            record "$name" PASS "$max" "$differing"
        elif [ "${vng4_differing:-1}" -eq 0 ] \
            && [ "${mono_differing:-1}" -eq 0 ]; then
            if analysis="$(classify_amaze "$base" "$cand" "$max" \
                "$differing" "$total" 2>>"$log")"; then
                read -r pixels samples outside allowed <<<"$analysis"
                amaze_note="$pixels differing pixels, $samples samples, "
                amaze_note+="$outside outside the ${amaze_halo}px halo of the "
                amaze_note+="${amaze_stride}px tile stride"
                if [ "$allowed" -eq 1 ]; then
                    record "$name" KNOWN "$max" "$differing"
                    amaze_note="KNOWN: $amaze_note"
                else
                    record "$name" FAIL "$max" "$differing"
                    failures=$((failures + 1))
                    amaze_note="FAIL: $amaze_note"
                fi
            else
                record "$name" FAIL "$max" "$differing"
                failures=$((failures + 1))
                amaze_note="FAIL: tile-boundary analysis failed (see $log)"
            fi
        else
            record "$name" FAIL "$max" "$differing"
            failures=$((failures + 1))
            amaze_note="FAIL: VNG4/MONO isolation rows are not exact"
        fi
    fi
fi

# -------------------------------------------------------------------- report
printf '\n%-22s %-6s %9s %14s\n' "row" "status" "max LSB" "differing"
printf -- '------------------------------------------------------------------------\n'
while IFS=$'\t' read -r row_name status max differing; do
    printf '%-22s %-6s %9s %14s\n' "$row_name" "$status" "$max" "$differing"
done <"$rows"
if [ -n "$amaze_note" ]; then
    printf '\n%s\n' "$amaze_note"
fi
printf '\nreport: %s\n' "$report"

if [ "$failures" -ne 0 ]; then
    printf 'result: FAIL (%s row(s))\n' "$failures"
    exit 1
fi
printf 'result: PASS\n'
