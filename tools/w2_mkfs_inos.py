#!/usr/bin/env python3
"""w2_mkfs_inos.py — W2-MKFS in-OS smoke: run MKFSFAT.BIN on the real OS.

Boots the lane-assembled minimal disk (tools/assemble_w2_disk.sh) in a
self-managed qemu (PID-tracked, never a broad pkill), opens the desktop
console via the proven Apps-menu path (run_console_test.py pattern), types
`mkfsfat -F 16 -C PROBE.IMG 1024`, then exits.  Evidence: serial log +
framebuffer PPMs in the lane's evidence/ dir; PROBE.IMG is verified from
OUTSIDE afterward with fat16img.py get (BPB + size) — the strongest cheap
proof that the ftruncate-only -C path works when the target file lives on
the OS's own FAT filesystem.

Bounded: exits non-zero and prints a clear reason on any failure.
"""
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EVID = os.path.join(ROOT, "evidence")
os.makedirs(EVID, exist_ok=True)
DISK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "disk-w2-min.img")
SERIAL = os.path.join(EVID, "w2-mkfs-inos-serial.log")
QMP = os.path.join(EVID, "qmp-w2m")
SHOT = EVID
EDK2 = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")
W, H = 1920, 1080
TASKBAR_Y = H - 26

QCODES = {
    " ": "spc", "-": "minus", ".": "dot", "/": "slash", ":": "colon",
    ">": "greater", "<": "less", "=": "equal",
}
QCODES.update({c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"})
QCODES.update({"\n": "ret"})


def qcode_for(ch):
    if ch in QCODES:
        return QCODES[ch]
    return ch


class Q:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(15)
        for _ in range(240):
            try:
                self.s.connect(path)
                break
            except Exception:
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never came up")
        self.s.recv(65536)
        self.call("qmp_capabilities")

    def call(self, ex, args=None):
        m = {"execute": ex}
        if args:
            m["arguments"] = args
        self.s.sendall(json.dumps(m).encode() + b"\n")
        buf = b""
        while True:
            buf += self.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    r = json.loads(part)
                except Exception:
                    continue
                if "return" in r or "error" in r:
                    return r
        # unreachable

    def mouse(self, x, y):
        self.call("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.1)

    def click(self, x, y):
        self.mouse(x, y)
        self.call("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        self.call("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.5)

    def key(self, qcode, pause=0.15):
        self.call("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def type(self, text, pause=0.08):
        for ch in text:
            if ch.isupper():
                self.call("send-key", {"keys": [
                    {"type": "qcode", "data": "shift"},
                    {"type": "qcode", "data": qcode_for(ch.lower())}]})
            else:
                self.key(qcode_for(ch), pause)
            time.sleep(pause)

    def shot(self, name):
        path = os.path.join(SHOT, name)
        self.call("screendump", {"filename": path})
        time.sleep(0.3)
        return path


def log(msg):
    print(f"[INOS] {msg}", flush=True)


def main():
    log(f"disk={DISK}")
    for f in (SERIAL, QMP):
        if os.path.exists(f):
            os.remove(f)
    cmd = [
        "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53", "-smp", "8",
        "-m", "16384M", "-accel", "tcg,thread=multi", "-bios", EDK2,
        "-display", "none", "-serial", f"file:{SERIAL}",
        "-drive", f"if=none,file={DISK},format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-device", "virtio-gpu-device",
        "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56",
        "-semihosting", "-action", "shutdown=poweroff",
        "-qmp", f"unix:{QMP},server,nowait",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def cleanup(_s=None, _f=None):
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        if _s is not None:
            sys.exit(_s)

    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)
    q = None
    try:
        # ---- wait for desktop ----
        t0 = time.time()
        while time.time() - t0 < 180:
            try:
                ser = open(SERIAL, errors="ignore").read()
            except FileNotFoundError:
                ser = ""
            if "Mode: DESKTOP" in ser or "Desktop starting" in ser:
                break
            if proc.poll() is not None:
                log("FAIL: qemu exited early")
                cleanup(2)
            time.sleep(1)
        time.sleep(4)
        log("desktop up")
        q = Q(QMP)
        q.shot("w2m_00_desktop.ppm")

        txt = open(SERIAL, errors="ignore").read()
        entries = dict((int(a), b) for a, b in re.findall(
            r"\[MENU\]\s*(?:\s*\d+\s*=)*\s*(\d+)\s*=\s*([A-Za-z0-9_.]+)", txt))
        if not entries:
            log("FAIL: no [MENU] dump in serial")
            cleanup(2)
        idx = next((i for i, n in entries.items() if n == "CONSOLE.BIN"), None)
        log(f"menu entries={entries}")
        if idx is None:
            log("FAIL: CONSOLE.BIN not in menu")
            cleanup(2)
        log(f"CONSOLE.BIN index={idx}")

        # ---- open the console from the Apps menu ----
        q.click(38, TASKBAR_Y + 13)
        q.key("home", 0.3)
        for _ in range(idx):
            q.key("down", 0.07)
        q.key("ret", 0.3)
        time.sleep(2.0)
        q.shot("w2m_01_console.ppm")

        # ---- type the MKFSFAT command ----
        q.type("mkfsfat -F 16 -C PROBE.IMG 1024")
        q.key("ret", 0.2)
        time.sleep(3.0)
        q.shot("w2m_02_mkfsfat_run.ppm")

        # ---- list the root to see PROBE.IMG ----
        q.type("ls")
        q.key("ret", 0.2)
        time.sleep(2.0)
        q.shot("w2m_03_ls.ppm")

        # ---- write-path control + kernel's own read of PROBE.IMG ----
        q.type("echo hello-from-w2 > T.TXT")
        q.key("ret", 0.2)
        time.sleep(1.0)
        q.type("cat T.TXT")
        q.key("ret", 0.2)
        time.sleep(1.5)
        q.shot("w2m_04_cat_txt.ppm")
        q.type("cat PROBE.IMG")
        q.key("ret", 0.2)
        time.sleep(2.0)
        q.shot("w2m_05_cat_probe.ppm")

        tail = open(SERIAL, errors="ignore").read()[-4000:]
        log("--- serial tail ---")
        log(tail)
        log("--- done ---")
    except Exception as e:
        log(f"FAIL: {e}")
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            pass
        sys.exit(2)
    finally:
        # Graceful QMP quit first (flushes any qemu-side writeback), so a
        # zero-content result cannot be blamed on SIGTERM teardown.
        if q is not None:
            try:
                q.call("quit")
            except Exception:
                pass
        try:
            proc.wait(timeout=20)
        except Exception:
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
        log("qemu stopped")


if __name__ == "__main__":
    main()
