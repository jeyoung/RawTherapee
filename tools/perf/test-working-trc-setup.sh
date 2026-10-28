#!/usr/bin/env bash
#
# Build and run the working-TRC setup-only regression test.
#
# Usage:
#   tools/perf/test-working-trc-setup.sh [cmake-build-dir]
#
# The default build directory is build/cpu. The target is built explicitly so
# it does not affect normal package builds. Settings and cache stay inside
# build/test-scratch/working-trc-setup.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$root/build/cpu}"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "not a configured build directory: $build_dir" >&2
    echo "configure one with: tools/build-rt.sh cpu" >&2
    exit 2
fi

scratch="$root/build/test-scratch/working-trc-setup"
mkdir -p "$scratch"

cmake --build "$build_dir" --target working-trc-setup-test --parallel "$(nproc)"
"$build_dir/rtengine/working-trc-setup-test" "$scratch"
