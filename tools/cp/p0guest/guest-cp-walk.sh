#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): walk a running game from launch to the main
# menu, recording every screen it passes (screenshot + OCR) in ~/runs/<tag>/.
#   - "PRESS ... TO CONTINUE" (or no readable text): Space
#   - "USER AGREEMENT": STOP (exit 3) — never accepted by this script
#   - "SKIP FOR NOW" (optional CD PROJEKT RED account offer): the skip key
#   - "Help Make Our Games Better" (telemetry consent): REJECT ALL (Esc, or a
#     click on the REJECT ALL button) — never Enter, which is CONSENT TO ALL
#   - main menu (NEW GAME / LOAD GAME / CREDITS / SETTINGS ...): exit 0
# usage: guest-cp-walk.sh <tag> [max_seconds]
set -uo pipefail
export DISPLAY=:0
press() { xdotool keydown "$1"; sleep 0.15; xdotool keyup "$1"; }
tag=$1; max=${2:-400}
d=$HOME/runs/$tag; mkdir -p "$d"
t0=$(cat "$d/t-launch.txt")
n=0
while :; do
    el=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
    awk -v e="$el" -v m="$max" 'BEGIN{exit !(e>m)}' && { echo "walk TIMEOUT at ${el}s"; exit 2; }
    pgrep -f Cyberpunk2077.exe >/dev/null || { echo "walk EXITED at ${el}s"; exit 4; }
    import -window root "$d/w.png" 2>/dev/null || { sleep 1; continue; }
    txt=$(tesseract "$d/w.png" - 2>/dev/null | tr '\n' ' ' | tr -s ' ')
    # The main menu's red-on-dark items defeat plain OCR (and a plain red
    # channel works only on some frames of the animated background): keep only
    # strongly red pixels of the menu column as black-on-white, upscaled.
    read -r W H < <(identify -format '%w %h' "$d/w.png")
    convert "$d/w.png" -crop "$((W*400/1280))x$((H*300/720))+$((W*90/1280))+$((H*260/720))" +repage \
        -fx '(r>0.6&&g<0.45)?0:1' -resize 300% "$d/wm.png"
    txt="$txt $(tesseract "$d/wm.png" - 2>/dev/null | tr '\n' ' ' | tr -s ' ')"
    n=$((n+1))
    if echo "$txt" | grep -q -i 'USER AGREEMENT'; then
        cp "$d/w.png" "$d/s$n-agreement.png"; echo "walk AGREEMENT at ${el}s: $(echo "$txt" | cut -c1-160)"; exit 3
    elif echo "$txt" | grep -q -i -E 'SKIP FOR NOW'; then
        cp "$d/w.png" "$d/s$n-account.png"; echo "$txt" > "$d/s$n-account.txt"
        echo "walk ACCOUNT-OFFER at ${el}s: $(echo "$txt" | cut -c1-200)"
        echo "$txt" > "$d/account-offer.txt"
        touch "$d/account-seen"
    elif echo "$txt" | grep -q -i -E 'Help Make Our Games Better'; then
        cp "$d/w.png" "$d/s$n-telemetry.png"; echo "$txt" > "$d/s$n-telemetry.txt"
        echo "walk TELEMETRY at ${el}s: $(echo "$txt" | cut -c1-200)"
        w=$(xdotool search --name 'Cyberpunk 2077' | head -1); [ -n "$w" ] && xdotool windowfocus "$w"
        press Escape; sleep 4
        import -window root "$d/w2.png"
        if tesseract "$d/w2.png" - 2>/dev/null | grep -q -i 'Help Make Our Games Better'; then
            # click REJECT ALL: centre the cursor, then walk it there by relative moves
            # (the game draws its own cursor from relative motion)
            xdotool mousemove 640 360; sleep 0.3
            for i in $(seq 20); do xdotool mousemove_relative -- 26 16; sleep 0.05; done
            sleep 0.5; xdotool mousedown 1; sleep 0.15; xdotool mouseup 1; sleep 4
            import -window root "$d/w2.png"
        fi
        cp "$d/w2.png" "$d/after-telemetry.png"
        if tesseract "$d/w2.png" - 2>/dev/null | grep -q -i 'Help Make Our Games Better'; then
            echo "walk TELEMETRY prompt did not take Esc or the REJECT ALL click; stop"; exit 6
        fi
        echo "walk TELEMETRY rejected"
    elif echo "$txt" | grep -q -E 'LOAD GAME.*SETTINGS|SETTINGS.*QUIT GAME|NEW GAME.*SETTINGS'; then
        cp "$d/w.png" "$d/s$n-menu.png"; echo "$txt" > "$d/menu.txt"
        date +%s.%N > "$d/t-menu.txt"
        echo "walk MENU at ${el}s: $(echo "$txt" | cut -c1-200)"; exit 0
    else
        [ $((n % 5)) = 1 ] && cp "$d/w.png" "$d/s$n-other.png"
        echo "walk other at ${el}s: $(echo "$txt" | cut -c1-120)"
        press space
    fi
    if [ -f "$d/account-seen" ] && [ ! -f "$d/account-skipped" ]; then
        # The skip key is drawn as an icon; OCR renders it "[=]"/"(=)". Try the
        # keyboard keys the game uses for "back/skip" in turn and re-check.
        for k in space Escape; do
            press "$k"; sleep 3
            import -window root "$d/w2.png"
            if ! tesseract "$d/w2.png" - 2>/dev/null | grep -q -i 'SKIP FOR NOW'; then
                echo "walk ACCOUNT-OFFER skipped with key $k"; echo "$k" > "$d/account-skipped"
                cp "$d/w2.png" "$d/after-skip.png"; break
            fi
        done
        [ -f "$d/account-skipped" ] || { echo "walk ACCOUNT-OFFER not skippable by space/Escape; stop"; exit 5; }
    fi
    sleep 2
done
