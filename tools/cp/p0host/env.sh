# tools/cp/p0host/env.sh — common paths for the C7-p0host lane (sourced, runs on strix).
# Nothing here writes outside $P0.  The game dir, the Steam library and the user's
# own prefix are READ-ONLY inputs.
P0=/home/sarah/cp/p0host
T=$P0/tools
GAME="/mnt/fast/games/steamapps/common/Cyberpunk 2077"
EXE="$GAME/bin/x64/Cyberpunk2077.exe"
PROTON_DIR="/mnt/fast/games/steamapps/common/Proton - Experimental"
STEAMROOT=/home/sarah/.local/share/Steam
USER_PFX=/mnt/fast/games/steamapps/compatdata/1091500      # never written
PFX_PROTON=$P0/pfx-proton/1091500                           # our copy (STEAM_COMPAT_DATA_PATH)
SETTINGS_REL="pfx/drive_c/users/steamuser/AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json"
LOCK=/home/sarah/cp/measure.lock
GPU_ID=1002:7550                                            # RX 9070 XT (65:00.0, card2, renderD129)
VRAM_USED=/sys/class/drm/card2/device/mem_info_vram_used
GTT_USED=/sys/class/drm/card2/device/mem_info_gtt_used
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
