#!/usr/bin/env bash
# cp/p0guest: run d3d12probe.exe on the HOST, directly on RADV (no VM), with
# the same Proton Experimental build the guest uses — the "native" column for
# ladder rows (c), (d) and (g).
#
# Proton's files are only read (its wine binary runs from the user's library;
# wine never writes to its install tree). Everything written — prefix, HOME,
# caches — is under ~/cp/p0guest/host. No display: DISPLAY/WAYLAND_DISPLAY are
# unset so nothing appears on the user's desktop; only the windowless probe
# modes (features, offscreen, fence) are meaningful here.
#
#   host-d3d12probe.sh features | offscreen W H S ITERS DRAWS | fence N
set -euo pipefail
ROOT=${CP_P0GUEST:-$HOME/cp/p0guest}
PFILES=${PFILES:-"/mnt/fast/games/steamapps/common/Proton - Experimental/files"}
H=$ROOT/host
mkdir -p "$H/home"
export HOME=$H/home XDG_CACHE_HOME=$H/home/.cache XDG_DATA_HOME=$H/home/.local/share XDG_CONFIG_HOME=$H/home/.config
unset DISPLAY WAYLAND_DISPLAY
export WINEPREFIX=$H/pfx WINEARCH=wow64 WINEDEBUG=${WINEDEBUG:--all}
# winemenubuilder disabled: it would write .desktop/mime files into $HOME.
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi,d3d11,d3d10core,d3d9=n;winemenubuilder.exe=d;mscoree,mshtml="
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
export MESA_VK_DEVICE_SELECT=1002:7550!
export SteamAppId=1091500 SteamGameId=1091500   # same vkd3d-proton app profile as in the guest
# As Proton's own session setup does (proton: ld_library_path / dllpaths).
export WINEDLLPATH="$PFILES/lib/vkd3d:$PFILES/lib/wine"
export LD_LIBRARY_PATH="$PFILES/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
WINE="$PFILES/bin/wine"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    "$WINE" wineboot -u >/dev/null 2>&1
    "$PFILES/bin/wineserver" -w
    for f in d3d12 d3d12core; do cp "$PFILES/lib/wine/vkd3d-proton/x86_64-windows/$f.dll" "$WINEPREFIX/drive_c/windows/system32/"; done
    for f in dxgi d3d11 d3d10core d3d9; do cp "$PFILES/lib/wine/dxvk/x86_64-windows/$f.dll" "$WINEPREFIX/drive_c/windows/system32/"; done
fi
# Proton's default_pfx carries Wine's libvkd3d*.dll in system32 (wined3d ->
# d3dcompiler_47 needs them); a plain wineboot prefix does not.
for f in "$PFILES"/lib/vkd3d/x86_64-windows/*.dll; do
    [ -f "$WINEPREFIX/drive_c/windows/system32/${f##*/}" ] || cp "$f" "$WINEPREFIX/drive_c/windows/system32/"
done
cd "$H"
"$WINE" "$ROOT/build/d3d12probe.exe" "$@"
rc=$?
"$PFILES/bin/wineserver" -k 2>/dev/null || true
exit $rc
