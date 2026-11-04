#!/usr/bin/env bash
# src/user/xcompat/stage.sh -- put the compat window/audio/device DSOs on a
# boot image's library search path (RimWorld plan M3, interface F3).
#
# A Linux-ABI process resolves its DT_NEEDED / dlopen() names through
# LD_LIBRARY_PATH, then the requesting object's DT_RUNPATH, then /LIB64
# (measured, docs/rw-ldso-design.md).  /LIB64 is the one location that needs
# no environment at all, so these DSOs live there: whatever launches the game
# (menu row, /XSTART.TXT, a test harness) gets them without exporting
# anything into a shell -- and exporting LD_LIBRARY_PATH is exactly what
# poisons a host shell with the pinned libc (plan §Appendix B).
#
# Real sonames, not 8.3 aliases: the disk is FAT16 with VFAT long names and
# LFN open was measured working (M1 gate stages /lib64/ld-linux-x86-64.so.2).
#
# Usage:
#   bash src/user/xcompat/stage.sh [disk.img]      # default ./disk.img
#   XCOMPAT_OBJ=obj/xcompat bash src/user/xcompat/stage.sh /tmp/disk.img
#
# Idempotent: overwrites in place, and only ever *creates* /LIB64.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IMG="${1:-$ROOT/disk.img}"
OBJ="${XCOMPAT_OBJ:-$ROOT/obj/xcompat}"
MMD="${MMD:-mmd}"
MCOPY="${MCOPY:-mcopy}"
MDIR="${MDIR:-mdir}"

die() { echo "xcompat-stage: $*" >&2; exit 1; }

[ -f "$IMG" ] || die "no image at $IMG (build disk.img first)"
[ -d "$OBJ" ] || die "no $OBJ (run: make ARCH=intel xcompat_dsos)"

# The required set.  Anything else this tree has built (extension DSOs, the
# GL frontend when it lands) is staged too, so a DSO that starts existing is
# never silently left off the image.
DSOS="libX11.so.6 libasound.so.2 libpulse-simple.so.0 libpulse.so.0 libudev.so.1"
for f in "$OBJ"/libX*.so.* "$OBJ"/libasound.so.* "$OBJ"/libpulse*.so.* \
         "$OBJ"/libudev.so.*; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  case " $DSOS " in *" $b "*) continue ;; esac
  DSOS="$DSOS $b"
done

"$MMD" -i "$IMG" ::/LIB64 >/dev/null 2>&1 || true
for b in $DSOS; do
  src="$OBJ/$b"
  [ -f "$src" ] || die "missing $src"
  "$MCOPY" -o -i "$IMG" "$src" "::/LIB64/$b" || die "mcopy failed for $b"
done

echo "xcompat-stage: $IMG ::/LIB64"
"$MDIR" -i "$IMG" ::/LIB64 | sed 's/^/  /'
