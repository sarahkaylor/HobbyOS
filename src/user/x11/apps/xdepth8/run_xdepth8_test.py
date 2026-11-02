#!/usr/bin/env python3
"""run_xdepth8_test.py -- boot HobbyOS with HB_XDEPTH=8 and prove, with
screendump pixels, that XHOBBY.BIN came up as a depth 8 PseudoColor screen.

The chain under test:

    kernel -> XSESSION.BIN (no IceWM: XSESS_NO_WM=1)
           -> XHOBBY.BIN with HB_XDEPTH=8 -> depth 8 PseudoColor screen
           -> /XSTART.TXT -> XDEPTH8.BIN
           -> XMatchVisualInfo(8, PseudoColor) + XCreateColormap(AllocAll)
              + XStoreColors + XCreateWindow(depth 8) + XPutImage (ZPixmap)
           -> DDX palette expansion -> framebuffer -> QMP screendump

Why a dedicated harness: the failure modes that must stay distinguishable are
"the server ignored HB_XDEPTH", "the server has no depth 8 visual", "the
client could not create an 8-bit window", and "the DDX expanded the palette
wrongly".  The serial console carries one marker per step ([XD8] lines are
relayed from the client's stderr), and the screendump measures the actual
framebuffer.  A menu-click harness would conflate all four.

Checks (exit 0 only when all pass):
  serial : the server's "depth 8 PseudoColor" line, XMatchVisualInfo ok,
           XPutImage done, and the server-side XGetImage read-back of the
           green band as index 0x50;
  pixels : white top strip, black bottom strip, red/green/blue bands at known
           window coordinates (window is mapped at 0,0), both band edges one
           pixel wide, and the area right of the 640 px screen untouched.

Usage:
  python3 src/user/x11/apps/xdepth8/run_xdepth8_test.py [--no-build]
      [--x11-build-dir DIR] [--out DIR] [--keep]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "../../../../.."))
sys.path.insert(0, REPO)
from run_icewm_test import Qmp, ppm_pixels, qemu_cmd  # noqa: E402

# Window geometry from main.c: 640x400 at 0,0, white rows 0..3, black rows
# 396..399, bands at x < 213 (red), 213..425 (green), >= 426 (blue).
W, H = 640, 400
RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)


def log(msg):
    print(f"[XD8T] {msg}", flush=True)


def build(args):
    env = dict(os.environ)
    env.setdefault("TOOLS_SKIP", "1")   # never build self-host LLVM here
    mk = ["make", f"ARCH={args.arch}", f"-j{args.jobs}", "DESKTOP=icewm",
          f"X11_BUILD_DIR={args.x11_build_dir}",
          f"X11_PREFIX={args.x11_prefix}", "TOOLS_SKIP=1"]
    log("building DESKTOP=icewm disk.img")
    r = subprocess.run(mk + ["disk.img"], cwd=REPO, env=env)
    if r.returncode != 0:
        log("FAIL: disk.img build failed")
        sys.exit(2)


def stage(disk, work, x11_build_dir, xdepth=True):
    """Put the freshly built server + probe on the disk, make /XDEPTH8.BIN the
    session autostart and (unless this is the 24-bit control run) switch the
    server to depth 8.  Never touch the repo's disk.img (caller passes a copy)."""
    bindir = os.path.join(REPO, x11_build_dir, "bin")
    for name in ("XHOBBY.BIN", "XDEPTH8.BIN"):
        src = os.path.join(bindir, name)
        if not os.path.exists(src):
            log(f"FAIL: no {src} (build the server/clients stages first)")
            sys.exit(2)
        subprocess.run(["mcopy", "-o", "-i", disk, src, f"::/{name}"],
                       check=True)

    start = os.path.join(work, "XSTART.TXT")
    with open(start, "w") as f:
        f.write("# staged by run_xdepth8_test.py\n/XDEPTH8.BIN\n")
    subprocess.run(["mcopy", "-o", "-i", disk, start, "::/XSTART.TXT"],
                   check=True)

    env = os.path.join(work, "XSESSION.ENV")
    subprocess.run(["mcopy", "-o", "-i", disk, "::/X11/XSESSION.ENV", env],
                   check=False, capture_output=True)
    keep = []
    if os.path.exists(env):
        keep = [ln for ln in open(env).read().splitlines()
                if not ln.startswith("HB_XDEPTH") and not ln.startswith("XSESS_")]
    with open(env, "w") as f:
        for ln in keep:
            f.write(ln + "\n")
        if xdepth:
            f.write("HB_XDEPTH=8\n")
        f.write("XSESS_NO_WM=1\n")
    subprocess.run(["mcopy", "-o", "-i", disk, env, "::/X11/XSESSION.ENV"],
                   check=True)
    log("staged /XSTART.TXT (/XDEPTH8.BIN) and "
        + ("HB_XDEPTH=8, " if xdepth else "(no HB_XDEPTH), ")
        + "XSESS_NO_WM=1")


