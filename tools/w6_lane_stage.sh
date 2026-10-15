#!/usr/bin/env bash
# w6-run helper: re-stage the lane scratch payload onto disk.img after any
# `make disk.img` rebuild (which restages only the frozen /SRC content).
set -u
cd "$(dirname "$0")/.."
IMG="${1:-disk.img}"
LANE=docs/selfhost/lanes
for pair in \
  "$LANE/P5.RUN::/SRC/P5.RUN" \
  "$LANE/T25-RUN.txt::/SRC/T25.RUN" \
  "$LANE/S2DBG.SH::/SRC/S2DBG.SH" \
  "$LANE/SELFHOST-RUN-w6-s0.txt::/SRC/S0.RUN" \
  "$LANE/SELFHOST-RUN-w6-s1-t25.txt::/SRC/S1.RUN" \
  "$LANE/SELFHOST-RUN-w6-s2.txt::/SRC/S2.RUN" ; do
  src="${pair%%::*}"; dst="::${pair##*::}"
  mcopy -o -i "$IMG" "$src" "$dst" || echo "stage FAIL $src"
done
# the arg-verbose probe script set for re-debugging
for n in MINP MINP2 MINP2D; do
  [ -f "$LANE/$n.MK" ] && mcopy -o -i "$IMG" "$LANE/$n.MK" "::/SRC/$n.MK"
  [ -f "$LANE/$n.SH" ] && mcopy -o -i "$IMG" "$LANE/$n.SH" "::/SRC/$n.SH"
done
echo "re-staged lane payload on $IMG"
