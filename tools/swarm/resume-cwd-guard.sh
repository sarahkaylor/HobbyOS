#!/usr/bin/env bash
# tools/swarm/resume-cwd-guard.sh — GB-104.
#
# A resumed agent session is handed the working directory it last ran in; if
# that worktree was cleaned up (session rotation / worktree reclaim between
# sessions) EVERY shell call in the resumed session opens with a failing `cd`
# into the dead dir — one error per command.  This guard is what a resume
# wrapper calls BEFORE starting a resumed session: it validates the working
# directory ONCE and, if it is gone, reports EXACTLY ONE error naming the path
# (never one per command) and exits non-zero so the wrapper can fall back to a
# fresh session instead of resuming into a dead cwd.
#
# Usage:
#   tools/swarm/resume-cwd-guard.sh [DIR]
#     DIR      the working directory a session is being resumed into
#              (default: $PWD)
#
# Exit: 0 = cwd exists (a normal resume may proceed).
#       1 = cwd does NOT exist — one RESUME-CWD-GUARD error line naming the path
#           has been printed to stderr.
set -u

TARGET="${1:-$PWD}"
if [ -d "$TARGET" ]; then
  exit 0
fi
echo "RESUME-CWD-GUARD: cannot resume into working directory '$TARGET' — it no longer exists (likely pruned/rotated). Start a fresh session instead." >&2
exit 1
