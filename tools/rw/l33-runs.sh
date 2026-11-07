#!/usr/bin/env bash
# Run the RimWorld desktop harness N times, keeping per-run evidence.
# Usage: bash tools/rw/l33-runs.sh <first-run> <count> [full-build-first]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
FIRST="${1:-1}"
COUNT="${2:-3}"
FULL="${3:-1}"
EV="$ROOT/obj/rw-desktop/evidence-l33"
mkdir -p "$EV"
for ((i = FIRST; i < FIRST + COUNT; i++)); do
  rm -f "$ROOT/qmp-sock-rw-desktop"
  SKIP=1
  if [ "$i" = "$FIRST" ] && [ "$FULL" = "1" ]; then SKIP=0; fi
  echo "=== l33 run $i (RW_SKIP_BUILD=$SKIP) $(date +%H:%M:%S)"
  if [ "$SKIP" = "1" ]; then export RW_SKIP_BUILD=1; else unset RW_SKIP_BUILD; fi
  timeout 1200 bash tools/rw/run-rimworld-desktop.sh --run-timeout 200 \
    > "/tmp/l33-desk$i.log" 2>&1
  rc=$?
  cp -f obj/rw-desktop/serial-desktop.log "$EV/run$i.serial.log" 2>/dev/null
  cp -f obj/rw-desktop/Player.log "$EV/run$i.Player.log" 2>/dev/null
  echo "=== l33 run $i rc=$rc $(date +%H:%M:%S)"
  grep -ac "\[SIGD\]" "$EV/run$i.serial.log" 2>/dev/null | sed 's/^/    SIGD=/'
  grep -ac "\[RT\]" "$EV/run$i.serial.log" 2>/dev/null | sed 's/^/    RT=/'
done
echo "=== l33 runs done"
