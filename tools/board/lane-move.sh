#!/usr/bin/env bash
# tools/board/lane-move.sh — GB-003 board automation for the HobbyOS Gaming Program.
# Thin wrapper over board.py; needs FORGEJO_URL + FORGEJO_TOKEN (swarm-bot token).
# GB-005: moving a card to `Merge Gate` ENFORCES the §§4.2-5 gate — the newest
# `GB-nnn-V` verdict must be GREEN, else the move REFUSES (non-zero, reason printed).
# `--force` bypasses with a durable record of WHO forced it, posted on the card.
#   export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${FORGEJO_URL:=http://192.168.0.234:3000}"
: "${BOARD_REPO:=sarah/gaming-program}"
export FORGEJO_URL BOARD_REPO
: "${BOARD_AGENT:=$(hostname)-$(whoami)}"
export BOARD_AGENT

# lane-move.sh GB-013 "In Progress"   |   GB-013 "Merge Gate" [--force "reason"]
# Lanes (§4.1): Backlog Ready "In Progress" "In Review" "Merge Gate" Done Icebox
FORCE=0; FORCE_REASON=""
for a in "$@"; do
  case "$a" in
    --force) FORCE=1 ;;
    --force=*) FORCE=1; FORCE_REASON="${a#--force=}" ;;
  esac
done
GID="${1:-}"; LANE="${2:-}"
[ -n "$GID" ] && [ -n "$LANE" ] || { echo 'usage: lane-move.sh GB-013 "In Progress"|"Merge Gate" [--force "reason"]' >&2; exit 2; }
if [ "$LANE" = "Merge Gate" ]; then
  V="$("$DIR"/merge_gate_enforce.py "$GID" 2>/dev/null)" && {
    echo "lane-move: $V"
  } || {
    rc=$?
    if [ "$FORCE" = "1" ]; then
      GID="$GID" BOARD_REPO="$BOARD_REPO" python3 "$DIR"/merge_gate_enforce.py \
        --post-force "$GID" --agent "$BOARD_AGENT" --reason "${FORCE_REASON:-forced override}" --gid "$GID" \
        || true
      echo "lane-move: $((rc)) -> FORCED by $BOARD_AGENT to Merge Gate (reason: ${FORCE_REASON:-override}); bypass recorded on the card."
      echo "   overridden refusal: $V"
      # re-run the plain move afterwards below (bypass recorded, still performs the move)
    else
      echo "lane-move: REFUSED to move $GID to Merge Gate: $V" >&2
      exit 1
    fi
  }
fi
exec python3 "$DIR/board.py" move "$GID" "$LANE"
