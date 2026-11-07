#!/usr/bin/env bash
# tools/rw/l74-fps-sizes.sh -- the L74 fps-vs-resolution ladder.
#
#   l74-fps-sizes.sh <secs> [sizes...]        (default: 1916x1018 1280x720 960x540)
#
# One virgl boot per size, the SAME binary and the same present path, a 120 s
# measurement window (l62-fps.py), and the rows that prove the workload really
# was that size: [XE] VSCREEN, [VGL] res_create ... dt (the GL surface the
# winsys allocated) and [XSW] w=/h=/gx=/gy= (the drawable and its fb0 rect).
#
# l62-fps-run.sh takes the size as its second argument and stages /RWSCREEN +
# /WMSIZE from it (honour=1), so a typo cannot silently produce a 1080p
# workload labelled 720p -- which is the defect this ladder exists to rule out.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
SECS="${1:?run seconds}"; shift
SIZES=("$@")
[ ${#SIZES[@]} -gt 0 ] || SIZES=(1916x1018 1280x720 960x540)
# The image must be the DESKTOP image.  Measured the hard way: the unit tier
# rebuilds disk.img as a MODE=test image, and a harness run started with
# RW_SKIP_BUILD=1 then boots the unit suite (431 tests, no desktop, no [MENU],
# "[drive] FAIL: desktop never printed [MENU]").  Build by default; a caller
# who really wants the current image says RW_SKIP_BUILD=1 *and* knows it is a
# desktop image.
RW_SKIP_BUILD="${RW_SKIP_BUILD:-0}"
export RW_SKIP_BUILD RW_SKIP_EXPORT="${RW_SKIP_EXPORT:-1}"
for S in "${SIZES[@]}"; do
  NAME="l74-virgl-$S"
  echo "===== $NAME  (${SECS}s run, 120s window)"
  RW_QEMU_GL=1 RW_GLMODE=virgl RW_GLDMG=1 RW_VIEWPORT=1 \
    RW_SHOT_EVERY="${RW_SHOT_EVERY:-60}" \
    bash tools/rw/l62-fps-run.sh "$NAME" "$S" "$SECS" 1 2>&1 | tail -22
  echo "--- rows for $NAME"
  grep -a "\[XE\] VSCREEN" "obj/l62/$NAME/serial.log" | head -1
  grep -a "\[VGL\] res_create.*dt" "obj/l62/$NAME/Player.log" \
    | grep -av " 64x64 \| 32x32 " | head -2
  grep -a "\[XSW\]" "obj/l62/$NAME/serial.log" | sed -n '2p'
  grep -a "\[GL\] viewport" "obj/l62/$NAME/Player.log" | sort -u | head -3
  echo "--- fps from the guest clock (tools/rw/l74-sizes.py)"
  python3 tools/rw/l74-sizes.py "obj/l62/$NAME/serial.log" --window 120 \
    --label "$NAME"
done
