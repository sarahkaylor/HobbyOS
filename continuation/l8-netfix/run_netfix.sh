#!/usr/bin/env bash
# l8-netfix on-device repro runner (ARM).
#
# Boots the lane kernel in MODE=webproc single-program mode with
# WEBPROC.BIN = the NETFIX.BIN repro app (src/user/netfix_test.c), which
# opens real TCP sockets to the host netfix echo server and reports
# NETFIX-TX-* / NETFIX-RX-* verdicts for the two kernel bugs.
#
# Usage:  run_netfix.sh [run_label=m1] [lane=/home/.../l8-netfix]
# Env:    NETFIX_PORT=8765, QEMU_SMP=1, TIMEOUT_QEMU=<s>
set -euo pipefail

RUN="${1:-m1}"
LANE="${2:-/home/sarah/Documents/GitHub/hobbyos-lanes/l8-netfix}"
OUT="${LANE}/continuation/l8-netfix"
SCRATCH="${SCRATCH:-/tmp/l8-netfix-scratch}"
PORT="${NETFIX_PORT:-8765}"
FW="${FW:-/usr/share/AAVMF/AAVMF_CODE.fd}"
[ -f "$FW" ] || FW="$HOME/.local/share/qemu/AAVMF_CODE.fd"
OBJCOPY="${LLVM_OBJCOPY:-/usr/bin/llvm-objcopy}"
[ -x "$OBJCOPY" ] || OBJCOPY="$(clang -print-prog-name=llvm-objcopy)"

mkdir -p "$SCRATCH" "$OUT"

echo "== l8-netfix run $RUN (lane $LANE)"
cd "$LANE"

# 1. build the single-program kernel + the NETFIX repro binary
echo "== building (MODE=webproc)"
make ARCH=arm MODE=webproc hobbyos.elf > /tmp/l8-netfix-make.log 2>&1 || {
  echo "build failed"; tail -40 /tmp/l8-netfix-make.log; exit 1; }
"$OBJCOPY" -O binary hobbyos.elf hobbyos.bin
make ARCH=arm MODE=webproc "$LANE/obj/arm/netfix.bin" >> /tmp/l8-netfix-make.log 2>&1
ls -la hobbyos.bin obj/arm/netfix.bin

# 2. assemble the boot disk: kernel + WEBPROC.BIN (= netfix repro)
DISK="${SCRATCH}/disk-netfix-${RUN}.img"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=128 status=none
mkfs.fat -F 16 -s 16 "$DISK"
mmd -i "$DISK" ::/EFI
mmd -i "$DISK" ::/EFI/BOOT
mmd -i "$DISK" ::/boot
mcopy -i "$DISK" "$LANE/bootloader/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
printf 'timeout: 0\ndefault_entry: 1\n\n/HobbyOS (ARM AArch64)\nprotocol: linux\npath: boot():/boot/hobbyos.bin\n' > "${SCRATCH}/limine-l8netfix.conf"
mcopy -i "$DISK" "${SCRATCH}/limine-l8netfix.conf" ::/boot/limine.conf
mcopy -i "$DISK" hobbyos.bin ::/boot/hobbyos.bin
mcopy -i "$DISK" obj/arm/netfix.bin ::/WEBPROC.BIN
echo "-- read-back: WEBPROC.BIN $(mcopy -i "$DISK" ::/WEBPROC.BIN - 2>/dev/null | wc -c) B"

# 3. host echo server
LOGDIR="${OUT}/run-${RUN}-server"
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR"
NETFIX_PORT="$PORT" python3 "$LANE/continuation/l8-netfix/netfix_server.py" "$LOGDIR" \
  > "${LOGDIR}/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
sleep 1.2

# 4. boot
SERLOG="${OUT}/run-${RUN}-serial.log"
rm -f "$SERLOG"
timeout "${TIMEOUT_QEMU:-330}" qemu-system-aarch64 -M virt -cpu cortex-a53 \
  -smp "${QEMU_SMP:-1}" -m 2048M -accel tcg,thread=multi -bios "$FW" \
  -display none -serial stdio -semihosting \
  -drive if=none,file="$DISK",format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -action shutdown=poweroff > "$SERLOG" 2>&1 || true

echo "== serial tail =="
tail -25 "$SERLOG"
echo
echo "== netfix tally (serial) =="
for m in "NETFIX START" "NETFIX TX-OK" "NETFIX TX-FAIL" "NETFIX RX-OK" "NETFIX RX-FAIL" "NETFIX RESULTS" "NETFIX FAIL:" "NETFIX SKIP:"; do
  echo "$(grep -c "$m" "$SERLOG" 2>/dev/null || true) x $m"
done
echo "== server received (rx-*.txt sizes in $LOGDIR) =="
ls -la "$LOGDIR" | grep rx- | head -20
for f in "$LOGDIR"/rx-*.txt; do
  [ -f "$f" ] && echo "-- $f: $(head -c 120 "$f" | tr -c '[:print:]' '.')"
done
