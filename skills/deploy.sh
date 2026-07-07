#!/bin/bash
# =============================================================================
# deploy.sh — install/update the HobbyOS skills into the local Hermes agent.
#
# Hermes discovers skills from the filesystem at ~/.hermes/skills/<category>/
# <name>/SKILL.md (source shows as "local", auto-enabled — no install step).
# This script syncs every skill under skills/ into the `hobbyos` category,
# overwriting existing copies with the current version and pruning skills that
# were removed from the repo, then verifies registration with `hermes`.
#
# Usage:
#   ./skills/deploy.sh            # sync + verify
#   ./skills/deploy.sh --dry-run  # show what would change, touch nothing
#   HERMES_HOME=/path ./skills/deploy.sh   # override Hermes home (default ~/.hermes)
# =============================================================================
set -euo pipefail

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HERMES_HOME="${HERMES_HOME:-$HOME/.hermes}"
CATEGORY="hobbyos"
DEST_DIR="$HERMES_HOME/skills/$CATEGORY"

say() { printf '%s\n' "$*"; }
run() { if [ "$DRY_RUN" = 1 ]; then say "  [dry-run] $*"; else eval "$@"; fi; }

if [ ! -d "$HERMES_HOME" ]; then
  say "error: Hermes home '$HERMES_HOME' not found. Is Hermes installed? (set HERMES_HOME to override)"
  exit 1
fi

# Collect source skills: any immediate subdir of SRC_DIR containing a SKILL.md.
# (Portable to bash 3.2 — no mapfile.)
SKILLS=()
for d in "$SRC_DIR"/*/; do
  [ -f "$d/SKILL.md" ] && SKILLS+=("$(basename "$d")")
done
if [ "${#SKILLS[@]}" -eq 0 ]; then
  say "error: no skills (dirs with SKILL.md) found under $SRC_DIR"
  exit 1
fi

say "HobbyOS skills → $DEST_DIR"
say "Found ${#SKILLS[@]} skill(s): ${SKILLS[*]}"
say ""

run "mkdir -p \"$DEST_DIR\""

# 1. Sync each skill directory (overwrite with current version).
for s in "${SKILLS[@]}"; do
  say "sync: $s"
  if command -v rsync >/dev/null 2>&1; then
    run "rsync -a --delete \"$SRC_DIR/$s/\" \"$DEST_DIR/$s/\""
  else
    run "rm -rf \"$DEST_DIR/$s\" && cp -R \"$SRC_DIR/$s\" \"$DEST_DIR/$s\""
  fi
done

# 2. Prune stale skills previously deployed but no longer in the repo.
if [ -d "$DEST_DIR" ]; then
  for d in "$DEST_DIR"/*/; do
    [ -d "$d" ] || continue
    name="$(basename "$d")"
    keep=0
    for s in "${SKILLS[@]}"; do [ "$s" = "$name" ] && keep=1; done
    if [ "$keep" = 0 ]; then
      say "prune (removed from repo): $name"
      run "rm -rf \"$d\""
    fi
  done
fi

# 3. Make helper scripts executable.
if [ "$DRY_RUN" = 0 ]; then
  find "$DEST_DIR" -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} + 2>/dev/null || true
fi

say ""
# 4. Register/verify. Filesystem discovery is automatic; `hermes skills list`
#    forces a rescan and confirms the skills are picked up and enabled.
if command -v hermes >/dev/null 2>&1; then
  if [ "$DRY_RUN" = 1 ]; then
    say "[dry-run] would run: hermes skills list | grep $CATEGORY"
  else
    say "Registered $CATEGORY skills (source=local, enabled):"
    if ! hermes skills list 2>/dev/null | grep -i "$CATEGORY" || true; then :; fi
  fi
  # If a local gateway/API server is running, it snapshots skills at startup —
  # restart it so a long-lived gateway serves the updated set. New `hermes chat`
  # / `hermes -z` sessions always pick them up without a restart.
  GW_PID_FILE="$HERMES_HOME/gateway.pid"
  if [ -s "$GW_PID_FILE" ] && kill -0 "$(cat "$GW_PID_FILE" 2>/dev/null)" 2>/dev/null; then
    say ""
    say "NOTE: a Hermes gateway is running (pid $(cat "$GW_PID_FILE")). Restart it to serve the"
    say "      updated skills to that long-lived process:  hermes gateway restart"
  fi
else
  say "warning: 'hermes' not on PATH — files are in place but could not verify registration."
fi

say ""
if [ "$DRY_RUN" = 1 ]; then say "Done. (dry-run)"; else say "Done."; fi
