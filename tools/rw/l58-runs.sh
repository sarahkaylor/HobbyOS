#!/usr/bin/env bash
# tools/rw/l58-runs.sh — L58 ladder: three 300 s desktop runs of the same build.
#
# The def pass needs 230-300 s at the measured ~0.73 fps, so a 220 s run cannot
# show it; 300 s is the shortest run that reaches the cross-reference phase.
# Each run's serial log and Player.log are kept per-run (the harness overwrites
# obj/rw-desktop/serial-desktop.log on every boot).
#
# Usage: tools/rw/l58-runs.sh [N] [SECONDS]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
export HOME=/home/sarah
OUT="$ROOT/obj/rw-desktop"
N="${1:-3}"
SECS="${2:-300}"
mkdir -p "$OUT/l58"

for i in $(seq 1 "$N"); do
  echo "== L58 run $i/$N (${SECS}s)"
  RW_SKIP_BUILD=1 RW_APP=RWLAUNCH.BIN bash tools/rw/run-rimworld-desktop.sh \
      --timeout 300 --run-timeout "$SECS" >"$OUT/l58/run$i.out" 2>&1
  cp -f "$OUT/serial-desktop.log" "$OUT/l58/run$i.serial.log" 2>/dev/null
  cp -f "$OUT/Player.log" "$OUT/l58/run$i.Player.log" 2>/dev/null
  cp -f "$OUT/shots/99_final.png" "$OUT/l58/run$i.final.png" 2>/dev/null
  echo "   serial=$(stat -c %s "$OUT/l58/run$i.serial.log" 2>/dev/null) Player.log=$(stat -c %s "$OUT/l58/run$i.Player.log" 2>/dev/null)"
done
echo "== done"
