#!/usr/bin/env bash
# docs/nv-n9v-present-split-evidence/reproduce.sh -- regenerate analysis.txt and the
# two window dumps for t_0552cbd5 (present's share of the frame on the venus path).
#
#   bash docs/nv-n9v-present-split-evidence/reproduce.sh > docs/nv-n9v-present-split-evidence/analysis.txt
#
# Every input is a committed counter dump from a run that already happened; this
# script measures nothing.  Guest clock throughout ([HBP] = Venus' WSI,
# CLOCK_MONOTONIC in the guest).  renderer=venus on every row.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2
T=tools/cp/bench/present-split.py
echo "== present-split.py: guest-clock present share, budget = 33.33 ms (N9v gate)"
echo "== regenerate with: bash docs/nv-n9v-present-split-evidence/reproduce.sh"
echo
echo "### A. 1280x720, the N9v flight (the 29.19 avg / 18.2 1%-low run, renderer=venus)"
echo "#    40 world-rate [HBP] windows, 2384 presents, 2026-11-24 boot (0ac70a7b)"
echo "#    input: docs/nv-n9v-tail-evidence/present-windows.csv (<- hbp.csv of that run)"
python3 "$T" --window-log docs/nv-n9v-tail-evidence/present-windows.csv \
        --label 1280x720-flight \
        --out-csv docs/nv-n9v-present-split-evidence/present-windows-720p-flight.csv
echo
echo "### B. 1280x720, the same run's loading/menu windows (189 fps: the A/B of the"
echo "#    instrument's sensitivity -- present is the limit there, not in the flight)"
python3 "$T" --window-log docs/cp-bench-evidence/hobbyos-venus-720p/hbp.csv \
        --label 1280x720-menu-189fps --min-fps 180
echo
echo "### C. 1920x1080, title -> main menu, renderer=venus (p7-venus2, 2026-11-17, d4b7fdd0)"
echo "#    all 19 windows: the two 17 fps ones are title/loading, not the menu"
echo "#    input: docs/cp-p7-evidence/game-venus2-menu.txt ([X] [HBP] rows of that boot)"
python3 "$T" --window-log docs/cp-p7-evidence/game-venus2-menu.txt \
        --label 1920x1080-all --out-csv docs/nv-n9v-present-split-evidence/present-windows-1080p-menu.csv
echo
echo "### D. 1920x1080, the 17 steady menu windows (fps >= 40)"
echo "#    set/flush are the kernel's two round trips inside the flip: SET_SCANOUT_BLOB"
echo "#    and RESOURCE_FLUSH (src/kernel/virtio_gpu_x64.c:1347)"
python3 "$T" --window-log docs/cp-p7-evidence/game-venus2-menu.txt \
        --label 1920x1080-menu --min-fps 40
echo
echo "### E. the flip term at 1280x720 across every committed world-phase [HBP] row"
echo "#    (17 rows in docs/cp-k7-evidence and docs/cp-present-evidence, same"
echo "#    921600-pixel surface every time -- the spread is the point: present cost at"
echo "#    a fixed size is not one number, so 1080p cannot be extrapolated from it)"
grep -h '^== world ' docs/cp-k7-evidence/thrw-hbv-world-strix.txt \
     docs/cp-present-evidence/*.txt 2>/dev/null | grep '\[HBP\]' | sort -u
echo
echo "### F. the gate arithmetic these shares feed"
echo "#    budget 33.33 ms; 1280x720x4 = 3686400 B/frame; 1920x1080x4 = 8294400 B/frame"
echo "#    (ratio 2.25x).  flip at 720p p50 2.86 ms -> 1080p p50 9.31 ms = 3.26x."
echo
echo "### G. an independent 1920x1080 surface through the same instrument: f5a's vkcube"
echo "#    (different app, FIFO/vsync, 54.1 fps) -- the flush cost is the surface's, not"
echo "#    the game's; present path 147 % of the frame = sub and flip overlap (async)"
python3 "$T" --window-log docs/cp-p7-evidence/f5-handback.txt \
        --label 1920x1080-vkcube-f5 --min-fps 20
