#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): FOR THE USER TO RUN — accepts the
# CD PROJEKT RED user agreement in the GUEST's fresh game profile
# (~/compat/1091500 inside the VM; never the host prefix).
#
# Run from the host:
#   bash ~/cp/p0guest/tools/host-venus.sh start
#   bash ~/cp/p0guest/tools/host-venus.sh push
#   bash ~/cp/p0guest/tools/host-venus.sh ssh 'bash tools/guest-x.sh kms 1280x720; bash tools/guest-proton.sh mount; bash tools/guest-cp-accept.sh'
#
# What it does: launches the game, presses Space at the "PRESS ... TO
# CONTINUE" prompts, scrolls the agreement to its end (the game requires
# that), screenshots + OCRs it (~/runs/accept/before.png/.txt), presses Enter
# (the game binds ACCEPT to Enter, DECLINE to Esc), waits, screenshots + OCRs
# what follows (~/runs/accept/after.png/.txt), prints both OCR tails and
# stops the game. Any further first-run prompt is left for a human to look at.
set -euo pipefail
T=$(dirname "$(readlink -f "$0")")
export DISPLAY=:0
d=$HOME/runs/accept; mkdir -p "$d"
bash "$T/guest-cp.sh" launch accept --launcher-skip >/dev/null
for i in $(seq 120); do
    sleep 2
    import -window root "$d/poll.png"
    txt=$(tesseract "$d/poll.png" - 2>/dev/null | tr '\n' ' ')
    echo "$txt" | grep -q -i 'USER AGREEMENT' && break
    # the title-screen prompt is not always OCR-readable: Space is harmless before the agreement
    xdotool key space
done
xdotool mousemove 640 360
for i in $(seq 600); do xdotool click 5; done
sleep 2
import -window root "$d/before.png"; tesseract "$d/before.png" "$d/before" 2>/dev/null
grep -q -i 'ACCEPT' "$d/before.txt" || { echo "agreement end not found; nothing pressed"; bash "$T/guest-cp.sh" kill; exit 1; }
xdotool key Return
sleep 10
import -window root "$d/after.png"; tesseract "$d/after.png" "$d/after" 2>/dev/null
echo "== before (tail)"; grep -v '^\s*$' "$d/before.txt" | tail -3
echo "== after"; grep -v '^\s*$' "$d/after.txt" | head -15
bash "$T/guest-cp.sh" kill >/dev/null
