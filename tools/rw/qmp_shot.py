#!/usr/bin/env python3
"""qmp_shot.py -- take a screendump of a live HobbyOS QEMU via QMP.

The host-agnostic counterpart of the harness's Linux helpers: no `screendump`
shell-out, no X11, no egl-headless requirement, so it works on a macOS host
(and on any QEMU whose display is `none`, which is how a headless macOS run
is booted: `QEMU_DISPLAY=none`).

Usage:
  qmp_shot.py --qmp /tmp/hb-qmp.sock --out shot.ppm [--wait 300]
              [--interval 20] [--until-marker serial.log --marker "Desktop"]

  --wait N        total seconds to wait before the shot (default 60)
  --interval N    re-issue the dump every N seconds (last one wins); useful on
                  a slow TCG host where the first paint is minutes away
  --out PATH      output .ppm (QEMU writes PPM; convert with sips/ffmpeg)
Exit: 0 a non-blank dump was written, 1 never got one, 2 environment.
"""
import argparse
import os
import socket
import sys
import time


class QMP:
    def __init__(self, path, timeout=30.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.f = self.s.makefile("rwb", buffering=0)
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.f.readline()
        if not line:
            raise RuntimeError("qmp: eof")
        import json
        return json.loads(line.decode())

    def cmd(self, name, **args):
        import json
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        while True:
            r = self._read()
            if "event" in r:
                continue
            if "error" in r:
                raise RuntimeError("qmp %s: %s" % (name, r["error"]))
            return r.get("return")

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})


def shot_is_blank(path):
    """PPM P6: true when every pixel is one value (an unpainted framebuffer)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
        if not data.startswith(b"P6"):
            return True
        # header: P6 <w> <h> <maxval>\n
        parts, idx = [], 2
        while len(parts) < 3:
            while idx < len(data) and data[idx:idx + 1].isspace():
                idx += 1
            if data[idx:idx + 1] == b"#":
                while data[idx:idx + 1] != b"\n":
                    idx += 1
                continue
            start = idx
            while idx < len(data) and not data[idx:idx + 1].isspace():
                idx += 1
            parts.append(int(data[start:idx]))
        px = data[idx + 1:]
        return len(set(px[i:i + 3] for i in range(0, min(len(px), 3 * 4096), 3))) <= 1
    except Exception:
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--wait", type=int, default=60)
    ap.add_argument("--interval", type=int, default=0)
    ap.add_argument("--until-marker")
    ap.add_argument("--marker")
    a = ap.parse_args()

    deadline = time.time() + a.wait
    while True:
        if a.marker and a.until_marker:
            try:
                with open(a.until_marker, "r", errors="replace") as f:
                    if a.marker in f.read():
                        break
            except OSError:
                pass
        if time.time() >= deadline:
            break
        time.sleep(2)

    try:
        q = QMP(a.qmp)
    except Exception as e:
        print("qmp_shot: cannot connect: %s" % e, file=sys.stderr)
        return 2

    attempts = 0
    while True:
        attempts += 1
        try:
            q.cmd("screendump", filename=os.path.abspath(a.out))
        except Exception as e:
            print("qmp_shot: screendump failed: %s" % e, file=sys.stderr)
            return 2
        time.sleep(1)
        blank = shot_is_blank(a.out)
        print("qmp_shot: attempt %d -> %s (%d bytes, blank=%s)"
              % (attempts, a.out, os.path.getsize(a.out) if os.path.exists(a.out) else -1, blank))
        if not blank:
            return 0
        if not a.interval or time.time() + a.interval > deadline + a.interval:
            return 1 if blank else 0
        time.sleep(a.interval)


if __name__ == "__main__":
    sys.exit(main())
