#!/usr/bin/env bash
# tools/cp/venus/run_vk.sh -- run a glibc Vulkan probe on Venus in a MODE=test
# wave boot (Gate P6, lane C6-venus; docs/cp-venus.md §4).
#
#   bash tools/cp/venus/run_vk.sh [ARGS...]        (runs /VNINFO.ELF ARGS)
#
# Builds src/user/cp/vn_info.c against the glibc 2.31 sysroot and C5-vk2's
# Vulkan headers, stages onto a COPY of the tree's MODE=test disk.img:
#   /lib64/ld-linux-x86-64.so.2 + /lib/x86_64-linux-gnu  glibc 2.31 runtime
#   /CP/VK/lib/libvulkan.so.1 (+ lavapipe)  C5-vk2's x11 stage (read-only copy)
#   /CP/VK/lib/libvulkan_virtio.so + icd.d/virtio_icd.x86_64.json  this lane's
#                                            Mesa Venus (tools/mesa/venus/build.sh)
#   /CP/X11/lib                              C4's X client libraries (the Venus
#                                            ICD links libxcb for its WSI)
# and boots it on the Venus host recipe (tools/cp/host-venus.sh), one slot.
#
# Env: VENUS_WORK (~/cp/venus-build), CP_VK_WORK (~/cp/vk2-build),
#      CP_X11_WORK (~/cp/x11-build-x2), VK_OUT (obj/venus-vk), VK_TIMEOUT (300),
#      VK_NO_BUILD=1 (reuse the tree's MODE=test kernel/disk).
set -euo pipefail
ARGS="$*"
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
VW=${VENUS_WORK:-$HOME/cp/venus-build}
VKW=${CP_VK_WORK:-$HOME/cp/vk2-build}
X11W=${CP_X11_WORK:-$HOME/cp/x11-build-x2}
OUT=${VK_OUT:-$REPO/obj/venus-vk}
TIMEOUT=${VK_TIMEOUT:-300}
SYS=$VKW/sysroot-glibc231
VKS=$VKW/stage-x11/CP/VK
VNS=$VW/stage/CP/VK
X11S=$X11W/stage/CP/X11
mkdir -p "$OUT"
cd "$REPO"
[ -f "$VNS/lib/libvulkan_virtio.so" ] || { echo "run_vk: no Venus stage (bash tools/mesa/venus/build.sh all)"; exit 2; }
if [ "${VK_NO_BUILD:-0}" != 1 ]; then
    rm -f hobbyos.elf
    TOOLS_SKIP=1 make ARCH=intel MODE=test hobbyos.elf disk.img > "$OUT/build.log" 2>&1 || {
        tail -30 "$OUT/build.log"; echo "run_vk: kernel/disk build FAILED"; exit 2; }
fi
clang --sysroot="$SYS" -O2 -g -Wall -Wextra -fPIE -pie -I"$VKW/vulkan-headers/include" \
    -Isrc/user/cp -fuse-ld=lld -Wl,--enable-new-dtags,-rpath,/CP/VK/lib -Wl,--build-id=sha1 \
    -o "$OUT/vn_info" src/user/cp/vn_info.c -ldl
IMG=$OUT/disk.img
cp --sparse=always disk.img "$IMG"
st() { mcopy -o -i "$IMG" "$1" "$2" || { echo "run_vk: mcopy $2 failed"; exit 2; }; }
for d in ::/lib64 ::/lib ::/lib/x86_64-linux-gnu ::/CP ::/CP/VK ::/CP/VK/lib ::/CP/VK/bin \
         ::/CP/VK/share ::/CP/VK/share/vulkan ::/CP/VK/share/vulkan/icd.d ::/CP/X11; do
    mmd -i "$IMG" "$d" >/dev/null 2>&1 || true
done
L=$SYS/lib/x86_64-linux-gnu
st "$L/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2
for p in "libc-2.31.so libc.so.6" "libm-2.31.so libm.so.6" "libpthread-2.31.so libpthread.so.0" \
         "libdl-2.31.so libdl.so.2" "librt-2.31.so librt.so.1" "libgcc_s.so.1 libgcc_s.so.1"; do
    a=${p% *}; b=${p#* }; st "$L/$a" "::/lib/x86_64-linux-gnu/$b"
done
st "$(readlink -f "$VKS/lib/libvulkan.so.1")" ::/CP/VK/lib/libvulkan.so.1
st "$VKS/lib/libvulkan_lvp.so" ::/CP/VK/lib/libvulkan_lvp.so
st "$VKS/share/vulkan/icd.d/lvp_icd.x86_64.json" ::/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json
st "$VNS/lib/libvulkan_virtio.so" ::/CP/VK/lib/libvulkan_virtio.so
st "$VNS/share/vulkan/icd.d/virtio_icd.x86_64.json" ::/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json
mcopy -s -o -i "$IMG" "$X11S/lib" ::/CP/X11/ || { echo "run_vk: X11 libs"; exit 2; }
st "$OUT/vn_info" ::/VNINFO.ELF
printf 'VNINFO.ELF %s\n' "$ARGS" > "$OUT/WAVE.CFG"
st "$OUT/WAVE.CFG" ::WAVE.CFG
rm -f "$OUT/serial.log"
bash tools/cp/qemu-slot.sh bash tools/cp/host-venus.sh boot "$IMG" "$OUT" &
outer=$!
t0=$(date +%s); qpid=
while :; do
    sleep 2
    [ -z "$qpid" ] && qpid=$(pgrep -f "^qemu-system-x86_64 -name hobbyos-venus.*$OUT/serial.log" | head -1 || true)
    grep -aq '\[VK\] done rc=' "$OUT/serial.log" 2>/dev/null && { sleep 2; break; }
    [ $(( $(date +%s) - t0 )) -ge "$TIMEOUT" ] && { echo "run_vk: TIMEOUT after ${TIMEOUT}s"; break; }
    kill -0 "$outer" 2>/dev/null || break
done
[ -n "$qpid" ] && kill "$qpid" 2>/dev/null || true
wait "$outer" 2>/dev/null || true
grep -aE '^\[VIRGL3D\] (feat|shm|irq)|\[VENUS\]|\[VK\]|\[LCOMPAT\]|MESA|vn_|venus|FATAL|fault|error' "$OUT/serial.log" | grep -av '\[PUB\]' > "$OUT/rows.txt" || true
cat "$OUT/rows.txt"
echo "--- qemu stderr (tail)"; tail -25 "$OUT/qemu-stderr.txt" 2>/dev/null || true
grep -aq '\[VK\] done rc=0' "$OUT/serial.log"
