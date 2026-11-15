#!/usr/bin/env bash
# tools/cp/p0host/launch.sh — start Cyberpunk 2077 under Proton inside a headless gamescope.
#
# usage: launch.sh <tag> <W> <H>       (returns immediately; the run lives in $P0/runs/<tag>)
# knobs (env):
#   WRAP="strace -f ..."   command prefix put in front of proton (inside gamescope)
#   XENV="K=V K2=V2"       extra environment for the game
#   PFXDIR=<dir>           STEAM_COMPAT_DATA_PATH (default: our copy $PFX_PROTON)
#   RUNNER=proton|wine     proton (default) or the distro /usr/bin/wine (item 9)
#   NS="snd,xdgrun,..."    extra ns.sh specs; every run is wrapped in `ns.sh rolib[,NS]` so the
#                          Steam library (game dir, Proton, shadercache, the user's prefix) is
#                          read-only for everything this run starts
#   MANGO=1                load MangoHud (per-frame CSV via Shift_L+F2 toggle)
#   GAMEARGS="..."         default: --launcher-skip
# Writes: meta.txt, t0_ns, display, gamescope.log, proton log, mangohud/ CSVs.
set -u
. "$(dirname "$0")/env.sh"
tag=$1; W=$2; H=$3
R=$P0/runs/$tag
mkdir -p "$R/mangohud" "$R/shots" "$R/plog"
PFXDIR=${PFXDIR:-$PFX_PROTON}
RUNNER=${RUNNER:-proton}
GAMEARGS=${GAMEARGS:---launcher-skip}

# env the game sees (Steam's own launch sets the same SteamAppId/SteamGameId pair)
GENV=(
  SteamAppId=1091500 SteamGameId=1091500 STEAM_COMPAT_APP_ID=1091500
  STEAM_COMPAT_DATA_PATH="$PFXDIR"
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAMROOT"
  STEAM_COMPAT_INSTALL_PATH="$GAME"
  MESA_VK_DEVICE_SELECT=$GPU_ID
  VKD3D_FILTER_DEVICE_NAME="RX 9070 XT"
  DXVK_FILTER_DEVICE_NAME="RX 9070 XT"
  PROTON_LOG=1 PROTON_LOG_DIR="$R/plog"
  PYTHONDONTWRITEBYTECODE=1        # python3 proton would drop __pycache__/*.pyc into Proton's dir
  WAYLAND_DISPLAY=
  # Every cache goes under $P0: without these, vkd3d-proton writes "vkd3d-proton.cache"
  # into the CWD (= the game's bin/x64 when launched like Steam does) — measured 2026-11-14.
  STEAM_COMPAT_SHADER_PATH="$P0/cache/shader"
  VKD3D_SHADER_CACHE_PATH="$P0/cache/vkd3d"
  DXVK_STATE_CACHE_PATH="$P0/cache/dxvk"
  MESA_SHADER_CACHE_DIR="$P0/cache/mesa"
)
mkdir -p "$P0/cache/shader" "$P0/cache/vkd3d" "$P0/cache/dxvk" "$P0/cache/mesa" "$P0/cwd" "$P0/cache/steam-shadercache"
OUTER="$T/ns.sh rolib${NS:+,$NS}"
if [ "${MANGO:-0}" = 1 ]; then
  GENV+=(MANGOHUD=1 "MANGOHUD_CONFIG=log_interval=0,output_folder=$R/mangohud,fps_only,position=bottom-right,toggle_logging=Shift_L+F2,toggle_hud=Shift_R+F12")
fi
for kv in ${XENV:-}; do GENV+=("$kv"); done

if [ "$RUNNER" = proton ]; then
  GCMD=(python3 "$PROTON_DIR/proton" waitforexitandrun "$EXE" $GAMEARGS)
else
  # item 9: distro Wine, a fresh prefix ($PFXDIR/pfx), Proton's vkd3d-proton + DXVK dxgi as natives
  GENV+=(WINEPREFIX="$PFXDIR/pfx" "WINEDLLOVERRIDES=d3d12,d3d12core,dxgi=n,b" WINEDEBUG=+loaddll,+seh,err+all,fixme-all)
  GCMD=(/usr/bin/wine "$EXE" $GAMEARGS)
fi

{
  echo "tag=$tag res=${W}x${H} runner=$RUNNER pfx=$PFXDIR"
  echo "WRAP=${WRAP:-} OUTER=$OUTER XENV=${XENV:-} MANGO=${MANGO:-0} GAMEARGS=$GAMEARGS"
  echo "date=$(date -Is) kernel=$(uname -r)"
  echo "proton=$(cat "$PROTON_DIR/version")"
  printf 'env: %s\n' "${GENV[@]}"
  echo "cmd: ${GCMD[*]}"
} > "$R/meta.txt"

# CWD is our own empty dir, not bin/x64: the game drops pid-named .txt files into its CWD
# at start-up (deleted at exit) and vkd3d-proton resolves relative cache paths against it.
cd "$P0/cwd" || exit 1
touch "$R/game_dir_stamp"
date +%s%N > "$R/t0_ns"
# shellcheck disable=SC2086
nohup $OUTER gamescope --backend headless -W "$W" -H "$H" -w "$W" -h "$H" -r 1000 \
    --prefer-vk-device $GPU_ID -- \
    sh -c 'echo "$DISPLAY" > "$0/display"; exec "$@"' "$R" \
    env "${GENV[@]}" ${WRAP:-} "${GCMD[@]}" \
    > "$R/gamescope.log" 2>&1 < /dev/null &
echo $! > "$R/gamescope.pid"
echo "launched $tag pid=$(cat "$R/gamescope.pid") dir=$R"
