#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  OpenCL and format verification helper.
#
#  Usage:
#    tools/opencl/verify.sh --quick
#    tools/opencl/verify.sh --formats
#    tools/opencl/verify.sh --monitor
#    tools/opencl/verify.sh --rgbproc
#    tools/opencl/verify.sh --stage-device
#    tools/opencl/verify.sh --list-coverage
#
#  Environment:
#    RT_BUILD_DIR   Build directory containing rtgui/rawtherapee-graph-cli
#    RT_TESTDATA    Test image directory (default: $HOME/rt-testdata)
#    RT_REPORT      Output directory for CSV results (default: $RT_TESTDATA)
#    RT_OPENCL_ALLOW_CPU=1  Select a CPU OpenCL device for testing
#    RT_ALLOW_CPU_PARITY=1 Compare CPU paths when no OpenCL device is selected
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
device_is_cpu=0
expect_stage=0
expect_stage_name=""
expect_stage_absent=""

usage() {
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

find_cli() {
    if [ -n "${RT_BUILD_DIR:-}" ]; then
        cli="$RT_BUILD_DIR/rtgui/rawtherapee-graph-cli"
    else
        cli="$(find "$ROOT/build" -maxdepth 2 -path '*/rtgui/rawtherapee-graph-cli' -type f -executable 2>/dev/null | sort | tail -1 || true)"
    fi

    [ -n "$cli" ] && [ -x "$cli" ] || die "rawtherapee-graph-cli not found; set RT_BUILD_DIR"

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
    local selected
    selected="$("$cli" --gpu-info 2>/dev/null | grep '\* \[' | head -1 || true)"
    if [ -n "$selected" ]; then
        has_device=1
        case "$selected" in
            *"[CPU]"*) device_is_cpu=1 ;;
            *) device_is_cpu=0 ;;
        esac
    else
        has_device=0
        device_is_cpu=0
        expect_stage=0
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
    local -a extraArgs=("${@:4}")
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

    local -a profileEnv=()
    if [ "$expect_stage" -eq 1 ] || [ -n "$expect_stage_absent" ]; then
        profileEnv=(RT_OPENCL_PROFILE=1)
    fi

    if ! env OMP_NUM_THREADS="$THREADS" "${profileEnv[@]}" "$cli" --gpu=auto "${extraArgs[@]}" "${profileArg[@]}" -o "$auto" -t -b16 -Y -c "$input" \
            >"$outdir/auto.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (gpu=auto)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi

    if [ "$expect_stage" -eq 1 ]; then
        local stage_pattern="^opencl "
        if [ -n "$expect_stage_name" ]; then
            stage_pattern="^opencl ${expect_stage_name}:"
        fi
        if ! grep -q "$stage_pattern" "$outdir/auto.log"; then
            printf 'FAIL\t%s\tOpenCL stage did not run\n' "$label"
            rm -rf "$outdir"
            return 1
        fi
    fi

    if [ -n "$expect_stage_absent" ] \
            && grep -q "^opencl ${expect_stage_absent}:" "$outdir/auto.log"; then
        printf 'FAIL\t%s\tOpenCL stage ran when it should be disabled\n' "$label"
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

    expect_stage=1
    expect_stage_name="lab2srgb"
    run_pair "pef/neutral" "$sample"
    expect_stage=0
    expect_stage_name=""
    run_monitor
    run_stage_device "pef/stage-device-resize" \
        "$ROOT/devnotes/opencl/pp3/resize.pp3" lanczos3
    run_stage_device "pef/stage-device-transform" \
        "$ROOT/devnotes/opencl/pp3/rotate.pp3" transform
    run_stage_device "pef/stage-device-output" "" output.convert
    run_stage_device "pef/stage-device-rgbproc" \
        "$ROOT/devnotes/opencl/pp3/rgbproc-exposure.pp3" rgb.proc
    run_stage_device "pef/stage-device-framing" \
        "$ROOT/devnotes/opencl/pp3/framing.pp3" framing
    run_stage_device "pef/stage-device-gamut" \
        "$ROOT/devnotes/opencl/pp3/gamut.pp3" gamut
    run_stage_device "pef/stage-device-sharpen" \
        "$ROOT/devnotes/opencl/pp3/sharpen-usm.pp3" output.sharpen
    run_stage_device "pef/stage-device-sharpen-rld" \
        "$ROOT/devnotes/opencl/pp3/sharpen-rld.pp3" output.sharpen
    run_stage_host "pef/stage-host-sharpen-rld-damped" \
        "$ROOT/devnotes/opencl/pp3/sharpen-rld-damped.pp3" output.sharpen
    run_stage_device "pef/stage-device-working-trc" \
        "$ROOT/devnotes/opencl/pp3/working-trc.pp3" working.trc
    run_stage_host "pef/stage-host-working-trc-residual" \
        "$ROOT/devnotes/opencl/pp3/working-trc-residual.pp3" working.trc
    run_stage_device "pef/stage-device-lab-tools-sh" \
        "$ROOT/devnotes/opencl/pp3/labtools-sh.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-tools-sh-rgb" \
        "$ROOT/devnotes/opencl/pp3/labtools-sh-rgb.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-tools-labgrid" \
        "$ROOT/devnotes/opencl/pp3/labtools-labgrid.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-tools-vibrance" \
        "$ROOT/devnotes/opencl/pp3/labtools-vibrance.pp3" lab.tools cpu
    run_stage_device "pef/stage-device-lab-tools-labcurve" \
        "$ROOT/devnotes/opencl/pp3/labtools-labcurve.pp3" lab.tools cpu
    run_stage_device "pef/stage-device-lab-tools-localcontrast" \
        "$ROOT/devnotes/opencl/pp3/labtools-localcontrast.pp3" lab.tools cpu
    run_stage_device "pef/stage-device-lab-tools-regions" \
        "$ROOT/devnotes/opencl/pp3/labtools-regions.pp3" lab.tools cpu
    run_stage_host "pef/stage-host-lab-tools-residual" \
        "$ROOT/devnotes/opencl/pp3/labtools-residual.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-wavelet" \
        "$ROOT/devnotes/opencl/pp3/labwavelet.pp3" lab.wavelet cpu
    run_stage_device "pef/stage-device-lab-wavelet-contrast" \
        "$ROOT/devnotes/opencl/pp3/labwavelet-contrast.pp3" lab.wavelet cpu
    run_stage_device "pef/stage-device-local-adjustments" \
        "$ROOT/devnotes/opencl/pp3/localadjustments.pp3" local.adjustments cpu
    run_stage_device "pef/stage-device-local-adjustments-multispot" \
        "$ROOT/devnotes/opencl/pp3/localadjustments-multispot.pp3" \
        local.adjustments cpu
}

