#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): drive Cyberpunk's menus. The game draws its
# own cursor from relative mouse motion (absolute XTest warps do nothing), so
# "at X Y" first pins the cursor to the top-left corner with large relative
# moves, then walks it to X,Y.
#   guest-cp-ui.sh at X Y            move the game cursor to X,Y (screen px)
#   guest-cp-ui.sh click X Y         move there and left-click (held 150 ms)
#   guest-cp-ui.sh key K             press key K (held 150 ms)
#   guest-cp-ui.sh shot <file.png>   screenshot
#   guest-cp-ui.sh ocr <file.png> [red]  OCR (red: red channel x3, for red menu text)
set -euo pipefail
export DISPLAY=:0
at() {
    w=$(xdotool search --name 'Cyberpunk 2077' | head -1); [ -n "$w" ] && xdotool windowfocus "$w" 2>/dev/null || true
    for i in $(seq 12); do xdotool mousemove_relative -- -400 -400; sleep 0.03; done
    local x=$1 y=$2 sx sy
    while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
        sx=$(( x > 40 ? 40 : x )); sy=$(( y > 40 ? 40 : y ))
        xdotool mousemove_relative -- "$sx" "$sy"; sleep 0.02
        x=$((x - sx)); y=$((y - sy))
    done
    sleep 0.3
}
case "$1" in
at) at "$2" "$3" ;;
click) at "$2" "$3"; xdotool mousedown 1; sleep 0.15; xdotool mouseup 1 ;;
key) xdotool keydown "$2"; sleep 0.15; xdotool keyup "$2" ;;
shot) import -window root "$2" ;;
ocr)
    if [ "${3:-}" = red ]; then
        convert "$2" -channel R -separate -resize 300% /tmp/ocr-red.png
        tesseract /tmp/ocr-red.png - 2>/dev/null
    else
        tesseract "$2" - 2>/dev/null
    fi | grep -v '^\s*$' ;;
*) sed -n '2,11p' "$0"; exit 2 ;;
esac
