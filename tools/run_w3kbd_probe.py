#!/usr/bin/env python3
"""Keystroke diagnosis: what does the guest keyboard actually deliver?
Types a pure-lowercase command and a bare '>' chord, screenshots after
each, and OCRs the console transcript.
"""
import json, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

W, H = 1920, 1080
TASKBAR_Y = H - 26
SERIAL = "/tmp/w3kbd_serial.log"
QMP = "/tmp/w3kbd-qmp.sock"
QEMU = "qemu-system-aarch64"
EDK2 = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")
IMG = sys.argv[1] if len(sys.argv) > 1 else "disk.img"


def log(msg):
    print("[W3KBD] " + msg, flush=True)


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

    def key(s, q, pause=0.35, shift=False):
        keys = [{"type": "qcode", "data": q}]
        if shift:
            keys.insert(0, {"type": "qcode", "data": "shift"})
        s.c("send-key", {"keys": keys})
        time.sleep(pause)

    def type_text(s, text, pause=0.2):
        qcodes = {" ": "spc", ".": "dot", "/": "slash", "-": "minus"}
        for ch in text:
            if ch == ">":
                s.key("dot", pause=pause, shift=True)
            else:
                s.key(qcodes.get(ch, ch), pause=pause)
        time.sleep(0.4)

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


def ocr(png):
    # convert via ppm2png-style: the caller passes a PPM; render text region
    import struct, subprocess
    d = open(png, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    body = parts[3]
    top = min(h, 160)
    with open("/tmp/w3kbd_region.ppm", "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, top) + body[: top * w * 3])
    r = subprocess.run(["tesseract", "/tmp/w3kbd_region.ppm", "/tmp/w3kbd_out"],
                       capture_output=True)
    try:
        return open("/tmp/w3kbd_out.txt").read()
    except Exception:
        return "(ocp fail)"


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
    mm = menu_map(open(SERIAL, errors="ignore").read())
    idx = next((i for i, n in mm.items() if n == "CONSOLE.BIN"), None)
    log("CONSOLE index %s" % idx)

    q.click(38, TASKBAR_Y + 13)
    time.sleep(1.2)
    q.key("home", 0.4)
    for _ in range(idx):
        q.key("down", 0.12)
    q.key("ret", 0.5)
    time.sleep(3.0)

    # 1: pure lowercase command, no redirect chars at all.
    q.type_text("cat CTL.TXT")
    q.key("ret", 0.4)
    time.sleep(8)
    q.shot("/tmp/w3kbd_cat.ppm")
    log("after 'cat CTL.TXT' transcript:\n" + ocr("/tmp/w3kbd_cat.ppm"))

    # 2: bare redirect-arrow test: echo x > F.TXT
    q.type_text("echo hi > F.TXT")
    q.key("ret", 0.4)
    time.sleep(8)
    q.shot("/tmp/w3kbd_gt.ppm")
    log("after 'echo hi > F.TXT' transcript:\n" + ocr("/tmp/w3kbd_gt.ppm"))

    qemu.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
