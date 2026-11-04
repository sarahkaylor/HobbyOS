#!/usr/bin/env bash
# build-doomgen.sh — build doomgeneric's stock X11 backend as a static-PIE
# x86_64 Linux ELF (musl).
#
#   ./build-doomgen.sh
#   DOOMGEN_SRC=/path/to/doomgeneric ./build-doomgen.sh
#
# Source: the doomgeneric tree vendored in this worktree
# (third_party/doomgeneric, upstream ozkl/doomgeneric @ e257a095).  The vendored
# i_video.c carries one HobbyOS VirtIO-GPU edit, so the script copies the tree
# and applies patches/doomgen-stock-ivideo.patch to get the *stock* engine.
# Then it drives the upstream Makefile with musl-gcc and links -static-pie
# against the musl X11 sysroot from build-x11-musl-sysroot.sh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${HOBBYOS_DOOM_BUILD:-$HOME/hobbyos-doom/build}"
CACHE="${HOBBYOS_DOOM_CACHE:-$HOME/.cache/hobbyos-doom}"
SRC="${DOOMGEN_SRC:-$REPO/third_party/doomgeneric}"
WORK="$BUILD/doomgen-src"
OUT="${OUT:-$BUILD/out}"
CC="${CC:-musl-gcc}"

[[ -f "$SRC/doomgeneric_xlib.c" ]] || { echo "ERROR: doomgeneric sources not found at $SRC" >&2; exit 1; }

if [[ ! -f "$CACHE/x11-musl.env" ]]; then
  echo "[doomgen] building the musl X11 sysroot (first run)"
  "$HERE/build-x11-musl-sysroot.sh"
fi
# shellcheck disable=SC1090
source "$CACHE/x11-musl.env"

rm -rf "$WORK"; mkdir -p "$WORK"
cp -a "$SRC/." "$WORK/"
rm -rf "$WORK/build"   # stale objects from an in-tree HobbyOS build
( cd "$WORK" && patch -p1 --no-backup-if-mismatch < "$HERE/patches/doomgen-stock-ivideo.patch" )

# The upstream Makefile owns the object list (SRC_DOOM); override its compiler
# and flags rather than duplicating that list here.
CFLAGS="-O2 -fcommon -fno-strict-aliasing -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE $X11_MUSL_CFLAGS"
LIBS="$X11_MUSL_LIBS -lm"

echo "[doomgen] building with $CC"
make -C "$WORK" -j"$(nproc)" CC="$CC" CFLAGS="$CFLAGS" \
     LDFLAGS="-static-pie" LIBS="$LIBS" >"$WORK/build.log" 2>&1 \
  || { echo "[doomgen] build failed; tail of $WORK/build.log:" >&2; tail -25 "$WORK/build.log" >&2; exit 1; }

mkdir -p "$OUT"
cp "$WORK/doomgeneric" "$OUT/doomgen.elf"
if [[ "${STRIP:-1}" == 1 ]]; then strip -s "$OUT/doomgen.elf"; fi
readelf -d "$OUT/doomgen.elf" | grep -q NEEDED && { echo "[doomgen] ERROR: dynamic deps" >&2; exit 1; }

echo
echo "=== doomgen.elf ==="
file "$OUT/doomgen.elf"
readelf -h "$OUT/doomgen.elf" | grep -E 'Class|Machine|Type'
ls -l "$OUT/doomgen.elf"
sha256sum "$OUT/doomgen.elf"
