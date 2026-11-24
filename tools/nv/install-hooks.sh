#!/bin/sh
# install-hooks.sh -- install the repo-local git hooks the nv swarm relies on.
# Hooks are not versioned, so this script is the reproducible way to get them back
# after a fresh clone or a worktree.
set -eu
root=$(git rev-parse --git-common-dir)
mkdir -p "$root/hooks"
for h in tools/nv/hooks/*; do
  name=$(basename "$h" | sed 's/^pre-commit-plan/pre-commit/')
  cp "$h" "$root/hooks/$name"
  chmod +x "$root/hooks/$name"
  echo "installed $root/hooks/$name"
done
