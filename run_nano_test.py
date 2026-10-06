#!/usr/bin/env python3
"""End-to-end test for the ported nano (NANO.BIN) on the HobbyOS desktop.

Boots the real desktop in QEMU, then drives it the way a user would:
opens the Apps menu over QMP, launches NANO from it, types two lines,
saves them with Ctrl+O, quits with Ctrl+X -- and then reads the saved
file back OUT OF THE FAT IMAGE to prove the bytes really landed on disk.

    ARCH=arm   python3 run_nano_test.py     # aarch64
    ARCH=intel python3 run_nano_test.py     # x86_64

Screenshots land in /tmp/nano-e2e-<arch>/ (ppm + png); the QEMU serial
output goes to /tmp/nano-e2e-<arch>/serial.log.  Exit 0 = all checks
passed.
"""

import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fat16img import Fat16                     # tiny read-only FAT16 reader

ARCH = os.environ.get("ARCH", "arm")
RUN_DIR = f"/tmp/nano-e2e-{ARCH}"
QMP_SOCK = f"{RUN_DIR}/qmp-sock"
SERIAL = f"{RUN_DIR}/serial.log"
FILE_NAME = "HELLO.TXT"                 # what nano will save (FAT root = cwd)
TYPED_LINES = ["hello from nano", "second line"]
EXPECT_BYTES = ("hello from nano\nsecond line").encode()
SCREEN_W, SCREEN_H = 1920, 1080         # desktop resolution (pixels)

os.makedirs(RUN_DIR, exist_ok=True)

checks = []           # (ok, description)


def check(ok, what):
    checks.append((bool(ok), what))
    print(f"  {'PASS' if ok else 'FAIL'}: {what}", flush=True)


# ------------------------------------------------------- screen-content diffs
# The E2E used to verify only the bytes nano saved.  The rendering regressions
# ("typed text doesn't show up", menu prompts never appearing) were invisible
# to that: they need assertions on WHAT IS ON SCREEN.  These helpers compare
# pixel regions between screendumps so the harness can require that typed text
# and menu prompts actually paint.
#
# ED_BOX = the first two content rows of the editor window (below the title
# bar); PROMPT_BOX = the status/prompt band at the VERY BOTTOM of the nano
# window (the write-out prompt is the last grid row, just above the taskbar;
# at the R6 1920x1080 mode the single window is 1920 wide and its 72-row
# (capped) grid puts the last row around y 750-770).
ED_BOX = (40, 40, 1900, 160)
PROMPT_BOX = (40, 730, 1900, 800)


def ppm_pixels(path):
    """Return (w, h, rgb-bytes) for a P6 PPM."""
    with open(path, "rb") as f:
        data = f.read()
    fields = []
    i = 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i + 1:i + 1 + w * h * 3]


def region_diff(a_path, b_path, box):
    """Count pixels differing between two PPMs inside box (x0,y0,x1,y1)."""
    x0, y0, x1, y1 = box
    wa, ha, a = ppm_pixels(a_path)
    _, _, b = ppm_pixels(b_path)
    n = 0
    for y in range(y0, min(y1, ha)):
        base = y * wa * 3
        for x in range(x0, min(x1, wa)):
            o = base + x * 3
            if a[o] != b[o] or a[o + 1] != b[o + 1] or a[o + 2] != b[o + 2]:
                n += 1
    return n


# ---------------------------------------------------------------- screenshots
def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        data = f.read()
    # P6 header: magic, width, height, maxval -- whitespace-separated, with
    # '#' comments possible.
    fields = []
    i = 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    w, h = int(fields[1]), int(fields[2])
    pixels = data[i:i + w * h * 3]
    raw = b"".join(b"\x00" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) +
           chunk(b"IEND", b""))
    with open(png_path, "wb") as f:
        f.write(png)
    return w, h


