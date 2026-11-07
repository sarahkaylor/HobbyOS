#!/usr/bin/env python3
"""virgl_probe.py — GB-197: is a 3D (virgl) GPU device available on the graded host?

`docs/graphics-accel.md` lists virgl 3D contexts as a future non-goal, and the
Program-2 feasibility doc names the 3D surface as the RTS/DSP blocker.  Before a
GL2 shim is designed, the graded host must answer whether a 3D device exists at
all: "no" is a result that closes the epic's entry criterion honestly.

The probe answers two questions, in order:

  1. HOST: does this QEMU offer `virtio-gpu-gl-pci` (or the mmio
     `virtio-gpu-gl-device`)?  A stock QEMU built without
     `--enable-virglrenderer` prints `Device '<name>' not found`.
  2. GUEST (control): boot with the plain 2D `virtio-gpu-pci` and record what
     the kernel negotiated.  The x64 driver asks for VIRTIO_F_VERSION_1 only
     (`src/kernel/virtio_gpu_x64.c` writes 0 to the driver-feature page), and
     the guest has no VIRTGPU_GET_CAPSET/VIRTGPU_GETPARAM path, so a plain-device
     boot cannot expose a 3D capset — that is the negative control.

A run is a valid measurement (exit 0) in both outcomes:
  * device present -> report the device line and the capset/feature report;
  * device absent  -> report the device line + the plain-2D control boot, verdict
    BLOCKED ON DEVICE.
The falsifier (exit 1) is a "3D supported" claim with no host device line; run it
with --force-3d to see the probe refuse.

Usage:
  python3 tools/t4/virgl_probe.py [--no-boot] [--boot-timeout 150]
                                  [--out evidence/virgl] [--force-3d]
"""

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

GL_DEVICES = ("virtio-gpu-gl-pci", "virtio-gpu-gl-device")
CONTROL_DEVICE = "virtio-gpu-pci"


def device_line(qemu, name):
    """Return (present, line) for `qemu -device NAME,help`."""
    try:
        p = subprocess.run([qemu, "-device", name + ",help"],
                           capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, "probe failed: %s" % e
    out = (p.stdout + p.stderr).strip().splitlines()
    line = out[0] if out else "(no output)"
    present = "not found" not in line.lower() and "options:" in line.lower()
    return present, line


def qemu_version(qemu):
    try:
        p = subprocess.run([qemu, "--version"], capture_output=True, text=True, timeout=20)
        return (p.stdout.splitlines() or ["(unknown)"])[0]
    except (OSError, subprocess.TimeoutExpired):
        return "(unknown)"


def control_boot(root, outdir, timeout_s):
    """Boot the plain 2D guest and capture the virtio-gpu negotiation line.

    `make run` supplies the graded QEMU line (UEFI, NVMe, -serial stdio).  We
    force `-display none` because this is a headless measurement; the probe
    stops as soon as the kernel's GPU line and the desktop menu are in.
    """
    serial = os.path.join(outdir, "control-serial.log")
    # The dot-on `QEMU_ARGS` so the device really is the plain 2D virtio-gpu:
    # the Makefile's soft x64 line has NO GPU device ("std VGA", BGA) whenever
    # `-display none` is present, so it must be added explicitly to exercise
    # the control the card names.
    qemu_make = ["make", "run", "DESKTOP=hobby", "ARCH=intel",
                 "QEMU_ARGS=-display none -device %s -vga none" % CONTROL_DEVICE]
    with open(serial, "w") as f:
        proc = subprocess.Popen(qemu_make, cwd=root, stdout=f,
                                stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.time() + timeout_s
    text = ""
    try:
        while time.time() < deadline:
            time.sleep(1.0)
            try:
                text = open(serial, errors="ignore").read()
            except OSError:
                text = ""
            if "VirtIO GPU:" in text and ("[MENU]" in text or "Desktop starting" in text):
                break
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except OSError:
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except OSError:
                pass
    gpu = re.search(r"^VirtIO GPU: (\S+)", text, re.M)
    return serial, (gpu.group(1) if gpu else None), text


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default="evidence/virgl")
    ap.add_argument("--no-boot", action="store_true",
                    help="host device line only (skip the control boot)")
    ap.add_argument("--boot-timeout", type=int, default=150)
    ap.add_argument("--force-3d", action="store_true",
                    help="falsifier: print '3D supported' without a device line; must exit 1")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = os.path.join(root, args.out, stamp)
    os.makedirs(outdir, exist_ok=True)
    report = os.path.join(outdir, "virgl-report.txt")

    lines = []
    lines.append("[VIRGL] host=%s qemu=%s" % (socket.gethostname(), qemu_version(args.qemu)))

    present = {}
    for name in GL_DEVICES:
        p, line = device_line(args.qemu, name)
        present[name] = p
        lines.append('[VIRGL] device=%s present=%s line="%s"' % (name, "yes" if p else "no", line))

    any_present = any(present.values())

    if args.force_3d and not any_present:
        lines.append("[VIRGL] 3D supported")
        lines.append("[VIRGL] RED: '3D supported' with no host device line "
                     "(the falsifier must fail)")
        with open(report, "w") as f:
            f.write("\n".join(lines) + "\n")
        print("\n".join(lines))
        return 1

    if any_present:
        lines.append("[VIRGL] 3D device present; boot with it and report the capset")
        # The guest driver path for a present device is the future shim work; the
        # probe still records the host line so a present host is distinguishable
        # from a RED claim.
        lines.append("[VIRGL] verdict: DEVICE PRESENT — capset query is the next card")
        rc = 0
    else:
        lines.append("[VIRGL] no virtio-gpu-gl device on this host "
                     "(stock QEMU without --enable-virglrenderer)")
        rc = 0
        if args.no_boot:
            lines.append("[VIRGL] control boot skipped (--no-boot)")
        else:
            serial, gpu, text = control_boot(root, outdir, args.boot_timeout)
            lines.append("[VIRGL] control boot device=%s serial=%s" % (CONTROL_DEVICE, serial))
            lines.append("[VIRGL] guest GPU line: VirtIO GPU: %s" % (gpu or "not found"))
            trio = re.findall(r"VIRGL|capset|3D", text)
            lines.append("[VIRGL] guest 3D markers: %d" % len(trio))
            if not gpu:
                lines.append("[VIRGL] RED: control boot never reported a VirtIO GPU line")
                rc = 1
            elif len(trio):
                lines.append("[VIRGL] RED: plain 2D device reported 3D markers")
                rc = 1
            else:
                lines.append("[VIRGL] 3D capset: none — no device, no VIRGL feature, "
                             "no capset path in the guest driver")
                lines.append("[VIRGL] verdict: BLOCKED ON DEVICE (valid measurement; "
                             "the control boot is the negative control)")
                rc = 0
    with open(report, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print("[VIRGL] report: %s" % report)
    return rc


if __name__ == "__main__":
    sys.exit(main())
