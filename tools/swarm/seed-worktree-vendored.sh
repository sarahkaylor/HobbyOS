#!/usr/bin/env bash
# tools/swarm/seed-worktree-vendored.sh — GB-006.
#
# A `git worktree` (what `hermes -w` gives every worker) does NOT carry
# gitignored paths.  The vendored extractions under third_party/ are exactly
# those, so inside a worker worktree the C++/RTTI lane fails with
#   src/host/rtti_test.cpp:29:10: fatal error: 'typeinfo' file not found
# while the very same commit is green in the main checkout on the same VM
# (gaming.md §11.2b #4).  That false red costs a FLASH agent a full
# investigation, so seeding is not optional: it is part of the worktree
# setup.
#
# This script symlinks every gitignored path under third_party/ from the
# main checkout into the worktree (one physical copy, N worktrees).  It is
# idempotent and never overwrites a real file that the worktree already
# tracks.
#
# Usage:
#   tools/swarm/seed-worktree-vendored.sh [WORKTREE] [MAIN_CHECKOUT]
#     WORKTREE      worktree to seed           (default: $PWD)
#     MAIN_CHECKOUT source of the vendored trees
#                   (default: the first entry of `git worktree list`, the
#                    main checkout)
#
# Exit: 0 when every gitignored third_party path is present in the worktree.
set -euo pipefail

WT="${1:-$PWD}"
WT="$(cd "$WT" && pwd)"

if ! git -C "$WT" rev-parse --git-dir >/dev/null 2>&1; then
  echo "seed-worktree-vendored: $WT is not a git worktree" >&2
  exit 2
fi

if [ $# -ge 2 ]; then
  MAIN="$(cd "$2" && pwd)"
else
  MAIN="$(git -C "$WT" worktree list --porcelain | awk '/^worktree /{print substr($0,10); exit}')"
fi

if [ ! -d "$MAIN/third_party" ]; then
  echo "seed-worktree-vendored: no third_party/ in main checkout $MAIN" >&2
  exit 2
fi

mkdir -p "$WT/third_party"

seeded=0 skipped=0 missing=0
# Every path under third_party of the main checkout that git ignores is
# vendored build material.  Walk the real tree (depth 2 covers
# third_party/<pkg>/<subdir> — src/, build/, dist-staging/, ...), ask git
# whether it is ignored, and link it.
while IFS= read -r -d '' path; do
  rel="${path#"$MAIN"/}"
  # Already present in the worktree (tracked file/dir, or an earlier seed).
  if [ -e "$WT/$rel" ] || [ -L "$WT/$rel" ]; then
    skipped=$((skipped + 1))
    continue
  fi
  git -C "$MAIN" check-ignore -q -- "$rel" || continue
  mkdir -p "$(dirname "$WT/$rel")"
  ln -s "$path" "$WT/$rel"
  seeded=$((seeded + 1))
done < <(find "$MAIN/third_party" -mindepth 1 -maxdepth 2 -print0 2>/dev/null)

# Sanity: the two trees that actually break the build when absent.
for probe in third_party/libcxx-21.1.8/src third_party/llvm-21.1.8/src; do
  [ -e "$MAIN/$probe" ] || continue
  if [ ! -e "$WT/$probe" ]; then
    echo "seed-worktree-vendored: MISSING $probe in $WT" >&2
    missing=$((missing + 1))
  fi
done

echo "seed-worktree-vendored: $WT <- $MAIN — $seeded linked, $skipped already present, $missing missing"
[ "$missing" -eq 0 ]
