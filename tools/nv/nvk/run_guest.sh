#!/usr/bin/env bash
# tools/nv/nvk/run_guest.sh -- boot HobbyOS's compat class and run the nvk
# loader probe against /dev/nvshim.
#
# What it does, in order:
#   1. stages the pinned glibc 2.31 runtime (the compat class's libc) into the
#      guest image's /LIB64 and /lib/x86_64-linux-gnu, exactly as
#      tools/gate/m1_glibc_hello.sh does;
#   2. stages the NVK ICD (libvulkan_nouveau.so) at /CP/VK/lib;
#   3. stages src/user/nvk_probe.c's binary as /PROBE.ELF and points WAVE.CFG
#      at it, so the kernel's wave launcher runs it at boot;
#   4. boots MODE=test with the host's OVMF and captures the serial log.
#
# The verdict is the serial log, never the build:
#   [NVK] createInstance: VK_SUCCESS           -- the ICD loaded and ran
#   [NVK] dev[0] name="AD102" ...              -- enumeration (the target)
#   [NVK] wall: ...                            -- a NAMED refusal (fallback)
#   [NVSHIM]/nvk: rows                         -- which layer refused
#
# Env: NVK_SYSROOT (glibc 2.31 sysroot + crt), NVK_ICD (path to
#      libvulkan_nouveau.so), NVK_VKH (Vulkan headers), NVK_RUN_DIR (out dir),
#      NVK_TIMEOUT (seconds), NVK_SKIP_BUILD=1 (probe already built),
#      NVK_SKIP_KERNEL_BUILD=1 (image already built), SKIP_KVM=1 (force TCG;
#      the same switch every other HobbyOS harness takes).
#
#      Defaults point at what tools/mesa/build-nvk.sh just built (honouring the
#      same NVK_WORK), so
#      `bash tools/mesa/build-nvk.sh all && bash tools/nv/nvk/run_guest.sh`
#      needs no environment at all.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${NVK_WORK:-$ROOT/obj/mesa-nvk}"
SYS="${NVK_SYSROOT:-$BUILD/sysroot-glibc231}"
ICD="${NVK_ICD:-$BUILD/build/src/nouveau/vulkan/libvulkan_nouveau.so}"
VKH="${NVK_VKH:-$BUILD/src/mesa-25.0.7/include}"
W="${NVK_RUN_DIR:-$ROOT/obj/nvk-run}"
TIMEOUT_S="${NVK_TIMEOUT:-300}"
mkdir -p "$W"

say() { printf 'nvk-guest: %s\n' "$*"; }
die() { say "ERROR: $*"; exit "${2:-2}"; }

for f in "$SYS/lib/x86_64-linux-gnu/libc-2.31.so" "$ICD" "$VKH/vulkan/vulkan.h"; do
  [ -f "$f" ] || die "missing $f"
done

# --- 1. probe -----------------------------------------------------------------
if [ "${NVK_SKIP_BUILD:-0}" != 1 ]; then
  say "building the probe (glibc 2.31 PIE, linked against the ICD)"
  clang --sysroot="$SYS" -O2 -fPIE -pie -Wall \
    -I"$VKH" -o "$W/nvk_probe" "$ROOT/src/user/nvk_probe.c" "$ICD" \
    -Wl,-rpath,/CP/VK/lib -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    || die "probe link failed"
fi
[ -f "$W/nvk_probe" ] || die "no probe binary"

# --- 2. the kernel + image ------------------------------------------------
if [ "${NVK_SKIP_KERNEL_BUILD:-0}" != 1 ]; then
  say "kernel + MODE=test image"
  TOOLS_SKIP=1 BROWSER_SKIP=1 make -C "$ROOT" -j"$(nproc)" ARCH=intel MODE=test \
    hobbyos.elf disk.img > "$W/kernel-build.log" 2>&1 \
    || { tail -20 "$W/kernel-build.log"; die "kernel build failed"; }
fi
[ -f "$ROOT/disk.img" ] || die "no $ROOT/disk.img"

# Keep a private copy beside the run: the repo's disk.img is shared with other
# lanes (a `make` in the same tree rebuilds and re-mkfs it), so staging into it
# is a race.  QEMU below boots this copy.
IMG="$W/run.img"
SRC_IMG="${NVK_DISK:-$ROOT/disk.img}"
cp "$SRC_IMG" "$IMG" || die "no $SRC_IMG to copy"

# --- 3. stage -------------------------------------------------------------
stage() { mcopy -o -i "$IMG" "$1" "$2" || die "mcopy failed: $2"; }
for d in "::/lib64" "::/lib" "::/lib/x86_64-linux-gnu" "::/LIB64" "::/CP" "::/CP/VK" \
         "::/CP/VK/lib"; do
  mmd -i "$IMG" "$d" >/dev/null 2>&1 || true
