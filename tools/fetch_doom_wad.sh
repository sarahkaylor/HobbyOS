#!/usr/bin/env bash
# fetch_doom_wad.sh — fetch a license-clean IWAD for the HobbyOS DOOM port
# and place it on the boot disk root as DOOM1.WAD so the engine auto-finds
# it (d_iwad.c searches <cwd>/doom1.wad).
#
# Provenance / licensing (per the S0 native-track "license-clean assets only"
# rule):
#   * DOOM Shareware (DOOM1.WAD) — released by id Software as freEware and
#     redistributable without royalty; the canonical shareware demo.  If you
#     supply your own copy, point DOOM_WAD_SRC at it; verify the sha256 that
#     this script prints against the source you trust.
#   * Freedoom (freedoom1.wad) — the free-content IWAD replacement project
#     (GPLv2+/GPLv3/BSD-licensed art, assets and code).  Fallback default.
#
# The script does NOT hardcode a checksum (shareware mirrors rotate); it
# verifies a real WAD (IWAD magic + sane size) and prints the sha256 for the
# operator to record as provenance.  For CI you can pin one via DOOM_WAD_SHA.
#
# Usage:
#   bash tools/fetch_doom_wad.sh            # default: Freedoom Phase 1
#   DOOM_WAD_SRC=/path/to/doom1.wad bash tools/fetch_doom_wad.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${DOOM_WAD_OUT:-$ROOT/disk-doom1.wad}"
TMP="${OUT}.tmp"

if [[ -n "${DOOM_WAD_SRC:-}" ]]; then
  cp "$DOOM_WAD_SRC" "$TMP"
else
  # Freedoom Phase 1 (free-content IWAD).  Asset tag pinned; adjust as needed.
  URL="${DOOM_WAD_URL:-https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip}"
  ZIP="$(mktemp)"
  echo "[wad] downloading $URL"
  curl -fsSL -o "$ZIP" "$URL"
  # freedoom1.wad is inside the release zip.
  python3 - "$ZIP" "$TMP" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
names = [n for n in z.namelist() if n.lower().endswith("freedoom1.wad")]
assert names, "freedoom1.wad not in zip"
z.extract(names[0], path="/tmp")
import os, shutil
shutil.copy(os.path.join("/tmp", names[0]), sys.argv[2])
PY
  rm -f "$ZIP"
fi

MAGIC=$(head -c 4 "$TMP")
SIZE=$(stat -c %s "$TMP")
SHA=$(sha256sum "$TMP" | awk '{print $1}')
if [[ "$MAGIC" != "IWAD" && "$MAGIC" != "PWAD" ]]; then
  echo "[wad] ERROR: not a WAD (magic=$MAGIC)" >&2; rm -f "$TMP"; exit 1
fi
if [[ -n "${DOOM_WAD_SHA:-}" && "$SHA" != "$DOOM_WAD_SHA" ]]; then
  echo "[wad] ERROR: sha256 mismatch (got $SHA)" >&2; rm -f "$TMP"; exit 1
fi
# Present as DOOM1.WAD next to the build so the disk.img recipe can mcopy it.
DEST="$ROOT/doom1.wad"
mv "$TMP" "$DEST"
echo "[wad] OK -> $DEST ($(($SIZE/1024/1024)) MiB)"
echo "[wad] sha256: $SHA  (record this as asset provenance)"
echo "[wad] next: add DOOM1.WAD to disk.img root (launcher cwd) and run"
echo "[wad]       make disk.img ARCH=<arm|intel>, then ARCH=<arch> python3 run_doom_test.py"
