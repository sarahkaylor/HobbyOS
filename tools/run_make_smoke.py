#!/usr/bin/env python3
"""In-OS smoke test for the GNU make 4.4.1 port (lane w2-make).

Boots the ARM disk image, opens a desktop console (CONSOLE.BIN -> SH.BIN)
via the Apps menu, types `make --version` twice -- once on screen and once
redirected to a file -- and verifies the file's bytes are the GNU Make
git-4.4.1 banner (the acceptance.md S5.4 file-based evidence pattern; the
screen is framebuffer-only so the file is the hard assertion).

Usage:
    python3 tools/run_make_smoke.py [disk.img]
        disk.img  default: disk.img in the repo root

Prereqs: a built disk.img that carries MAKE.BIN (the disk recipe stages it
as ::/MAKE.BIN) and the AAVMF firmware at ~/.local/share/AAVMF/AAVMF_CODE.fd.
QEMU's -serial is a file; the Apps menu indexes come from the serial
[MENU] dump, exactly like run_console_test.py.

Verdict rule (acceptance.md S8.4): marker substrings + evidence, not exit
codes.  Exits 0 iff the extracted file contains "GNU Make 4.4.1".
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

W, H = 1920, 1080
TASKBAR_Y = H - 26
SERIAL = "/tmp/make_smoke_serial.log"
QMP = "/tmp/make-smoke-qmp.sock"
SHOT = "/tmp/make_smoke_shot.ppm"
VERFILE = "VERSION.TXT"
CTLFILE = "CTL.TXT"
QEMU = "qemu-system-aarch64"
EDK2 = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")
IMG = sys.argv[1] if len(sys.argv) > 1 else "disk.img"

fails = []


def log(msg):
    print("[SMOKE] " + msg, flush=True)


class Q:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX)
        s.s.settimeout(15)
        for _ in range(200):
            try:
                s.s.connect(p)
                break
            except Exception:
                time.sleep(0.5)
        s.s.recv(65536)
        s.c("qmp_capabilities")

    def c(s, ex, args=None):
        m = {"execute": ex}
        if args:
            m["arguments"] = args
        s.s.sendall(json.dumps(m).encode() + b"\n")
        buf = b""
        while True:
            buf += s.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    r = json.loads(part)
                except Exception:
                    continue
                if "return" in r or "error" in r:
                    return r

    def mv(s, x, y):
        s.c("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.1)

    def click(s, x, y):
        s.mv(x, y)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)

    def key(s, q, pause=0.15):
        s.c("send-key", {"keys": [{"type": "qcode", "data": q}]})
        time.sleep(pause)

    def type_text(s, text):
        # Lowercase shell input: the shell uppercases bare command names and
        # everything here is lowercase; hyphens are the "minus" qcode.
        qcodes = {"-": "minus", " ": "spc", ".": "dot", "/": "slash"}
        for ch in text:
            s.key(qcodes.get(ch, ch), 0.09)
        time.sleep(0.2)

    def shot(s, path):
        s.c("screendump", {"filename": path})
        time.sleep(0.4)


def main():
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
    for f in (SERIAL, QMP):
        if os.path.exists(f):
            os.remove(f)
    serial = open(SERIAL, "w")
    qemu = subprocess.Popen([
        QEMU, "-M", "virt", "-cpu", "cortex-a53", "-smp", "8", "-m", "16384M",
        "-accel", "tcg,thread=multi", "-bios", EDK2,
        "-display", "none", "-serial", "file:" + SERIAL,
        "-drive", "if=none,file=" + IMG + ",format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-device", "virtio-gpu-device",
        "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56",
        "-semihosting", "-action", "shutdown=poweroff",
        "-qmp", "unix:" + QMP + ",server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    q = Q(QMP)
    t0 = time.time()
    while time.time() - t0 < 120:
        txt = open(SERIAL, errors="ignore").read()
        if "Desktop starting" in txt:
            break
        time.sleep(0.5)
    time.sleep(3)
    txt = open(SERIAL, errors="ignore").read()

    entries = dict((int(a), b) for a, b in re.findall(
        r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)(?:\[CONSOLE\]\s*)*="
        r"(?:\[CONSOLE\]\s*)*([A-Za-z0-9_.]+)", txt))
    idx = next((i for i, n in entries.items() if n == "CONSOLE.BIN"), None)
    log("CONSOLE.BIN menu index: %s (menu size %d)" % (idx, len(entries)))
    if idx is None:
        fails.append("no CONSOLE.BIN in the serial [MENU] dump")
        log("serial tail:\n" + txt[-1500:])
        qemu.kill()
        return 1

    # Apps menu -> console.
    base = q.shot("/tmp/make_smoke_baseline.ppm")
    q.click(38, TASKBAR_Y + 13)
    time.sleep(0.8)
    q.key("home", 0.3)
    for _ in range(idx):
        q.key("down", 0.06)
    q.key("ret", 0.3)
    time.sleep(2.0)

    # Fire 1: make --version straight to the screen (framebuffer evidence).
    q.type_text("make --version")
    q.key("ret", 0.3)
    time.sleep(3.0)
    q.shot("/tmp/make_smoke_shot_v1.ppm")

    # Redirection control: prove `>` fills a file for an EXTERNAL command
    # (cat, not a builtin — builtins may write to the console fd directly),
    # separating a shell-redirect bug from a make-write bug.
    q.type_text("cat HELLO.BIN > " + CTLFILE)
    q.key("ret", 0.3)
    time.sleep(2.0)

    # Fire 2: redirect to a file for the hard byte assertion (S5.4 pattern).
    q.type_text("make --version > " + VERFILE)
    q.key("ret", 0.3)
    time.sleep(4.0)
    q.shot("/tmp/make_smoke_shot_v2.ppm")

    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)

    # Redirection control file: must contain the control text.
    try:
        ctl = subprocess.run(["python3", "fat16img.py", "get", IMG, CTLFILE],
                             capture_output=True, text=True, timeout=60)
    except Exception as e:
        fails.append("fat16img control extraction failed: %s" % e)
        return 1
    log("CTL extracted: %d bytes; HELLO.BIN is 34320 bytes"
        % len(ctl.stdout))
    if len(ctl.stdout) != 34320:
        fails.append("redirection control (cat HELLO.BIN) wrote %d bytes, expected 34320"
                     % len(ctl.stdout))

    # Extract the redirected output and check for the banner.
    try:
        out = subprocess.run(["python3", "fat16img.py", "get", IMG, VERFILE],
                             capture_output=True, text=True, timeout=60)
    except Exception as e:
        fails.append("fat16img extraction failed: %s" % e)
        return 1
    data = out.stdout
    ok = "GNU Make 4.4.1" in data
    log("VERSION.TXT extracted: %d bytes; banner match: %s" % (len(data), ok))
    if not ok:
        fails.append("VERSION.TXT missing 'GNU Make 4.4.1'; got: %r" % data[:400])
    log("screenshots saved to /tmp/make_smoke_shot_v1.ppm, v2.ppm")
    if fails:
        log("SMOKE FAILED (%d): %s" % (len(fails), fails))
        return 1
    log("SMOKE PASSED: MAKE.BIN printed GNU Make 4.4.1 in-OS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