stage() {
    local name="$1"
    local sample="${sample_override:-$TESTDATA/colorspace_flowers.pef}"
    local profile="$ROOT/devnotes/opencl/pp3/$name.pp3"
    local -a extraArgs=()

    [ -f "$sample" ] || die "missing $sample (run tools/opencl/fetch-testdata.sh)"
    [ -f "$profile" ] || die "missing profile $profile"

    printf 'binary\t%s\n' "$cli"
    "$cli" --gpu-info | tail -n +2 | sed 's/^/gpu\t/'
    detect_device
    expect_stage_absent=""
    expect_stage=1
    case "$name" in
        capture)
            local settings_dir
            settings_dir="$(mktemp -d "${TMPDIR:-/tmp}/rtcap.XXXXXX")"
            local saved_settings="${RT_SETTINGS-}"
            export RT_SETTINGS="$settings_dir"

            expect_stage=0
            expect_stage_name=""
            expect_stage_absent="capture sharpening"
            extraArgs=()
            local default_status=0
            run_pair "$(basename "$sample")/$name-default" "$sample" "$profile" "${extraArgs[@]}" \
                || default_status=$?

            if [ -n "$saved_settings" ]; then
                export RT_SETTINGS="$saved_settings"
            else
                unset RT_SETTINGS
            fi
            rm -rf "$settings_dir"

            if [ "$default_status" -ne 0 ]; then
                return "$default_status"
            fi

            expect_stage=0
            expect_stage_absent=""
            extraArgs=(--gpu-capture=auto)
            run_pair "$(basename "$sample")/$name-auto" "$sample" "$profile" "${extraArgs[@]}"

            expect_stage=1
            expect_stage_absent=""
            expect_stage_name="capture sharpening"
            extraArgs=(--gpu-capture=on)
            ;;
        resize)
            expect_stage_name="resize"
            ;;
        rotate*)
            expect_stage_name="transform"
            ;;
        *)
            expect_stage_name=""
            ;;
    esac
    run_pair "$(basename "$sample")/$name" "$sample" "$profile" "${extraArgs[@]}"
}

