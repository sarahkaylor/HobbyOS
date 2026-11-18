#!/usr/bin/env bash
# tools/cp/audio2/hoststall.sh -- deterministic whole-guest host stalls for the
# audio repro (lane H5-audio2, docs/cp-audio2.md; the same method as
# H4-kglitch's console A/B): SIGSTOP / SIGCONT of THIS run's own QEMU process
# (found by the WAV path on its command line), never any other process.  A
# stopped QEMU freezes every vCPU and its main loop together, like the host
# stalls H1-15 and the [CONS] stalls= rows record.
#
#   hoststall.sh WAVPATH CONSOLE_LOG STALL_MS EVERY_S COUNT
#
# Waits for the QEMU whose argv holds "path=WAVPATH", then for the player's
# "[AT] alsa period=" row in CONSOLE_LOG, then 5 s, then COUNT stalls of
# STALL_MS every EVERY_S.  Prints one "[HS] stall n= ms= at=" row per stall.
set -u
WAV="$1"; LOG="$2"; MS="$3"; EVERY="$4"; N="$5"
pid=""
for _ in $(seq 1 600); do
  pid=$(pgrep -f "qemu-system-x86_64.*path=$WAV" | head -1)
  [ -n "$pid" ] && break
  sleep 1
done
[ -n "$pid" ] || { echo "[HS] no qemu with path=$WAV"; exit 1; }
trap 'kill -CONT "$pid" 2>/dev/null' EXIT
for _ in $(seq 1 600); do
  grep -aq '\[AT\] alsa period=' "$LOG" 2>/dev/null && break
  kill -0 "$pid" 2>/dev/null || exit 0
  sleep 1
done
sleep 5
echo "[HS] qemu pid=$pid stall_ms=$MS every_s=$EVERY count=$N"
S=$(python3 -c "print($MS/1000)")
for i in $(seq 1 "$N"); do
  kill -0 "$pid" 2>/dev/null || break
  t=$(date +%s.%N)
  kill -STOP "$pid"; sleep "$S"; kill -CONT "$pid"
  echo "[HS] stall n=$i ms=$MS at=$t"
  sleep "$EVERY"
done
