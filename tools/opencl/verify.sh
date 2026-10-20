#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  OpenCL and format verification helper.
#
#  Usage:
#    tools/opencl/verify.sh --quick
#    tools/opencl/verify.sh --formats
#    tools/opencl/verify.sh --list-coverage
#
#  Environment:
#    RT_BUILD_DIR   Build directory containing rtgui/rawtherapee-cli
#    RT_TESTDATA    Test image directory (default: $HOME/rt-testdata)
#    RT_REPORT      Output directory for CSV results (default: $RT_TESTDATA)
#
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
readonly MANIFEST="$ROOT/devnotes/opencl/testdata.manifest"
readonly TESTDATA="${RT_TESTDATA:-$HOME/rt-testdata}"
readonly REPORT="${RT_REPORT:-$TESTDATA}"
readonly PAE_LIMIT="${RT_PAE_LIMIT:-0.0000165}"   # 1 LSB at 16-bit
readonly THREADS="${RT_VERIFY_THREADS:-1}"

mode=""
stage_name=""
cli=""
sample_override=""
compare_cmd=()
has_device=0

usage() {
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

find_cli() {
    if [ -n "${RT_BUILD_DIR:-}" ]; then
        cli="$RT_BUILD_DIR/rtgui/rawtherapee-cli"
    else
        cli="$(find "$ROOT/build" -maxdepth 2 -path '*/rtgui/rawtherapee-cli' -type f -executable 2>/dev/null | sort | tail -1 || true)"
    fi

    [ -n "$cli" ] && [ -x "$cli" ] || die "rawtherapee-cli not found; set RT_BUILD_DIR"

    if command -v magick >/dev/null 2>&1; then
        compare_cmd=(magick compare)
    elif command -v compare >/dev/null 2>&1; then
        compare_cmd=(compare)
    else
        die "ImageMagick (magick or compare) is required"
    fi
}

LAST_PAE=""

compare_images() {
    local a="$1"
    local b="$2"

    LAST_PAE=""
    [ -f "$a" ] && [ -f "$b" ] || return 1

    local size_a size_b
    size_a="$(identify -format '%wx%h' "$a" 2>/dev/null || true)"
    size_b="$(identify -format '%wx%h' "$b" 2>/dev/null || true)"
    [ "$size_a" = "$size_b" ] || return 1

    local pixels
    pixels="$(identify -format '%[fx:w*h]' "$a" 2>/dev/null | awk '{printf "%d", $1}')"

    if [ "${pixels:-0}" -gt 30000000 ] && command -v python3 >/dev/null 2>&1 \
            && python3 -c 'import numpy' >/dev/null 2>&1; then
        local bits
        bits="$(identify -format '%[bit-depth]' "$a" 2>/dev/null || echo 16)"
        local metric rc=0
        metric="$(python3 "$SCRIPT_DIR/compare_tiff.py" "$a" "$b" --bits "$bits")" || rc=$?
        LAST_PAE="$(printf '%s\n' "$metric" | awk '{print $2}')"
        return "$rc"
    fi

    local pae
    pae="$("${compare_cmd[@]}" -metric PAE "$a" "$b" null: 2>&1 || true)"
    if [[ "$pae" == *"("*")"* ]]; then
        pae="${pae##*(}"
        pae="${pae%%)*}"
    else
        pae="${pae%% *}"
    fi
    LAST_PAE="$pae"

    awk -v m="$pae" -v limit="$PAE_LIMIT" \
        'BEGIN { exit (m != "" && m <= limit ? 0 : 1) }'
}

detect_device() {
    if "$cli" --gpu-info 2>/dev/null | grep -q '\* \['; then
        has_device=1
    else
        has_device=0
    fi
}

skip_if_no_device() {
    if [ "$has_device" -eq 0 ] && [ "${RT_ALLOW_CPU_PARITY:-0}" != "1" ]; then
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\t%s\tno OpenCL device\n' "$1"
            return 1
        fi
        printf 'SKIP\t%s\tno OpenCL device (set RT_ALLOW_CPU_PARITY=1 to compare CPU paths)\n' "$1"
        return 2
    fi
    return 0
}

run_pair() {
    local label="$1"
    local input="$2"
    local profile="${3:-}"
    local skip_status=0
    skip_if_no_device "$label" || skip_status=$?
    if [ "$skip_status" -ne 0 ]; then
        if [ "$skip_status" -eq 1 ]; then
            return 1
        fi
        return 0
    fi

    local outdir
    outdir="$(mktemp -d "${TMPDIR:-/tmp}/rtverify.XXXXXX")"

    local off="$outdir/off.tif"
    local auto="$outdir/auto.tif"
    local -a profileArg=()

    if [ -n "$profile" ]; then
        profileArg=(-p "$profile")
    fi

    if ! OMP_NUM_THREADS="$THREADS" "$cli" --gpu=off "${profileArg[@]}" -o "$off" -t -b16 -Y -c "$input" \
            >"$outdir/off.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (gpu=off)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi

    if ! OMP_NUM_THREADS="$THREADS" "$cli" --gpu=auto "${profileArg[@]}" -o "$auto" -t -b16 -Y -c "$input" \
            >"$outdir/auto.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (gpu=auto)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi

    local result="PASS"
    if ! compare_images "$off" "$auto"; then
        result="FAIL"
    fi

    printf '%s\t%s\tmax_pae=%s\n' "$result" "$label" "${LAST_PAE:-unknown}"

    rm -rf "$outdir"
    [ "$result" = "PASS" ]
}

quick() {
    local sample="${sample_override:-$TESTDATA/colorspace_flowers.pef}"
    [ -f "$sample" ] || die "missing $sample (run tools/opencl/fetch-testdata.sh)"

    printf 'binary\t%s\n' "$cli"
    printf 'formats\t%s\n' "$("$cli" --list-formats | tail -n +2 | tr '\n' ' ')"
    "$cli" --gpu-info | tail -n +2 | sed 's/^/gpu\t/'
    detect_device

    run_pair "pef/neutral" "$sample"
}

stage() {
    local name="$1"
    local sample="${sample_override:-$TESTDATA/colorspace_flowers.pef}"
    local profile="$ROOT/devnotes/opencl/pp3/$name.pp3"

    [ -f "$sample" ] || die "missing $sample (run tools/opencl/fetch-testdata.sh)"
    [ -f "$profile" ] || die "missing profile $profile"

    printf 'binary\t%s\n' "$cli"
    "$cli" --gpu-info | tail -n +2 | sed 's/^/gpu\t/'
    detect_device
    run_pair "$(basename "$sample")/$name" "$sample" "$profile"
}

formats() {
    [ -f "$MANIFEST" ] || die "missing manifest: $MANIFEST"
    detect_device

    if [ "$has_device" -eq 0 ] && [ "${RT_ALLOW_CPU_PARITY:-0}" != "1" ]; then
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\tformats\tno OpenCL device\n'
            return 1
        fi
        printf 'SKIP\tformats\tno OpenCL device (set RT_ALLOW_CPU_PARITY=1 to compare CPU paths)\n'
        return 0
    fi

    local failures=0
    local rows=0

    while IFS=$'\t' read -r ext file variant _sha _source _licence; do
        case "$ext" in
            ''|'#'*) continue ;;
        esac

        local path="$TESTDATA/$file"
        rows=$((rows + 1))

        if [ ! -f "$path" ]; then
            printf 'NO_SAMPLE\t%s/%s\t%s\n' "$ext" "$variant" "$file"
            continue
        fi

        if ! run_pair "$ext/$variant" "$path"; then
            failures=$((failures + 1))
        fi
    done < "$MANIFEST"

    [ "$rows" -gt 0 ] || die "manifest has no entries"
    printf 'checked %d entr(ies), %d failure(s)\n' "$rows" "$failures"
    [ "$failures" -eq 0 ]
}

