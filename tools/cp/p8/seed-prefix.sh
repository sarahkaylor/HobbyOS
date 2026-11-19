#!/usr/bin/env bash
# tools/cp/p8/seed-prefix.sh IMG PE_DIR PROFILE_DIR KEYS LOG -- seed a P8 prefix
# image (lane C8-game, docs/cp-p8.md §2) after the in-guest `wineboot -i` boot.
# Host side, on the UNMOUNTED ext2 image, with debugfs (no loop mount, no root):
#
#   1. vkd3d-proton 3.1 d3d12.dll + d3d12core.dll and DXVK 3.1.1 dxgi.dll +
#      d3d11.dll replace Wine's builtin placeholders in
#      pfx/drive_c/windows/system32 (Proton's layout; the launcher's
#      WINEDLLOVERRIDES=d3d12,d3d12core,dxgi,d3d11=n then loads these files);
#   2. the user's own first-run profile (docs/cyberpunk.md §0.2 item 1: the
#      agreement accepted, the account offer skipped, telemetry rejected; staged
#      read-only at ~/cp/game-profile by the integrator) goes to
#        users/<user>/Saved Games/CD Projekt Red/Cyberpunk 2077/{user.gls,EnabledMods.json,HDRUserSettings.json}
#        users/<user>/AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json
#      byte-identical (sha256 checked after the write);
#   3. the cache directories on ext2: pfx/drive_c/vc (VKD3D_SHADER_CACHE_PATH
#      = C:\vc) and mc at the volume root (MESA_SHADER_CACHE_DIR = /EXT/mc).
#      Short names on purpose: a HobbyOS process has 512 bytes of env.
#
# UserSettings.json is only CHECKED against the P0 key list (KEYS =
# tools/cp/p0host/settings-low-native.keys) plus the resolution 1280x720; it is
# never rewritten unless P8_SETTINGS_FIX=1, which applies exactly the KEYS lines
# that differ (and logs them) to a copy before the write.
set -euo pipefail
IMG="$1"; PE="$2"; PROF="$3"; KEYS="$4"; LOG="$5"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DBG=/usr/sbin/debugfs
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
dq() { "$DBG" -R "$1" "$IMG" 2>/dev/null; }
exec > >(tee -a "$LOG") 2>&1

SG="Saved Games/CD Projekt Red/Cyberpunk 2077"
AL="AppData/Local/CD Projekt Red/Cyberpunk 2077"
for f in "$SG/user.gls" "$SG/EnabledMods.json" "$SG/HDRUserSettings.json" "$AL/UserSettings.json"; do
  [ -f "$PROF/$f" ] || { echo "seed: no $PROF/$f" >&2; exit 2; }
done
for f in vkd3d/d3d12.dll vkd3d/d3d12core.dll dxvk/dxgi.dll dxvk/d3d11.dll; do
  [ -f "$PE/$f" ] || { echo "seed: no $PE/$f" >&2; exit 2; }
done

# The prefix's user directory: the one wineboot made that is not Public.
U="$(dq 'ls -p /pfx/drive_c/users' | awk -F/ '$6!="" && $6!="." && $6!=".." && $6!="Public" {print $6}' | head -1)"
[ -n "$U" ] || { echo "seed: no user directory under /pfx/drive_c/users (wineboot did not finish?)" >&2; exit 2; }
echo "== seed: prefix user directory: users/$U"
UD="/pfx/drive_c/users/$U"
S32="/pfx/drive_c/windows/system32"

# ---- settings check (P0 key list) -------------------------------------------
US="$T/UserSettings.json"
cp "$PROF/$AL/UserSettings.json" "$US"; chmod u+w "$US"
python3 -I - "$US" "$KEYS" "${P8_SETTINGS_FIX:-0}" <<'PY'
import json, sys
path, keys, fix = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
d = json.load(open(path, encoding="utf-8"))
flat = {g["group_name"] + "/" + o["name"]: o for g in d["data"] for o in g["options"]}
want = {}
for ln in open(keys, encoding="utf-8"):
    ln = ln.strip()
    if not ln or ln.startswith("#"):
        continue
    k, v = ln.split("=", 1)
    want[k] = json.loads(v)
want["/video/display/Resolution"] = "1280x720"
diff = []
for k, v in want.items():
    have = flat.get(k, {}).get("value", "<absent>")
    if have != v:
        diff.append((k, have, v))
