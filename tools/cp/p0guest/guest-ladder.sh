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
    # Needs the user agreement accepted in the guest prefix (done by the
    # coordinator with the user's go-ahead) and the Low preset chosen on the
    # Graphics page once (it persists in UserSettings.json). Per run:
    #   launch (MangoHud loaded, not logging) -> guest-cp-walk.sh to the main
    #   menu (Space at prompts; account offer skipped; telemetry rejected;
    #   stops at an agreement) -> launch-to-menu time -> 5 s settle ->
    #   MangoHud log for MENU_SECS (Shift_L+F2 on/off) while sampling the
    #   game's RSS and thread count -> stats -> screenshots of the VIDEO and
    #   GRAPHICS settings pages as the record of what was measured.
    geom=${GEOM:-1280x720}
    W=${geom%x*}; H=${geom#*x}
    kms_mode "$geom"
    python3 "$T/guest-cp-settings.py" "$geom" | sed 's/^/settings: /'
    tag=menu-$geom-$(grep -q -w avx /proc/cpuinfo && echo avx || echo noavx)-$(date +%H%M%S)
    CP_MANGO=1 CP_MANGO_HOTKEY=1 bash "$T/guest-cp.sh" launch "$tag" --launcher-skip >/dev/null
    d=$HOME/runs/$tag
    bash "$T/guest-cp-walk.sh" "$tag" 400 > "$d/walk.txt"; rc=$?
    grep -v 'walk other' "$d/walk.txt"
    t0=$(cat "$d/t-launch.txt")
    if [ $rc = 0 ]; then
        tm=$(cat "$d/t-menu.txt")
        echo "cpmenu tag=$tag geom=$geom launch_to_menu_s=$(awk -v a="$t0" -v b="$tm" 'BEGIN{printf "%.1f", b-a}') (OCR poll, ~3-4 s granularity)"
        bash "$T/guest-cp-ui.sh" at $((W*78/100)) $((H*50/100))   # cursor off the menu items
        sleep 5
        mh() { xdotool keydown Shift_L keydown F2; sleep 0.3; xdotool keyup F2 keyup Shift_L; }
        mh; ts=$(date +%s.%N); maxrss=0; maxthr=0
        for i in $(seq $(( ${MENU_SECS:-60} / 5 ))); do
            sleep 5
            # (comm is truncated to "Cyberpunk2077.e", so find the pid by its command line)
            gp=$(pgrep -f '^\./Cyberpunk2077\.exe' | head -1)
            read -r rss thr < <(ps -o rss=,nlwp= -p "${gp:-1}" 2>/dev/null || echo "0 0")
            [ "${rss:-0}" -gt "$maxrss" ] && maxrss=$rss
            [ "${thr:-0}" -gt "$maxthr" ] && maxthr=$thr
        done
        mh; te=$(date +%s.%N); sleep 3
        echo "cpmenu hold_s=$(awk -v a="$ts" -v b="$te" 'BEGIN{printf "%.1f", b-a}') peak_rss_mib=$((maxrss/1024)) peak_threads=$maxthr (5 s samples of Cyberpunk2077.exe)"
        csv=$(ls -t "$d"/*.csv 2>/dev/null | grep -v summary | head -1)
        if [ -n "$csv" ]; then python3 "$T/mango-stats.py" "$csv" 0; else echo "cpmenu no MangoHud CSV"; fi
        free -m | sed -n 2p | awk '{print "guest_mem_used_mib=" $3}'
        # record of the settings in effect
        U="bash $T/guest-cp-ui.sh"
        $U click $((W*158/1280)) $((H*364/720)); sleep 4
        $U click $((W*564/1280)) $((H*32/720)); sleep 3; $U at $((W*95/100)) $((H*95/100)); sleep 1; $U shot "$d/settings-video.png"
        $U click $((W*497/1280)) $((H*32/720)); sleep 3; $U at $((W*95/100)) $((H*95/100)); sleep 1; $U shot "$d/settings-graphics.png"
        echo "settings-video OCR: $(tesseract "$d/settings-video.png" - 2>/dev/null | grep -i -E 'VSync|Maximum|Windowed|Resolution' | tr '\n' '|')"
        echo "settings-graphics OCR: $(tesseract "$d/settings-graphics.png" - 2>/dev/null | grep -i -E 'Quick Preset|Scaling|Texture|Ray Tracing|Path Tracing' | tr '\n' '|')"
        $U key Escape; sleep 2
    else
        echo "cpmenu tag=$tag geom=$geom walk rc=$rc (no menu) after $(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}') s"
    fi
    bash "$T/guest-cp.sh" kill >/dev/null ;;
*) sed -n '2,8p' "$0"; exit 2 ;;
esac
