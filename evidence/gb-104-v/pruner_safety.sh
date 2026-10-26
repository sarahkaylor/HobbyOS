#!/usr/bin/env bash
# GB-104-V clause 3 reproduction on PR #197's head d222dae (unchanged since the
# 13:03Z RED). Builds a scratch repo with (a) a live registered worktree,
# (b) an unregistered dir, (c) an unregistered dir holding uncommitted work —
# then runs the PR's pruner and reports which survived.
set -u
REPO=${1:-/tmp/gb104-scratch}
PRUNER=${2:?usage: pruner_safety.sh <scratch> <path-to-pruner>}

rm -rf "$REPO"; mkdir -p "$REPO"; cd "$REPO" || exit 1
git init -q .
git config user.email v@x; git config user.name v
echo base > BASE.txt; git add BASE.txt; git commit -qm base

# (a) a live worktree git knows about
git worktree add -q .worktrees/live -b livebranch
echo live > .worktrees/live/LIVE.txt

# (b) an unregistered directory (orphan)
mkdir -p .worktrees/orphan; echo orphan > .worktrees/orphan/x.txt

# (c) an unregistered directory holding UNCOMMITTED work
mkdir -p .worktrees/dirty
echo "precious uncommitted work" > .worktrees/dirty/work.txt
echo "more" >> .worktrees/dirty/work.txt

echo "=== before"
ls -d .worktrees/*/ 2>/dev/null
echo "dirty work.txt bytes: $(wc -c < .worktrees/dirty/work.txt)"

echo
echo "=== running the PR's pruner"
bash "$PRUNER" "$REPO"
echo "pruner exit=$?"

echo
echo "=== after"
echo "a) live worktree dir present:      $([ -d .worktrees/live ] && echo YES || echo NO)"
echo "a) live worktree LIVE.txt present: $([ -f .worktrees/live/LIVE.txt ] && echo YES || echo NO)"
echo "b) orphan dir present:             $([ -d .worktrees/orphan ] && echo YES || echo NO)"
echo "c) dirty dir present:              $([ -d .worktrees/dirty ] && echo YES || echo NO)"
echo "c) dirty work.txt present:         $([ -f .worktrees/dirty/work.txt ] && echo YES || echo NO)"
echo
echo "=== git worktree list"
git worktree list