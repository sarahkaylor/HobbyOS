#!/usr/bin/env bash
# tools/cp/vk/gate.sh <enum|cube> — run a C5-vk Vulkan probe inside HobbyOS
# (x86_64, MODE=test wave, KVM) and grade it from the REAL serial log.
#
#   enum   /VKENUM.ELF: the vulkaninfo --summary equivalent; PASS needs a
#          device whose name contains "llvmpipe" and "[VK] done rc=0".
#   cube   /VKCUBE.ELF: lavapipe renders the spinning cube offscreen at
#          640x480 and 1280x720, reads it back and blits it into /dev/fb0;
#          while the probe holds its last frame this script takes a QMP
#          screendump (PPM -> PNG).  PASS needs both RESULT rows and done rc=0.
#
# Inputs: the staged tree from `bash tools/cp/vk/build.sh all`
# ($CP_VK_WORK/stage/CP/VK) and the same glibc 2.31 sysroot.  Staging on the
# FAT disk.img (F4 proposal): /CP/VK/{lib,bin,share/vulkan/icd.d} with real
# sonames (FAT has no symlinks: libvulkan.so.1 is a copy of the versioned
# file), the glibc runtime in /lib64 + /lib/x86_64-linux-gnu (as the M1 gate),
# and a root copy of the probe named in WAVE.CFG.
#
# Every boot runs under a QEMU slot (tools/cp/qemu-slot.sh).
# Env: CP_VK_WORK (default ~/cp/vk-build), VK_TIMEOUT (s, default 420),
#      VK_FRAMES (default 90), VK_HOLD (default 20), VK_QEMU_EXTRA (e.g. "-cpu host"),
#      VK_SMP (QEMU_SMP override), VK_TAG (suffix for log/shot names), VK_SKIP_BUILD=1.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
MODE_ARG="${1:?usage: gate.sh <enum|cube>}"
WORK="${CP_VK_WORK:-$HOME/cp/vk-build}"
SYS="$WORK/sysroot-glibc231"
VK="$WORK/stage/CP/VK"
TAG="${VK_TAG:-}"
OUT="$ROOT/obj/cp-vk"; mkdir -p "$OUT"
LOG="$OUT/$MODE_ARG$TAG.serial.log"
TIMEOUT_S="${VK_TIMEOUT:-420}"
FRAMES="${VK_FRAMES:-90}"
HOLD="${VK_HOLD:-20}"
SOCK="$OUT/qmp-$MODE_ARG.sock"
IMG="$ROOT/disk.img"
say() { printf 'cp-vk-gate: %s\n' "$*"; }

[ -f "$VK/lib/libvulkan_lvp.so" ] || { say "no staged tree at $VK (run tools/cp/vk/build.sh all)"; exit 2; }
case "$MODE_ARG" in
  enum) PROG=VKENUM.ELF; SRCBIN="$VK/bin/vkenum"; LINE="VKENUM.ELF" ;;
  cube) PROG=VKCUBE.ELF; SRCBIN="$VK/bin/vkcube"; LINE="VKCUBE.ELF $FRAMES $HOLD" ;;
  *) say "unknown mode $MODE_ARG"; exit 2 ;;
esac

if [ "${VK_SKIP_BUILD:-0}" != 1 ]; then
  say "build kernel + disk (MODE=test, TOOLS_SKIP=1; log $OUT/build.log)"
  TOOLS_SKIP=1 make -C "$ROOT" ARCH=intel MODE=test hobbyos.elf disk.img > "$OUT/build.log" 2>&1 \
    || { tail -30 "$OUT/build.log"; say "BUILD FAILED"; exit 2; }
fi

say "stage glibc 2.31 + /CP/VK onto $IMG"
stage() { mcopy -o -i "$IMG" "$1" "$2" || { say "mcopy failed $2"; exit 2; }; }
mdir_p() { mmd -i "$IMG" "$1" >/dev/null 2>&1 || true; }
for d in ::/lib64 ::/lib ::/lib/x86_64-linux-gnu ::/CP ::/CP/VK ::/CP/VK/lib ::/CP/VK/bin \
         ::/CP/VK/share ::/CP/VK/share/vulkan ::/CP/VK/share/vulkan/icd.d; do mdir_p "$d"; done
L="$SYS/lib/x86_64-linux-gnu"
stage "$L/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2
for p in "libc-2.31.so libc.so.6" "libm-2.31.so libm.so.6" "libpthread-2.31.so libpthread.so.0" \
         "libdl-2.31.so libdl.so.2" "librt-2.31.so librt.so.1" "libgcc_s.so.1 libgcc_s.so.1"; do
  set -- $p; stage "$L/$1" "::/lib/x86_64-linux-gnu/$2"
