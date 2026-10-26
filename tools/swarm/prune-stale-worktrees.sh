#!/usr/bin/env bash
# tools/swarm/prune-stale-worktrees.sh — GB-104.
#
# Stale hermes worktrees (from rotated/ended agent sessions) must be pruned so a
# resume is NEVER handed a working directory that no longer exists.  Prunes two
# kinds of stale state in the repo's .worktrees/ dir:
#   1. `git worktree prune` drops worktree entries whose backing
#      .git/worktrees/<name> metadata is gone (the canonical stale case).
#   2. A .worktrees/<name> directory that `git worktree list` no longer knows
#      about is a stale leftover and is removed -- BUT never a directory holding
#      work (GB-104 RED: deleting uncommitted/untracked work is worse than the
#      bug).  Only a leftover we can prove is safe is removed: an EMPTY dir, or a
#      working tree git confirms is CLEAN.
#
# It never touches a directory git still lists as a live worktree.
#
# Usage:
#   tools/swarm/prune-stale-worktrees.sh [REPO]
#     REPO   the git repo whose .worktrees/ should be pruned (default: repo root
#            relative to this script).
#
# Exit: 0 always (a prune that has nothing to do, or refused work-dirs, is a
# success -- refusing is the safe answer).
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$ROOT"

git worktree prune || true   # drop entries whose .git metadata is gone

removed=0
refused=0
for d in .worktrees/*/; do
  [ -d "$d" ] || continue
  name="${d%/}"
  name="${name#.worktrees/}"
  if git worktree list --porcelain | grep -q "\.worktrees/${name}$"; then
    continue   # a live, git-listed worktree is never touched
  fi
  # GB-104 RED: never delete a directory holding work -- an uncommitted edit
  # destroyed quietly is worse than the bug.  Only remove what we can PROVE safe:
  # an EMPTY dir, or a working tree git confirms is CLEAN.
  if git -C "$d" rev-parse --git-dir >/dev/null 2>&1; then
    if git -C "$d" diff --quiet HEAD 2>/dev/null \
       && [ -z "$(git -C "$d" ls-files --others --exclude-standard 2>/dev/null)" ]; then
      rm -rf "$d"; removed=$((removed + 1))
      echo "PRUNE-STALE-WORKTREES: removed stale (clean) worktree .worktrees/$name"
    else
      refused=$((refused + 1))
      echo "PRUNE-STALE-WORKTREES: REFUSED .worktrees/$name — holds uncommitted/untracked work; NOT deleted (GB-104)"
    fi
  else
    if [ -z "$(ls -A "$d" 2>/dev/null)" ]; then
      rm -rf "$d"; removed=$((removed + 1))
      echo "PRUNE-STALE-WORKTREES: removed stale (empty) worktree .worktrees/$name"
    else
      refused=$((refused + 1))
      echo "PRUNE-STALE-WORKTREES: REFUSED .worktrees/$name — cannot confirm it holds no work (broken/unreadable git); NOT deleted (GB-104)"
    fi
  fi
done
echo "PRUNE-STALE-WORKTREES: ready (git pruned; removed=$removed stale worktree dirs, refused=$refused carrying work)"
exit 0
