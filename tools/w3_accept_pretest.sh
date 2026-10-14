#!/usr/bin/env bash
# tools/w3_accept_pretest.sh — w3-accept lane: hand-build the S4/S5
# pre-test artifacts (mtools mformat/mcopy ONLY — mkfs.fat and dd are
# command-blocked for this lane; mformat is the L5-proven path).
#
# Builds, into <workdir>:
#   hobbyos.bin               kernel flat binary (llvm-objcopy of hobbyos.elf)
#   limine.conf               ARM boot template (verbatim Makefile branch)
#   OUTMARK.TXT               marker file (copied onto every OUT.IMG)
#   OUT.IMG-hand-min.img      128 MiB FAT16, kernel-LESS: OUTPROG.BIN +
#                             OUTMARK.TXT only  (S4 extraction-only pre-test)
#   OUT.IMG-hand-boot.img     128 MiB FAT16, BOOTABLE: /boot/hobbyos.bin +
#                             /boot/limine.conf + /EFI/BOOT/BOOTAA64.EFI +
#                             OUTPROG.BIN + OUTMARK.TXT  (S4+S5 full pre-test)
#   bootdisk.img              1 GiB FAT16 pre-test boot disk: real DESKTOP
#                             boot shape + SELFHOST.BIN + /SRC/SELFHOST.RUN +
#                             OUT.IMG-hand-boot.img planted at ::/OUT.IMG
#
# 128 MiB OUT.IMG sizing (frozen acceptance): mkfs.fat's -C BLOCK argument is
# 1024-byte units -> DISK_BLOCKS=131072 (see w2-mkfs-report.json notes); the
# image FILE is 134,217,728 B.  mformat (mtools) is NOT mkfs.fat: its -F means
# "force FAT32" and its -T is total sectors — the L5-proven FAT16 recipe is
# `truncate -s <bytes> img && mformat -i img ::`, which here yields
# BPB bps=512 spc=4 nfats=2 root=512 fatsize=256 total=262144 (same geometry
# dosfstools picks for the frozen 128 MiB FAT16, matching w2-mkfs "128 MiB ->
# spc4").  Never pass -F to mformat for OUT.IMG.
#
# Usage: tools/w3_accept_pretest.sh <workdir>
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
W=${1:?usage: w3_accept_pretest.sh <workdir>}
mkdir -p "$W"
OBJ="$ROOT/obj/arm"

for t in mformat mcopy mmd mdir mdeltree llvm-objcopy truncate; do
  command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t" >&2; exit 1; }
done

[ -f "$ROOT/hobbyos.elf" ] || { echo "missing hobbyos.elf (run: make ARCH=arm all)" >&2; exit 1; }
[ -f "$OBJ/hello.bin" ]    || { echo "missing $OBJ/hello.bin" >&2; exit 1; }
[ -f "$OBJ/selfhost.bin" ] || { echo "missing $OBJ/selfhost.bin" >&2; exit 1; }

# 1) kernel flat binary + limine.conf template (verbatim Makefile arm branch)
llvm-objcopy -O binary "$ROOT/hobbyos.elf" "$W/hobbyos.bin"
printf 'timeout: 0\ndefault_entry: 1\n\n/HobbyOS (ARM AArch64)\nprotocol: linux\npath: boot():/boot/hobbyos.bin\n' > "$W/limine.conf"
printf 'OUTMARK:v1:hand-built-for-w3-accept-S4S5-pretest\n' > "$W/OUTMARK.TXT"

# 2) kernel-less OUT.IMG (S4 extraction-only shape)
truncate -s 134217728 "$W/OUT.IMG-hand-min.img"
mformat -i "$W/OUT.IMG-hand-min.img" ::
mcopy -i "$W/OUT.IMG-hand-min.img" "$OBJ/hello.bin" ::/OUTPROG.BIN
mcopy -i "$W/OUT.IMG-hand-min.img" "$W/OUTMARK.TXT" ::/OUTMARK.TXT

