#!/usr/bin/env bash
# tools/cp/audio2/repro-batch.sh -- the cheap repro's boot matrix, one boot at
# a time in one slot per boot (lane H5-audio2, docs/cp-audio2.md §3).
#
#   bash tools/cp/audio2/repro-batch.sh PREFIX [SECS] [ROWS]
#
# ROWS (default "idle stall lowpad load") picks from:
#   idle    tone, the game fills its 40 ms buffer every event, no injected stall
#   stall   idle + 10 whole-guest host stalls of 300 ms (hoststall.sh)
#   lowpad  tone, the game renders one period per event (fill=1)
#   load    idle + ALOAD.ELF threads=8 mode=mix in the guest
# Each row's summary is obj/cp-audio2/PREFIX-ROW.summary.txt; one line per
# row goes to obj/cp-audio2/PREFIX.batch.txt.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
P="${1:?prefix}"; SECS="${2:-150}"; ROWS="${3:-idle stall lowpad load}"
O="$ROOT/obj/cp-audio2"; mkdir -p "$O"
B="$O/$P.batch.txt"
for row in $ROWS; do
  case $row in
    idle)   a=""; l=""; hs="" ;;
    stall)  a=""; l=""; hs="300 10 10" ;;
    lowpad) a="fill=1"; l=""; hs="" ;;
    load)   a=""; l="threads=8 mode=mix"; hs="" ;;
    *) echo "unknown row $row"; continue ;;
  esac
  HOSTSTALL="$hs" bash "$ROOT/tools/cp/qemu-slot.sh" bash "$ROOT/tools/cp/audio2/run-atone.sh" "$P-$row" "$SECS" "$a" "$l" > "$O/$P-$row.out" 2>&1
  S="$O/$P-$row.summary.txt"
  cl=$(grep -a '\[SND\] pcm closed' "$S" | sed 's/.*xruns=\([0-9]*\).* held=\([0-9]*\).*/xruns=\1 held=\2/; s/.*xruns=\([0-9]*\) .*/xruns=\1/')
  st=$(grep -aoE 'stalls=[0-9]+ stolen_ms=[0-9]+' "$S" | tail -1)
  wg=$(grep -aE 'wavgaps: (dropouts|dropout_ms_total|duration_s)=' "$S" | sed 's/wavgaps: //' | tr '\n' ' ')
  at=$(grep -aoE '\[AT\] total .*' "$S" | grep -oE 'late40=[0-9]+ xrun_state=[0-9]+ epipe=[0-9]+ leadin=[0-9]+')
  echo "$P-$row $(date +%H:%M:%S) $cl $at cons:$st $wg" >> "$B"
  sleep 20 # leave the slot free for others between boots
done
echo "BATCHDONE $(date +%H:%M:%S)" >> "$B"
