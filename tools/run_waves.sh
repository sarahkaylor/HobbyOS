#!/usr/bin/env bash
#
# Run the full HobbyOS test wave for BOTH architectures in parallel.
#
# The stock `make test` / `make test_intel` targets each rebuild ./disk.img
# and boot it directly, so they cannot run at the same time (and each is a
# ~40-minute single-threaded software-emulation run).  This script builds a
# per-architecture disk image, copies it aside, and boots both QEMU
# instances concurrently:
#   - ARM   : TCG with multi-threaded execution (MTTCG) so the 4 vCPUs
#             spread across host cores.
#   - intel : KVM (near-native speed) when /dev/kvm exists, else MTTCG.
#
# Usage:
#   tools/run_waves.sh              build + run both waves, then summarize
#   tools/run_waves.sh --build-only build the two disk images only
#   SKIP_BUILD=1 tools/run_waves.sh reuse existing *-test.img
#   LOG_DIR=/path tools/run_waves.sh logs elsewhere (default /tmp/hw_waves)
set -u
cd "$(dirname "$0")/.." || exit 1

LOG_DIR="${LOG_DIR:-/tmp/hw_waves}"
ARM_IMG=disk-arm-test.img
INTEL_IMG=disk-intel-test.img
ARM_LOG="$LOG_DIR/wave_arm.log"
INTEL_LOG="$LOG_DIR/wave_intel.log"

# EDK2 firmware paths, mirroring the Makefile's per-OS defaults.
if [ "$(uname)" = "Darwin" ]; then
  EDK2_X86_64=/opt/homebrew/share/qemu/edk2-x86_64-code.fd
  EDK2_AARCH64=/opt/homebrew/share/qemu/edk2-aarch64-code.fd
else
  EDK2_X86_64="$HOME/.local/share/OVMF/OVMF_CODE_4M.fd"
  EDK2_AARCH64="$HOME/.local/share/AAVMF/AAVMF_CODE.fd"
fi

build_disk() {
  local arch="$1"
  echo "[run_waves] building $arch test disk..."
  if ! make "ARCH=$arch" MODE=test disk.img >"$LOG_DIR/build_$arch.log" 2>&1; then
    echo "[run_waves] build for $arch FAILED; see $LOG_DIR/build_$arch.log" >&2
    exit 1
  fi
}

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  mkdir -p "$LOG_DIR"
  build_disk arm
  cp -f disk.img "$ARM_IMG"
  echo "[run_waves] ARM disk -> $ARM_IMG"
  build_disk intel
  cp -f disk.img "$INTEL_IMG"
  echo "[run_waves] intel disk -> $INTEL_IMG"
fi

if [ "${1:-}" = "--build-only" ]; then
  echo "[run_waves] build-only: done."
  exit 0
fi

run_arm() {
  qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 8 -m 8192M -accel tcg,thread=multi \
    -bios "$EDK2_AARCH64" -serial stdio \
    -drive if=none,file="$ARM_IMG",format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -device virtio-gpu-device -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
    -semihosting -action shutdown=poweroff -display none \
    -accel tcg,thread=multi >"$ARM_LOG" 2>&1
  echo "[run_waves] ARM wave rc=$?" >>"$ARM_LOG"
}

run_intel() {
  local accel
  if [ -e /dev/kvm ] && [ "${SKIP_KVM:-0}" != "1" ]; then
    accel=(-enable-kvm)
  else
    accel=(-accel tcg,thread=multi)
  fi
  qemu-system-x86_64 -M q35 -smp 8 -m 6144M -pflash "$EDK2_X86_64" -serial stdio \
    -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
    -drive file="$INTEL_IMG",format=raw,id=disk0,if=none \
    -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -action shutdown=poweroff -display none \
    "${accel[@]}" >"$INTEL_LOG" 2>&1
  echo "[run_waves] intel wave rc=$?" >>"$INTEL_LOG"
}

echo "[run_waves] launching ARM (MTTCG) and intel (KVM) waves in parallel"
run_arm &
ARM_PID=$!
run_intel &
INTEL_PID=$!
wait "$ARM_PID"
wait "$INTEL_PID"
echo "[run_waves] both waves done."
echo "  ARM   log: $ARM_LOG"
echo "  intel log: $INTEL_LOG"
echo "---- ARM verdicts ----"
grep -aE "TEST\]|Tests failed|EXIT=" "$ARM_LOG" | tail -25 || true
echo "---- intel verdicts ----"
grep -aE "TEST\]|Tests failed|EXIT=" "$INTEL_LOG" | tail -25 || true