print(f"== settings: version {d.get('version')}; {len(want)} keys checked (P0 list + Resolution), {len(diff)} differ")
for k in ("/graphics/presets/QuickPresets", "/video/display/Resolution", "/video/display/WindowMode",
          "/graphics/raytracing/RayTracing", "/graphics/presets/ResolutionScaling",
          "/graphics/presets/TextureQuality", "/gameplay/misc/EnableTelemetry", "/gameplay/misc/EnableMarketing"):
    print(f"   {k} = {json.dumps(flat.get(k, {}).get('value', '<absent>'))}")
for k, have, v in diff:
    print(f"   DIFF {k}: {json.dumps(have)} (want {json.dumps(v)})")
if diff and fix:
    for k, have, v in diff:
        if k in flat:
            o = flat[k]
            o["value"] = v
            if "values" in o and v in o["values"]:
                o["index"] = o["values"].index(v)
    json.dump(d, open(path, "w", encoding="utf-8"), indent=2)
    print(f"== settings: P8_SETTINGS_FIX=1 applied {len(diff)} key(s)")
elif diff:
    print("== settings: NOT changed (P8_SETTINGS_FIX=1 applies the P0 keys)")
PY

# ---- debugfs batch ------------------------------------------------------------
B="$T/cmds"
{
  # mkdir only what is missing: debugfs's mkdir of an existing name allocates
  # the inode before the link fails and leaves it unconnected (e2fsck pass 3;
  # measured on the first seeded image -- wineboot already made AppData/Local).
  for d in "Saved Games" "Saved Games/CD Projekt Red" "$SG" \
           "AppData" "AppData/Local" "AppData/Local/CD Projekt Red" "$AL"; do
    dq "stat \"$UD/$d\"" | grep -q 'Type: directory' || echo "mkdir \"$UD/$d\""
  done
  for f in user.gls EnabledMods.json HDRUserSettings.json; do
    echo "rm \"$UD/$SG/$f\""
    echo "write \"$PROF/$SG/$f\" \"$UD/$SG/$f\""
    echo "sif \"$UD/$SG/$f\" mode 0100644"
  done
  echo "rm \"$UD/$AL/UserSettings.json\""
  echo "write \"$US\" \"$UD/$AL/UserSettings.json\""
  echo "sif \"$UD/$AL/UserSettings.json\" mode 0100644"
  for f in vkd3d/d3d12.dll vkd3d/d3d12core.dll dxvk/dxgi.dll dxvk/d3d11.dll; do
    b="${f#*/}"
    echo "rm \"$S32/$b\""
    echo "write \"$PE/$f\" \"$S32/$b\""
    echo "sif \"$S32/$b\" mode 0100644"
  done
  for d in /pfx/drive_c/vc /mc; do
    dq "stat $d" | grep -q 'Type: directory' || echo "mkdir $d"
  done
} > "$B"
"$DBG" -w -f "$B" "$IMG" > "$T/debugfs.out" 2>&1 || { cat "$T/debugfs.out"; echo "seed: debugfs failed" >&2; exit 1; }
grep -E 'rm: |File not found' "$T/debugfs.out" | sed 's/^/   (expected for a fresh prefix) /' | head -20 || true

# ---- verify (sha256 of every seeded file read back through debugfs) ---------
bad=0
chk() { # $1 host file  $2 image path
  local want got
  want="$(sha256sum "$1" | cut -d' ' -f1)"
  got="$("$DBG" -R "cat \"$2\"" "$IMG" 2>/dev/null | sha256sum | cut -d' ' -f1)"
  if [ "$want" = "$got" ]; then printf '   ok %s %10d %s\n' "${want:0:16}" "$(stat -c %s "$1")" "$2"
  else printf '   MISMATCH %s want %s got %s\n' "$2" "${want:0:16}" "${got:0:16}"; bad=1; fi
}
echo "== seeded files (sha256 read back from the image)"
for f in user.gls EnabledMods.json HDRUserSettings.json; do chk "$PROF/$SG/$f" "$UD/$SG/$f"; done
chk "$US" "$UD/$AL/UserSettings.json"
for f in vkd3d/d3d12.dll vkd3d/d3d12core.dll dxvk/dxgi.dll dxvk/d3d11.dll; do chk "$PE/$f" "$S32/${f#*/}"; done
for d in /pfx/drive_c/vc /mc; do
  dq "stat $d" | grep -q 'Type: directory' && echo "   ok dir $d" || { echo "   MISSING dir $d"; bad=1; }
done
[ "$bad" = 0 ] || { echo "seed: verification failed" >&2; exit 1; }
echo "== seed: done"
