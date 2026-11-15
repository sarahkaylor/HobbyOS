#!/usr/bin/env bash
# tools/cp/p0host/ocrbox.sh <png> <x0> <y0> <x1> <y1> — OCR one rectangle (coordinates in the
# screenshot's own pixels), upscaled 3x and inverted so red/cyan-on-dark UI text reads well.
set -u
f=$1; x0=$2; y0=$3; x1=$4; y1=$5
convert "$f" -alpha off -crop "$((x1-x0))x$((y1-y0))+$x0+$y0" +repage -colorspace Gray -resize 300% -negate -normalize png:- \
  | tesseract stdin stdout --psm 7 2>/dev/null | tr -s '\n ' '  '
echo
