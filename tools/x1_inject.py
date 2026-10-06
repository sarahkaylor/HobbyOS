#!/usr/bin/env python3
# FS-X1 QMP injection helper: calibrated rel-pointer driver for the x64
# PS/2 mouse (mirrors drive_acceptance.py Qmp rel mode).
# Usage: x1_inject.py <qmp-sock> <cmd> [args...]
#   cmds: click x y [button] | move x y | btn button down|up | key qcode... |
#         shot <path> | info | wait <seconds>
import json, socket, sys, time, os

W, H = 1024, 768

class Qmp:
    def __init__(self, path, pointer="rel"):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        deadline = time.time() + 120
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("QMP socket never appeared: %s" % path)
                time.sleep(0.5)
        self.sock.recv(1 << 20)
        self.cmd("qmp_capabilities")
        self.pointer = pointer
        self.px, self.py = W // 2, H // 2
        self.rel_sx, self.rel_sy = 1.25, 0.9375

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        deadline = time.time() + 60
        while True:
            try:
                chunk = self.sock.recv(1 << 20)
            except socket.timeout:
                if time.time() > deadline:
                    raise RuntimeError("QMP timeout waiting for %s" % execute)
                continue
            if not chunk:
                raise RuntimeError("QMP socket closed during %s" % execute)
            buf += chunk
            text = buf.decode(errors="ignore")
            parts = text.strip().split("\n")
            if not text.endswith("\n"):
                parts = parts[:-1]
            for part in parts:
                part = part.strip()
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
            buf = b""

    def _rel_delta(self, dx, dy):
        for axis, value in (("x", dx), ("y", dy)):
            rem = value
            while abs(rem) > 0.001:
                step = max(min(rem, 250), -250)
                self.cmd("input-send-event", {"events": [
                    {"type": "rel", "data": {"axis": axis, "value": int(step)}}]})
                rem -= step

    def move(self, x, y):
        dx = (x - self.px) / self.rel_sx
        dy = (y - self.py) / self.rel_sy
        self.px, self.py = x, y
        self._rel_delta(dx, dy)
        time.sleep(0.2)

    def click(self, x, y, button="left", settle=0.4):
        self.move(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.1)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": False}}]})
        print("clicked %s at %d,%d" % (button, x, y), flush=True)
        time.sleep(settle)

    def key(self, codes, pause=0.4):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": c} for c in codes]})
        time.sleep(pause)

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path, "format": "png"})
        print("shot -> %s" % path, flush=True)
        return r

def main():
    q = Qmp(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "click":
        q.click(int(sys.argv[3]), int(sys.argv[4]), sys.argv[5] if len(sys.argv) > 5 else "left")
    elif cmd == "move":
        q.move(int(sys.argv[3]), int(sys.argv[4]))
    elif cmd == "btn":
        q.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": sys.argv[3], "down": sys.argv[4] == "down"}}]})
        time.sleep(0.15)
    elif cmd == "key":
        q.key(sys.argv[3:])
    elif cmd == "shot":
        q.shot(sys.argv[3])
    elif cmd == "wait":
        time.sleep(float(sys.argv[3]))
    else:
        print("unknown cmd %s" % cmd)

if __name__ == "__main__":
    main()
