# docs/selfhost/lanes/w4-selfhost-boottest.sh — w4-kernel lane: MODE=selfhost
# real-boot serial evidence (acceptance 5.2/5.3 primary drive).
#
# Hand-assembles a boot disk with mtools (mformat/mcopy ONLY — mkfs.fat and
# dd are command-blocked; see tools/w3_accept_pretest.sh for the same
# recipe) and boots MODE=selfhost, capturing serial.  Expected serial:
#   Booting AArch64 OS...
#   Mode: SELFHOST - Running in-OS acceptance build driver...
#   [SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
#   [SELFHOST] phase=s0-check-start
#     (MAKE.BIN /TOOLS exec + relay)
#   [SELFHOST] phase=s0-check-done rc=0 | phase=s0-check-fail ...
#   [SELFHOST] ACCEPTANCE-COMPLETE | FAILED-AT | ABORT
#   System halt.
#
# Tables produced/consumed by w4-kernel-report.json.
#
# Usage: docs/selfhost/lanes/w4-selfhost-boottest.sh <workdir> [label]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
W=${1:?usage: w4-selfhost-boottest.sh <workdir> [label]}
LABEL=${2:-boot}
mkdir -p "$W"
OBJ="$ROOT/obj/arm"

for t in mformat mcopy mmd mdir llvm-objcopy truncate qemu-system-aarch64; do
  command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t" >&2; exit 1; }
done

[ -f "$ROOT/hobbyos.elf" ]  || { echo "missing hobbyos.elf (make MODE=selfhost kernel)" >&2; exit 1; }
[ -f "$OBJ/selfhost.bin" ]  || { echo "missing $OBJ/selfhost.bin" >&2; exit 1; }
[ -f "$OBJ/make.bin" ]      || { echo "missing $OBJ/make.bin" >&2; exit 1; }
[ -f "$ROOT/bootloader/BOOTAA64.EFI" ] || { echo "missing bootloader/BOOTAA64.EFI" >&2; exit 1; }

llvm-objcopy -O binary "$ROOT/hobbyos.elf" "$W/hobbyos.bin"
printf 'timeout: 0\ndefault_entry: 1\n\n/HobbyOS (ARM AArch64)\nprotocol: linux\npath: boot():/boot/hobbyos.bin\n' > "$W/limine.conf"

# Minimal in-OS make fixture (lane scratch only — NOT the real
# Makefile.hobbyos, which is w4-src's file).  cwd for the spawned MAKE.BIN
# is / (kernel default), so the fixture lives at /Makefile.hobbyos.
cat > "$W/Makefile.hobbyos" <<'EOF'
# w4-kernel lane fixture for the MODE=selfhost serial-path evidence only.
# The real S0..S3 recipes are w4-src's Makefile.hobbyos; this one proves the
# driver -> MAKE.BIN -> phase markers -> serial relay chain end to end.
check:
	@printf '[SELFHOST] fixture-make-check-ran\n'
user:
	@printf '[SELFHOST] fixture-make-user-ran\n'
kernel:
	@printf '[SELFHOST] fixture-make-kernel-ran\n'
disk:
	@printf '[SELFHOST] fixture-make-disk-ran\n'
EOF

# Boot disk (128 MiB FAT16 — plenty for kernel + SELFHOST + MAKE + fixtures).
truncate -s 134217728 "$W/selfhost-boot-$LABEL.img"
mformat -i "$W/selfhost-boot-$LABEL.img" ::
mmd -i "$W/selfhost-boot-$LABEL.img" ::/EFI ::/EFI/BOOT ::/boot ::/TOOLS ::/SRC
mcopy -i "$W/selfhost-boot-$LABEL.img" "$ROOT/bootloader/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$W/selfhost-boot-$LABEL.img" "$W/limine.conf" ::/boot/limine.conf
mcopy -i "$W/selfhost-boot-$LABEL.img" "$W/hobbyos.bin" ::/boot/hobbyos.bin
mcopy -i "$W/selfhost-boot-$LABEL.img" "$OBJ/selfhost.bin" ::/SELFHOST.BIN
mcopy -i "$W/selfhost-boot-$LABEL.img" "$OBJ/make.bin" ::/TOOLS/MAKE.BIN
mcopy -i "$W/selfhost-boot-$LABEL.img" "$ROOT/src/user/selfhost/SELFHOST.RUN" ::/SRC/SELFHOST.RUN
mcopy -i "$W/selfhost-boot-$LABEL.img" "$W/Makefile.hobbyos" ::/Makefile.hobbyos

SERIAL="$W/serial-$LABEL.log"
QEMU=$(command -v qemu-system-aarch64)
timeout 360 "$QEMU" -M virt -cpu cortex-a53 -smp 8 -m 16384M \
  -accel tcg,thread=multi \
  -bios "$HOME/.local/share/AAVMF/AAVMF_CODE.fd" \
  -display none \
  -serial file:"$SERIAL" \
  -drive if=none,file="$W/selfhost-boot-$LABEL.img",format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -device virtio-gpu-device -device virtio-keyboard-device \
  -device virtio-tablet-device \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -semihosting -action shutdown=poweroff >/dev/null 2>&1 || true

echo "=== serial tail ($SERIAL) ==="
tail -40 "$SERIAL"
echo "=== verdict greps ==="
grep -n "Mode: SELFHOST" "$SERIAL" || echo "MISSING Mode: SELFHOST"
grep -n "\[SELFHOST\]" "$SERIAL" || echo "MISSING [SELFHOST] markers"
grep -n "System halt." "$SERIAL" || echo "MISSING System halt."
grep -cn "FAIL" "$SERIAL" || true
