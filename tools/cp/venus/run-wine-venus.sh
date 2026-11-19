#!/usr/bin/env bash
# tools/cp/venus/run-wine-venus.sh -- Gate P6 probe 2: the P5 D3D12 triangle
# (d3d12tri.exe, vkd3d-proton 3.1) under Wine on Venus (lane C6-venus).
#
# Runs C5-vk2's tools/cp/p5/run-wine-p5.sh unchanged, with two seams:
#   * CP_VK_WORK points at a merged Vulkan tree ($VENUS_WORK/vkmerge) = C5's
#     x11 stage (loader) with lavapipe's ICD JSON removed and this lane's Venus
#     ICD added, so winevulkan's only driver is Venus (the Radeon);
#   * VENUS_P5=1 + PYTHONPATH=tools/cp/venus/pyhook make boot_wine_p5.py boot
#     with tools/cp/host-venus.sh's QEMU line (tools/cp/venus/p5venus.py).
# Everything else (NFS export via exports.sh, grading, screendumps) is P5's.
#
#   bash tools/cp/venus/run-wine-venus.sh          (P5_PROBES default "d3d12")
# Env: as run-wine-p5.sh (CP_WINE_STAGE, WINE_BUILD, CP_X11_WORK, CP_P5_PE,
#      P5_EXPORT default /srv/nfs/cp-venus-p5, P5_XHOBBY..., P5_XSESSION_ENV),
#      VENUS_WORK (~/cp/venus-build), CP_VK_SRC (C5's work dir, ~/cp/vk2-build).
#      P5_OUT (default obj/cp-p5/venus: its own dir, so P5's obj/cp-p5/wine
#      evidence survives a P6 rerun; C0-console).
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
VW=${VENUS_WORK:-$HOME/cp/venus-build}
SRC=${CP_VK_SRC:-$HOME/cp/vk2-build}
M=$VW/vkmerge
[ -f "$VW/stage/CP/VK/lib/libvulkan_virtio.so" ] || { echo "no Venus stage (tools/mesa/venus/build.sh all)"; exit 2; }
rm -rf "$M"; mkdir -p "$M/stage-x11/CP"
cp -a "$SRC/stage-x11/CP/VK" "$M/stage-x11/CP/"
rm -f "$M/stage-x11/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json"
cp "$VW/stage/CP/VK/lib/libvulkan_virtio.so" "$M/stage-x11/CP/VK/lib/"
cp "$VW/stage/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json" "$M/stage-x11/CP/VK/share/vulkan/icd.d/"
ln -s "$SRC/sysroot-glibc231" "$M/sysroot-glibc231"
ln -s "$SRC/vulkan-headers" "$M/vulkan-headers"
echo "== merged Vulkan tree $M: $(ls "$M/stage-x11/CP/VK/share/vulkan/icd.d")"
cd "$REPO"
CP_VK_WORK="$M" VENUS_P5=1 PYTHONPATH="$REPO/tools/cp/venus/pyhook${PYTHONPATH:+:$PYTHONPATH}" \
  P5_EXPORT="${P5_EXPORT:-/srv/nfs/cp-venus-p5}" P5_PROBES="${P5_PROBES:-d3d12}" \
  P5_OUT="${P5_OUT:-$REPO/obj/cp-p5/venus}" \
  bash tools/cp/p5/run-wine-p5.sh