run_monitor() {
    if [ "$has_device" -eq 0 ]; then
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\tmonitor\tno OpenCL device\n'
            return 1
        fi
        printf 'SKIP\tmonitor\tno OpenCL device\n'
        return 0
    fi

    local log
    log="$(mktemp "${TMPDIR:-/tmp}/rtmonitor.XXXXXX.log")"

    local status=0
    "$cli" --gpu-test-monitor >"$log" 2>&1 || status=$?

    if [ "$status" -eq 0 ]; then
        printf 'PASS\tmonitor\t%s\n' "$(tail -n 1 "$log")"
        rm -f "$log"
        return 0
    fi

    if [ "$status" -eq 2 ]; then
        printf 'SKIP\tmonitor\t%s\n' "$(tail -n 1 "$log")"
        rm -f "$log"
        return 0
    fi

    printf 'FAIL\tmonitor\t%s\n' "$(tail -n 1 "$log")"
    rm -f "$log"
    return "$status"
}

monitor() {
    printf 'binary\t%s\n' "$cli"
    "$cli" --gpu-info | tail -n +2 | sed 's/^/gpu\t/'
    detect_device
    run_monitor
}

# The stage device is opt-in and separate from the classic backend: the device
# path needs both the user preference (`--gpu=auto`) and RT_STAGE_OPENCL=1, and
# it reports through the stage profile record rather than the classic log. The
# node must appear with impl="opencl"; impl="host" means the device declined
# and the built-in or module ran instead, which is a SKIP unless a GPU is
# required.
#
# Usage: run_stage_device <label> <profile> <node> [device-class]
#
# device-class "cpu" marks a row whose node runs only on a CPU OpenCL device
# (the vibrance row). On a GPU the row is SKIP with the specific reason, or
# FAIL under RT_REQUIRE_GPU=1, rather than the generic "stage device declined".
run_stage_device() {
    local label="$1"
    local profile="$2"
    local node="$3"
    local device_class="${4:-any}"
    local sample="${sample_override:-$TESTDATA/colorspace_flowers.pef}"

    [ -f "$sample" ] || die "missing $sample (run tools/opencl/fetch-testdata.sh)"
    # An empty profile selects the neutral defaults, which is what the output
    # conversion needs: no sidecar, no resize.
    [ -z "$profile" ] || [ -f "$profile" ] || die "missing profile $profile"

    local -a profileArg=()
    [ -n "$profile" ] && profileArg=(-p "$profile")

    if [ "$has_device" -eq 0 ]; then
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\t%s\tno OpenCL device for the stage path\n' "$label"
            return 1
        fi
        printf 'SKIP\t%s\tno OpenCL device (set RT_OPENCL_ALLOW_CPU=1 for a CPU device)\n' "$label"
        return 0
    fi

    if [ "$device_class" = "cpu" ] && [ "$device_is_cpu" -ne 1 ]; then
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\t%s\tthe row is CPU-device-only; the selected device is not a CPU OpenCL device\n' "$label"
            return 1
        fi
        printf 'SKIP\t%s\tthe row is CPU-device-only; the selected device is not a CPU OpenCL device\n' "$label"
        return 0
    fi

    local outdir
    outdir="$(mktemp -d "${TMPDIR:-/tmp}/rtstagedev.XXXXXX")"
    local record="$outdir/stages.json"
    local host="$outdir/host.tif"
    local device="$outdir/device.tif"

    if ! env OMP_NUM_THREADS="$THREADS" "$cli" --gpu=off "${profileArg[@]}" \
            -o "$host" -t -b16 -Y -c "$sample" >"$outdir/host.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (gpu=off)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi

    # RT_OPENCL_RGBPROC opts the RGB tone stage in; it affects only rgb.proc,
    # so every device run may set it.
    if ! env OMP_NUM_THREADS="$THREADS" RT_STAGE_OPENCL=1 \
            RT_OPENCL_RGBPROC=1 RT_STAGE_PROFILE="$record" \
            "$cli" --gpu=auto "${profileArg[@]}" \
            -o "$device" -t -b16 -Y -c "$sample" >"$outdir/device.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (stage device)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi

    if [ ! -s "$record" ] || ! grep -q "\"stage\":\"${node}\"" "$record"; then
        printf 'FAIL\t%s\tno stage record for %s\n' "$label" "$node"
        rm -rf "$outdir"
        return 1
    fi

    if ! grep -Eq "\"stage\":\"${node}\".*\"impl\":\"opencl\"" "$record"; then
        local why
        why="$(grep "\"stage\":\"${node}\"" "$record" | head -1)"
        rm -rf "$outdir"
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\t%s\tstage device did not execute: %s\n' "$label" "$why"
            return 1
        fi
        printf 'SKIP\t%s\tstage device declined: %s\n' "$label" "$why"
        return 0
    fi

    local result="PASS"
    if ! compare_images "$host" "$device"; then
        result="FAIL"
    fi
    printf '%s\t%s\tmax_pae=%s\n' "$result" "$label" "${LAST_PAE:-unknown}"
    rm -rf "$outdir"
    [ "$result" = "PASS" ]
}

