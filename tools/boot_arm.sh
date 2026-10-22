#!/bin/bash
# Boot HobbyOS ARM desktop headless with QMP + serial log (for scripted GUI driving).
# Usage: tools/boot_arm.sh [extra qemu args...]
set -u
cd "$(dirname "$0")/.."
pkill -f qemu-system-aarch64 2>/dev/null
sleep 1
rm -f qmp-sock serial.log
exec qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 8 -m 16384M \
  -accel tcg,thread=multi -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
  -display none -serial file:serial.log \
  -drive if=none,file=disk.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
  -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -semihosting -action shutdown=poweroff \
  -qmp unix:./qmp-sock,server,nowait "$@"