def read_serial(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_for(serial, needle, deadline_s, proc, start=0):
    end = time.time() + deadline_s
    while time.time() < end:
        i = read_serial(serial).find(needle, start)
        if i >= 0:
            return i
        if proc.poll() is not None:
            return read_serial(serial).find(needle, start)
        time.sleep(0.3)
    return read_serial(serial).find(needle, start)


def pixel(px, w, x, y):
    o = (y * w + x) * 3
    return (px[o], px[o + 1], px[o + 2])


def near(rgb, want, tol=4):
    return all(abs(a - b) <= tol for a, b in zip(rgb, want))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--arch", default="intel", choices=["intel", "arm"])
    ap.add_argument("--x11-build-dir", default="third_party/x11/build/intel")
    ap.add_argument("--x11-prefix", default=os.path.expanduser(
        "~/hobbyos-icewm-lanes/prefix/intel"))
    ap.add_argument("--out", default="/tmp/hobbyos_xdepth8")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--keep", action="store_true",
                    help="keep the booted disk copy and serial log")
    ap.add_argument("--control24", action="store_true",
                    help="negative control: same client/disk WITHOUT HB_XDEPTH; "
                         "expect a depth 24 root and a failed visual match")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    if not args.no_build:
        build(args)

    work = tempfile.mkdtemp(prefix="xdepth8_")
    disk = os.path.join(work, "disk.img")
    shutil.copy(os.path.join(REPO, "disk.img"), disk)
    log(f"booting a copy of disk.img in {work}")
    stage(disk, work, args.x11_build_dir)

    class _A:                          # qemu_cmd only reads args.arch
        arch = args.arch
    cmd, qmp_path, serial = qemu_cmd(_A(), work, disk)
    log("qemu: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(work, "qemu.err"), "w"))
    fails, notes = [], []
    try:
        qmp = Qmp(qmp_path, deadline_s=40)
        t0 = time.time()

        def need(needle, what, dl=None):
            i = wait_for(serial, needle, dl or args.timeout, proc)
            if i < 0:
                fails.append(f"missing serial marker: {what} ({needle!r})")
                log(f"FAIL {what}")
            else:
                log(f"ok   {what}  (+{time.time() - t0:.1f}s)")
            return max(i, 0)

        if args.control24:
            # Negative control: the identical client, the identical disk, only
            # HB_XDEPTH absent.  The server must stay depth 24 and the visual
            # match must fail exactly where Doom would I_Error.
            need("[XD8] display :0 open: screen 0 1920x1080, root depth 24",
                 "control: no HB_XDEPTH -> depth 24 root")
            need("[XD8] FAIL XMatchVisualInfo(depth 8, PseudoColor)",
                 "control: XMatchVisualInfo(8, PseudoColor) fails at depth 24",
                 dl=60)
        else:
            pos = need("hobbyos: screen 640x400 depth 8 PseudoColor",
                       "server came up as a depth 8 PseudoColor screen")
            pos = need("[XD8] XMatchVisualInfo depth 8 PseudoColor ok",
                       "client XMatchVisualInfo(8, PseudoColor)", dl=60)
            pos = need("[XD8] XPutImage 640x400 ZPixmap depth 8 done",
                       "client XPutImage of a depth 8 ZPixmap", dl=60)
            pos = need("[XD8] XGetImage green band (320,200) = index 0x50",
                       "server read-back of the green band as index 0x50", dl=60)
            need("[XD8] ready", "client reached its idle loop", dl=60)
        time.sleep(3)                  # let the present reach the framebuffer

        ppm = os.path.join(args.out,
                           "xdepth8-control24.ppm" if args.control24
                           else "xdepth8.ppm")
        if not qmp.shot(ppm):
            fails.append("QMP screendump failed")
        else:
            time.sleep(0.5)
            px, w, h = ppm_pixels(ppm)
            log(f"screendump {ppm} ({w}x{h})")
            if not px:
                fails.append("screendump is not a P6 PPM")
            elif args.control24:
                got = pixel(px, w, 320, 200)
                ok = near(got, BLACK, 8)
                log(f"{'ok  ' if ok else 'FAIL'} control: (320,200) = {got} "
                    f"(no 8-bit window was drawn)")
                if not ok:
                    fails.append(f"control (320,200) is {got}, want black")
            else:
                probes = [
                    ("red band   (100,200)", 100, 200, RED),
                    ("green band (320,200)", 320, 200, GREEN),
                    ("blue band  (600,200)", 600, 200, BLUE),
                    ("white strip (320,2)", 320, 2, WHITE),
                    ("black strip (320,398)", 320, 398, BLACK),
                ]
                for what, x, y, want in probes:
                    got = pixel(px, w, x, y)
                    ok = near(got, want)
                    log(f"{'ok  ' if ok else 'FAIL'} {what} = {got} "
                        f"(want {want})")
                    if not ok:
                        fails.append(f"{what} is {got}, want {want}")

                # Band edges: one pixel wide at x=212/213 and 425/426.
                edges = [("red|green", 212, 213, RED, GREEN),
                         ("green|blue", 425, 426, GREEN, BLUE)]
                for what, xa, xb, ca, cb in edges:
                    a, b = pixel(px, w, xa, 200), pixel(px, w, xb, 200)
                    ok = near(a, ca) and near(b, cb)
                    log(f"{'ok  ' if ok else 'FAIL'} edge {what}: "
                        f"x={xa} {a}, x={xb} {b}")
                    if not ok:
                        fails.append(f"band edge {what} is {a}/{b}, "
                                     f"want {ca}/{cb}")

                # Outside the 640x400 screen the framebuffer must be untouched
                # (the root window is black), so the client did not paint
                # beyond its window.
                out = pixel(px, w, 700, 500)
                ok = near(out, BLACK, 8)
                log(f"{'ok  ' if ok else 'FAIL'} outside screen (700,500) = {out}")
                if not ok:
                    fails.append(f"(700,500) is {out}, want black")

                # Save a PNG next to the PPM when ImageMagick is around.
                if shutil.which("convert"):
                    subprocess.run(["convert", ppm, ppm[:-4] + ".png"],
                                   stderr=subprocess.DEVNULL)

        text = read_serial(serial)
        with open(os.path.join(args.out, "serial.log"), "w") as f:
            f.write(text)
        rows = [ln for ln in text.splitlines() if "[XD8]" in ln]
        notes.append(f"{len(rows)} [XD8] console rows")
        for ln in rows:
            log("console: " + ln.strip())
        log(f"serial log: {os.path.join(args.out, 'serial.log')}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if args.keep:
            log(f"kept work dir {work}")

    verdict = "PASS" if not fails else "FAIL"
    with open(os.path.join(args.out, "REPORT.txt"), "w") as f:
        f.write(f"xdepth8 depth-8 PseudoColor screen test: {verdict}\n")
        for n in notes:
            f.write(f"  {n}\n")
        for x in fails:
            f.write(f"  FAIL {x}\n")
    log(f"RESULT: {verdict}" + ("" if not fails else " -- " + "; ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