# 3) bootable OUT.IMG (S4 boot + S5 launch shape).  IMPORTANT (w3-accept
#    finding): a standalone boot of a bare FAT16 needs DESKTOP.BIN + SH.BIN +
#    CONSOLE.BIN on the image — the kernel's DESKTOP mode loads DESKTOP.BIN
#    from the boot disk (main.c "Parallel Program Loading"), and without it
#    no desktop / Apps menu ever comes up for S5's keystroke path.  This is
#    part of the exact S5 OUT.IMG layout requirement (see acceptance.md).
truncate -s 134217728 "$W/OUT.IMG-hand-boot.img"
mformat -i "$W/OUT.IMG-hand-boot.img" ::
mmd -i "$W/OUT.IMG-hand-boot.img" ::/boot ::/EFI ::/EFI/BOOT
mcopy -i "$W/OUT.IMG-hand-boot.img" "$W/hobbyos.bin" ::/boot/hobbyos.bin
mcopy -i "$W/OUT.IMG-hand-boot.img" "$W/limine.conf" ::/boot/limine.conf
mcopy -i "$W/OUT.IMG-hand-boot.img" "$ROOT/bootloader/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$W/OUT.IMG-hand-boot.img" "$OBJ/desktop.bin" ::/DESKTOP.BIN
mcopy -i "$W/OUT.IMG-hand-boot.img" "$OBJ/console.bin" ::/CONSOLE.BIN
mcopy -i "$W/OUT.IMG-hand-boot.img" "$OBJ/sh.bin" ::/SH.BIN
mcopy -i "$W/OUT.IMG-hand-boot.img" "$OBJ/hello.bin" ::/OUTPROG.BIN
mcopy -i "$W/OUT.IMG-hand-boot.img" "$W/OUTMARK.TXT" ::/OUTMARK.TXT

# 4) pre-test boot disk (1 GiB FAT16; real-DESKTOP shape like w2-harness)
truncate -s 1073741824 "$W/bootdisk.img"
mformat -i "$W/bootdisk.img" ::
mmd -i "$W/bootdisk.img" ::/EFI ::/EFI/BOOT ::/boot ::/SRC
mcopy -i "$W/bootdisk.img" "$ROOT/bootloader/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$W/bootdisk.img" "$W/limine.conf" ::/boot/limine.conf
mcopy -i "$W/bootdisk.img" "$W/hobbyos.bin" ::/boot/hobbyos.bin
mcopy -i "$W/bootdisk.img" "$OBJ/desktop.bin" ::/DESKTOP.BIN
mcopy -i "$W/bootdisk.img" "$OBJ/console.bin" ::/CONSOLE.BIN
mcopy -i "$W/bootdisk.img" "$OBJ/sh.bin" ::/SH.BIN
mcopy -i "$W/bootdisk.img" "$OBJ/hello.bin" ::/HELLO.BIN
mcopy -i "$W/bootdisk.img" "$OBJ/ls.bin" ::/LS.BIN
mcopy -i "$W/bootdisk.img" "$OBJ/cat.bin" ::/CAT.BIN
mcopy -i "$W/bootdisk.img" "$OBJ/selfhost.bin" ::/SELFHOST.BIN
mcopy -i "$W/bootdisk.img" "$ROOT/src/user/selfhost/SELFHOST.RUN" ::/SRC/SELFHOST.RUN
mcopy -i "$W/bootdisk.img" "$W/OUT.IMG-hand-boot.img" ::/OUT.IMG

echo "=== artifacts in $W ==="
sha256sum "$W/hobbyos.bin" "$W/limine.conf" "$W/OUTMARK.TXT" \
          "$OBJ/hello.bin" "$OBJ/selfhost.bin" \
          "$W/OUT.IMG-hand-min.img" "$W/OUT.IMG-hand-boot.img" "$W/bootdisk.img"
echo "=== bootable OUT.IMG listing ==="
mdir -i "$W/OUT.IMG-hand-boot.img" ::/
echo "=== boot disk listing ==="
mdir -i "$W/bootdisk.img" ::/
echo "done"