#
# A residual request must be recorded as a host execution, never counted as
# device parity. The TIFF comparison is informational: the device may still
# have run other nodes in the same export.
#
# Usage: run_stage_host <label> <profile> <node>
#
run_stage_host() {
    local label="$1"
    local profile="$2"
    local node="$3"
    local sample="${sample_override:-$TESTDATA/colorspace_flowers.pef}"

    [ -f "$sample" ] || die "missing $sample (run tools/opencl/fetch-testdata.sh)"
    [ -z "$profile" ] || [ -f "$profile" ] || die "missing profile $profile"

    if [ "$has_device" -eq 0 ]; then
        if [ "${RT_REQUIRE_GPU:-0}" = "1" ]; then
            printf 'FAIL\t%s\tno OpenCL device for the host residual\n' "$label"
            return 1
        fi
        printf 'SKIP\t%s\tno OpenCL device (set RT_OPENCL_ALLOW_CPU=1 for a CPU device)\n' "$label"
        return 0
    fi

    local outdir
    outdir="$(mktemp -d "${TMPDIR:-/tmp}/rtstagehost.XXXXXX")"
    local record="$outdir/stages.json"
    local host="$outdir/host.tif"
    local device="$outdir/device.tif"

    if ! env OMP_NUM_THREADS="$THREADS" "$cli" --gpu=off -p "$profile" \
            -o "$host" -t -b16 -Y -c "$sample" >"$outdir/host.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (gpu=off)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi
    if ! env OMP_NUM_THREADS="$THREADS" RT_STAGE_OPENCL=1 \
            RT_OPENCL_RGBPROC=1 RT_STAGE_PROFILE="$record" \
            "$cli" --gpu=auto -p "$profile" \
            -o "$device" -t -b16 -Y -c "$sample" >"$outdir/device.log" 2>&1; then
        printf 'FAIL\t%s\tload or export failed (stage host)\n' "$label"
        rm -rf "$outdir"
        return 1
    fi

    if [ ! -s "$record" ] || ! grep -q "\"stage\":\"${node}\"" "$record"; then
        printf 'FAIL\t%s\tno stage record for %s\n' "$label" "$node"
        rm -rf "$outdir"
        return 1
    fi
    if ! grep -Eq "\"stage\":\"${node}\".*\"impl\":\"host\"" "$record" ||
            grep -Eq "\"stage\":\"${node}\".*\"impl\":\"opencl\"" "$record"; then
        local why
        why="$(grep "\"stage\":\"${node}\"" "$record" | head -1)"
        printf 'FAIL\t%s\tresidual did not run on the host: %s\n' "$label" "$why"
        rm -rf "$outdir"
        return 1
    fi

    local pae="not counted"
    if compare_images "$host" "$device"; then
        pae="${LAST_PAE} (informational)"
    fi
    printf 'PASS\t%s\timpl=host, %s\n' "$label" "$pae"
    rm -rf "$outdir"
    return 0
}

