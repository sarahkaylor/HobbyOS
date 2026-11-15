#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): ladder rows (b), (c) and (g), raw.
# Called by host-side measure.sh, which holds ~/cp/measure.lock on the host.
#   guest-ladder.sh vkcube   -> vkcube fps matrix (present path x size x mode)
#   guest-ladder.sh d3d12    -> d3d12probe present/offscreen matrix
#   guest-ladder.sh fence    -> d3d12probe fence round-trip
# Output: raw rows on stdout.
set -uo pipefail
T=$(dirname "$(readlink -f "$0")")
. "$T/guest-env.sh"
P=$HOME/build/d3d12probe.exe
SECS=${SECS:-15}

kms_mode() { bash "$T/guest-x.sh" mode "$1" >/dev/null; sleep 1; }
xvfb_on() { bash "$T/guest-x.sh" xvfb 1920x1080 >/dev/null; }

probe() {  # probe <label> <display> <env...> -- <args...>
    local label=$1 disp=$2; shift 2
    local envs=()
    while [ "$1" != -- ]; do envs+=("$1"); shift; done; shift
    echo "### $label DISPLAY=$disp ${envs[*]} d3d12probe $*"
    (cd "$HOME/build" && env DISPLAY="$disp" "${envs[@]}" timeout 120 bash "$T/guest-proton.sh" runinprefix "$P" "$@" 2>/dev/null \
        | grep -E '^(START|RESULT|FAIL|SEC)' | grep -v '^SEC' )
    WINEPREFIX=$HOME/compat/1091500/pfx /opt/proton/files/bin/wineserver -w 2>/dev/null || true
}

case "${1:-}" in
vkcube)
    xvfb_on
    for geom in 1280x720 1920x1080; do
        kms_mode "$geom"
        for pm in 0 1 2; do bash "$T/guest-vkcube.sh" "kms-dmabuf" :0 "$pm" "$geom"; done
        bash "$T/guest-vkcube.sh" "kms-swcopy" :0 0 "$geom" MESA_VK_WSI_DEBUG=sw
        bash "$T/guest-vkcube.sh" "xvfb-swcopy" :1 0 "$geom" MESA_VK_WSI_DEBUG=sw
    done
    kms_mode 1920x1080 ;;
d3d12)
    xvfb_on
    for geom in 1280x720 1920x1080; do
        W=${geom%x*}; H=${geom#*x}
        kms_mode "$geom"
        probe "present-kms-dmabuf light" :0 -- present "$W" "$H" "$SECS" 16 1
        probe "present-kms-dmabuf draws2000" :0 -- present "$W" "$H" "$SECS" 16 2000
        probe "present-kms-swcopy light" :0 MESA_VK_WSI_DEBUG=sw -- present "$W" "$H" "$SECS" 16 1
        probe "present-xvfb-swcopy light" :1 MESA_VK_WSI_DEBUG=sw -- present "$W" "$H" "$SECS" 16 1
        probe "offscreen light" :0 -- offscreen "$W" "$H" "$SECS" 16 1
        probe "offscreen gpuheavy" :0 -- offscreen "$W" "$H" "$SECS" 2048 1
        probe "offscreen draws2000" :0 -- offscreen "$W" "$H" "$SECS" 16 2000
    done
    kms_mode 1920x1080 ;;
fence)
    probe "fence" :0 -- fence 5000
    probe "fence" :0 -- fence 5000 ;;
cpfirst)
    # Launch-to-first-interactive-screen: launch the game, OCR the X root every
    # 2 s until the "PRESS ... TO CONTINUE" prompt or the user agreement shows,
    # then stop the game. Nothing is clicked or accepted. Twice: cold (first)
    # and warm (second; guest page cache + vkd3d-proton cache).
    geom=${GEOM:-1280x720}
    kms_mode "$geom"
    for run in 1 2; do
        tag=first-$(date +%H%M%S)-$run
        bash "$T/guest-cp.sh" launch "$tag" --launcher-skip >/dev/null
        d=$HOME/runs/$tag; t0=$(cat "$d/t-launch.txt"); found=
        for i in $(seq 200); do
            sleep 2
            DISPLAY=:0 import -window root "$d/poll.png" 2>/dev/null || continue
            txt=$(tesseract "$d/poll.png" - 2>/dev/null | tr '\n' ' ')
            if echo "$txt" | grep -q -i -E 'TO CONTINUE|USER AGREEMENT'; then
                found=$(echo "$txt" | grep -o -i -E 'PRESS.{0,12}TO CONTINUE|USER AGREEMENT' | head -1); break
            fi
            pgrep -f Cyberpunk2077.exe >/dev/null || { found="EXITED"; break; }
        done
        t1=$(date +%s.%N)
        cp "$d/poll.png" "$d/first.png" 2>/dev/null || true
        echo "cpfirst run=$run tag=$tag geom=$geom seconds=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}') screen=\"${found:-TIMEOUT}\""
        bash "$T/guest-cp.sh" kill >/dev/null
        sleep 3
    done ;;
cpmenu)
    # NEEDS the user agreement already accepted in the guest prefix (by the
    # user). Launch with MangoHud CSV logging, press Space at each "PRESS ...
    # TO CONTINUE" prompt, detect the main menu by OCR, then hold the menu
    # for MENU_SECS and report frame times from the window after the menu
    # appeared (+5 s settle). Never clicks anything else.
    geom=${GEOM:-1280x720}
    kms_mode "$geom"
    python3 "$T/guest-cp-settings.py" "$geom" | sed 's/^/settings: /'
    tag=menu-$geom-$(date +%H%M%S)
    CP_MANGO=1 bash "$T/guest-cp.sh" launch "$tag" --launcher-skip >/dev/null
    d=$HOME/runs/$tag; t0=$(cat "$d/t-launch.txt"); state=TIMEOUT
    for i in $(seq 300); do
        sleep 2
        DISPLAY=:0 import -window root "$d/poll.png" 2>/dev/null || continue
        txt=$(tesseract "$d/poll.png" - 2>/dev/null | tr '\n' ' ')
        if echo "$txt" | grep -q -i 'USER AGREEMENT'; then state=AGREEMENT; break; fi
        if echo "$txt" | grep -q -i -E 'NEW GAME|LOAD GAME|CREDITS|ADDITIONAL CONTENT'; then state=MENU; break; fi
        if echo "$txt" | grep -q -i 'TO CONTINUE'; then DISPLAY=:0 xdotool key space; fi
        pgrep -f Cyberpunk2077.exe >/dev/null || { state=EXITED; break; }
    done
    tm=$(date +%s.%N)
    cp "$d/poll.png" "$d/menu.png" 2>/dev/null || true
    echo "cpmenu tag=$tag geom=$geom state=$state launch_to_state_s=$(awk -v a="$t0" -v b="$tm" 'BEGIN{printf "%.1f", b-a}')"
    if [ "$state" = MENU ]; then
        sleep "${MENU_SECS:-60}"
        csv=$(ls -t "$d"/*.csv 2>/dev/null | grep -v summary | head -1)
        python3 "$T/mango-stats.py" "$csv" "$(awk -v a="$t0" -v b="$tm" 'BEGIN{printf "%.3f", b-a+5}')"
    fi
    bash "$T/guest-cp.sh" kill >/dev/null ;;
*) sed -n '2,8p' "$0"; exit 2 ;;
esac
