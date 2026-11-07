#!/usr/bin/env bash
# tools/rw/l74-pair.sh -- the same scene, the same window, two rasterisers.
#
#   l74-pair.sh <WxH> <secs>
#
# M3 could not produce a same-phase softpipe frame, so "the same UI layout as
# softpipe" stayed unproven.  This runs the SAME binary at the SAME requested
# size, once on virgl and once on softpipe, with the same screenshot cadence
# (RW_SHOT_EVERY), so the two shot series can be compared phase for phase by
# tools/rw/m3-frame-compare.py.
#
# The size must be one the WM honours (/WMSIZE) -- at 1280x720 both drivers
# render 1280x720, which is what makes the comparison meaningful at all.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
SIZE="${1:?WxH}"; SECS="${2:?run seconds}"
for MODE in virgl softpipe; do
  NAME="l74-pair-$MODE-$SIZE"
  echo "===== $NAME"
  RW_QEMU_GL=1 RW_GLMODE="$MODE" RW_GLDMG="${RW_GLDMG:-1}" RW_VIEWPORT=1 \
    RW_SKIP_BUILD="${RW_SKIP_BUILD:-0}" RW_SKIP_EXPORT="${RW_SKIP_EXPORT:-1}" \
    RW_SHOT_EVERY="${RW_SHOT_EVERY:-20}" \
    bash tools/rw/l62-fps-run.sh "$NAME" "$SIZE" "$SECS" 1 2>&1 | tail -18
  mkdir -p "obj/l62/$NAME/shots"
  cp -f obj/rw-desktop/shots/*.png "obj/l62/$NAME/shots/" 2>/dev/null
  grep -a "\[XE\] VSCREEN" "obj/l62/$NAME/serial.log" | head -1
  grep -a "\[VGL\] res_create.*dt" "obj/l62/$NAME/Player.log" \
    | grep -av " 64x64 \| 32x32 " | head -2
  grep -a "\[XSW\]" "obj/l62/$NAME/serial.log" | sed -n '2p'
  python3 tools/rw/l74-sizes.py "obj/l62/$NAME/serial.log" --window 120 \
    --label "$NAME"
done
