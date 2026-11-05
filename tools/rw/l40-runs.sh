#!/usr/bin/env bash
# tools/rw/l40-runs.sh — run the RimWorld desktop harness N times, one after
# another, and copy each run's evidence into obj/rw-desktop/evidence-l40/.
#
# Sequential by construction: two desktop harnesses in one worktree fight over
# disk.img (rule 5).  Nothing here rebuilds the kernel; the RUNNER must have a
# disk.img whose /boot/hobbyos.elf is the current kernel (rule 88 in the
# hobbyos-rimworld-track skill: a seeded image may carry an older one, and the
# symptom is a Mono JIT assertion with no [XHB] row).
#
# Usage: tools/rw/l40-runs.sh <tag> <n> [run-timeout] [extra env assignments]
#   tools/rw/l40-runs.sh final 3 240
#   RW_TRACE_X11=1 tools/rw/l40-runs.sh trace 1 240
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
TAG="${1:-final}"; N="${2:-3}"; RUN_TO="${3:-240}"
EV="$ROOT/obj/rw-desktop/evidence-l40"
mkdir -p "$EV"

for i in $(seq 1 "$N"); do
  echo "== run $TAG-$i/$N (run-timeout ${RUN_TO}s)"
  RW_SKIP_BUILD=1 bash tools/rw/run-rimworld-desktop.sh \
      --timeout 480 --run-timeout "$RUN_TO" >"$EV/$TAG-$i.harness.log" 2>&1
  rc=$?
  cp obj/rw-desktop/Player.log "$EV/$TAG-$i.Player.log" 2>/dev/null
  cp obj/rw-desktop/serial-desktop.log "$EV/$TAG-$i.serial.log" 2>/dev/null
  cp obj/rw-desktop/shots/99_final.png "$EV/$TAG-$i.final.png" 2>/dev/null
  echo "   harness rc=$rc"
  python3 tools/rw/l40-summary.py "$EV/$TAG-$i.Player.log" \
          "$EV/$TAG-$i.serial.log" 2>/dev/null | sed -n '1,26p'
  fuser -k disk.img >/dev/null 2>&1
done
