#!/usr/bin/env bash
#
# Build and run the 100% FAST preview regression test.
#
# Usage:
#   tools/perf/test-fast-preview.sh [cmake-build-dir]
#
# The default build directory is build/cpu-<branch>. The target is built
# explicitly so it does not affect normal package builds.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$root/build/cpu-$(git -C "$root" rev-parse --abbrev-ref HEAD)}"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "not a configured build directory: $build_dir" >&2
    echo "configure one with: tools/build-rt.sh cpu" >&2
    exit 2
fi

cmake --build "$build_dir" --target fast-preview-test --parallel 6
"$build_dir/rtengine/fast-preview-test"
