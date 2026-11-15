#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): launch and observe Cyberpunk 2077 under
# Proton Experimental (runinprefix: wine directly, no Proton steam.exe) on Venus.
#
#   guest-cp.sh launch <tag> [game args...]   start in the background; logs in ~/runs/<tag>/
#   guest-cp.sh shot <tag> <name>             X root screenshot -> ~/runs/<tag>/<name>.png
#   guest-cp.sh ps                            game/wine processes
#   guest-cp.sh kill                          wineserver -k (ends every wine process)
#
# Env: CP_LOG=1 -> PROTON_LOG=1 (+loaddll,+seh,... into ~/runs/<tag>/steam-1091500.log)
#      CP_MANGO=1 -> MangoHud CSV logging (no overlay) into ~/runs/<tag>/
set -euo pipefail
T=$(dirname "$(readlink -f "$0")")
. "$T/guest-env.sh"
cmd=${1:-}; shift || true
case "$cmd" in
launch)
    tag=$1; shift
    d=$HOME/runs/$tag; mkdir -p "$d"
    export PROTON_LOG_DIR=$d
    [ "${CP_LOG:-0}" = 1 ] && export PROTON_LOG=1
    if [ "${CP_MANGO:-0}" = 1 ]; then
        export MANGOHUD=1
        # MangoHud 0.8.2~rc2 writes no CSV with no_display, so the HUD is drawn
        # fully transparent instead (fps_only, alpha 0).
        # CP_MANGO_HOTKEY=1: no autostart; logging is toggled with MangoHud's
        # default Shift_L+F2 (the file is written when logging stops).
        if [ "${CP_MANGO_HOTKEY:-0}" = 1 ]; then
            export MANGOHUD_CONFIG="fps_only,alpha=0.0,background_alpha=0.0,log_interval=0,output_folder=$d"
        else
            export MANGOHUD_CONFIG="fps_only,alpha=0.0,background_alpha=0.0,log_interval=0,output_folder=$d,autostart_log=${CP_MANGO_DELAY:-1}${CP_MANGO_SECS:+,log_duration=$CP_MANGO_SECS}"
        fi
    fi
    date +%s.%N > "$d/t-launch.txt"
    cd /opt/cp2077/bin/x64
    nohup "$T/guest-proton.sh" runinprefix ./Cyberpunk2077.exe "$@" > "$d/stdout.txt" 2>&1 &
    echo $! > "$d/pid.txt"
    echo "launched $tag pid $(cat "$d/pid.txt")" ;;
shot)
    d=$HOME/runs/$1; mkdir -p "$d"
    DISPLAY=:0 import -window root "$d/$2.png"
    echo "$d/$2.png" ;;
ps)
    pgrep -a -f 'Cyberpunk|wineserver|REDprelauncher|REDEngine|CrashReporter|steam.exe' | cut -c1-160 || true ;;
kill)
    WINEPREFIX=$HOME/compat/1091500/pfx /opt/proton/files/bin/wineserver -k 2>/dev/null || true
    sleep 2; pgrep -a -f 'Cyberpunk|wineserver' | cut -c1-120 || true ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
