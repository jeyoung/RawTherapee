#!/usr/bin/env bash
# Minimal GUI driver for isolated Xvfb preview checks.

set -euo pipefail

gui_display="${RT_PREVIEW_DISPLAY:-:98}"
gui_xvfb_pid=""
gui_app_pid=""

gui_start_display() {
    export DISPLAY="$gui_display"
    if xdpyinfo -display "$gui_display" >/dev/null 2>&1; then
        return 0
    fi
    Xvfb "$gui_display" -screen 0 1600x1000x24 -nolisten tcp >/dev/null 2>&1 &
    gui_xvfb_pid=$!
    for _ in $(seq 1 40); do
        if xdpyinfo -display "$gui_display" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

gui_stop_display() {
    gui_stop_app
    if [ -n "$gui_xvfb_pid" ]; then
        kill "$gui_xvfb_pid" >/dev/null 2>&1 || true
        wait "$gui_xvfb_pid" 2>/dev/null || true
        gui_xvfb_pid=""
    fi
}

gui_start_app() {
    local binary="$1" image="$2" log="$3"
    export GDK_BACKEND=x11
    unset WAYLAND_DISPLAY || true
    "$binary" "$image" >"$log" 2>&1 &
    gui_app_pid=$!
}

gui_stop_app() {
    if [ -n "$gui_app_pid" ]; then
        kill "$gui_app_pid" >/dev/null 2>&1 || true
        wait "$gui_app_pid" 2>/dev/null || true
        gui_app_pid=""
    fi
}

gui_wait_log() {
    local log="$1" pattern="$2" timeout="${3:-120}"
    local deadline=$((SECONDS + timeout))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if grep -q "$pattern" "$log" 2>/dev/null; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

gui_window_id() {
    xdotool search --name '^RawTherapee' 2>/dev/null | head -1
}

gui_prepare_window() {
    # The first-run notice is modal and owns the pointer until dismissed.
    xdotool key Escape >/dev/null 2>&1 || true
    sleep 0.5
    local wid
    wid="$(gui_window_id || true)"
    [ -n "$wid" ] || return 1
    xdotool windowsize "$wid" 1600 1000 >/dev/null 2>&1 || true
    xdotool windowmove "$wid" 0 0 >/dev/null 2>&1 || true
    xdotool windowfocus "$wid" >/dev/null 2>&1 || true
    xdotool mousemove 800 500 click 1
    sleep 0.5
}

gui_key() {
    xdotool key --clearmodifiers "$1"
}

gui_drag() {
    xdotool mousemove 800 500 mousedown 1 mousemove "$1" "$2" mouseup 1
}

gui_screenshot() {
    local path="$1"
    import -window root -depth 8 "$path"
}
