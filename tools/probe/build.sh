#!/bin/bash
# Build the aarch64 UEFI hardware probe and wrap it in a bootable GPT+ESP image.
#
#   tools/probe/build.sh            -> obj/probe/BOOTAA64.EFI, obj/probe/probe.img
#
# probe.img is a 128 MiB GPT disk with one EFI System Partition holding
# EFI/BOOT/BOOTAA64.EFI.  It boots under Parallels' efi-arm64 firmware as a hard
# disk, and the probe writes \PROBE.TXT onto that FAT volume, which the host
# reads back with `mtype -i <esp> ::\PROBE.TXT` (no serial port to rely on).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/obj/probe"
LLVM_BIN="$(brew --prefix llvm 2>/dev/null)/bin"
CLANG="$LLVM_BIN/clang"; [ -x "$CLANG" ] || CLANG=/usr/bin/clang
LLD_LINK="$LLVM_BIN/lld-link"; [ -x "$LLD_LINK" ] || LLD_LINK="$(command -v lld-link)"
MCOPY="$(command -v mcopy)"; MMD="$(command -v mmd)"; MKFS_FAT="$(command -v mkfs.fat || echo /opt/homebrew/sbin/mkfs.fat)"

mkdir -p "$OUT"

echo "== compiling prl_probe.c (aarch64-none-elf, PE32+ EFI app)"
"$CLANG" --target=aarch64-windows-gnu -std=c11 -O2 -ffreestanding -fno-builtin \
         -fno-stack-protector -fshort-wchar -mno-red-zone \
         -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
         -c "$ROOT/tools/probe/prl_probe.c" -o "$OUT/prl_probe.o"

echo "== linking $OUT/BOOTAA64.EFI"
"$LLD_LINK" /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:arm64 \
            /out:"$OUT/BOOTAA64.EFI" "$OUT/prl_probe.o"

echo "== building the FAT ESP (64 MiB)"
rm -f "$OUT/esp.fat"
"$MKFS_FAT" -F 32 -n PROBE -C "$OUT/esp.fat" 65536
"$MMD" -i "$OUT/esp.fat" ::/EFI ::/EFI/BOOT
"$MCOPY" -i "$OUT/esp.fat" "$OUT/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI

echo "== wrapping in a GPT disk image"
python3 "$ROOT/tools/probe/mkgpt.py" "$OUT/esp.fat" "$OUT/probe.img" 128

echo "== done"
ls -la "$OUT/BOOTAA64.EFI" "$OUT/probe.img"
