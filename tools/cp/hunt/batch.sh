#!/usr/bin/env bash
# tools/cp/hunt/batch.sh QUEUE -- the H1-hunt matrix runner (docs/cp-hunt.md):
# runs the boots listed in QUEUE one after another through run-hunt.sh.
# QUEUE lines: "KIND RUN [VAR=VALUE ...]" ("#" comments).  The file is re-read
# before every boot, so lines can be added or edited while the batch runs; a
# run whose name is already in QUEUE.done is skipped; a line "STOP" ends the
# batch.  One line per boot -> QUEUE.done ("RUN rc=.. verdict=.. HH:MM:SS").
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
Q="${1:?queue file}"; D="$Q.done"; touch "$D"
mkdir -p "${HUNT_LOGS:-$ROOT/obj/hunt-logs}"
while :; do
  next=""
  while read -r line; do
    line="${line%%#*}"; set -- $line
    [ $# -ge 1 ] || continue
    [ "$1" = STOP ] && { echo "batch: STOP $(date +%T)" >> "$D"; exit 0; }
    [ $# -ge 2 ] || continue
    grep -q "^$2 " "$D" && continue
    next="$line"; break
  done < "$Q"
  [ -n "$next" ] || { echo "batch: queue empty $(date +%T)" >> "$D"; exit 0; }
  set -- $next
  kind="$1"; run="$2"; shift 2
  echo "batch: start $kind $run $* $(date +%T)" >&2
  env "$@" bash "$ROOT/tools/cp/hunt/run-hunt.sh" "$kind" "$run" > "${HUNT_LOGS:-$ROOT/obj/hunt-logs}/$run.out" 2>&1
  rc=$?
  echo "$run rc=$rc $(tail -1 "${HUNT_LOGS:-$ROOT/obj/hunt-logs}/$run.out" | sed 's/^run-hunt: [^ ]* //') $(date +%T)" >> "$D"
done
