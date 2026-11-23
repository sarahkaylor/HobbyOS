#!/usr/bin/env python3
"""tools/nv/present_gate.py -- the N0 (PLAN.md work package E) present gate.

What it measures, and why it is not self-reporting:

  The guest boots MODE=fb_test, whose only program is FBPROBE.BIN: it opens
  /dev/fb0, mmap()s the full 1920x1080 B8G8R8A8 frame (8,294,400 B), paints
  a flat fill, a magenta 64x64 bottom-right block and ONE pure-red pixel at
  (1919,1079), then flush_fb()s.  After the guest stops painting, this script
  asks QEMU (QMP `screendump`) for the HOST's view of the scanout as a PPM and
  reads the pixel bytes out of that file.

  That is the difference between this and the probe's own printf: the dump is
  produced by QEMU from the device state, so it can disagree with the guest
  (wrong framebuffer mapped, flush never landed, mapping clamped short).  The
  script prints the exact byte values it read and fails when any of them
  differs - a wrong picture cannot pass.

Checks (all on the PPM, no guest output involved):

  * dump dimensions are 1920x1080 (the mode the probe asked for);
  * pixel (1919,1079) == the probe's corner colour, RGB(255,0,0);
  * pixel (1918,1079) and (1918,1078) == the magenta block, RGB(255,0,255)
    (so the corner pixel is inside the block it was drawn in, not a stray);
  * pixel (0,0) and (960,540) == the fill colour, RGB(20,48,64) 0x14,0x30,0x40;
  * the frame is NOT uniform (a black/blank scanout fails the fill row).

Usage (in the built tree, on the build host):

    python3 tools/nv/present_gate.py [--serial obj/wp-e-present.serial.log]
                                     [--shot /tmp/wp-e-n0.ppm]
                                     [--timeout 300] [--keep]

Exit status 0 = gate passed; 1 = gate failed; 2 = the guest never got far
enough to be measured (a setup failure, not a pass either).
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W, H = 1920, 1080
FILL = (32, 48, 64)      # 0xFF203040 word -> memory bytes B,G,R = 40,30,20 hex
BLOCK = (255, 0, 255)    # magenta
CORNER = (255, 0, 0)     # pure red, the gate pixel


def log(msg):
    print("[N0] " + msg, flush=True)


# ------------------------------------------------------------------ PPM ---
def read_ppm(path):
    """Read a binary P6 PPM into (w, h, bytearray of RGB triples)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("not a P6 PPM: " + path)
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace byte after maxval
    w, h, maxv = fields
    if maxv != 255:
        raise SystemExit("PPM maxval %d is not 255" % maxv)
    pix = data[pos : pos + w * h * 3]
    if len(pix) != w * h * 3:
        raise SystemExit("PPM truncated: %d of %d bytes" % (len(pix), w * h * 3))
    return w, h, pix


def px(pix, w, x, y):
    i = (y * w + x) * 3
    return (pix[i], pix[i + 1], pix[i + 2])


# ------------------------------------------------------------------ QMP ---
class Qmp:
    def __init__(self, path, deadline_s=60):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(30)
        end = time.time() + deadline_s
        while True:
            try:
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise RuntimeError("QMP socket never appeared: " + path)
                time.sleep(0.2)
        self.s.recv(65536)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp


