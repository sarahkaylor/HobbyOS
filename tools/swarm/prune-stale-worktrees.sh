#!/usr/bin/env bash
# tools/swarm/prune-stale-worktrees.sh — GB-104.
#
# Stale hermes worktrees (from rotated/ended agent sessions) must be pruned so a
# resume is NEVER handed a working directory that no longer exists.  Prunes two
# kinds of stale state in the repo's .worktrees/ dir:
#   1. `git worktree prune` drops worktree entries whose backing
#      .git/worktrees/<name> metadata is gone (the canonical stale case).
#   2. Any .worktrees/<name> directory that `git worktree list` no longer knows
#      about is an orphaned/rotated checkout and is removed.
#
# It never touches a directory git still lists as a live worktree.
#
# Usage:
#   tools/swarm/prune-stale-worktrees.sh [REPO]
#     REPO   the git repo whose .worktrees/ should be pruned (default: repo root
#            relative to this script).
#
# Exit: 0 always (a prune that has nothing to do is a success).
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$ROOT"

git worktree prune || true   # drop entries whose .git metadata is gone

removed=0
for d in .worktrees/*/; do
  [ -d "$d" ] || continue
  name="${d%/}"
  name="${name#.worktrees/}"
  if ! git worktree list --porcelain | grep -q "\.worktrees/${name}$"; then
    rm -rf "$d"
    removed=$((removed + 1))
    echo "PRUNE-STALE-WORKTREES: removed stale worktree .worktrees/$name"
  fi
done
echo "PRUNE-STALE-WORKTREES: ready (git pruned; removed=$removed stale worktree dirs)"
exit 0
