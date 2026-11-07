#!/usr/bin/env bash
# L38: N desktop runs of the JIT-arena tip, one serial + Player.log per run,
# plus a per-run summary of the rows the launch log's ladder quotes.
#
# Sequential by construction: two desktop harnesses in one worktree fight over
# disk.img (skill rule 5).  The first run builds; later runs reuse that image
# with RW_SKIP_BUILD=1 (no gate runs in between, so the image is still the
# desktop one).
#
# Usage: tools/rw/l38-runs.sh <tag> <n> [run-timeout]
set -uo pipefail
ROOT=/home/sarah/hobbyos-rw-lanes/l38-jit
cd "$ROOT" || exit 2
export HOME=/home/sarah
TAG="${1:?usage: l38-runs.sh <tag> <n> [run-timeout]}"
N="${2:?n}"
RT="${3:-600}"
EV="$ROOT/obj/rw-desktop/evidence-l38"
mkdir -p "$EV"
for i in $(seq 1 "$N"); do
  echo "=== $TAG run $i/$N $(date +%H:%M:%S)"
  SKIP=1
  [ "$i" = 1 ] && SKIP=0
  RW_SKIP_BUILD=$SKIP timeout 1500 tools/rw/run-rimworld-desktop.sh \
      --timeout 420 --run-timeout "$RT" > "$EV/$TAG-$i.harness.log" 2>&1
  rc=$?
  cp -f obj/rw-desktop/serial-desktop.log "$EV/$TAG-$i.serial.log" 2>/dev/null
  cp -f obj/rw-desktop/Player.log          "$EV/$TAG-$i.Player.log" 2>/dev/null
  cp -f obj/rw-desktop/shots/*.png         "$EV/" 2>/dev/null
  {
    echo "harness_rc=$rc"
    echo "--- arena rows"
    grep -aE '\[JITA\]|\[JITS\]' obj/rw-desktop/serial-desktop.log | tail -8 | cut -c1-300
    echo "--- JIT windows: $(grep -ac '\[JITV\]' obj/rw-desktop/serial-desktop.log) rows, \
src=arena $(grep -ac 'src=arena' obj/rw-desktop/serial-desktop.log), \
src=fit $(grep -ac 'src=fit' obj/rw-desktop/serial-desktop.log)"
    grep -a '\[JITV\]' obj/rw-desktop/serial-desktop.log | tail -3 | cut -c1-300
    echo "--- ladder"
    grep -aE 'Initialize engine version|RimWorld 1\.6|Assertion|x86-codegen|tramp-amd64|Finished resetting|Loaded All Assemblies|memory fault|VA=0x' \
        obj/rw-desktop/Player.log 2>/dev/null | tail -8 | cut -c1-220
    grep -aE 'Initialize engine version|RimWorld 1\.6|Assertion|x86-codegen|tramp-amd64' \
        obj/rw-desktop/serial-desktop.log | tail -6 | cut -c1-220
    echo "--- present: [XE] PUT rows = $(grep -ac '\[XE\] PUT' obj/rw-desktop/serial-desktop.log)"
    grep -a '\[XE\] PUT' obj/rw-desktop/serial-desktop.log | tail -2 | cut -c1-220
    echo "--- resumes: $(grep -ac 'RESUME-RZ' obj/rw-desktop/serial-desktop.log)"
  } > "$EV/$TAG-$i.summary"
  echo "--- summary"; cat "$EV/$TAG-$i.summary"
done
echo "=== $TAG done $(date +%H:%M:%S)"
