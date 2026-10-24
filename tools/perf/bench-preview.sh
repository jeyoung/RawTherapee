#!/usr/bin/env bash
#
# Measure preview-update wall time by driving the Exposure slider in a
# throw-away Xvfb session.
#
# Usage:
#   tools/perf/bench-preview.sh <rawtherapee> <image> [runs]
#
# Environment:
#   RT_PREVIEW_DISPLAY  X display to use (default: :99)
#   RT_PREVIEW_DELAY    Delay after the initial load (default: 15)
#   RT_PREVIEW_WAIT     Delay after each slider edit (default: 10)
#
# The image and sidecar are never copied or committed. The script requires
# Xvfb, xdotool and fonts for the GTK UI.
set -euo pipefail

binary="${1:-}"
image="${2:-}"
runs="${3:-5}"

if [ -z "$binary" ] || [ -z "$image" ]; then
    echo "usage: $0 <rawtherapee> <image> [runs]" >&2
    exit 2
fi

[ -x "$binary" ] || { echo "not executable: $binary" >&2; exit 2; }
[ -f "$image" ] || { echo "not a file: $image" >&2; exit 2; }
command -v Xvfb >/dev/null || { echo "Xvfb is required" >&2; exit 2; }
command -v xdotool >/dev/null || { echo "xdotool is required" >&2; exit 2; }

display="${RT_PREVIEW_DISPLAY:-:98}"
load_delay="${RT_PREVIEW_DELAY:-15}"
edit_wait="${RT_PREVIEW_WAIT:-10}"
log="$(mktemp "${TMPDIR:-/tmp}/rt-preview.XXXXXX.log")"
xvfb_pid=""
gui_pid=""

cleanup() {
    [ -n "$gui_pid" ] && kill "$gui_pid" 2>/dev/null || true
    [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null || true
    rm -f "$log" "$log.timings"
}
trap cleanup EXIT

Xvfb "$display" -screen 0 1600x1000x24 >/dev/null 2>&1 &
xvfb_pid=$!
export DISPLAY="$display"
sleep 2

RT_PREVIEW_PROFILE=1 "$binary" "$image" >"$log" 2>&1 &
gui_pid=$!
sleep "$load_delay"

# Dismiss the first-run notice and pin the window. Without this the notice
# owns the pointer and every later click lands on the modal dialog.
xdotool key Escape >/dev/null 2>&1 || true
sleep 1
window_id="$(xdotool search --name '^RawTherapee' 2>/dev/null | head -1 || true)"
if [ -n "$window_id" ]; then
    xdotool windowsize "$window_id" 1600 1000 >/dev/null 2>&1 || true
    xdotool windowmove "$window_id" 0 0 >/dev/null 2>&1 || true
    xdotool windowfocus "$window_id" >/dev/null 2>&1 || true
fi

# Open the Exposure tool, then drag Exposure Compensation to the right and
# back. The coordinates are for the 1600x1000 Xvfb layout used above.
xdotool mousemove 850 143 click 1
sleep 2

for _ in $(seq 1 "$runs"); do
    xdotool mousemove 1013 406 mousedown 1 mousemove 1080 406 mouseup 1
    sleep "$edit_wait"
    xdotool mousemove 1080 406 mousedown 1 mousemove 1013 406 mouseup 1
    sleep "$edit_wait"
done

grep 'preview update:' "$log" | tail -n +2 | tee "$log.timings" >/dev/null
if [ ! -s "$log.timings" ]; then
    echo "no preview timings captured; see $log" >&2
    exit 1
fi

# Emit the measurements and their p50/p95. The cold first render is excluded.
awk '
    {
        for (i = 1; i <= NF; ++i) {
            if ($i == "ms") {
                print $(i - 1)
            }
        }
    }
' "$log.timings" | sort -n | awk '
    { values[NR] = $1 }
    END {
        if (NR == 0) {
            exit 1
        }
        p50 = values[int((NR + 1) * 0.50)]
        p95 = values[int((NR + 1) * 0.95)]
        printf "samples=%d p50=%.0f ms p95=%.0f ms\n", NR, p50, p95
    }
'

