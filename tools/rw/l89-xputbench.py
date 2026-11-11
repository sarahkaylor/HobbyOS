#!/usr/bin/env python3
"""tools/rw/l89-xputbench.py -- price one XPutImage on the real XHOBBY.

Boots HobbyOS with the bare X session (XSESS_NO_WM=1), stages
XPUTBENCH.BIN as /XSTART.TXT, and prints the client's [XPB] rows:
what the XPutImage call costs, what the following XSync costs, and what a
bare round trip costs -- for a contiguous 1280x720 image, for a 1280x720
sub-rect of a 1920x1080 image (HBHOST's exact call), and for one scanline.

The point is to stop conflating "the wire is slow" with "the server is slow":
the in-guest AF_UNIX wire moves 3.7 MB in 1-4.5 ms (linux_probe xwire), while
L88 measured HBHOST's XPutImage at 135 ms for the same bytes.

Usage:
  python3 tools/rw/l89-xputbench.py                # GL device, like the game
  python3 tools/rw/l89-xputbench.py --gpu soft     # plain 2D device
  python3 tools/rw/l89-xputbench.py --no-build     # reuse the staged BIN
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, REPO)
from run_icewm_test import qemu_cmd  # noqa: E402

BIN = "XPUTBENCH.BIN"


def log(msg):
    print(f"[XPB-RUN] {msg}", flush=True)


def build(args):
    env = dict(os.environ, HOME="/home/sarah", TOOLS_SKIP="1")
    subprocess.run(
        ["bash", "third_party/x11/build.sh", "--arch", args.arch,
         "--stage", "clients", "--only", "xputbench", "-j", str(args.jobs)],
        cwd=REPO, env=env, check=True)


def stage(disk, work, x11_build_dir, sndbuf=0):
    bindir = os.path.join(REPO, x11_build_dir, "bin")
    for name in ("XHOBBY.BIN", BIN):
        src = os.path.join(bindir, name)
        if not os.path.exists(src):
            log(f"FAIL: no {src} (build the server/clients stages first)")
            sys.exit(2)
        subprocess.run(["mcopy", "-o", "-i", disk, src, f"::/{name}"],
                       check=True)

    start = os.path.join(work, "XSTART.TXT")
    with open(start, "w") as f:
        f.write(f"# staged by l89-xputbench.py\n/{BIN}\n")
    subprocess.run(["mcopy", "-o", "-i", disk, start, "::/XSTART.TXT"],
                   check=True)

    envp = os.path.join(work, "XSESSION.ENV")
    # The image does not always carry XSESSION.ENV (main's desktop image does
    # not; the rimworld line's did), so read it if it is there and never fail
    # on its absence -- we write the file back either way.
    subprocess.run(["mcopy", "-o", "-i", disk, "::/X11/XSESSION.ENV", envp],
                   check=False, capture_output=True)
    keep = []
    if os.path.exists(envp):
        keep = [ln for ln in open(envp).read().splitlines()
                if not ln.startswith("HB_XDEPTH") and not ln.startswith("XSESS_")]
    with open(envp, "w") as f:
        for ln in keep:
            f.write(ln + "\n")
        f.write("XSESS_NO_WM=1\n")       # no WM: the bench owns the screen
        f.write("HB_PRESPROF=1\n")        # [HBX] present-cost rows
        if sndbuf:
            f.write(f"XPB_SNDBUF={sndbuf}\n")
    subprocess.run(["mmd", "-i", disk, "::/X11"], check=False,
                   capture_output=True)
    subprocess.run(["mcopy", "-o", "-i", disk, envp, "::/X11/XSESSION.ENV"],
                   check=True)
    log(f"staged /{BIN} as /XSTART.TXT, XSESS_NO_WM=1")


def read_serial(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


class _A:
    """qemu_cmd only reads args.arch."""
    arch = "intel"


def qemu_args(args, work, disk):
    cmd, qmp, serial = qemu_cmd(_A(), work, disk)
    if args.gpu == "gl":
        # The game's device pair (Makefile QEMU_GPU=gl QEMU_DISPLAY=egl-headless):
        # swap the helper's plain virtio-gpu-pci for the GL device and turn the
        # display backend's GL on.
        out, i = [], 0
        while i < len(cmd):
            if (cmd[i] == "-device" and i + 1 < len(cmd)
                    and cmd[i + 1] == "virtio-gpu-pci"):
                i += 2
                continue
            out.append(cmd[i])
            i += 1
        cmd = out
        j = cmd.index("-display")
        cmd[j + 1] = "egl-headless,gl=on"
        cmd += ["-device", "virtio-gpu-gl-pci"]
    return cmd, qmp, serial


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--arch", default="intel", choices=["intel", "arm"])
    ap.add_argument("--x11-build-dir", default="third_party/x11/build/intel")
    ap.add_argument("--gpu", default="gl", choices=["gl", "soft"])
    ap.add_argument("--sndbuf", type=int, default=0,
                    help="XPB_SNDBUF for the client: ask the kernel for an N-byte X send ring")
    ap.add_argument("--out", default="obj/l89-xputbench")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    os.makedirs(os.path.join(REPO, args.out), exist_ok=True)
    if not args.no_build:
        build(args)

    work = tempfile.mkdtemp(prefix="xputbench_")
    disk = os.path.join(work, "disk.img")
    shutil.copy(os.path.join(REPO, "disk.img"), disk)
    log(f"booting a copy of disk.img in {work}")
    stage(disk, work, args.x11_build_dir, args.sndbuf)

    cmd, qmp, serial = qemu_args(args, work, disk)
    log("qemu: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(work, "qemu.err"), "w"))
    rows = []
    try:
        t0 = time.time()
        done = False
        while time.time() - t0 < args.timeout:
            txt = read_serial(serial)
            rows = [ln for ln in txt.splitlines() if ln.startswith("[XPB]")]
            if any("done rc=" in r for r in rows):
                done = True
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        for r in rows:
            print(r, flush=True)
        if not done:
            log(f"FAIL: no [XPB] done row in {args.timeout}s")
            print(read_serial(serial)[-3000:])
            return 1
        if any("FAIL" in r for r in rows):
            return 1
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
        out = os.path.join(REPO, args.out, "serial.log")
        shutil.copy(serial, out)
        log(f"serial kept at {out}")


if __name__ == "__main__":
    sys.exit(main())