def screenshot(qmp, name):
    ppm = f"{RUN_DIR}/{name}.ppm"
    png = f"{RUN_DIR}/{name}.png"
    qmp.cmd("screendump", {"filename": ppm})
    time.sleep(0.6)
    if not os.path.exists(ppm):
        print(f"  (screenshot {name}: no ppm produced)", flush=True)
        return None
    w, h = ppm_to_png(ppm, png)
    with open(ppm, "rb") as f:
        blob = f.read()
    blank = not any(blob[100:])
    print(f"  (screenshot {name}: {w}x{h} -> {png}{' [BLANK]' if blank else ''})",
          flush=True)
    return not blank


# ---------------------------------------------------------------- QMP driver
class QMP:
    def __init__(self, path, timeout=10.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buf = b""
        self.ptr_mode = None                   # "abs" (tablet) or "rel" (PS/2)
        self.vx = self.vy = 16384
        self._read_json_line()                       # greeting
        self.cmd("qmp_capabilities")

    def _read_json_line(self):
        while b"\n" not in self.buf:
            data = self.sock.recv(4096)
            if not data:
                raise RuntimeError("QMP socket closed")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._read_json_line()
            if "event" in resp:
                continue                             # async event: skip
            if "error" in resp:
                raise RuntimeError(f"QMP {execute}: {resp['error']}")
            return resp

    def move(self, x, y):
        """Move the pointer to a SCREEN PIXEL position.

        arm/aarch64 has a virtio-tablet: the desktop scales its 0..32767
        axes, so one absolute event does it.  intel is different -- the
        kernel there drives a PS/2 mouse, feeding it relative deltas
        (scaled x40, clamped to 0..32767, starting mid-range) and
        synthesising the EV_ABS events the desktop reads.  So hit the same
        target by walking the relative path in small steps, tracking the
        kernel's own arithmetic.
        """
        if self.ptr_mode is None:
            try:
                self._abs_move(x, y)
                self.ptr_mode = "abs"
                return
            except RuntimeError as e:
                if "Input handler not found" not in str(e):
                    raise
                self.ptr_mode = "rel"          # no tablet on this machine
                self.vx, self.vy = 16384, 16384  # PS/2 mouse starts centered

        if self.ptr_mode == "abs":
            self._abs_move(x, y)
            return

        tx = min((x * 0x7FFF + SCREEN_W - 1) // SCREEN_W, 0x7FFF)
        ty = min((y * 0x7FFF + SCREEN_H - 1) // SCREEN_H, 0x7FFF)
        for _ in range(40):                    # bounded walk
            ux = (tx - self.vx) // 40          # kernel adds 40 * value
            uy = (ty - self.vy) // 40
            if ux == 0 and uy == 0:
                break
            ux = max(-100, min(100, ux))       # stay well inside the
            uy = max(-100, min(100, uy))       # PS/2 packet's +-127 byte
            self.cmd("input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": ux}},
                {"type": "rel", "data": {"axis": "y", "value": uy}},
            ]})
            self.vx = max(0, min(0x7FFF, self.vx + ux * 40))
            self.vy = max(0, min(0x7FFF, self.vy + uy * 40))
            time.sleep(0.05)

    def _abs_move(self, x, y):
        vx = (x * 0x7FFF + SCREEN_W // 2) // SCREEN_W
        vy = (y * 0x7FFF + SCREEN_H // 2) // SCREEN_H
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": min(vx, 0x7FFF)}},
            {"type": "abs", "data": {"axis": "y", "value": min(vy, 0x7FFF)}},
        ]})

    def click(self, x, y, settle=0.8):
        self.move(x, y)
        time.sleep(0.25)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.12)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(settle)

    def keys(self, *qcodes, settle=0.25):
        """Send keys simultaneously (e.g. ctrl + o)."""
        self.cmd("send-key", {"keys": [
            {"type": "qcode", "data": q} for q in qcodes]})
        time.sleep(settle)

    def type_text(self, text, delay=0.35):
        """Type printable text.  Letters/digits map 1:1 to qcodes; uppercase
        goes as shift+letter (the desktop composes the shifted legend)."""
        for ch in text:
            if ch == " ":
                self.keys("spc", settle=delay)
            elif ch == ".":
                self.keys("dot", settle=delay)
            elif ch == "-":
                self.keys("minus", settle=delay)
            elif ch == "\n":
                self.keys("ret", settle=delay)
            elif ch.isupper():
                self.keys("shift", ch.lower(), settle=delay)
            else:
                self.keys(ch, settle=delay)


