#!/usr/bin/env bash
# tools/cp/hunt/prep-settings.sh SRC.img DST.img NAME=VALUE[:INDEX] ... -- a copy
# of a seeded P8/P9 data image whose game UserSettings.json has the given
# options changed (lane H1-hunt; generalises the integrator's
# strix:~/cp/shot/prep.sh, which sets WindowMode=Windowed).  Edits only the
# copy, with debugfs; never touches ~/cp/game-profile or SRC.  e2fsck -fn after.
#   WindowMode=Windowed:0      Resolution=1920x1080      QuickPresets=Low
# INDEX is optional (the game recomputes dynamic lists' indices from value).
set -euo pipefail
DBG=/usr/sbin/debugfs
SRC="$1"; IMG="$2"; shift 2
[ $# -gt 0 ] || { echo "usage: prep-settings.sh SRC DST NAME=VALUE[:INDEX]..." >&2; exit 2; }
mkdir -p "$(dirname "$IMG")"
cp --sparse=always "$SRC" "$IMG"
U="$($DBG -R 'ls -p /pfx/drive_c/users' "$IMG" 2>/dev/null | awk -F/ '$6!="" && $6!="." && $6!=".." && $6!="Public" {print $6}' | head -1)"
D="/pfx/drive_c/users/$U/AppData/Local/CD Projekt Red/Cyberpunk 2077"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
$DBG -R "dump \"$D/UserSettings.json\" $T/orig.json" "$IMG" 2>/dev/null
python3 -I - "$T/orig.json" "$T/UserSettings.json" "$@" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
for spec in sys.argv[3:]:
    name, val = spec.split("=", 1)
    idx = None
    if ":" in val:
        val, idx = val.rsplit(":", 1)
    n = 0
    for g in d["data"]:
        for o in g["options"]:
            if o["name"] == name:
                before = json.dumps({k: o.get(k) for k in ("value", "index")})
                o["value"] = val
                if idx is not None:
                    o["index"] = int(idx)
                n += 1
                print(f"prep-settings: {g['group_name']}/{name}: {before} -> "
                      f"{json.dumps({k: o.get(k) for k in ('value', 'index')})}")
    assert n == 1, f"{name}: {n} matches"
json.dump(d, open(sys.argv[2], "w", encoding="utf-8"), indent=1)
PY
$DBG -w -f - "$IMG" >/dev/null 2>&1 <<DF
cd "$D"
rm UserSettings.json
write $T/UserSettings.json UserSettings.json
DF
for spec in "$@"; do
  n="${spec%%=*}"
  $DBG -R "cat \"$D/UserSettings.json\"" "$IMG" 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
print("prep-settings: readback", [(o["name"], o["value"], o.get("index")) for g in d["data"] for o in g["options"] if o["name"] == sys.argv[1]])' "$n"
done
/usr/sbin/e2fsck -fn "$IMG" | tail -1
