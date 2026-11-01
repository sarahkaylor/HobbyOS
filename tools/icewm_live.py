#!/usr/bin/env python3
"""Interactive control for a live headless HobbyOS IceWM session.

The desktop runs forever by design, so this keeps QEMU up across shell calls:

  python3 tools/icewm_live.py start          # boot DESKTOP=icewm (intel/KVM)
  python3 tools/icewm_live.py shot NAME      # QMP screendump -> PNG
  python3 tools/icewm_live.py click X Y      # absolute pointer click
  python3 tools/icewm_live.py move X Y
  python3 tools/icewm_live.py key ret        # one qcode
  python3 tools/icewm_live.py type "echo hi" # a string
  python3 tools/icewm_live.py serial [-f]    # dump (or follow) the serial log
  python3 tools/icewm_live.py stop

State (sock, pid, serial log, shots) lives in $ICEWM_LIVE_DIR
(default /tmp/icewm-live).
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 1920, 1080
STATE = os.environ.get("ICEWM_LIVE_DIR", "/tmp/icewm-live")
QMP = os.path.join(STATE, "qmp-sock")
SERIAL = os.path.join(STATE, "serial.log")
PIDF = os.path.join(STATE, "qemu.pid")
SHOTS = os.path.join(STATE, "shots")
CONVERT = os.path.join(REPO, "skills/hobbyos-screenshot/scripts/ppm2png.py")


def qemu_cmd(disk):
    fw = os.path.join(STATE, "ovmf.fd")
    cmd = ["qemu-system-x86_64", "-M", "q35", "-smp", "4", "-m", "8192M",
           "-drive", f"if=pflash,format=raw,file={fw}",
           "-display", "none", "-serial", f"file:{SERIAL}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", f"file={disk},format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-device", "virtio-gpu-pci", "-vga", "none",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{QMP},server,nowait"]
    if os.path.exists("/dev/kvm") and not os.environ.get("SKIP_KVM"):
        cmd.append("-enable-kvm")
    return cmd


def cmd_start():
    os.makedirs(SHOTS, exist_ok=True)
    for stale in (QMP, SERIAL, PIDF):
        try:
            os.unlink(stale)
        except OSError:
            pass
    disk = os.path.join(STATE, "disk.img")
    shutil.copy(os.path.join(REPO, "disk.img"), disk)
    shutil.copy(os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
                os.path.join(STATE, "ovmf.fd"))
    proc = subprocess.Popen(qemu_cmd(disk), stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(STATE, "qemu.err"), "w"),
                            start_new_session=True)
    open(PIDF, "w").write(str(proc.pid))
    print(f"qemu pid={proc.pid}")


class Qmp:
    def __init__(self, path, deadline_s=60):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(20)
        end = time.time() + deadline_s
        while time.time() < end:
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.3)
        else:
            raise RuntimeError("QMP socket never appeared")
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

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})

    def button(self, down):
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}}]})

    def click(self, x, y, settle=0.35):
        self.move(x, y)
        time.sleep(0.15)
        self.button(True)
        time.sleep(0.09)
        self.button(False)
        time.sleep(settle)

    def key(self, q):
        keys = [{"type": "qcode", "data": k} for k in q.split("+")]
        self.cmd("send-key", {"keys": keys})
        time.sleep(0.12)

    def drag(self, x1, y1, x2, y2, steps=12, settle=0.4):
        self.move(x1, y1)
        time.sleep(0.2)
        self.button(True)
        time.sleep(0.2)
        for i in range(1, steps + 1):
            self.move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
            time.sleep(0.06)
        time.sleep(0.2)
        self.button(False)
        time.sleep(settle)

    def shot(self, name):
        ppm = os.path.join(SHOTS, name + ".ppm")
        r = self.cmd("screendump", {"filename": ppm})
        time.sleep(0.6)
        if "error" in r:
            raise RuntimeError(f"screendump failed: {r}")
        png = os.path.join(SHOTS, name + ".png")
        subprocess.run([sys.executable, CONVERT, ppm, png], check=True)
        print(png)


KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal",
    ".": "dot", ",": "comma", "/": "slash", ";": "semicolon", "'": "apostrophe",
    "[": "bracket_left", "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
    ":": "shift+semicolon", "_": "shift+minus", '"': "shift+apostrophe",
    "!": "shift+1", "@": "shift+2", "#": "shift+3", "$": "shift+4", "%": "shift+5",
    "^": "shift+6", "&": "shift+7", "*": "shift+8", "(": "shift+9", ")": "shift+0",
    "?": "shift+slash", ">": "shift+dot", "<": "shift+comma", "+": "shift+equal",
    "|": "shift+backslash", "{": "shift+bracket_left", "}": "shift+bracket_right",
    "~": "shift+grave_accent",
}


def type_text(q, text):
    for ch in text:
        if ch in KEYMAP:
            q.key(KEYMAP[ch])
        elif ch.isupper():
            q.key("shift+" + ch.lower())
        else:
            q.key(ch)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    op = sys.argv[1]
    if op == "start":
        cmd_start()
        return 0
    if op == "stop":
        try:
            pid = int(open(PIDF).read().strip())
            os.kill(pid, 15)
            print(f"terminated {pid}")
        except (OSError, ValueError) as e:
            print(f"no pid: {e}")
        return 0
    if op == "serial":
        if "-f" in sys.argv:
            subprocess.run(["tail", "-f", SERIAL])
        else:
            sys.stdout.write(open(SERIAL, errors="replace").read()[-6000:])
        return 0
    q = Qmp(QMP)
    if op == "shot":
        q.shot(sys.argv[2] if len(sys.argv) > 2 else "shot")
    elif op == "click":
        q.click(int(sys.argv[2]), int(sys.argv[3]))
        print(f"clicked {sys.argv[2]},{sys.argv[3]}")
    elif op == "move":
        q.move(int(sys.argv[2]), int(sys.argv[3]))
        print(f"moved {sys.argv[2]},{sys.argv[3]}")
    elif op == "drag":
        q.drag(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
        print(f"dragged {sys.argv[2]},{sys.argv[3]} -> {sys.argv[4]},{sys.argv[5]}")
    elif op == "key":
        q.key(sys.argv[2])
        print(f"key {sys.argv[2]}")
    elif op == "type":
        type_text(q, sys.argv[2])
        print("typed")
    elif op == "raw":
        print(q.cmd(sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else None))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
