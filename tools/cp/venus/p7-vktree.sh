#!/usr/bin/env bash
# tools/cp/venus/p7-vktree.sh -- the Vulkan tree P7's probes boot with (lane
# C6-p7, docs/cp-p7.md §5).
#
#   bash tools/cp/venus/p7-vktree.sh          -> prints the tree's dir ($M)
#
# $M (default $P7_WORK/vkmerge, P7_WORK default ~/cp/p7work) is a CP_VK_WORK
# for the P5/P6 harnesses: C5-vk2's x11 stage (loader, lavapipe, probes;
# read-only source, copied) with
#   * this lane's Venus ICD (VENUS_WORK = $P7_WORK/venus-build, built by
#     tools/mesa/venus/build.sh) as the only ICD JSON, and
#   * vkcubex rebuilt from this tree's src/user/cp/vkcubex.c (its `fs`
#     fullscreen mode is P7's).
# sysroot-glibc231 and vulkan-headers are symlinks to C5's.
# P7_KEEP_LVP=1 keeps lavapipe's ICD JSON (dir $M-lvp): tools/cp/p5/run_vkcubex.py
# stages it unconditionally and run_vkcubex_venus.py deletes it from the disk.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
P7W=${P7_WORK:-$HOME/cp/p7work}
VW=${VENUS_WORK:-$P7W/venus-build}
SRC=${CP_VK_SRC:-$HOME/cp/vk2-build}
X11DEV=${CP_X11_DEV:-$HOME/cp/vk2-x11}/destdir/CP/X11
M=${P7_VKMERGE:-$P7W/vkmerge}
[ "${P7_KEEP_LVP:-0}" = 1 ] && M=$M-lvp
[ -f "$VW/stage/CP/VK/lib/libvulkan_virtio.so" ] || {
  echo "p7-vktree: no Venus stage in $VW (VENUS_WORK=$VW bash tools/mesa/venus/build.sh all)" >&2; exit 2; }
rm -rf "$M"; mkdir -p "$M/stage-x11/CP"
cp -a "$SRC/stage-x11/CP/VK" "$M/stage-x11/CP/"
[ "${P7_KEEP_LVP:-0}" = 1 ] || rm -f "$M/stage-x11/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json"
cp "$VW/stage/CP/VK/lib/libvulkan_virtio.so" "$M/stage-x11/CP/VK/lib/"
cp "$VW/stage/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json" "$M/stage-x11/CP/VK/share/vulkan/icd.d/"
ln -s "$SRC/sysroot-glibc231" "$M/sysroot-glibc231"
ln -s "$SRC/vulkan-headers" "$M/vulkan-headers"
P="$REPO/src/user/cp"
clang --sysroot="$SRC/sysroot-glibc231" -O2 -g -Wall -Wextra -fPIE -pie -I"$SRC/vulkan-headers/include" -I"$P" \
  -I"$X11DEV/include" -fuse-ld=lld -L"$X11DEV/lib" -Wl,-rpath-link,"$X11DEV/lib" \
  -Wl,--disable-new-dtags,-rpath,/CP/X11/lib:/CP/VK/lib -Wl,--build-id=sha1 \
  -o "$M/stage-x11/CP/VK/bin/vkcubex" "$P/vkcubex.c" -lxcb -ldl -lm >&2
echo "p7-vktree: $M ($(ls "$M/stage-x11/CP/VK/share/vulkan/icd.d"), vkcubex from $P/vkcubex.c)" >&2
echo "$M"
