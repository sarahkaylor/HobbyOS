#!/usr/bin/env bash
# tools/board/lane-move.sh — GB-003 board automation for the HobbyOS Gaming Program.
# Thin wrapper over board.py; needs FORGEJO_URL + FORGEJO_TOKEN (swarm-bot token).
#   export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${FORGEJO_URL:=http://192.168.0.234:3000}"
: "${BOARD_REPO:=sarah/gaming-program}"
export FORGEJO_URL BOARD_REPO
: "${BOARD_AGENT:=$(hostname)-$(whoami)}"
export BOARD_AGENT

# lane-move.sh GB-013 "In Progress"
# Lanes (§4.1): Backlog Ready "In Progress" "In Review" "Merge Gate" Done Icebox
[ $# -eq 2 ] || { echo 'usage: lane-move.sh GB-013 "In Progress"' >&2; exit 2; }
exec python3 "$DIR/board.py" move "$1" "$2"
