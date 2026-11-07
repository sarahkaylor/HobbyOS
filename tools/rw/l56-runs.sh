#!/usr/bin/env bash
# tools/rw/l56-runs.sh — run the RimWorld desktop harness N times, one after
# another, and keep each run's evidence under obj/rw-desktop/evidence-l56/.
#
# Sequential by construction: two desktop harnesses in one worktree fight over
# disk.img.  Nothing here rebuilds the kernel; the RUNNER must have a disk.img
# whose /boot/hobbyos.elf is the current kernel AND must have rebuilt the
# launcher (tools/rw/rebuild-launcher.sh) — a seeded lane's launcher predates
# the L54 condvar fix, and the symptom is one 1280x720 frame, no def reads and
# no swap (rule 99 in the hobbyos-rimworld-track skill).
#
# Usage: tools/rw/l56-runs.sh <tag> <n> [run-timeout]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
TAG="${1:-fix}"; N="${2:-3}"; RUN_TO="${3:-220}"
EV="$ROOT/obj/rw-desktop/evidence-l56"
mkdir -p "$EV"

for i in $(seq 1 "$N"); do
  echo "== run $TAG-$i/$N (run-timeout ${RUN_TO}s)"
  RW_SKIP_BUILD=1 RW_SKIP_EXPORT=1 bash tools/rw/run-rimworld-desktop.sh \
      --timeout 480 --run-timeout "$RUN_TO" >"$EV/$TAG-$i.harness.log" 2>&1
  rc=$?
  cp obj/rw-desktop/Player.log "$EV/$TAG-$i.Player.log" 2>/dev/null
  cp obj/rw-desktop/serial-desktop.log "$EV/$TAG-$i.serial.log" 2>/dev/null
  cp obj/rw-desktop/shots/99_final.png "$EV/$TAG-$i.final.png" 2>/dev/null
  cp obj/rw-desktop/shots/02_running.png "$EV/$TAG-$i.02.png" 2>/dev/null
  echo "   harness rc=$rc"
  python3 tools/rw/l56-summary.py "$EV/$TAG-$i.serial.log" \
          "$EV/$TAG-$i.Player.log" "$EV/$TAG-$i.final.png" 2>/dev/null
  fuser -k disk.img >/dev/null 2>&1
done