list_coverage() {
    [ -f "$MANIFEST" ] || die "missing manifest: $MANIFEST"

    local formats covered missing
    formats="$("$cli" --list-formats | tail -n +2)"
    covered="$(awk -F'\t' '!/^#/ && NF > 0 {print $1}' "$MANIFEST" | sort -u)"
    missing="$(comm -23 <(printf '%s\n' "$formats" | sort -u) <(printf '%s\n' "$covered"))"

    if [ -n "$missing" ]; then
        printf 'missing samples:\n%s\n' "$missing"
        return 1
    fi

    printf 'all %d supported extensions have a manifest entry\n' \
        "$(printf '%s\n' "$formats" | wc -l)"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --quick) mode="quick" ;;
        --formats) mode="formats" ;;
        --stage)
            [ "$#" -ge 2 ] || die "--stage needs a name"
            mode="stage"
            stage_name="$2"
            shift
            ;;
        --image)
            [ "$#" -ge 2 ] || die "--image needs a path"
            sample_override="$2"
            shift
            ;;
        --list-coverage) mode="coverage" ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1'" ;;
    esac
    shift
done

[ -n "$mode" ] || { usage; exit 1; }
find_cli
mkdir -p "$REPORT"

case "$mode" in
    quick) quick ;;
    formats) formats ;;
    stage) stage "$stage_name" ;;
    coverage) list_coverage ;;
esac
