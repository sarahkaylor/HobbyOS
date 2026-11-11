#!/bin/bash
# Boot the staged image N times and measure RimWorld's load milestones each time.
cd ~/Documents/GitHub/HobbyOS || exit 1
N=${1:-3}
LABEL_PREFIX=${2:-run}
for i in $(seq 1 "$N"); do
  echo "=== run $i ==="
  for p in $(pgrep -f qemu-system-x86_64); do kill -9 "$p" 2>/dev/null; done
  sleep 2
  L="$LABEL_PREFIX$i"
  rm -f /tmp/hb-qmp.sock "/tmp/rw-perf-$L.log"
  (qemu-system-x86_64 -M q35,vmport=on -smp 8 -m 12288M \
    -pflash /opt/homebrew/share/qemu/edk2-x86_64-code.fd -display none \
    -serial "file:/tmp/rw-perf-$L.log" \
    -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
    -drive file=disk.img,format=raw,id=disk0,if=none \
    -device nvme,drive=disk0,serial=1234,bus=pcie.1 \
    -device edu -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -device virtio-tablet-pci -action shutdown=poweroff \
    -qmp unix:/tmp/hb-qmp.sock,server,nowait \
    > "/tmp/rw-perf-qemu-$L.log" 2>&1 &)
  python3 tools/rw/rw_perf.py "$L" 240
done
for p in $(pgrep -f qemu-system-x86_64); do kill -9 "$p" 2>/dev/null; done
