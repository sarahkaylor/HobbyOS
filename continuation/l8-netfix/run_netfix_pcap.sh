#!/usr/bin/env bash
# l8-netfix pcap capture run: like run_netfix.sh but adds QEMU filter-dump
# so we can see the EXACT frames delivered to the guest NIC.
set -euo pipefail
RUN="${1:-pc1}"
LANE="${2:-/home/sarah/Documents/GitHub/hobbyos-lanes/l8-netfix}"
OUT="${LANE}/continuation/l8-netfix"
SCRATCH="${SCRATCH:-/tmp/l8-netfix-scratch}"
PORT="${NETFIX_PORT:-8765}"
# GX (docs/graphics-accel.md §3-D5): pass-through parity only — headless
# pcap capture run (`-display none`), no GPU device in the invocation;
# QEMU_GPU defaults to soft (see run_netfix.sh).
QEMU_GPU="${QEMU_GPU:-soft}"
FW="${FW:-/usr/share/AAVMF/AAVMF_CODE.fd}"
[ -f "$FW" ] || FW="$HOME/.local/share/qemu/AAVMF_CODE.fd"
OBJCOPY="${LLVM_OBJCOPY:-/usr/bin/llvm-objcopy}"

mkdir -p "$SCRATCH" "$OUT"
PCAP="${OUT}/run-${RUN}.pcap"
SERLOG="${OUT}/run-${RUN}-serial.log"
LOGDIR="${OUT}/run-${RUN}-server"
rm -f "$PCAP" "$SERLOG"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

cd "$LANE"
DISK="${SCRATCH}/disk-netfix-${RUN}.img"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=128 status=none
mkfs.fat -F 16 -s 16 "$DISK"
mmd -i "$DISK" ::/EFI; mmd -i "$DISK" ::/EFI/BOOT; mmd -i "$DISK" ::/boot
mcopy -i "$DISK" "$LANE/bootloader/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
printf 'timeout: 0\ndefault_entry: 1\n\n/HobbyOS (ARM AArch64)\nprotocol: linux\npath: boot():/boot/hobbyos.bin\n' > "${SCRATCH}/limine-l8netfix.conf"
mcopy -i "$DISK" "${SCRATCH}/limine-l8netfix.conf" ::/boot/limine.conf
mcopy -i "$DISK" hobbyos.bin ::/boot/hobbyos.bin
mcopy -i "$DISK" obj/arm/netfix.bin ::/WEBPROC.BIN

NETFIX_PORT="$PORT" python3 "$LANE/continuation/l8-netfix/netfix_server.py" "$LOGDIR" > "${LOGDIR}/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
sleep 1.2

timeout "${TIMEOUT_QEMU:-330}" qemu-system-aarch64 -M virt -cpu cortex-a53 \
  -smp "${QEMU_SMP:-1}" -m 2048M -accel tcg,thread=multi -bios "$FW" \
  -display none -serial stdio -semihosting \
  -drive if=none,file="$DISK",format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -object filter-dump,id=fd0,netdev=net0,file="$PCAP" \
  -action shutdown=poweroff > "$SERLOG" 2>&1 || true

echo "== pcap: $PCAP"
ls -la "$PCAP"
echo "== tally =="
for m in "NETFIX TX-OK" "NETFIX TX-FAIL" "NETFIX RX-OK" "NETFIX RX-FAIL" "NETFIX RESULTS"; do
  echo "$(grep -c "$m" "$SERLOG" 2>/dev/null || true) x $m"
done