# ---------------------------------------------------------------- boot
def boot():
    """Start QEMU (through make run) and wait for the desktop."""
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    logf = open(SERIAL, "w")

    proc = subprocess.Popen(
        ["make", "run", f"ARCH={ARCH}",
         "QEMU_ARGS=-display none -qmp unix:" + QMP_SOCK + ",server,nowait " +
         os.environ.get("EXTRA_QEMU", "")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    state = {"ready": False, "log": ""}

    def drain():
        assert proc.stdout is not None
        for line in proc.stdout:
            state["log"] += line
            logf.write(line)
            logf.flush()
            if "Desktop starting" in line:
                state["ready"] = True

    t = threading.Thread(target=drain, daemon=True)
    t.start()

    deadline = time.time() + 90
    while time.time() < deadline and not state["ready"]:
        if proc.poll() is not None:
            break
        time.sleep(0.5)
    if not state["ready"]:
        print("QEMU serial tail:\n" + state["log"][-2000:], flush=True)
    return proc, state["ready"]


def kill_qemu(proc):
    subprocess.run(["pkill", "-9", "-f", "qemu-system"], stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()


# ---------------------------------------------------------------- main
def main():
    print(f"=== nano end-to-end test (ARCH={ARCH}) ===", flush=True)
    print(f"run dir: {RUN_DIR}", flush=True)

    # The FAT image the test saves into: make sure NANO.BIN is on it.
    # Force a rebuild: hobbyos.elf / disk.img are shared Makefile targets
    # between arches (no arch in the filename), so after building the other
    # architecture make would call these "up to date" and boot the wrong
    # kernel -- the test then fails with a mystifying "desktop never booted".
    print("[build] make disk.img ...", flush=True)
    for stale in ("hobbyos.elf", "disk.img"):
        try:
            os.remove(stale)
        except FileNotFoundError:
            pass
    if subprocess.run(["make", "disk.img", f"ARCH={ARCH}",
                       "-j4"]).returncode != 0:
        print("build failed", flush=True)
        return 1

    print(f"[boot] QEMU ({ARCH}) ...", flush=True)
    proc, ready = boot()
    check(ready, "the desktop booted (serial: 'Desktop starting')")
    if not ready:
        kill_qemu(proc)
        return 1
    time.sleep(3)          # let the desktop paint its taskbar

    try:
        qmp = QMP(QMP_SOCK)
        qmp.move(SCREEN_W // 2, SCREEN_H // 2)   # park the pointer mid-screen
        time.sleep(0.5)

        ok = screenshot(qmp, "00-desktop")
        check(ok, "the desktop is up and painted (screenshot not blank)")

        # --- launch NANO.BIN from the Apps menu ---------------------------
        # The desktop dumps "…[MENU] <idx>=<name>…" at boot: that is the
        # exact index -> name mapping the menu uses, so read it instead of
        # guessing.  (The dump goes through the console window, so the
        # serial log has the data with "[CONSOLE]" markers mixed in.)
        menu_text = serial_clean()
        menu = dict((m.group(2), int(m.group(1)))
                    for m in re.finditer(r"\[MENU\] (\d+)=([^\s\[]+)", menu_text))
        print(f"  (menu: {len(menu)} entries; NANO.BIN -> index "
              f"{menu.get('NANO.BIN')})", flush=True)
        check("NANO.BIN" in menu, "NANO.BIN is listed in the Apps menu")
        nano_idx = menu.get("NANO.BIN", 0)

        # Taskbar APPS button: x 6..70, y SCREEN_H-26..SCREEN_H (taskbar 26px).
        qmp.click(38, SCREEN_H - 13)
        screenshot(qmp, "01-apps-menu")
        # Keyboard navigation: Down moves the selection (the desktop scrolls
        # the menu as needed), Enter launches it.
        for _ in range(nano_idx):
            qmp.keys("down", settle=0.18)
        qmp.keys("ret", settle=1.0)
        time.sleep(3.0)                        # nano starts + paints
        screenshot(qmp, "02-nano-open")
        check("[LAUNCH] NANO.BIN" in serial_clean(),
              "the desktop launched NANO.BIN (serial: '[LAUNCH] NANO.BIN')")

        # --- type two lines ----------------------------------------------
        for i, line in enumerate(TYPED_LINES):
            qmp.type_text(line)
            if i == 0:
                qmp.keys("ret", settle=0.4)
        time.sleep(1.0)
        screenshot(qmp, "03-typed")

        # --- on-screen regression: the typed text must actually PAINT ------
        # (guards the "_CURSES_H_ / wgetch-flush" rendering bug where every
        # keystroke stayed invisible until a full refresh)
        d = region_diff(f"{RUN_DIR}/02-nano-open.ppm",
                        f"{RUN_DIR}/03-typed.ppm", ED_BOX)
        check(d > 300,
              f"the typed lines are visible in the edit area (pixel delta {d})")

        # per-keystroke burst: each fast char must still land on screen
        for ch in "xyz":
            qmp.keys(ch, settle=0.1)
        time.sleep(1.0)
        screenshot(qmp, "03b-perkey")
        d = region_diff(f"{RUN_DIR}/02-nano-open.ppm",
                        f"{RUN_DIR}/03b-perkey.ppm", ED_BOX)
        check(d > 150,
              f"fast-typed characters paint too (pixel delta {d})")

        # --- save with Ctrl+O --------------------------------------------
        qmp.keys("ctrl", "o", settle=1.0)
        screenshot(qmp, "04-write-prompt")
        # on-screen regression: the write-out prompt row must render
        d = region_diff(f"{RUN_DIR}/03b-perkey.ppm",
                        f"{RUN_DIR}/04-write-prompt.ppm", PROMPT_BOX)
        check(d > 15,
              f"the Write-Out prompt row appears after ^O (pixel delta {d})")
        qmp.type_text(FILE_NAME, delay=0.3)
        qmp.keys("ret", settle=2.0)
        screenshot(qmp, "05-saved")

        # --- quit with Ctrl+X --------------------------------------------
        qmp.keys("ctrl", "x", settle=2.5)
        screenshot(qmp, "06-exited")

    finally:
        kill_qemu(proc)

    # --- the evidence that counts: the file on the FAT image -------------
    print("[verify] reading HELLO.TXT back out of disk.img ...", flush=True)
    img = Fat16("disk.img")
    try:
        got = img.read_file(FILE_NAME)
    except (FileNotFoundError, IsADirectoryError):
        got = None
    check(got is not None, f"{FILE_NAME} exists on the FAT image")
    if got is not None:
        print(f"  (image bytes: {got!r} -- {len(got)} bytes)", flush=True)
        check(got.startswith(EXPECT_BYTES),
              f"the saved bytes start with what was typed ({EXPECT_BYTES!r})")
        names = [n for (n, _d, _c, _s) in img.root_raw()]
        check(FILE_NAME in [n.upper() for n in names],
              f"{FILE_NAME} shows up in the root directory listing")

    print()
    failed = [c for c in checks if not c[0]]
    print(f"=== {len(checks)} checks, {len(failed)} failed ===")
    if not failed:
        print(f"ALL NANO E2E CHECKS PASSED ({ARCH})")
    else:
        for _, what in failed:
            print(f"  FAILED: {what}")
    return 0 if not failed else 1


def serial_log_text():
    try:
        with open(SERIAL) as f:
            return f.read()
    except OSError:
        return ""


def serial_clean():
    """Serial log with the console window's interleaved "[CONSOLE]" markers
    removed, so lines read like the plain program output they carry."""
    t = serial_log_text().replace("[CONSOLE] ", "").replace("[CONSOLE]", "")
    return re.sub(r"[ \t]*\n[ \t]*", "\n", t)


if __name__ == "__main__":
    sys.exit(main())
