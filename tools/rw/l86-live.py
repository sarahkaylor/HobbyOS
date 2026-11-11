#!/usr/bin/env python3
"""l86-live.py -- drive the already-running RimWorld VM over QMP, without
rebooting it.

Why: the harness kills QEMU when its driver returns (`fuser -k disk.img`), so
every driver edit costs a boot plus ~10 min of def loading.  This script talks
to the same QMP socket and reuses rw_l86_input's helpers, so a screen can be
inspected, clicked and re-clicked while the game stays where it is.

Boot the VM yourself first (the disk must already be staged by a harness run):

    make -o disk.img ARCH=intel MODE=desktop DESKTOP=icewm \
         X11_BUILD_DIR=... QEMU_GPU=gl QEMU_DISPLAY=egl-headless \
         run QEMU_ARGS="-display none -enable-kvm \
         -qmp unix:$PWD/qmp-l86,server,nowait" >> /tmp/l86-live.log 2>&1 &

then:

    tools/rw/l86-live.py --qmp qmp-l86 --fb-phys 0x... --ocr
    tools/rw/l86-live.py --qmp qmp-l86 --click 1215 267
    tools/rw/l86-live.py --qmp qmp-l86 --key tab
    tools/rw/l86-live.py --qmp qmp-l86 --auto      # the rw_l86_input flow
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rw_l86_input as L
from rw_desktop_drive import Qmp


class Grabber:
    def __init__(self, q, out_dir, fb_phys, fb_w, fb_h):
        self.q = q
        self.out = out_dir
        self.fb = fb_phys
        self.w, self.h = fb_w, fb_h
        self.n = 0
        os.makedirs(out_dir, exist_ok=True)

    def __call__(self, tag):
        self.n += 1
        p = os.path.join(self.out, "%03d_%s.ppm" % (self.n, tag))
        raw = p + ".raw"
        self.q.pmem(self.fb, self.w * self.h * 4, raw)
        time.sleep(0.3)
        from rw_desktop_drive import bgra_to_ppm
        if not bgra_to_ppm(raw, self.w, self.h, p):
            print("shot failed")
            return None
        os.remove(raw)
        png = p[:-4] + ".png"
        subprocess.run(["convert", p, png], stderr=subprocess.DEVNULL)
        print("[live] %s" % png, flush=True)
        return png


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--log", default="obj/rw-desktop/serial-desktop.log")
    ap.add_argument("--shot-dir", default="obj/l86/live")
    ap.add_argument("--fb-phys", required=True)
    ap.add_argument("--fb-w", type=int, default=1920)
    ap.add_argument("--fb-h", type=int, default=1080)
    ap.add_argument("--ocr", action="store_true")
    ap.add_argument("--click", nargs=2, type=int)
    ap.add_argument("--hold", type=float, default=0.15)
    ap.add_argument("--key", nargs="+")
    ap.add_argument("--auto", action="store_true")
    ap.add_argument("--stage", default="live")
    args = ap.parse_args()

    q = Qmp(args.qmp)
    grab = Grabber(q, args.shot_dir, args.fb_phys, args.fb_w, args.fb_h)
    ns = argparse.Namespace(log=args.log, shot_dir=args.shot_dir,
                            fb_w=args.fb_w, fb_h=args.fb_h)
    d = L.L86(q, grab, args.log, ns, [])
    d.establish_geometry(30)
    print("[live] client=%s content=%s" % (d.geom, d.content), flush=True)

    if args.ocr:
        png = grab("ocr")
        words = d.ocr(png, tag="live")
        for w in sorted(words, key=lambda k: (k["y"], k["x"])):
            if w["conf"] >= 40:
                print("  %-24s (%4d,%4d) %3dx%-3d conf=%.0f"
                      % (w["t"][:24], w["x"], w["y"], w["w"], w["h"], w["conf"]))
    if args.click:
        d.q.click(args.click[0], args.click[1], args.fb_w, args.fb_h,
                  hold=args.hold)
        print("[live] clicked %s" % (args.click,), flush=True)
    for k in (args.key or []):
        d.q.key(k, pause=0.4)
        print("[live] key %s" % k, flush=True)
    if args.auto:
        return L.run(q, grab, args.log, ns, [])
    return 0


if __name__ == "__main__":
    sys.exit(main())
