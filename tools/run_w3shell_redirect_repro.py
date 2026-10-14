#!/usr/bin/env python3
"""Deterministic in-OS repro for BUG A (sh external redirect) v2.

Boots the ARM disk image in DESKTOP mode (no test wave -> no slot
pressure -> deterministic), opens CONSOLE.BIN (SH.BIN with normal pipes)
from the Apps menu, types the redirect command SLOWLY, SCREENSHOTS the
console after each command (so the transcript state is verifiable), then
extracts CTL.TXT / VERSION.TXT from disk.img with fat16img.py and
byte-compares.  The screenshots tell us whether the command line was
received intact and whether the shell returned to a prompt (vs. a cat with
an empty argv sitting on stdin, eating subsequent keystrokes).
"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

W, H = 1920, 1080
TASKBAR_Y = H - 26
SERIAL = "/tmp/w3shell_serial.log"
QMP = "/tmp/w3shell-qmp.sock"
QEMU = "qemu-system-aarch64"
EDK2 = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")
IMG = sys.argv[1] if len(sys.argv) > 1 else "disk.img"

CTL, VER = "CTL.TXT", "VERSION.TXT"
fails = []


def log(msg):
    print("[W3SHELL] " + msg, flush=True)


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
        time.sleep(0.15)

    def click(s, x, y):
        s.mv(x, y)
        s.c("input-send-event", {"events": [{"type": "btn",
             "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn",
             "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)

    def key(s, q, pause=0.3, shift=False):
        keys = [{"type": "qcode", "data": q}]
        if shift:
            keys.insert(0, {"type": "qcode", "data": "shift"})
        s.c("send-key", {"keys": keys})
        time.sleep(pause)

    def type_text(s, text, pause=0.18):
        qcodes = {"-": "minus", " ": "spc", ".": "dot", "/": "slash"}
        for ch in text:
            if ch == ">":
                s.key("dot", pause=pause, shift=True)
            elif ch == "$":
                s.key("4", pause=pause, shift=True)
            else:
                s.key(qcodes.get(ch, ch), pause=pause)
        time.sleep(0.5)

    def shot(s, path):
        s.c("screendump", {"filename": path})
        time.sleep(1.0)


def menu_map(txt):
    out = {}
    for line in txt.splitlines():
        if "[MENU]" in line and "=" in line:
            seg = line.split("[MENU]", 1)[1].replace("[CONSOLE]", "")
            for tok in seg.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    try:
                        out[int(k)] = v.strip()
                    except ValueError:
                        pass
    return out


def main():
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"],
                   stderr=subprocess.DEVNULL)
    for f in (SERIAL, QMP):
        if os.path.exists(f):
            os.remove(f)
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
    time.sleep(4)
    txt = open(SERIAL, errors="ignore").read()
    mm = menu_map(txt)
    idx = next((i for i, n in mm.items() if n == "CONSOLE.BIN"), None)
    log("CONSOLE.BIN menu index: %s (menu size %d)" % (idx, len(mm)))
    if idx is None:
        log("serial tail:\n" + txt[-1500:])
        qemu.kill()
        return 2

    q.click(38, TASKBAR_Y + 13)
    time.sleep(1.2)
    q.key("home", 0.4)
    for _ in range(idx):
        q.key("down", 0.12)
    q.key("ret", 0.5)
    time.sleep(3.0)
    q.shot("/tmp/w3shell_after_open.ppm")
    log("screendump: /tmp/w3shell_after_open.ppm")

    q.type_text("cat SHTEST.TXT > " + CTL, pause=0.18)
    q.key("ret", 0.4)
    time.sleep(15)
    q.shot("/tmp/w3shell_after_cat.ppm")
    log("screendump: /tmp/w3shell_after_cat.ppm")

    q.type_text("make --version > " + VER, pause=0.18)
    q.key("ret", 0.4)
    time.sleep(15)
    q.shot("/tmp/w3shell_after_make.ppm")
    log("screendump: /tmp/w3shell_after_make.ppm")

    qemu.kill()

    def getf(fn):
        r = subprocess.run(["python3", "fat16img.py", "get", IMG, fn],
                           capture_output=True, text=True, timeout=60)
        return r.stdout

    srcdat = getf("SHTEST.TXT")
    ctl = getf(CTL)
    vertxt = getf(VER)
    log("SHTEST.TXT=%d CTL.TXT=%d VERSION.TXT=%d" %
        (len(srcdat), len(ctl), len(vertxt)))
    if ctl != srcdat:
        fails.append("cat SHTEST.TXT > CTL.TXT: CTL.TXT=%d bytes, expected %d"
                     % (len(ctl), len(srcdat)))
    else:
        log("CTL.TXT byte-identical to SHTEST.TXT (%d bytes)" % len(ctl))
    if "GNU Make 4.4.1" not in vertxt:
        fails.append("make --version > VERSION.TXT: %d bytes, no banner: %r"
                     % (len(vertxt), vertxt[:120]))
    else:
        log("VERSION.TXT banner OK (%d bytes)" % len(vertxt))

    if fails:
        log("W3SHELL REPRO FAILED (%d): %s" % (len(fails), fails))
        return 1
    log("W3SHELL REPRO PASSED: external redirects write full bytes in-OS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
