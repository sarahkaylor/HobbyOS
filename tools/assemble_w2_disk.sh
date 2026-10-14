#!/bin/sh
# assemble_w2_disk.sh — build a minimal bootable ARM disk for the W2-MKFS
# in-OS smoke (kernel + bootloader + console/desktop + MKFSFAT.BIN only).
#
# Uses the port's OWN host build (obj/mkfs_host/mkfsfat_host) to create the
# 1 GiB FAT16 image via ftruncate (no dd/zero-fill needed — the formatter
# materializes the metadata), then mtools mdir/mcopy for the payload,
# mirroring the Makefile disk.img recipe but with only the files a desktop
# boot to a console needs.
#
# Usage: tools/assemble_w2_disk.sh <disk.img>
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMG=${1:-"$ROOT/disk-w2-min.img"}
HOSTFAT="$ROOT/obj/mkfs_host/mkfsfat_host"
OBJ="$ROOT/obj/arm"

[ -x "$HOSTFAT" ] || { echo "missing host mkfsfat: run tools/build_mkfs_host.sh"; exit 1; }
[ -f "$ROOT/hobbyos.elf" ] || { echo "missing hobbyos.elf"; exit 1; }
for b in desktop.bin console.bin sh.bin mkfsfat.bin hello.bin ls.bin cat.bin; do
  [ -f "$OBJ/$b" ] || { echo "missing obj/arm/$b"; exit 1; }
done

rm -f "$IMG"
# 1 GiB FAT16, created via the port's ftruncate -C path (also re-validates
# that path on a real boot-disk-shaped image).
"$HOSTFAT" -F 16 -C "$IMG" 1048576 >/dev/null 2>&1

mkd() {
  if mdir -i "$IMG" "$1" >/dev/null 2>&1; then :; else
    command mmd -i "$IMG" "$1" >/dev/null 2>&1 || return 1
  fi
}

mkd "::/EFI" || { echo "mkd ::/EFI failed"; exit 1; }
mkd "::/EFI/BOOT" || { echo "mkd ::/EFI/BOOT failed"; exit 1; }
mkd "::/boot" || { echo "mkd ::/boot failed"; exit 1; }

# limine.conf (ARM branch verbatim from Makefile:1795-1801)
cat > "$ROOT/obj/arm/limine.conf" <<EOF
timeout: 0
default_entry: 1

/HobbyOS (ARM AArch64)
protocol: linux
path: boot():/boot/hobbyos.bin
EOF

llvm-objcopy -O binary "$ROOT/hobbyos.elf" "$ROOT/hobbyos.bin"

mcopy -i "$IMG" "$ROOT/hobbyos.bin" "::/boot/hobbyos.bin"
mcopy -i "$IMG" "$ROOT/obj/arm/limine.conf" "::/boot/limine.conf"
mcopy -i "$IMG" "$ROOT/bootloader/BOOTAA64.EFI" "::/EFI/BOOT/BOOTAA64.EFI"
# Uppercase 8.3 names, matching the Makefile disk recipe (::/MKFSFAT.BIN ...)
mcopy -i "$IMG" "$OBJ/desktop.bin" "::/DESKTOP.BIN"
mcopy -i "$IMG" "$OBJ/console.bin" "::/CONSOLE.BIN"
mcopy -i "$IMG" "$OBJ/sh.bin" "::/SH.BIN"
mcopy -i "$IMG" "$OBJ/mkfsfat.bin" "::/MKFSFAT.BIN"
mcopy -i "$IMG" "$OBJ/hello.bin" "::/HELLO.BIN"
mcopy -i "$IMG" "$OBJ/ls.bin" "::/LS.BIN"
mcopy -i "$IMG" "$OBJ/cat.bin" "::/CAT.BIN"

echo "assembled: $IMG"
mdir -i "$IMG" :: | head -20
