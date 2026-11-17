#!/usr/bin/env python3
"""tools/cp/venus/run_vkcubex_venus.py -- Gate P6 probe 1: Linux vkcube on Venus
in the guest X session (lane C6-venus, docs/cp-venus.md §4).

This is C5-vk2's Gate P5 harness (tools/cp/p5/run_vkcubex.py, owned by the
P5/X lanes) run unchanged except for three seams, patched in from here so the
P5 file is not edited:
  * the QEMU command is tools/cp/host-venus.sh's (`argv`): virtio-gpu-gl-pci
    venus=true,blob=true,hostmem=8G on a 12 GiB memfd guest, egl-headless on the
    RX 9070 XT render node, run under `env` with the host-venus.sh environment
    (radeon ICD ordered 9070 XT first, the dma-buf RDWR shim) and without
    DISPLAY/WAYLAND_DISPLAY;
  * the staged Vulkan tree carries this lane's Mesa Venus ICD
    (libvulkan_virtio.so + virtio_icd.x86_64.json from tools/mesa/venus/build.sh)
    and NOT lavapipe's ICD JSON, so the loader's only driver is Venus and
    vkcubex's device 0 is the Radeon;
  * the PASS line names Venus, and the [VK] rows must show a Venus device.

The grading (>= 60 s, frames > 0, two distinct screendumps with the cube, no
fault rows) is P5's own.  Same CLI as run_vkcubex.py.
Env: VENUS_WORK (~/cp/venus-build) plus run_vkcubex.py's.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tools", "cp", "p5"))
sys.path.insert(0, HERE)
import p5venus  # noqa: E402

p5venus.install()
import run_vkcubex as P5  # noqa: E402

VW = os.environ.get("VENUS_WORK", os.path.expanduser("~/cp/venus-build"))
VNS = os.path.join(VW, "stage", "CP", "VK")


_p5_stage = P5.stage


def venus_stage(disk, args, tmp):
    _p5_stage(disk, args, tmp)
    m = P5.m
    # lavapipe's ICD JSON out, Venus in: Venus is the loader's only driver.
    m("mdel", "-i", disk, "::/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json", check=False)
    m("mcopy", "-o", "-i", disk, os.path.join(VNS, "lib/libvulkan_virtio.so"),
      "::/CP/VK/lib/libvulkan_virtio.so")
    m("mcopy", "-o", "-i", disk, os.path.join(VNS, "share/vulkan/icd.d/virtio_icd.x86_64.json"),
      "::/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json")
    P5.log("venus: staged /CP/VK/lib/libvulkan_virtio.so + virtio_icd.x86_64.json; lvp ICD JSON removed")


def main():
    if not os.path.exists(os.path.join(VNS, "lib/libvulkan_virtio.so")):
        P5.log(f"FAIL: no Venus stage at {VNS} (bash tools/mesa/venus/build.sh all)")
        return 2
    P5.qemu_cmd = p5venus.venus_qemu_cmd
    P5.stage = venus_stage
    if "--out" not in sys.argv:
        sys.argv += ["--out", os.path.join(REPO, "obj", "cp-venus", "vkcubex")]
    rc = P5.main()
    out = sys.argv[sys.argv.index("--out") + 1]
    ser = P5.read(os.path.join(out, "serial.log"))
    venus = [ln for ln in ser.splitlines() if "[VK]" in ln and "Venus" in ln]
    if not venus:
        P5.log("FAIL: no [VK] row names a Venus device")
        return 1
    P5.log("device row: " + venus[0][venus[0].index("[VK]"):])
    if rc == 0:
        P5.log("PASS: Linux vkcube on Venus (host RX 9070 XT) in the X session for >= 60 s, "
               "two distinct screendumps")
    return rc


if __name__ == "__main__":
    sys.exit(main())
