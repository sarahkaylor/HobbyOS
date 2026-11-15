#!/usr/bin/env bash
# tools/cp/p0host/shot.sh <tag> <name> — gamescope screenshot of the run + tesseract OCR.
# gamescope writes /tmp/gamescope.png when GAMESCOPECTRL_REQUEST_SCREENSHOT is set on its
# Xwayland root.  The old file is removed first so a stale image can never be OCR'd.
# Prints the OCR text (one line) or NOSHOT.  PNG and .txt land in runs/<tag>/shots/.
set -u
. "$(dirname "$0")/env.sh"
R=$P0/runs/$1; n=$2
export DISPLAY=$(cat "$R/display" 2>/dev/null)
[ -n "$DISPLAY" ] || { echo NOSHOT; exit 1; }
rm -f /tmp/gamescope.png
xprop -root -f GAMESCOPECTRL_REQUEST_SCREENSHOT 32c -set GAMESCOPECTRL_REQUEST_SCREENSHOT 1 2>/dev/null \
  || { echo NOSHOT; exit 1; }
for _ in $(seq 1 50); do
  [ -s /tmp/gamescope.png ] && break
  sleep 0.1
done
sleep 0.4
[ -s /tmp/gamescope.png ] || { echo NOSHOT; exit 1; }
mv /tmp/gamescope.png "$R/shots/$n.png"
tesseract "$R/shots/$n.png" - --psm 11 2>/dev/null | tr -s '\n ' '  ' | tee "$R/shots/$n.txt"
echo
