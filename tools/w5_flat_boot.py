#!/usr/bin/env python3
"""w5_flat_boot.py — lane w5-flat: boot a MODE=selfhost disk.img and capture
the serial evidence of the in-OS /TOOLS run.

The MODE=selfhost kernel (KERNEL_MODE_SELFHOST, w4-kernel) boots ONLY
SELFHOST.BIN with argv[1]=/SRC/SELFHOST.RUN and relays the driver's stdout
to the serial console — no desktop, no QMP keystrokes.  The stock harness
(run_selfhost_build_test.py) assumes a desktop Apps-menu launch, so this
lane helper boots the same QEMU line and waits for terminal [SELFHOST]
markers directly.

Usage:
  python3 tools/w5_flat_boot.py [--image disk.img] [--serial serial-w5.log]
      [--timeout 420] [--script-copy FILE] [--restore-script FILE]
  --script-copy FILE   mcopy FILE over ::/SRC/SELFHOST.RUN before boot
                       (lane probe script; the pipeline's real script is
                       preserved at ::/SRC/SELFHOST.RUN and restored unless
                       --restore-script is given)
  --restore-script FILE mcopy FILE back to ::/SRC/SELFHOST.RUN after boot
  --expect TEXT        marker that defines a clean PASS (default
                       ACCEPTANCE-COMPLETE)
  --fail-on TEXT       marker that defines an early terminal FAIL (default
                       [SELFHOST] ABORT); also any phase=<x>-fail
Exit 0 on the expect-marker, 1 on fail-marker, 2 on timeout, 3 on error.
The full capture is left at --serial for evidence extraction.
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import run_selfhost_build_test as H  # noqa: E402  (reuse boot/stop/read machinery)


def mcopy(src, image, dst):
    r = subprocess.run(["mcopy", "-o", "-i", image, src, dst],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("mcopy %s -> %s failed: %s" % (src, dst, r.stderr.strip()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="disk.img")
    ap.add_argument("--serial", default="./serial-w5.log")
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--script-copy", default=None)
    ap.add_argument("--restore-script", default=None)
    ap.add_argument("--expect", default="ACCEPTANCE-COMPLETE")
    ap.add_argument("--fail-on", default="[SELFHOST] ABORT")
    args = ap.parse_args()

    img = os.path.abspath(args.image)
    if not os.path.exists(img):
        raise SystemExit("image not found: %s" % img)
    if args.script_copy:
        mcopy(args.script_copy, img, "::/SRC/SELFHOST.RUN")
        print("[w5boot] staged script %s -> ::/SRC/SELFHOST.RUN" % args.script_copy)

    def text():
        return H.read_serial(args.serial)

    q = H.boot(img, args.serial, qmp=None)
    try:
        t0 = time.time()
        if H.wait_for(args.serial, H.BOOT_MARKER, min(120, args.timeout)) is None:
            print("[w5boot] boot marker '%s' NOT seen" % H.BOOT_MARKER)
            return 3
        print("[w5boot] booted (%.1fs)" % (time.time() - t0))
        deadline = time.time() + args.timeout
        outcome = 2  # timeout
        while time.time() < deadline:
            t = text()
            if args.fail_on in t or re.search(r"phase=[A-Za-z0-9_-]+-fail", t):
                outcome = 1
                break
            if args.expect in t:
                outcome = 0
                break
            time.sleep(1)
        elapsed = time.time() - t0
        print("[w5boot] outcome=%d (0=pass,1=fail,2=timeout) elapsed=%.1fs"
              % (outcome, elapsed))
        print("[w5boot] capture: %s" % args.serial)
        return outcome
    finally:
        H.stop_qemu(q)
        if args.restore_script:
            mcopy(args.restore_script, img, "::/SRC/SELFHOST.RUN")
            print("[w5boot] restored %s -> ::/SRC/SELFHOST.RUN" % args.restore_script)


if __name__ == "__main__":
    sys.exit(main())
