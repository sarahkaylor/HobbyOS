#!/usr/bin/env bash
# tools/board/merge_pr.sh — the PRODUCTION call site for the GB-106 merge guard.
#   merge_pr.sh <pr> [--head <sha> ...]
# Runs `merge_pr.py --merge` (which FAILS CLOSED when the GREEN's graded sha does not
# match the PR's current head, or names no sha) and ONLY proceeds to merge on a pass.
# A failed guard exits non-zero HERE so an automation calling this wrapper never
# merges a tree that was not graded.  Overrides go through `--override <reason> --card <n>`.
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: merge_pr.sh <pr> [--head <sha> --graded <sha> | --override <reason> --card <n>]"; exit 2; }
pr="$1"; shift
self=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
set -a; [ -f "$HOME/.hermes/.env" ] && . "$HOME/.hermes/.env"; set +a
export FORGEJO_TOKEN FORGEJO_URL BOARD_REPO GITHUB_TOKEN
python3 "$self/merge_pr.py" --repo sarahkaylor/HobbyOS --pr "$pr" "$@"
