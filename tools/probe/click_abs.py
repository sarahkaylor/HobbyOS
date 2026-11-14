#!/usr/bin/env python3
"""click_abs.py - absolute-pointer clicking + screendumps over QMP.

    click_abs.py <qmp-sock> click <x> <y> [shot.png]     click at guest pixels
    click_abs.py <qmp-sock> shot  <out.png>
    click_abs.py <qmp-sock> key   <qcode> [qcode...]

x1_inject.py drives a relative pointer calibrated for 1024x768; this uses the
virtio-tablet's EV_ABS path instead, so one event lands on an exact pixel at any
resolution (HobbyOS handles EV_ABS in src/kernel/virtio_input.c).
"""
import json
import socket
import sys
import time

W, H = 1920, 1080
ABS_MAX = 32767


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(30)
        self.s.connect(path)
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        buf = b""
        while True:
            buf += self.s.recv(1 << 20)
            text = buf.decode(errors="ignore")
            if not text.endswith("\n"):
                continue
            for line in text.strip().split("\n"):
                line = line.strip()
                if not line:
                    continue
                try:
                    return json.loads(line)
                except Exception:
                    pass

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.s.sendall((json.dumps(msg) + "\n").encode())
        while True:
            r = self._read()
            if "return" in r or "error" in r:
                return r

    def click(self, x, y, button="left"):
        ax = int(x * ABS_MAX / (W - 1))
        ay = int(y * ABS_MAX / (H - 1))
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]})
        time.sleep(0.25)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.12)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": False}}]})
        time.sleep(0.5)
        print(f"clicked {button} at {x},{y}", flush=True)


def main():
    q = Qmp(sys.argv[1])
    verb = sys.argv[2]
    if verb == "click":
        q.click(int(sys.argv[3]), int(sys.argv[4]))
        if len(sys.argv) > 5:
            q.cmd("screendump", {"filename": sys.argv[5], "format": "png"})
            print("shot ->", sys.argv[5], flush=True)
    elif verb == "shot":
        q.cmd("screendump", {"filename": sys.argv[3], "format": "png"})
        print("shot ->", sys.argv[3], flush=True)
    elif verb == "key":
        q.cmd("send-key", {"keys": [{"type": "qcode", "data": c} for c in sys.argv[3:]]})
        time.sleep(0.4)
        print("keys sent", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
