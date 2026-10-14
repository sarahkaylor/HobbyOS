#!/usr/bin/env python3
"""Deterministic in-OS repro for BUG A (sh external redirect).

Boots the ARM disk image in DESKTOP mode (NO test wave -> no slot
pressure -> deterministic), opens a console (CONSOLE.BIN -> SH.BIN) from
the Apps menu, and runs:

    cat SHTEST.TXT > CTL.TXT        (EXTERNAL command, stdout redirect)
    cat CTL.TXT                     (read it back)
    make --version > VERSION.TXT    (the w2 deferred byte check)

Then extracts CTL.TXT / VERSION.TXT from disk.img with fat16img.py and
byte-compares against the fixture.  Verdict: exit 0 = redirect writes the
full bytes in-OS; exit 1 = lossy (report actual byte count).
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
        time.sleep(0.1)

    def click(s, x, y):
        s.mv(x, y)
        s.c("input-send-event", {"events": [{"type": "btn",
             "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn",
             "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)

    def key(s, q, pause=0.15):
        s.c("send-key", {"keys": [{"type": "qcode", "data": q}]})
        time.sleep(pause)

    def type_text(s, text):
        # Lowercase input; ">" must be typed as shift+dot held together, so
        # it is emitted as a simultaneous two-key chord (QMP send-key sends
        # the listed keys down together, then up).
        qcodes = {"-": "minus", " ": "spc", ".": "dot", "/": "slash",
                  "$": "dollar"}
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == ">":
                s.c("send-key", {"keys": [{"type": "qcode", "data": "shift"},
                                          {"type": "qcode", "data": "dot"}]})
                time.sleep(0.12)
                i += 1
                continue
            s.key(qcodes.get(ch, ch), 0.09)
            i += 1
        time.sleep(0.2)

    def shot(s, path):
        s.c("screendump", {"filename": path})
        time.sleep(0.4)


def main():
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"],
                   stderr=subprocess.DEVNULL)
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

    entries = {}
    for line in txt.splitlines():
        if "[MENU]" in line and "=" in line:
            seg = line.split("[MENU]", 1)[1].replace("[CONSOLE]", "")
            for tok in seg.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    try:
                        entries[int(k)] = v.strip()
                    except ValueError:
                        pass
    idx = next((i for i, n in entries.items() if n == "CONSOLE.BIN"), None)
    log("CONSOLE.BIN menu index: %s (menu size %d)" % (idx, len(entries)))
    if idx is None:
        fails.append("no CONSOLE.BIN in the serial [MENU] dump")
        log("serial tail:\n" + txt[-1500:])
        qemu.kill()
        return 1

    q.click(38, TASKBAR_Y + 13)
    time.sleep(0.8)
    q.key("home", 0.3)
    for _ in range(idx):
        q.key("down", 0.06)
    q.key("ret", 0.3)
    time.sleep(2.0)

    # 1: external command with redirect to a file.
    q.type_text("cat SHTEST.TXT > CTL.TXT")
    q.key("ret", 0.3)
    time.sleep(3.0)
    # 2: read it back on screen (for the serial log).
    q.type_text("cat CTL.TXT")
    q.key("ret", 0.3)
    time.sleep(2.0)
    # 3: the w2 deferred make byte check.
    q.type_text("make --version > VERSION.TXT")
    q.key("ret", 0.3)
    time.sleep(4.0)
    q.shot("/tmp/w3shell_shot.ppm")

    serial.flush()
    time.sleep(1)
    txt = open(SERIAL, errors="ignore").read()
    # Capture the on-screen cat CTL.TXT echo if it reached serial.
    m = re.findall(r"cat CTL\.TXT\n(.*?)\nuser@", txt, re.S)
    if m:
        log("on-screen cat CTL.TXT:\n" + m[-1][:300])

    qemu.kill()

    def getf(fn):
        r = subprocess.run(["python3", "fat16img.py", "get", IMG, fn],
                           capture_output=True, text=True, timeout=60)
        return r.stdout

    ctl = getf("CTL.TXT")
    srcdat = subprocess.run(["python3", "fat16img.py", "get", IMG, "SHTEST.TXT"],
                            capture_output=True, text=True, timeout=60).stdout
    log("SHTEST.TXT extracted: %d bytes" % len(srcdat))
    log("CTL.TXT extracted: %d bytes" % len(ctl))
    if ctl != srcdat:
        fails.append("cat SHTEST.TXT > CTL.TXT LOSSY: %d bytes, expected %d; "
                     "tail: %r" % (len(ctl), len(srcdat), ctl[-60:]))
    else:
        log("CTL.TXT byte-identical to SHTEST.TXT (%d bytes)" % len(ctl))

    vertxt = getf("VERSION.TXT")
    if "GNU Make 4.4.1" in vertxt:
        log("VERSION.TXT %d bytes, banner match OK" % len(vertxt))
    else:
        fails.append("make --version > VERSION.TXT LOSSY/BROKEN: %d bytes: %r"
                     % (len(vertxt), vertxt[:200]))

    if fails:
        log("W3SHELL REPRO FAILED (%d): %s" % (len(fails), fails))
        return 1
    log("W3SHELL REPRO PASSED: external redirects write full bytes in-OS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