done
L="$SYS/lib/x86_64-linux-gnu"
for pair in "libc-2.31.so:libc.so.6" "libm-2.31.so:libm.so.6" \
            "libpthread-2.31.so:libpthread.so.0" "libdl-2.31.so:libdl.so.2" \
            "librt-2.31.so:librt.so.1" "libgcc_s.so.1:libgcc_s.so.1"; do
  src="${pair%%:*}"; dst="${pair##*:}"
  stage "$L/$src" "::/lib/x86_64-linux-gnu/$dst"
  stage "$L/$src" "::/LIB64/$dst"
done
stage "$L/ld-2.31.so" "::/lib64/ld-linux-x86-64.so.2"
stage "$L/ld-2.31.so" "::/LIB64/ld-linux-x86-64.so.2"
stage "$ICD" "::/CP/VK/lib/libvulkan_nouveau.so"
stage "$W/nvk_probe" "::PROBE.ELF"
printf 'PROBE.ELF\n' > "$W/WAVE.CFG"
stage "$W/WAVE.CFG" "::WAVE.CFG"
say "staged: glibc 2.31 -> /LIB64 + /lib/x86_64-linux-gnu, ICD -> /CP/VK/lib, PROBE.ELF"

# --- 4. boot --------------------------------------------------------------
# QEMU is invoked directly rather than through `make run`: `run` re-checks the
# whole build graph (and rebuilds the kernel when the recorded MODE string
# moved), which would eat the boot deadline.  The command line is the one
# `make -s --eval='print-%' print-QEMU_CMD ARCH=intel MODE=test ...` prints.
OVMF=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF="$c"; break; }
done
[ -n "$OVMF" ] || die "no OVMF_CODE_4M.fd"

LOG="$W/serial.log"
: > "$LOG"
say "boot (deadline ${TIMEOUT_S}s, log $LOG)"
(
  cd "$ROOT" || exit 1
  QEMU_ARGS_LINE=(-M q35,vmport=on -smp 8 -m 12288M
    -pflash "$OVMF" -display none -serial stdio
    -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1
    -drive file=$IMG,format=raw,id=disk0,if=none
    -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56
    -device virtio-tablet-pci -action shutdown=poweroff)
  # Acceleration: KVM when it is usable, TCG otherwise -- the same rule as
  # run_unit_tests_intel.sh and tools/nv/el0_wave.sh, with the writability test
  # this harness already needed (a /dev/kvm that exists but is root:kvm-only is
  # not usable, and -enable-kvm would abort the boot instead of slowing it).
  ACCEL=(); SG=()
  if [ "${SKIP_KVM:-0}" != 1 ]; then
    if [ -w /dev/kvm ]; then
      ACCEL=(-enable-kvm -cpu host); say "accel: kvm"
    elif id -nG 2>/dev/null | tr ' ' '\n' | grep -qx kvm; then
      # A session started before the user joined group `kvm` has no access to
      # /dev/kvm even though /etc/group says otherwise; sg(1) picks it up.
      ACCEL=(-enable-kvm -cpu host); SG=(1); say "accel: kvm (via sg kvm)"
    fi
  fi
  if [ "${#ACCEL[@]}" = 0 ]; then
    say "accel: tcg (SKIP_KVM=${SKIP_KVM:-0}; /dev/kvm writable: $([ -w /dev/kvm ] && echo yes || echo no))"
  fi
  if [ "${#SG[@]}" != 0 ]; then
    exec sg kvm -c "qemu-system-x86_64 $(printf '%q ' "${QEMU_ARGS_LINE[@]}" "${ACCEL[@]}")"
  fi
  exec qemu-system-x86_64 "${QEMU_ARGS_LINE[@]}" "${ACCEL[@]}"
) >> "$LOG" 2>&1 &
QEMU_PID=$!
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "\[NVK\] done rc=" "$LOG" && break
  grep -aq "System halt" "$LOG" && break
  kill -0 "$QEMU_PID" 2>/dev/null || break
  sleep 1
done
sleep 2
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "--- compat-track rows ---------------------------------------------"
grep -aE '^\[LCOMPAT\]|PT_INTERP|v2 loader' "$LOG" | head -8 || true
echo "--- probe rows ----------------------------------------------------"
grep -a '^\[NVK\]' "$LOG" || echo "(none: the probe never produced a row)"
echo "--- nvk's own diagnostics -----------------------------------------"
grep -a '^nvk:' "$LOG" | head -20 || true
echo "--- nvshim rows (kernel) ------------------------------------------"
grep -a '\[NVSHIM\]' "$LOG" | head -40 || echo "(none)"
echo "--- faults / exit -------------------------------------------------"
grep -aE "memory fault|page fault|FATAL|System halt" "$LOG" | tail -8 || true

if grep -aq '^\[NVK\] dev\[0\]' "$LOG"; then
  say "RESULT: enumerated (see dev[0] above)"; exit 0
fi
if grep -aq '^\[NVK\] done rc=' "$LOG"; then
  say "RESULT: reached the probe's end without enumerating"; exit 1
fi
say "RESULT: the probe did not complete"
exit 1