stage_device() {
    printf 'binary\t%s\n' "$cli"
    "$cli" --gpu-info | tail -n +2 | sed 's/^/gpu\t/'
    detect_device
    run_stage_device "pef/stage-device-resize" \
        "$ROOT/devnotes/opencl/pp3/resize.pp3" lanczos3
    run_stage_device "pef/stage-device-transform" \
        "$ROOT/devnotes/opencl/pp3/rotate.pp3" transform
    run_stage_device "pef/stage-device-output" "" output.convert
    run_stage_device "pef/stage-device-rgbproc" \
        "$ROOT/devnotes/opencl/pp3/rgbproc-exposure.pp3" rgb.proc
    run_stage_device "pef/stage-device-framing" \
        "$ROOT/devnotes/opencl/pp3/framing.pp3" framing
    run_stage_device "pef/stage-device-gamut" \
        "$ROOT/devnotes/opencl/pp3/gamut.pp3" gamut
    run_stage_device "pef/stage-device-sharpen" \
        "$ROOT/devnotes/opencl/pp3/sharpen-usm.pp3" output.sharpen
    run_stage_device "pef/stage-device-sharpen-rld" \
        "$ROOT/devnotes/opencl/pp3/sharpen-rld.pp3" output.sharpen
    run_stage_host "pef/stage-host-sharpen-rld-damped" \
        "$ROOT/devnotes/opencl/pp3/sharpen-rld-damped.pp3" output.sharpen
    run_stage_device "pef/stage-device-working-trc" \
        "$ROOT/devnotes/opencl/pp3/working-trc.pp3" working.trc
    run_stage_host "pef/stage-host-working-trc-residual" \
        "$ROOT/devnotes/opencl/pp3/working-trc-residual.pp3" working.trc
    run_stage_device "pef/stage-device-lab-tools-sh" \
        "$ROOT/devnotes/opencl/pp3/labtools-sh.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-tools-sh-rgb" \
        "$ROOT/devnotes/opencl/pp3/labtools-sh-rgb.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-tools-labgrid" \
        "$ROOT/devnotes/opencl/pp3/labtools-labgrid.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-tools-vibrance" \
        "$ROOT/devnotes/opencl/pp3/labtools-vibrance.pp3" lab.tools cpu
    run_stage_device "pef/stage-device-lab-tools-labcurve" \
        "$ROOT/devnotes/opencl/pp3/labtools-labcurve.pp3" lab.tools cpu
    run_stage_device "pef/stage-device-lab-tools-localcontrast" \
        "$ROOT/devnotes/opencl/pp3/labtools-localcontrast.pp3" lab.tools cpu
    run_stage_device "pef/stage-device-lab-tools-regions" \
        "$ROOT/devnotes/opencl/pp3/labtools-regions.pp3" lab.tools cpu
    run_stage_host "pef/stage-host-lab-tools-residual" \
        "$ROOT/devnotes/opencl/pp3/labtools-residual.pp3" lab.tools
    run_stage_device "pef/stage-device-lab-wavelet" \
        "$ROOT/devnotes/opencl/pp3/labwavelet.pp3" lab.wavelet cpu
    run_stage_device "pef/stage-device-lab-wavelet-contrast" \
        "$ROOT/devnotes/opencl/pp3/labwavelet-contrast.pp3" lab.wavelet cpu
    run_stage_device "pef/stage-device-local-adjustments" \
        "$ROOT/devnotes/opencl/pp3/localadjustments.pp3" local.adjustments cpu
    run_stage_device "pef/stage-device-local-adjustments-multispot" \
        "$ROOT/devnotes/opencl/pp3/localadjustments-multispot.pp3" \
        local.adjustments cpu
}

rgbproc() {
    local sample="${sample_override:-$TESTDATA/colorspace_flowers.pef}"
    local profile="$ROOT/devnotes/opencl/pp3/rgbproc-exposure.pp3"

    [ -f "$sample" ] || die "missing $sample (run tools/opencl/fetch-testdata.sh)"
    [ -f "$profile" ] || die "missing profile $profile"

    printf 'binary\t%s\n' "$cli"
    "$cli" --gpu-info | tail -n +2 | sed 's/^/gpu\t/'
    detect_device

    export RT_OPENCL_RGBPROC=1
    expect_stage=1
    expect_stage_name="rgbproc"

    run_pair "pef/rgbproc-neutral" "$sample"
    run_pair "pef/rgbproc-exposure" "$sample" "$profile"
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
        --monitor) mode="monitor" ;;
        --rgbproc) mode="rgbproc" ;;
        --stage-device) mode="stage_device" ;;
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
    monitor) monitor ;;
    rgbproc) rgbproc ;;
    stage_device) stage_device ;;
    stage) stage "$stage_name" ;;
    coverage) list_coverage ;;
esac
