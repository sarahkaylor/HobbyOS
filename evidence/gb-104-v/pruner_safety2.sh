#!/usr/bin/env bash
# GB-104-V clause 3 on the FIXED head 7c0175b.
# Cases, matching the card's (a)/(b)/(c):
#   (a) a live, git-registered worktree                      -> must be untouched
#   (b) a directory under .worktrees/ git no longer lists     -> must be removed
#       (an EMPTY leftover: the only class the fix can prove safe)
#   (c) an unlisted dir holding UNCOMMITTED work              -> must be REFUSED with a reason
#   (d) an unlisted dir holding untracked files (extra case)  -> must be REFUSED
set -u
REPO=${1:-/tmp/gb104-scratch3}
PRUNER=${2:?usage: pruner_safety2.sh <scratch> <pruner>}

rm -rf "$REPO"; mkdir -p "$REPO"; cd "$REPO" || exit 1
git init -q .; git config user.email v@x; git config user.name v
echo base > BASE.txt; git add BASE.txt; git commit -qm base

git worktree add -q .worktrees/live -b livebranch
echo live > .worktrees/live/LIVE.txt

mkdir -p .worktrees/empty_orphan                       # (b) empty leftover
mkdir -p .worktrees/dirty; echo "precious" > .worktrees/dirty/work.txt   # (c) untracked work
mkdir -p .worktrees/untracked; echo "also precious" > .worktrees/untracked/u.txt  # (d)

echo "=== before"
ls -d .worktrees/*/ | sed 's/^/  /'

echo
echo "=== running the pruner (head 7c0175b)"
bash "$PRUNER" "$REPO"
echo "pruner exit=$?"

echo
echo "=== after"
printf '  (a) live worktree present:      %s\n' "$([ -d .worktrees/live ] && echo YES || echo NO)"
printf '  (a) live worktree LIVE.txt:     %s\n' "$([ -f .worktrees/live/LIVE.txt ] && echo YES || echo NO)"
printf '  (b) empty orphan present:       %s  (want NO)\n' "$([ -d .worktrees/empty_orphan ] && echo YES || echo NO)"
printf '  (c) dirty dir present:          %s  (want YES)\n' "$([ -d .worktrees/dirty ] && echo YES || echo NO)"
printf '  (c) dirty work.txt present:     %s  (want YES)\n' "$([ -f .worktrees/dirty/work.txt ] && echo YES || echo NO)"
printf '  (d) untracked dir present:      %s  (want YES)\n' "$([ -d .worktrees/untracked ] && echo YES || echo NO)"
echo
echo "=== git worktree list"
git worktree list