# ----------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default=os.path.join(ROOT, "obj", "wp-e-present.serial.log"))
    ap.add_argument("--shot", default="/tmp/wp-e-n0.ppm")
    ap.add_argument("--qmp", default="/tmp/wp-e-n0-qmp.sock")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    ap.add_argument("--mode", default="fb_test",
                    choices=("fb_test", "fb_compat_test"),
                    help="fb_test = the native probe + pixel gate; "
                         "fb_compat_test = the Linux-ABI (musl) probe, judged "
                         "on its own serial row")
    args = ap.parse_args()

    for p in (args.qmp,):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    os.makedirs(os.path.dirname(args.serial) or ".", exist_ok=True)
    serial = open(args.serial, "wb")
    qemu_args = "-display none -qmp unix:%s,server=on,wait=off -no-reboot" % args.qmp
    log("booting MODE=%s (QEMU_ARGS=%r)" % (args.mode, qemu_args))
    qemu = subprocess.Popen(
        ["make", "TOOLS_SKIP=1", "BROWSER_SKIP=1", "ARCH=intel",
         "MODE=" + args.mode, "LO_ON_DISK=", "run", "QEMU_ARGS=" + qemu_args],
        cwd=ROOT, stdout=serial, stderr=subprocess.STDOUT)

    if args.mode == "fb_compat_test":
        pass_marks = ("sdl test app: fb /dev/fb0 fd=",)
        fail_marks = ("sdl test app: fb unavailable",)
    else:
        pass_marks = ("[FBP] RESULT PASS past_frame",)
        fail_marks = ("[FBP] RESULT FAIL mmap", "[FBP] RESULT FAIL readback",
                      "[FBP] RESULT FAIL past_frame")

    verdict = 2
    try:
        q = Qmp(args.qmp, deadline_s=90)
        end = time.time() + args.timeout
        passed = failed = False
        text = ""
        while time.time() < end:
            if qemu.poll() is not None and qemu.returncode != 0:
                log("QEMU exited rc=%s before the probe reported" % qemu.returncode)
                break
            with open(args.serial, "rb") as f:
                text = f.read().decode(errors="ignore")
            if any(m in text for m in fail_marks):
                failed = True
                break
            if any(m in text for m in pass_marks):
                passed = True
                break
            time.sleep(0.5)
        for line in text.splitlines():
            if "fb" in line and ("sdl test app" in line or "[FBP]" in line):
                log("serial: " + line.strip())
        if not passed and not failed:
            log("guest never reported: no probe row within %ds" % args.timeout)
        if failed:
            log("guest reported a FAIL row; dumping anyway for the record")

        # The probe paints, flushes, then holds.  Give the flush a moment to
        # reach the device, then take the host's picture of the scanout.
        time.sleep(1.5)
        for old in (args.shot,):
            try:
                os.unlink(old)
            except FileNotFoundError:
                pass
        r = q.cmd("screendump", {"filename": args.shot})
        if "error" in r:
            log("screendump refused: %r" % (r["error"],))
        else:
            log("screendump -> " + args.shot)

        w, h, pix = read_ppm(args.shot)
        log("dump %dx%d (%d bytes)" % (w, h, len(pix)))

        if args.mode == "fb_compat_test":
            # The musl probe's own row is the verdict for this mode; the dump
            # is taken so the frame it painted is on record.
            log("compat probe row: %s" % ("PASS" if passed else "FAIL"))
            log("GATE %s" % ("PASS" if passed else "FAIL"))
            verdict = 0 if passed else 1
        else:
            for name, (x, y), want in (
                    ("corner (1919,1079)", (1919, 1079), CORNER),
                    ("block  (1918,1079)", (1918, 1079), BLOCK),
                    ("block  (1918,1078)", (1918, 1078), BLOCK),
                    ("fill   (0,0)", (0, 0), FILL),
                    ("fill   (960,540)", (960, 540), FILL)):
                got = px(pix, w, x, y)
                ok = got == want
                log("%-19s = %-15r want %-15r %s" % (name, got, want, "OK" if ok else "MISMATCH"))
                if not ok:
                    verdict = 1
            if (w, h) != (W, H):
                log("FAIL: dump is %dx%d, expected %dx%d" % (w, h, W, H))
                verdict = 1
            elif verdict == 2:
                verdict = 0
            if not passed:
                log("FAIL: the guest's own [FBP] rows did not report PASS")
                verdict = 1
            log("GATE %s" % ("PASS" if verdict == 0 else "FAIL"))
    finally:
        if not args.keep:
            qemu.terminate()
            try:
                qemu.wait(timeout=15)
            except subprocess.TimeoutExpired:
                qemu.kill()
        serial.close()
    return verdict


if __name__ == "__main__":
    sys.exit(main())
