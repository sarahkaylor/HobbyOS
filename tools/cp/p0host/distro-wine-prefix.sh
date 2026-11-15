#!/usr/bin/env bash
# tools/cp/p0host/distro-wine-prefix.sh — item 9: a FRESH prefix for Ubuntu's /usr/bin/wine
# (not a copy of the user's), with the vkd3d-proton d3d12/d3d12core and DXVK dxgi DLLs that
# Proton Experimental ships copied into system32, plus our Low/native UserSettings.json.
set -eu
. "$(dirname "$0")/env.sh"
D=$P0/pfx-distro
export WINEPREFIX=$D/pfx WINEDEBUG=-all
mkdir -p "$D"
[ -f "$WINEPREFIX/system.reg" ] || wineboot -i
/usr/lib/x86_64-linux-gnu/wine/wineserver -w
S32=$WINEPREFIX/drive_c/windows/system32
cp -v "$PROTON_DIR/files/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll" \
      "$PROTON_DIR/files/lib/wine/vkd3d-proton/x86_64-windows/d3d12core.dll" \
      "$PROTON_DIR/files/lib/wine/dxvk/x86_64-windows/dxgi.dll" "$S32/"
U=$(ls "$WINEPREFIX/drive_c/users" | grep -v Public | head -1)
SD="$WINEPREFIX/drive_c/users/$U/AppData/Local/CD Projekt Red/Cyberpunk 2077"
mkdir -p "$SD"
cp "$P0/settings-low-native.json" "$SD/UserSettings.json"
python3 "$T/settings.py" "$SD/UserSettings.json" set /video/display/Resolution=1280x720
echo "prefix ready: $WINEPREFIX user=$U wine=$(wine --version)"
