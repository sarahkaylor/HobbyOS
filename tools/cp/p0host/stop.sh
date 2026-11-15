#!/usr/bin/env bash
# tools/cp/p0host/stop.sh <tag> — end a run: wineserver -k on its prefix, then gamescope.
set -u
. "$(dirname "$0")/env.sh"
R=$P0/runs/$1
PFXDIR=$(sed -n 's/.* pfx=//p' "$R/meta.txt" | head -1)
WINEPREFIX="$PFXDIR/pfx" "$PROTON_DIR/files/bin/wineserver" -k 2>/dev/null
WINEPREFIX="$PFXDIR/pfx" /usr/lib/x86_64-linux-gnu/wine/wineserver -k 2>/dev/null
sleep 3
gp=$(cat "$R/gamescope.pid" 2>/dev/null)
[ -n "$gp" ] && kill "$gp" 2>/dev/null
sleep 2
[ -n "$gp" ] && kill -9 "$gp" 2>/dev/null
for p in $(pgrep -f "$PFXDIR"); do
  case " $$ $PPID " in *" $p "*) ;; *) kill "$p" 2>/dev/null ;; esac
done
sleep 1
left=$(pgrep -af "Cyberpunk2077|wineserver|wine-preloader|winedevice|explorer.exe|services.exe|gamescope" | grep -v -e pgrep -e qemu)
echo "stopped $1; leftover: ${left:-none}"
# the hard rule: nothing may appear or change in the game directory
if [ -e "$R/game_dir_stamp" ]; then
  L=/mnt/fast/games/steamapps
  { find "$GAME" "$PROTON_DIR" "$L/shadercache/1091500" "$L/compatdata/1091500" -newer "$R/game_dir_stamp" ! -name '.nfs*'
    find "$L" -maxdepth 1 -newer "$R/game_dir_stamp"; } > "$R/game_dir_changes.txt" 2>/dev/null
  if [ -s "$R/game_dir_changes.txt" ]; then echo "WARNING: game dir changed during $1:"; cat "$R/game_dir_changes.txt"; fi
fi
date +%s%N > "$R/tend_ns"
