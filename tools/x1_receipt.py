#!/usr/bin/env python3
# FS-X1 receipt driver (v2): click-launch + click-nav, staged small moves.
# Logs every step; every QMP call bounded.
import json, socket, sys, time, os, traceback

W, H = 1024, 768
SX, SY = 1.25, 0.9375

def log(*a):
    print("[REC]", *a, flush=True)

class Q:
    def __init__(self, sock):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(5)
        deadline = time.time() + 120
        while True:
            try:
                self.s.connect(sock)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("qmp never appeared")
                time.sleep(0.5)
        self.s.recv(1 << 20)
        self.cmd("qmp_capabilities")
        self.px, self.py = W // 2, H // 2
    def cmd(self, e, a=None, sl=0.05):
        m = {"execute": e}
        if a is not None:
            m["arguments"] = a
        self.s.sendall(json.dumps(m).encode() + b"\n")
        end = time.time() + 10
        buf = b""
        while time.time() < end:
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if b"return" in buf or b"error" in buf:
                break
        time.sleep(sl)
    def smove_to(self, x, y):
        dx = (x - self.px) / SX
        dy = (y - self.py) / SY
        done_x = done_y = 0.0
        guard = 0
        while (abs(done_x) < abs(dx) or abs(done_y) < abs(dy)) and guard < 400:
            guard += 1
            rx = min(dx - done_x, 60) if dx > 0 else max(dx - done_x, -60)
            ry = min(dy - done_y, 60) if dy > 0 else max(dy - done_y, -60)
            if abs(dx - done_x) > 0.01:
                self.cmd("input-send-event", {"events": [
                    {"type": "rel", "data": {"axis": "x", "value": int(rx)}}]})
                done_x += int(rx)
            if abs(dy - done_y) > 0.01:
                self.cmd("input-send-event", {"events": [
                    {"type": "rel", "data": {"axis": "y", "value": int(ry)}}]})
                done_y += int(ry)
            time.sleep(0.04)
        self.px, self.py = x, y
        time.sleep(0.25)
    def click(self, x, y):
        log("click", x, y)
        self.smove_to(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.12)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.3)
    def key(self, codes, pause=0.35):
        log("key", codes)
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": c} for c in codes]})
        time.sleep(pause)
    def shot(self, p):
        self.cmd("screendump", {"filename": p, "format": "png"}, sl=0.4)
        log("shot", p)

q = Q(sys.argv[1])
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)

try:
    q.shot(f"{OUT}/r00-fresh-desktop.png")
    q.click(38, 755)          # Apps button
    q.shot(f"{OUT}/r01-apps-click.png")
    q.click(110, 448)         # BROWSER menu item (idx 1)
    time.sleep(2)
    q.shot(f"{OUT}/r02-after-browser-click.png")
    q.key(["ret"])            # belt-and-braces: launch selected item
    time.sleep(3)
    q.shot(f"{OUT}/r03-window-up.png")
    q.click(512, 400)         # click inside the browser window
    q.shot(f"{OUT}/r04-click-in-window.png")
    log("ALL STEPS DONE")
except Exception:
    log("ERROR:", traceback.format_exc())
    sys.exit(1)
