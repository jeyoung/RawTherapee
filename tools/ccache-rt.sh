#!/bin/sh
# Keep ccache local to the project so build trees and worktrees share it.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
export CCACHE_DIR="${CCACHE_DIR:-$root/build/ccache}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}"
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$root}"
export CCACHE_COMPILERCHECK="${CCACHE_COMPILERCHECK:-content}"
exec ccache "$@"
