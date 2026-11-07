#!/usr/bin/env bash
# L39: N desktop runs of the present-probe tip, one serial + Player.log per
# run, plus a per-run summary of the rows that answer "does it present?".
#
# Row destination matters: Unity dup2()s its -logFile over fd 1 AND fd 2 before
# the first window call (skill rule 36), so everything the shim writes to fd 2
# after that point lands in Player.log.  Both logs are searched here.
#
# Sequential by construction: two desktop harnesses in one worktree fight over
# disk.img (skill rule 5).  The first run builds; later runs reuse that image
# with RW_SKIP_BUILD=1 (no gate runs in between, so the image is still the
# desktop one).
#
# Usage: tools/rw/l39-runs.sh <tag> <n> [run-timeout]
set -uo pipefail
ROOT=/home/sarah/hobbyos-rw-lanes/l39-present
cd "$ROOT" || exit 2
export HOME=/home/sarah
TAG="${1:?usage: l39-runs.sh <tag> <n> [run-timeout]}"
N="${2:?n}"
RT="${3:-600}"
EV="$ROOT/obj/rw-desktop/evidence-l39"
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
  S="$EV/$TAG-$i.serial.log"
  P="$EV/$TAG-$i.Player.log"
  cnt() { echo $(( $(grep -ac "$1" "$S" 2>/dev/null) + $(grep -ac "$1" "$P" 2>/dev/null) )); }
  {
    echo "harness_rc=$rc"
    echo "--- ladder"
    grep -aE 'Initialize engine version|RimWorld 1\.6|Assertion|x86-codegen|tramp-amd64|Finished resetting|Loaded All Assemblies|memory fault|VA=0x' \
        "$P" 2>/dev/null | tail -6 | cut -c1-200
    grep -aE 'Initialize engine version|RimWorld 1\.6|Assertion|x86-codegen|tramp-amd64' \
        "$S" 2>/dev/null | tail -4 | cut -c1-200
    echo "--- present counts (serial + Player.log)"
    echo "  [XSW] rows      : $(cnt '\[XSW\]')"
    echo "  [XHB] rows      : $(cnt '\[XHB\]')"
    echo "  [XE] PUT rows   : $(cnt '\[XE\] PUT ')"
    echo "  [XE] PUTSKIP    : $(cnt '\[XE\] PUTSKIP')"
    echo "  [XE] PUTDEFER   : $(cnt '\[XE\] PUTDEFER')"
    echo "  [XE] PUTLATE    : $(cnt '\[XE\] PUTLATE')"
    echo "  [XE] MAPGEOM    : $(cnt '\[XE\] MAPGEOM')"
    echo "  [XE] FLUSH rows : $(cnt '\[XE\] FLUSH')"
    echo "  [XI] grab rows  : $(cnt '\[XI\] XIGrabTouchBegin')"
    echo "  [XI] total rows : $(cnt '\[XI\]')"
    echo "  Player.log bytes: $(stat -c %s "$P" 2>/dev/null || echo 0)"
    echo "--- [XSW] series (first 6, last 6)"
    { grep -a '\[XSW\]' "$S" 2>/dev/null; grep -a '\[XSW\]' "$P" 2>/dev/null; } | head -6 | cut -c1-200
    echo "  ..."
    { grep -a '\[XSW\]' "$S" 2>/dev/null; grep -a '\[XSW\]' "$P" 2>/dev/null; } | tail -6 | cut -c1-200
    echo "--- [XHB] series"
    { grep -a '\[XHB\]' "$S" 2>/dev/null; grep -a '\[XHB\]' "$P" 2>/dev/null; } | head -12 | cut -c1-200
    echo "--- [XE] first-contact rows"
    { grep -aE '\[XE\] (MAPGEOM|PUTLATE|PUTSKIP|PUTDEFER|PUT |FLUSH)' "$S" 2>/dev/null;
      grep -aE '\[XE\] (MAPGEOM|PUTLATE|PUTSKIP|PUTDEFER|PUT |FLUSH)' "$P" 2>/dev/null; } \
      | head -16 | cut -c1-200
    echo "--- [XI] first rows"
    { grep -a '\[XI\]' "$S" 2>/dev/null; grep -a '\[XI\]' "$P" 2>/dev/null; } | head -6 | cut -c1-200
  } > "$EV/$TAG-$i.summary"
  echo "--- summary"; cat "$EV/$TAG-$i.summary"
done
echo "=== $TAG done $(date +%H:%M:%S)"
