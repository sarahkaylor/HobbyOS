#!/usr/bin/env bash
# tools/cp/p9/saves.sh IMG -- the save directories on a P9 data disk (lane
# C9-play, docs/cp-p9.md): for each save, its files (sizes, mtimes) and from the
# game's own metadata.*.json only name, timestamp, play time and
# playerPosition -- enough to tell whether a reloaded save is the same place.
# Read-only (debugfs -R), never copies a save out of the image.
set -uo pipefail
IMG="$1"
DBG=/usr/sbin/debugfs
dq() { "$DBG" -R "$1" "$IMG" 2>/dev/null; }
U="$(dq 'ls -p /pfx/drive_c/users' | awk -F/ '$6!="" && $6!="." && $6!=".." && $6!="Public" {print $6}' | head -1)"
D="/pfx/drive_c/users/$U/Saved Games/CD Projekt Red/Cyberpunk 2077"
dq "ls -p \"$D\"" | awk -F/ '$6!="" && $6!="." && $6!=".." && $3 ~ /^04/ {print $6}' | sort | while read -r s; do
  files="$(dq "ls -l \"$D/$s\"" | awk 'NF>=8 && $NF!="." && $NF!=".." {printf "%s(%s) ", $NF, $6}')"
  meta="$(dq "ls -p \"$D/$s\"" | awk -F/ '$6 ~ /^metadata\..*\.json$/ {print $6}' | head -1)"
  pos=""
  if [ -n "$meta" ]; then
    pos="$(dq "cat \"$D/$s/$meta\"" | python3 -I -c '
import json, sys
try:
    m = json.load(sys.stdin)["Data"]["metadata"]
    p = m.get("playerPosition", {})
    print("name=%s time=%s playTime=%.1f pos=(%.2f, %.2f, %.2f)" % (m.get("name"), m.get("timestampString"),
          float(m.get("playTime", 0)), float(p.get("X", 0)), float(p.get("Y", 0)), float(p.get("Z", 0))))
except Exception as e:
    print("metadata unreadable: %s" % e)
')"
  fi
  printf '%-16s %s\n%-16s %s\n' "$s" "$pos" "" "$files"
done