done
stage "$(readlink -f "$VK/lib/libvulkan.so.1")" ::/CP/VK/lib/libvulkan.so.1
stage "$VK/lib/libvulkan_lvp.so" ::/CP/VK/lib/libvulkan_lvp.so
stage "$VK/share/vulkan/icd.d/lvp_icd.x86_64.json" ::/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json
stage "$VK/bin/vkenum" ::/CP/VK/bin/vkenum
stage "$VK/bin/vkcube" ::/CP/VK/bin/vkcube
stage "$SRCBIN" "::$PROG"
printf '%s\n' "$LINE" > "$OUT/WAVE.CFG"
stage "$OUT/WAVE.CFG" ::WAVE.CFG
mdir -i "$IMG" -/ ::/CP/VK | grep -E 'lib|json|vk' | head -12

OVMF="$OUT/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" "$OVMF"
rm -f "$SOCK" "$LOG"
SMPARG=(); [ -n "${VK_SMP:-}" ] && SMPARG=(QEMU_SMP="$VK_SMP")

say "boot under a QEMU slot (tools/cp/qemu-slot.sh; deadline ${TIMEOUT_S}s, serial $LOG)"
. "$ROOT/tools/cp/qemu-slot.sh"; cp_qemu_slot_acquire
say "lock acquired $(date -u +%T)"
make -C "$ROOT" -o disk.img -o hobbyos.elf ARCH=intel MODE=test EDK2_X86_64="$OVMF" "${SMPARG[@]}" run \
  QEMU_ARGS="-display none -enable-kvm -qmp unix:$SOCK,server,nowait ${VK_QEMU_EXTRA:-}" > "$LOG" 2>&1 &
MAKEPID=$!
shot_done=0
t0=$SECONDS
while [ $((SECONDS - t0)) -lt "$TIMEOUT_S" ]; do
  if [ "$MODE_ARG" = cube ] && [ "$shot_done" = 0 ] && grep -aq '^\[VK\] hold' "$LOG"; then
    sleep 2
    PPM="$OUT/cube$TAG.ppm"
    ( printf '{"execute":"qmp_capabilities"}\n'; sleep 0.3
      printf '{"execute":"screendump","arguments":{"filename":"%s"}}\n' "$PPM"; sleep 2 ) \
      | socat - "UNIX-CONNECT:$SOCK" > "$OUT/qmp.out" 2>&1
    python3 "$ROOT/skills/hobbyos-screenshot/scripts/ppm2png.py" "$PPM" "$OUT/cube$TAG.png" \
      && say "screendump: $OUT/cube$TAG.png ($(stat -c%s "$OUT/cube$TAG.png") B)"
    shot_done=1
  fi
  grep -aq 'System halt' "$LOG" && break
  kill -0 "$MAKEPID" 2>/dev/null || break
  sleep 1
done
sleep 1
if kill -0 "$MAKEPID" 2>/dev/null; then
  say "deadline: stopping the QEMU this script started"
  for q in $(pgrep -P "$MAKEPID"); do kill "$q" 2>/dev/null; done
  sleep 2; kill "$MAKEPID" 2>/dev/null
fi
wait "$MAKEPID" 2>/dev/null
flock -u 197; exec 197>&-; unset CP_QEMU_SLOT_HELD
say "boot finished after $((SECONDS - t0)) s"

echo "--- [VK] rows ---"
grep -a '\[VK\]' "$LOG" || echo "(none)"
echo "--- unhandled Linux syscalls ---"
grep -a '\[LCOMPAT\]' "$LOG" | sort | uniq -c | sort -rn | head -20 || true
echo "--- faults / exits ---"
grep -aE 'memory fault|FATAL|\[EXITPC\]|page fault|System halt' "$LOG" | tail -10 || true
ok=1
grep -aq '\[VK\] done rc=0' "$LOG" || ok=0
if [ "$MODE_ARG" = enum ]; then
  grep -aq '\[VK\]     deviceName *= llvmpipe' "$LOG" || ok=0
else
  [ "$(grep -ac '\[VK\] size [0-9x]* RESULT' "$LOG")" -ge 2 ] || ok=0
  [ "$shot_done" = 1 ] || ok=0
fi
[ "$ok" = 1 ] && { say "PASS ($MODE_ARG)"; exit 0; }
say "FAIL ($MODE_ARG)"; exit 1
