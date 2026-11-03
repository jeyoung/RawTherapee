#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  Run a command with the POCL-only OpenCL environment.
#
#  Usage:
#    tools/opencl/pocl-run.sh <command> [arguments...]
#
#  The command runs with OCL_ICD_VENDORS pointing at a pocl-only directory and
#  POCL_DEVICES=cpu. By default it also runs under bubblewrap with /dev/dri
#  replaced by an empty tmpfs, so even a command that drops the loader
#  override cannot reach the GPU driver.
#
#  Environment:
#    RT_POCL_CONTAIN=0  Run without the bubblewrap /dev/dri mask.
#
set -euo pipefail

_rt_pocl_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=tools/opencl/pocl-env.sh
source "$_rt_pocl_here/pocl-env.sh"

if ! rt_pocl_assert; then
    exit 1
fi

if [ "$#" -eq 0 ]; then
    printf 'usage: %s <command> [arguments...]\n' "$0" >&2
    exit 2
fi

if [ "${RT_POCL_CONTAIN:-1}" != "0" ] && command -v bwrap >/dev/null 2>&1; then
    exec bwrap --dev-bind / / --tmpfs /dev/dri -- "$@"
fi

exec "$@"
