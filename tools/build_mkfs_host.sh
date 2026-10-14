#!/bin/sh
# build_mkfs_host.sh — host-native build of the MKFSFAT port sources, for
# functional proof WITHOUT HobbyOS (gate 4 of docs/selfhost/lanes/w2-mkfs).
#
# Compiles the same transcribed sources that produce the ARM MKFSFAT.BIN
# (src/user/mkfs/*) with the same src/user/mkfs/config.h, but as a normal
# Linux program.  The binary is named "mkfsfat_host" (not mkfs.fat) so the
# workspace harness can tell it apart and because it must never be confused
# with a system-formatter.
#
# It only ever operates on scratch image files in the workspace — never on
# block devices — so it is safe to run inside the lane.
#
# Usage: tools/build_mkfs_host.sh [out]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/obj/mkfs_host/mkfsfat_host"}
mkdir -p "$(dirname "$OUT")"

clang -O2 -Wall -Wextra -g \
    -Isrc/user/mkfs -include src/user/mkfs/config.h \
    -Wno-sign-compare -Wno-missing-field-initializers \
    src/user/mkfs/mkfs.fat.c src/user/mkfs/common.c src/user/mkfs/charconv.c \
    src/user/mkfs/device_info.c src/user/mkfs/hb_blkdev.c \
    -o "$OUT" || exit 1

echo "built: $OUT"
