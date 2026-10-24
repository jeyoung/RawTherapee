#!/usr/bin/env bash
# Shared environment and helpers for the performance/parity harness.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
STAMP="$(date +%Y%m%dT%H%M%S)"
RUN="${RT_RUN_DIR:-$ROOT/build/test-scratch/harness/$STAMP}"

export RT_SETTINGS="${RT_SETTINGS:-$RUN/settings}"
export RT_CACHE="${RT_CACHE:-$RUN/cache}"
export TMPDIR="${TMPDIR:-$RUN/tmp}"
export GDK_BACKEND="${GDK_BACKEND:-x11}"
unset WAYLAND_DISPLAY || true

mkdir -p "$RUN"/{settings,cache,tmp,out,shots,logs}

info() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

require() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

cli_for_build() {
    local build_dir="$1"
    [ -x "$build_dir/rtgui/rawtherapee-cli" ] || \
        die "no rawtherapee-cli in $build_dir"
    printf '%s\n' "$build_dir/rtgui/rawtherapee-cli"
}

gui_for_build() {
    local build_dir="$1"
    [ -x "$build_dir/rtgui/rawtherapee" ] || \
        die "no rawtherapee GUI binary in $build_dir"
    printf '%s\n' "$build_dir/rtgui/rawtherapee"
}

test_image() {
    local image="${RT_TEST_IMAGE_45MP:-}"
    [ -n "$image" ] && [ -f "$image" ] || \
        die "set RT_TEST_IMAGE_45MP to a 45 MP raw file"
    printf '%s\n' "$image"
}

test_pp3() {
    printf '%s\n' "${RT_TEST_PP3_45MP:-}"
}

sha256_file() {
    sha256sum "$1" | awk '{print $1}'
}

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}
