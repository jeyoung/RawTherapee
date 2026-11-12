#!/usr/bin/env bash
#
# Build and run the snapshot comparison geometry-merge regression test.
#
# Usage:
#   tools/perf/test-snapshot-compare-geometry.sh [cmake-build-dir]
#
# The default build directory is build/cpu. The target is built explicitly so
# it does not affect normal package builds.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$root/build/cpu}"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "not a configured build directory: $build_dir" >&2
    echo "configure one with: tools/build-rt.sh cpu" >&2
    exit 2
fi

cmake --build "$build_dir" --target snapshot-compare-geometry-test --parallel 6
"$build_dir/rtengine/snapshot-compare-geometry-test"
