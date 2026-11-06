#!/usr/bin/env bash
# Compare preview screenshots between two builds under identical settings.
#
# Usage:
#   tools/perf/test-preview-parity.sh --build-dir DIR --baseline-dir DIR \
#       --image FILE [--out DIR]

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
source "$ROOT/tools/perf/lib/common.sh"
source "$ROOT/tools/perf/lib/gui.sh"

build_dir=""
baseline_dir=""
image=""
out=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --baseline-dir) baseline_dir="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$build_dir" ] || die "--build-dir is required"
[ -n "$baseline_dir" ] || die "--baseline-dir is required"
[ -n "$image" ] || image="$(test_image)"
[ -f "$image" ] || die "image not found: $image"
[ -n "$out" ] || out="$RUN/preview-parity"
mkdir -p "$out"

require xdotool
if ! command -v import >/dev/null 2>&1 && ! command -v scrot >/dev/null 2>&1; then
    die "install ImageMagick (import) or scrot"
fi
if ! command -v magick >/dev/null 2>&1; then
    python3 -c 'from PIL import Image, ImageChops' 2>/dev/null || \
        die "install ImageMagick (magick) or Python Pillow"
fi

capture() {
    local binary="$1" tag="$2"
    local settings="$out/$tag-settings" cache="$out/$tag-cache"
    mkdir -p "$settings" "$cache"
    export RT_SETTINGS="$settings" RT_CACHE="$cache" RT_PREVIEW_PROFILE=1 RT_LOAD_PROFILE=1
    gui_start_app "$binary" "$image" "$out/$tag.log"
    gui_wait_log "$out/$tag.log" 'preview imageReady' 180 || {
        gui_stop_app
        die "$tag: preview never became ready"
    }
    gui_prepare_window || {
        gui_stop_app
        die "$tag: no RawTherapee window"
    }
    sleep 1
    gui_screenshot "$out/$tag-fit.png"
    gui_key z
    sleep 2
    gui_screenshot "$out/$tag-100.png"
    gui_key plus
    sleep 2
    gui_screenshot "$out/$tag-zoom.png"
    gui_drag 950 550
    sleep 2
    gui_screenshot "$out/$tag-pan.png"
    gui_key f
    sleep 2
    gui_screenshot "$out/$tag-fit2.png"
    gui_stop_app
}

gui_start_display || die "cannot start Xvfb on $gui_display"
trap gui_stop_display EXIT

info "capturing baseline"
if [ -x "$baseline_dir/rtgui/rawtherapee-graph" ]; then
    capture "$baseline_dir/rtgui/rawtherapee-graph" baseline
else
    capture "$baseline_dir/rtgui/rawtherapee" baseline
fi
info "capturing candidate"
capture "$build_dir/rtgui/rawtherapee-graph" candidate

failures=0
for scene in fit 100 zoom pan fit2; do
    a="$out/baseline-$scene.png"
    b="$out/candidate-$scene.png"
    if command -v magick >/dev/null 2>&1; then
        value="$(magick compare -metric AE "$a" "$b" null: 2>&1 || true)"
        if [ "$value" = "0" ]; then
            printf 'PASS\tpreview %s AE=0\n' "$scene"
            continue
        fi
        printf 'FAIL\tpreview %s AE=%s\n' "$scene" "$value"
        magick compare "$a" "$b" "$out/diff-$scene.png" 2>/dev/null || true
    else
        value="$(python3 - "$a" "$b" "$out/diff-$scene.png" <<'PY'
import sys

import numpy as np
from PIL import Image, ImageChops

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
diff = ImageChops.difference(a, b)
count = int(np.any(np.array(diff) != 0, axis=2).sum())
if count:
    diff.save(sys.argv[3])
print(count)
PY
)"
        if [ "$value" = "0" ]; then
            printf 'PASS\tpreview %s AE=0\n' "$scene"
            continue
        fi
        printf 'FAIL\tpreview %s AE=%s\n' "$scene" "$value"
    fi
    failures=$((failures + 1))
done

[ "$failures" -eq 0 ] || die "$failures preview scene(s) differ"
info "preview parity PASS"
