#!/usr/bin/env bash
# selfhost-seed-worktree.sh — seed a selfhost lane worktree with the gitignored
# reference sources the builds need (selfhost.md §7.6). Idempotent.
#
# Usage: tools/selfhost-seed-worktree.sh <worktree-path>
#
# Seeds:
#   third_party/libcxx-21.1.8/{llvm-project-21.1.8.src.tar.xz, src/, .fetched-ok}
#   third_party/_staging/            (hardlinked; host-test refs)
set -euo pipefail

WT="${1:?usage: selfhost-seed-worktree.sh <worktree-path>}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[ -d "$WT" ] || { echo "no such worktree: $WT" >&2; exit 1; }

SRC_VENDOR="$REPO/third_party/libcxx-21.1.8"
DST_VENDOR="$WT/third_party/libcxx-21.1.8"
mkdir -p "$DST_VENDOR"

CONTAINER="llvm-project-21.1.8.src.tar.xz"
if [ ! -f "$DST_VENDOR/$CONTAINER" ]; then
  cp -l "$SRC_VENDOR/$CONTAINER" "$DST_VENDOR/$CONTAINER" 2>/dev/null \
    || cp "$SRC_VENDOR/$CONTAINER" "$DST_VENDOR/$CONTAINER"
  echo "seeded container: $DST_VENDOR/$CONTAINER"
else
  echo "container present: $DST_VENDOR/$CONTAINER"
fi

if [ ! -d "$DST_VENDOR/src/libcxx" ]; then
  cp -a "$SRC_VENDOR/src" "$DST_VENDOR/src"
  echo "seeded libcxx extraction"
else
  echo "libcxx extraction present"
fi

[ -f "$DST_VENDOR/.fetched-ok" ] || cp "$SRC_VENDOR/.fetched-ok" "$DST_VENDOR/.fetched-ok"

if [ -d "$REPO/third_party/_staging" ] && [ ! -d "$WT/third_party/_staging" ]; then
  cp -al "$REPO/third_party/_staging" "$WT/third_party/_staging"
  echo "seeded _staging (hardlinked)"
fi

echo "seeded: $WT"
