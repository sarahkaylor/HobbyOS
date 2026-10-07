#!/usr/bin/env bash
# extract.sh — reassemble + verify + extract the vendored WebKit fork snapshot
# (third_party/webkit-hobbyos/, fork ref browser/rp-f @ 45349cb2).
#
#   bash third_party/webkit-hobbyos/extract.sh [--dest DIR] [--check]
#
# Default dest: third_party/webkit-hobbyos/src/ (gitignored; rebuilt from the
# committed parts — same convention as the wpewebkit-2.54.0 tarball extraction
# in this repo).  --check verifies the reassembled sha256 and exits without
# extracting.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XZ_NAME="webkit-hobbyos-rp-f-45349cb2.tar.xz"
DEST=""
CHECK_ONLY=""

while [ $# -gt 0 ]; do
  case "$1" in
    --dest) DEST="$2"; shift 2;;
    --check) CHECK_ONLY=1; shift;;
    *) echo "usage: $0 [--dest DIR] [--check]" >&2; exit 2;;
  esac
done

cd "$HERE"
EXPECTED="$(grep " ${XZ_NAME}\$" SHA256SUMS | cut -d' ' -f1)"
[ -n "$EXPECTED" ] || { echo "FATAL: no ${XZ_NAME} record in SHA256SUMS" >&2; exit 1; }

shopt -s nullglob
PARTS=( "${XZ_NAME}".part-* )
shopt -u nullglob
[ "${#PARTS[@]}" -gt 0 ] || { echo "FATAL: no parts found (${XZ_NAME}.part-*)" >&2; exit 1; }

ACTUAL="$(cat "${PARTS[@]}" | sha256sum | cut -d' ' -f1)"
if [ "$ACTUAL" != "$EXPECTED" ]; then
  echo "FATAL: reassembled snapshot sha256 mismatch" >&2
  echo "  expected $EXPECTED" >&2
  echo "  actual   $ACTUAL" >&2
  exit 1
fi
echo "snapshot sha256 OK: $EXPECTED (${#PARTS[@]} parts)"

if [ -n "$CHECK_ONLY" ]; then
  echo "--check: parts verified; nothing extracted."
  exit 0
fi

[ -n "$DEST" ] || DEST="$HERE/src"
mkdir -p "$DEST"
echo "extracting -> $DEST ..."
cat "${PARTS[@]}" | tar -xJf - -C "$DEST" --strip-components=1
if [ -f "$DEST/CMakeLists.txt" ] && [ -f "$DEST/HobbyOS/toolchain-hobbyos.cmake" ]; then
  echo "extract OK: $DEST ($(find "$DEST" -type f | wc -l) files)"
else
  echo "FATAL: extraction looks wrong (CMakeLists.txt / HobbyOS/toolchain-hobbyos.cmake missing)" >&2
  exit 1
fi
