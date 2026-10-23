#!/usr/bin/env python3
"""QMP driver helper for HobbyOS ARM desktop sessions.

Usage:
  qmp_drive.py shot <out.png>
  qmp_drive.py move <x> <y>
  qmp_drive.py click <x> <y> [--btn left|right]
  qmp_drive.py dclick <x> <y>
  qmp_drive.py key <qcode>          # single key tap
  qmp_drive.py keys <string>        # type an ASCII string as qcodes
  qmp_drive.py kbd <qcode> <ms>     # hold for ms

Connects to ./qmp-sock relative to HobbyOS repo root, captures via screendump
to a PPM then converts with the ppm2png helper.
"""
import json
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = os.path.join(REPO, "qmp-sock")
PPM2PNG = os.path.expanduser("~/.hermes/skills/hobbyos/hobbyos-screenshot/scripts/ppm2png.py")

# ASCII -> qcode map for typing
SPECIAL = {
    " ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon", "-": "minus",
    "_": "shift-minus", "@": "shift-2", "?": "shift-slash", "=": "equal",
    "&": "shift-7", "%": "shift-5", "#": "shift-3", "+": "shift-equal",
    ",": "comma", ";": "semicolon", "'": "apostrophe", '"': "shift-apostrophe",
    "\\": "backslash", "~": "shift-grave", "(": "shift-9", ")": "shift-0",
    "|": "shift-backslash", "!": "shift-1", "*": "shift-8", "$": "shift-4",
    "[": "bracket_left", "]": "bracket_right", "`": "grave", "<": "shift-comma",
    ">": "shift-dot", "{": "shift-bracket_left", "}": "shift-bracket_right",
}


def qcode_for(ch):
    if ch.isalnum() and ch.isascii():
        return ch.lower() if not ch.isupper() else "shift-" + ch.lower()
    if ch in SPECIAL:
        return SPECIAL[ch]
    raise SystemExit("no qcode for %r" % ch)


ABS_MAX = 32767


def abs_val(v, extent):
    """QEMU abs pointer axes are in device units (virtio-tablet: 0..32767)."""
    n = int(round(float(v) / float(extent) * ABS_MAX))
    return max(0, min(ABS_MAX, n))


class Qmp:
    def __init__(self, width=1920, height=1080):
        self.width = width
        self.height = height
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(SOCK)
        self.f = self.sock.makefile("rw")
        self.f.readline()  # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("qmp closed")
            obj = json.loads(line)
            if "return" in obj or "error" in obj:
                return obj

    def event(self, events):
        return self.cmd("input-send-event", events=events)

    def move(self, x, y):
        return self.event([
            {"type": "abs", "data": {"axis": "x", "value": abs_val(x, self.width)}},
            {"type": "abs", "data": {"axis": "y", "value": abs_val(y, self.height)}},
        ])

    def btn(self, down, button="left"):
        return self.event([{"type": "btn", "data": {"button": button, "down": down}}])

    def click(self, x, y, button="left"):
        self.move(x, y)
        time.sleep(0.12)
        self.btn(True, button)
        time.sleep(0.08)
        self.btn(False, button)

    def key(self, qcode):
        # QMP takes qcodes as a chord, not "shift-x" strings: a shifted legend
        # is the pair [shift, base].
        keys = qcode.split("-")
        if len(keys) > 1 and keys[0] == "shift":
            keys = ["shift", keys[1]]
        return self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys])

    def keys(self, text, delay=0.05):
        for ch in text:
            self.key(qcode_for(ch))
            time.sleep(delay)

    def shot(self, out):
        ppm = os.path.join(REPO, "_shot.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        r = self.cmd("screendump", filename=ppm)
        if "error" in r:
            raise SystemExit("screendump error: %s" % r)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.1)
        subprocess.run([sys.executable, PPM2PNG, ppm, out], check=True)
        print("shot ->", out)


def main():
    argv = sys.argv[1:]
    q = Qmp()
    cmd = argv[0]
    if cmd == "shot":
        q.shot(argv[1])
    elif cmd == "move":
        print(q.move(argv[1], argv[2]))
    elif cmd == "click":
        q.click(argv[1], argv[2], argv[3] if len(argv) > 3 else "left")
        print("clicked", argv[1], argv[2])
    elif cmd == "dclick":
        q.click(argv[1], argv[2])
        time.sleep(0.08)
        q.btn(True)
        time.sleep(0.05)
        q.btn(False)
        print("dclicked", argv[1], argv[2])
    elif cmd == "key":
        print(q.key(argv[1]))
    elif cmd == "keys":
        q.keys(" ".join(argv[1:]))
        print("typed", " ".join(argv[1:]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
