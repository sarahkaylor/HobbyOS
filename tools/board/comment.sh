#!/usr/bin/env bash
# tools/board/comment.sh — GB-003 board automation for the HobbyOS Gaming Program.
# Thin wrapper over board.py; needs FORGEJO_URL + FORGEJO_TOKEN (swarm-bot token).
#   export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${FORGEJO_URL:=http://192.168.0.234:3000}"
: "${BOARD_REPO:=sarah/gaming-program}"
export FORGEJO_URL BOARD_REPO
: "${BOARD_AGENT:=$(hostname)-$(whoami)}"
export BOARD_AGENT

# comment.sh GB-013 -m "EVIDENCE: make host_tests -> ALL APPS SUITE TESTS PASSED"
# or:  some-command 2>&1 | comment.sh GB-013
[ $# -ge 1 ] || { echo "usage: comment.sh GB-013 [-m text | --file f | stdin]" >&2; exit 2; }
gid="$1"; shift
if [ $# -eq 0 ]; then exec python3 "$DIR/board.py" comment "$gid"; fi
exec python3 "$DIR/board.py" comment "$gid" "$@"
