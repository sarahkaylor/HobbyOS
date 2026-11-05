#!/usr/bin/env bash
# L37: N desktop runs, one serial + Player.log per run, plus a per-run summary
# of the rows the launch log's table quotes.  Sequential by construction: two
# desktop harnesses in one worktree fight over disk.img (skill rule 5).
set -uo pipefail
ROOT=/home/sarah/hobbyos-rw-lanes/l37-garbage
cd "$ROOT" || exit 2
export HOME=/home/sarah
TAG="${1:?usage: l37-runs.sh <tag> <n> [run-timeout]}"
N="${2:?n}"
RT="${3:-150}"
EV="$ROOT/obj/rw-desktop/evidence-l37"
mkdir -p "$EV"
for i in $(seq 1 "$N"); do
  echo "=== $TAG run $i/$N $(date +%H:%M:%S)"
  RW_SKIP_BUILD=1 timeout 900 tools/rw/run-rimworld-desktop.sh \
      --timeout 420 --run-timeout "$RT" > "$EV/$TAG-$i.harness.log" 2>&1
  rc=$?
  cp -f obj/rw-desktop/serial-desktop.log "$EV/$TAG-$i.serial.log" 2>/dev/null
  cp -f obj/rw-desktop/Player.log          "$EV/$TAG-$i.Player.log" 2>/dev/null
  {
    echo "harness_rc=$rc"
    grep -aE '\[SIGDUMP\]|\[SGPR\]|\[SLOT-50\]|\[SREG\]|\[SREG2F\]|\[SRESUME\]|\[SIGRZ\]|\[SIGP\]|memory fault|Process [0-9]+: .*exited' \
        obj/rw-desktop/serial-desktop.log | cut -c1-400
    echo "--- player.log banner rows"
    grep -aE 'Initialize engine version|RimWorld 1\.6|Assertion|tramp-amd64|Finished resetting|Loaded All Assemblies' \
        obj/rw-desktop/Player.log 2>/dev/null | cut -c1-200
    echo "--- resume rows: $(grep -ac 'RESUME-RZ' obj/rw-desktop/serial-desktop.log) printed"
    grep -a 'RESUME-RZ' obj/rw-desktop/serial-desktop.log | tail -1
  } > "$EV/$TAG-$i.summary"
done
echo "=== $TAG done $(date +%H:%M:%S)"
