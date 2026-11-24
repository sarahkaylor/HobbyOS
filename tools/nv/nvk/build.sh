#!/usr/bin/env bash
# tools/nv/nvk/build.sh -- thin driver for the supported build.
#
# The NVK ICD is part of the tree's Mesa configuration now: the build (pins,
# cross file, steps, audit) lives in tools/mesa/build-nvk.sh, next to
# build-hobbyos.sh, and the one Mesa change lives with the other patches as
# tools/mesa/patches/0014-nvk-hobbyos-node-and-named-refusals.patch.
#
# This wrapper exists so the lane's documented command keeps working:
#
#   bash tools/nv/nvk/build.sh all        # == bash tools/mesa/build-nvk.sh all
#   bash tools/nv/nvk/build.sh audit      # SONAME / DT_NEEDED / entry points
#
# Nothing is built here.  Changing the build means changing
# tools/mesa/build-nvk.sh; changing Mesa means changing the patch.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
exec bash "$ROOT/tools/mesa/build-nvk.sh" "$@"
