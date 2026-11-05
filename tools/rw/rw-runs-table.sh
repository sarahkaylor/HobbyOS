#!/usr/bin/env bash
# tools/rw/rw-runs-table.sh — run the desktop harness N times and archive one
# directory per run, then print the determinism table.
#
# Why: the M6 managed-layer failure moved its death site run to run.  A table
# of (last log line, fault VA/PC/module, live PCB count) either shows one
# stable line with a moving fault (memory/thread state) or a stable fault with
# a moving line (a timing-dependent path).  The archive keeps every run's
# serial log and Player.log so a later conclusion can be re-checked.
#
# Usage: tools/rw/rw-runs-table.sh [N] [run-timeout-s]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
N="${1:-5}"
RT="${2:-120}"
OUT="$ROOT/obj/rw-desktop"
mkdir -p "$OUT/runs"

for i in $(seq 1 "$N"); do
  echo "=== run $i/$N $(date +%H:%M:%S)"
  RW_SKIP_BUILD=1 bash tools/rw/run-rimworld-desktop.sh --timeout 420 \
      --run-timeout "$RT" >"$OUT/runs/run$i.out" 2>&1
  cp -f "$OUT/serial-desktop.log" "$OUT/runs/run$i.serial.log" 2>/dev/null
  cp -f "$OUT/Player.log" "$OUT/runs/run$i.Player.log" 2>/dev/null
  cp -f "$OUT/shots/99_final.png" "$OUT/runs/run$i.final.png" 2>/dev/null
done

echo
printf '%-4s %-8s %-10s %-26s %-28s %s\n' run fault va pc module alive
for i in $(seq 1 "$N"); do
  S="$OUT/runs/run$i.serial.log"
  [ -f "$S" ] || continue
  line=$(grep -a "memory fault" "$S" | head -1)
  if [ -n "$line" ]; then
    va=$(echo "$line" | sed -n 's/.*VA=\(0x[0-9A-Fa-f]*\).*/\1/p')
    pc=$(echo "$line" | sed -n 's/.*PC=\(0x[0-9A-Fa-f]*\).*/\1/p')
    mod=$(grep -a "SIGDUMP] uctx rip=$pc" "$S" | sed -n 's/.*(\(.*\)+0x[0-9a-f]*).*/\1/p' | head -1)
    [ -n "$mod" ] || mod="?"
  else
    va="-"; pc="-"; mod="no fault"
  fi
  alive=$(grep -ac "ALIVE-SET" "$S")
  exitline=$(grep -a "exited (signal" "$S" | head -1 | sed 's/.*exited /exited /')
  printf '%-4s %-8s %-10s %-26s %-28s %s\n' "$i" "$va" "$pc" "$mod" "${exitline:-alive}" "$alive"
done
echo
echo "last Player.log line before the first fault, per run:"
for i in $(seq 1 "$N"); do
  P="$OUT/runs/run$i.Player.log"
  [ -f "$P" ] || continue
  last=$(grep -av "SIGDUMP" "$P" | tail -1 | cut -c1-100)
  echo "  run$i: $last"
done
