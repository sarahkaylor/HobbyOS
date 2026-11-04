#!/usr/bin/env bash
# tools/mesa/stage.sh -- put the Mesa GL frontend on a boot image's library
# search path (RimWorld plan M4, interface F3).
#
# libGL.so.1 has no DT_NEEDED: it is dlopen()ed into a compat process that is
# already running the pinned glibc 2.31 and /LIB64/libX11.so.6, and every one of
# its 252 undefined symbols must be in that global scope when it loads.  So the
# DSOs live in /LIB64, which needs no LD_LIBRARY_PATH at all.
#
# FAT16 has no symlinks, so libGLX.so.0 (the name SDL/Unity dlopen, a symlink to
# libGL.so.1 in obj/mesa/lib) is staged as a second *copy* of the same bytes.
# 20 MB twice, out of ~600 MB free; the alternative -- teaching the FAT driver
# symlinks -- buys nothing.
#
# Usage:
#   bash tools/mesa/stage.sh [disk.img]          # default ./disk.img
#   MESA_LIB=obj/mesa/lib bash tools/mesa/stage.sh /tmp/disk.img
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMG="${1:-$ROOT/disk.img}"
LIB="${MESA_LIB:-$ROOT/obj/mesa/lib}"
MMD="${MMD:-mmd}"
MCOPY="${MCOPY:-mcopy}"
MDIR="${MDIR:-mdir}"

die() { echo "mesa-stage: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "no image at $IMG (build disk.img first)"
[ -f "$LIB/libGL.so.1" ] || die "no $LIB/libGL.so.1 (run: bash tools/mesa/build-hobbyos.sh)"

# realpath -f resolves the libGLX.so.0 symlink to the one file it points at.
src_gl=$(readlink -f "$LIB/libGL.so.1")
[ -f "$src_gl" ] || die "libGL.so.1 does not resolve"

"$MMD" -i "$IMG" ::/LIB64 >/dev/null 2>&1 || true
"$MCOPY" -o -i "$IMG" "$src_gl" ::/LIB64/libGL.so.1 || die "mcopy libGL.so.1"
"$MCOPY" -o -i "$IMG" "$src_gl" ::/LIB64/libGLX.so.0 || die "mcopy libGLX.so.0"

echo "mesa-stage: $IMG ::/LIB64 (libGL.so.1 + libGLX.so.0, $(stat -c%s "$src_gl") bytes each)"
"$MDIR" -i "$IMG" ::/LIB64 | sed 's/^/  /'
