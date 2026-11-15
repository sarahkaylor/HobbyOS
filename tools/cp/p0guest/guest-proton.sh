#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): Proton Experimental from the host's Steam
# library, run outside Steam, on Venus.
#
# The host exports the game and Proton directories read-only (virtiofsd
# --readonly, mounted ro). Proton wants to flock dist.lock in its own directory
# and the game may write next to itself, so each gets a guest-local overlayfs:
# reads come from the user's files, writes land in ~/ovl/<name>/up on the
# guest disk. Nothing ever reaches the host's copies.
#
#   guest-proton.sh mount                      overlays at /opt/proton and /opt/cp2077
#   guest-proton.sh run <exe> [args...]        proton "run" (via Proton's steam.exe shim)
#   guest-proton.sh runinprefix <exe> [args]   proton "runinprefix" (wine directly, no steam.exe)
#   guest-proton.sh wine <args...>             the prefix's wine with Proton's environment
set -euo pipefail
. "$(dirname "$0")/guest-env.sh"
ovl() {  # ovl <lower> <mountpoint> <name>
    mountpoint -q "$2" && return 0
    mkdir -p "$HOME/ovl/$3/up" "$HOME/ovl/$3/work"
    sudo mkdir -p "$2"
    sudo mount -t overlay "ovl-$3" -o "lowerdir=$1,upperdir=$HOME/ovl/$3/up,workdir=$HOME/ovl/$3/work" "$2"
}
export STEAM_COMPAT_DATA_PATH=${STEAM_COMPAT_DATA_PATH:-$HOME/compat/1091500}
export STEAM_COMPAT_CLIENT_INSTALL_PATH=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/steam-none}
export SteamGameId=${SteamGameId:-1091500}
export SteamAppId=${SteamAppId:-1091500}
export PROTON_LOG_DIR=${PROTON_LOG_DIR:-$HOME/logs}
# 64-bit-only guest (no i386 libc): without this Proton's 32-bit loader fails
# with "/lib/ld-linux.so.2: could not open". New-style WoW64 needs no i386 libs.
export PROTON_USE_WOW64=${PROTON_USE_WOW64:-1}
# Proton only sets the d3d/dxgi overrides on its "run" path (prefix update),
# which needs a Steam client (steam.exe asserts: "unable to load native
# steamclient library"). For runinprefix we set them ourselves, exactly as
# Proton would: vkd3d-proton for d3d12/d3d12core, DXVK for dxgi/d3d11/d3d10core/d3d9.
# The DLLs themselves were copied into the prefix by one "run" (guest-setup).
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"d3d12,d3d12core,dxgi,d3d11,d3d10core,d3d9=n"}
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$STEAM_COMPAT_CLIENT_INSTALL_PATH" "$PROTON_LOG_DIR"
case "${1:-}" in
mount)
    ovl /mnt/proton /opt/proton proton
    ovl /mnt/cp2077 /opt/cp2077 cp2077
    mount | grep -E '/opt/(proton|cp2077)' ;;
run|runinprefix|waitforexitandrun)
    verb=$1; shift
    exec python3 /opt/proton/proton "$verb" "$@" ;;
wine)
    shift
    export WINEPREFIX=$STEAM_COMPAT_DATA_PATH/pfx
    exec /opt/proton/files/bin/wine "$@" ;;
*) sed -n '2,16p' "$0"; exit 2 ;;
esac
