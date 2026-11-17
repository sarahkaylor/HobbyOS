#!/usr/bin/env bash
# tools/cp/p9/seed-saves.sh IMG PROFILE_DIR LOG SAVE... -- add the user's own
# saves to a seeded P8 prefix image (lane C9-play, docs/cp-p9.md §1).
#
# The user approved (2026-11-17, docs/cyberpunk.md §7.2f) P9 using their saves
# AutoSave-16 and ManualSave-86, staged read-only at
#   ~/cp/game-profile/Saved Games/CD Projekt Red/Cyberpunk 2077/<SAVE>/
# This copies each named save directory, byte-identical (sha256 read back),
# next to the profile tools/cp/p8/seed-prefix.sh already wrote:
#   pfx/drive_c/users/<user>/Saved Games/CD Projekt Red/Cyberpunk 2077/<SAVE>/
# Host side, on the UNMOUNTED ext2 image, with debugfs (no loop mount, no root).
# Save files never go into git; this script only moves them host -> image.
set -euo pipefail
IMG="$1"; PROF="$2"; LOG="$3"; shift 3
[ "$#" -ge 1 ] || { echo "usage: seed-saves.sh IMG PROFILE_DIR LOG SAVE..." >&2; exit 2; }
DBG=/usr/sbin/debugfs
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
dq() { "$DBG" -R "$1" "$IMG" 2>/dev/null; }
exec > >(tee -a "$LOG") 2>&1

SG="Saved Games/CD Projekt Red/Cyberpunk 2077"
U="$(dq 'ls -p /pfx/drive_c/users' | awk -F/ '$6!="" && $6!="." && $6!=".." && $6!="Public" {print $6}' | head -1)"
[ -n "$U" ] || { echo "seed-saves: no user directory in the prefix" >&2; exit 2; }
D="/pfx/drive_c/users/$U/$SG"
dq "stat \"$D\"" | grep -q 'Type: directory' || { echo "seed-saves: no $D (run seed-prefix.sh first)" >&2; exit 2; }

B="$T/cmds"; : > "$B"
for s in "$@"; do
  case "$s" in */*|.*|'') echo "seed-saves: bad save name '$s'" >&2; exit 2;; esac
  src="$PROF/$SG/$s"
  [ -d "$src" ] || { echo "seed-saves: no $src" >&2; exit 2; }
  ls "$src"/metadata.*.json "$src/sav.dat" >/dev/null || { echo "seed-saves: $src is not a save" >&2; exit 2; }
  dq "stat \"$D/$s\"" | grep -q 'Type: directory' || echo "mkdir \"$D/$s\"" >> "$B"
  for f in "$src"/*; do
    b="${f##*/}"
    { echo "rm \"$D/$s/$b\""; echo "write \"$f\" \"$D/$s/$b\""; echo "sif \"$D/$s/$b\" mode 0100644"; } >> "$B"
  done
done
"$DBG" -w -f "$B" "$IMG" > "$T/debugfs.out" 2>&1 || { cat "$T/debugfs.out"; echo "seed-saves: debugfs failed" >&2; exit 1; }

bad=0
echo "== seed-saves: users/$U/$SG (sha256 read back from the image; names and sizes only)"
for s in "$@"; do
  for f in "$PROF/$SG/$s"/*; do
    b="${f##*/}"
    want="$(sha256sum "$f" | cut -d' ' -f1)"
    got="$("$DBG" -R "cat \"$D/$s/$b\"" "$IMG" 2>/dev/null | sha256sum | cut -d' ' -f1)"
    if [ "$want" = "$got" ]; then printf '   ok %s %10d %s/%s\n' "${want:0:16}" "$(stat -c %s "$f")" "$s" "$b"
    else printf '   MISMATCH %s/%s\n' "$s" "$b"; bad=1; fi
  done
done
[ "$bad" = 0 ] || { echo "seed-saves: verification failed" >&2; exit 1; }
echo "== seed-saves: done"
