#!/usr/bin/env bash
# tools/board/claim.sh — GB-003 board automation for the HobbyOS Gaming Program.
# Thin wrapper over board.py; needs FORGEJO_URL + FORGEJO_TOKEN (swarm-bot token).
#   export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${FORGEJO_URL:=http://192.168.0.234:3000}"
: "${BOARD_REPO:=sarah/gaming-program}"
export FORGEJO_URL BOARD_REPO
: "${BOARD_AGENT:=$(hostname)-$(whoami)}"
export BOARD_AGENT

# claim.sh GB-013 [agent-name]
# Marks a ticket In Progress and comments the claim (workflow §4.2 step 1).
[ $# -ge 1 ] || { echo "usage: claim.sh GB-013 [agent-name]" >&2; exit 2; }
if [ $# -ge 2 ]; then board_args=(--by "$2"); else board_args=(); fi
exec python3 "$DIR/board.py" claim "$1" "${board_args[@]+"${board_args[@]}"}" --host "$(hostname)"
