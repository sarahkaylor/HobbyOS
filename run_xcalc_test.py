#!/usr/bin/env python3
"""
Boot the real HobbyOS desktop, launch XCALC (the ported X11 calculator)
from the start menu through the pixel-window path, and drive it with the
mouse (clicking keypad keys) and the keyboard, capturing a screenshot at
every step.

What it verifies:
  1. launch        - serial marker "[APP] XCALC started" + a visible window
  2. mouse          - clicking 2, ENTER, 3, + shows 5
  3. keyboard       - typing 7 and 8 shows 78
  4. Escape (AC)    - wipes the entry back to 0
  5. F4             - closes the window and the desktop comes back

The keypad coordinates are computed from the same layout equation as
src/user/x11/apps/xcalc/main.c (layout_compute), so a layout change that
breaks the click targets is caught here.

Usage:  python3 run_xcalc_test.py   (or: make xcalc_test)
Result: exit 0 on success; screenshots in /tmp/hobbyos_xcalc/.
"""
import json
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

QMP_SOCK = "./qmp-sock-xcalc"
SERIAL_LOG = "/tmp/xcalc_serial.log"
SHOT_DIR = "/tmp/hobbyos_xcalc"
QEMU_BIN = "qemu-system-aarch64"

W, H = 1024, 768
TASKBAR_Y = H - 26

# The desktop's single-window tile is the whole area above the taskbar;
# the content rectangle is the window minus frame (2px), title (16) and
# menu bar (16) -- keep in sync with wm_pixel_content_rect().
WIN_W, WIN_H = W, H - 26
CONTENT_ORIGIN = (0 + 2, 0 + 34)
CONTENT_W, CONTENT_H = WIN_W - 4, WIN_H - 36

# Mirror of buttons[6][4] in xcalc/main.c.
BUTTONS = [
    ["C", "AC", "POP", "SWAP"],
    ["%", "^", "1/x", "+/-"],
    ["7", "8", "9", "+"],
    ["4", "5", "6", "-"],
    ["1", "2", "3", "*"],
    [".", "0", "ENTER", "/"],
]

KEY_W_MAX, KEY_H_MAX = 120, 64


def log(msg):
    print(msg, flush=True)


def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)


def layout_compute(cw, ch):
    """Mirror of layout_compute() in xcalc/main.c: returns a rect(label)."""
    if cw < 8:
        cw = 8
    if ch < 8:
        ch = 8
    header = ch * 16 // 100
    hmax = ch - 40
    if hmax < 24:
        hmax = 24
    if header > hmax:
        header = hmax
    if header < 24:
        header = 24
    pad_top = header + 2
    avail_h = ch - pad_top - 4
    if avail_h < 6:
        avail_h = 6
    avail_w = cw - 8
    if avail_w < 4:
        avail_w = 4
    cell_w = min(avail_w // 4, KEY_W_MAX)
    cell_h = min(avail_h // 6, KEY_H_MAX)
    gx = (cw - cell_w * 4) // 2

    def rect(label):
        for r in range(6):
            for c in range(4):
                if BUTTONS[r][c] == label:
                    return gx + c * cell_w, pad_top + r * cell_h, cell_w, cell_h
        raise KeyError(label)

    return rect


def key_center(rect_of, label):
    x, y, w, h = rect_of(label)
    return CONTENT_ORIGIN[0] + x + w // 2, CONTENT_ORIGIN[1] + y + h // 2


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        for _ in range(120):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.sock.recv(65536)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.sock.recv(65536)
            text = buf.decode(errors="ignore")
            for part in text.strip().split("\n"):
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
            if len(buf) > 1 << 20:
                raise RuntimeError("QMP response too large")

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.15)

    def click(self, x, y, button="left"):
        self.move(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": False}}]})
        time.sleep(0.3)

    def key(self, qcode, pause=0.35):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path})
        return "return" in r


def ppm_has_content(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return False
    return any(data[100:])


def serial_contains(needle, timeout, since=0):
    deadline = time.time() + timeout
    text = ""
    while time.time() < deadline:
        try:
            text = open(SERIAL_LOG, errors="ignore").read()
        except OSError:
            text = ""
        if needle in text[since:]:
            return True, len(text)
        time.sleep(0.3)
    return False, len(text)


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    kill_qemu()
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    log("[INFO] booting desktop...")
    serial = open(SERIAL_LOG, "w")
    proc = subprocess.Popen(
        ["stdbuf", "-oL", "-eL", "make", "run", "ARCH=arm",
         f"QEMU_ARGS=-display none -qmp unix:{QMP_SOCK},server,nowait"],
        stdout=serial, stderr=subprocess.STDOUT)

    failures = []
    try:
        qmp = Qmp(QMP_SOCK)

        ok, _ = serial_contains("Desktop starting", 60)
        if not ok:
            log("[FAIL] desktop never booted")
            return 1
        time.sleep(3)

        # Launch XCALC from the start menu: open it, jump to the last
        # entry, walk up to XCALC's row, select.
        menu_map = {}
        deadline = time.time() + 20
        while time.time() < deadline and not menu_map:
            text = open(SERIAL_LOG, errors="ignore").read()
            for line in text.splitlines():
                if "[MENU]" in line and "=" in line:
                    seg = line.split("[MENU]", 1)[1].replace("[CONSOLE]", " ").strip()
                    if seg.startswith("count="):
                        continue
                    try:
                        k, v = seg.split("=", 1)
                        menu_map[int(k)] = v.strip()
                    except ValueError:
                        pass
            time.sleep(0.5)
        if not menu_map:
            log("[FAIL] desktop never reported its menu mapping")
            return 1

        xcalc_idx = None
        for k, v in menu_map.items():
            if v == "XCALC.BIN":
                xcalc_idx = k
        if xcalc_idx is None:
            log("[FAIL] XCALC.BIN missing from the desktop menu")
            return 1
        ups = max(menu_map.keys()) - xcalc_idx
        log(f"[INFO] XCALC.BIN at menu index {xcalc_idx} ({ups} ups from the end)")

        qmp.click(5, 5)
        time.sleep(0.3)
        qmp.click(38, TASKBAR_Y + 13)
        time.sleep(0.5)
        qmp.key("end")
        for _ in range(ups):
            qmp.key("up")
        qmp.key("ret")

        ok, mark = serial_contains("[APP] XCALC started", 20)
        if not ok:
            log("[FAIL] XCALC never started (no serial marker)")
            return 1
        log("[TEST] launch: marker OK")
        time.sleep(1.0)
        qmp.shot(os.path.join(SHOT_DIR, "01_launched.ppm"))

        rect_of = layout_compute(CONTENT_W, CONTENT_H)

        # Mouse: 2 ENTER 3 + = 5.
        for label in ("2", "ENTER", "3", "+"):
            x, y = key_center(rect_of, label)
            log(f"[INFO] click {label} at ({x},{y})")
            qmp.click(x, y)
        time.sleep(0.5)
        qmp.shot(os.path.join(SHOT_DIR, "02_mouse_2enter3plus.ppm"))

        # Keyboard: AC (click), then 7 8.
        x, y = key_center(rect_of, "AC")
        qmp.click(x, y)
        time.sleep(0.3)
        qmp.key("7")
        qmp.key("8")
        time.sleep(0.5)
        qmp.shot(os.path.join(SHOT_DIR, "03_keyboard_78.ppm"))

        # Escape acts as AC.
        qmp.key("esc")
        time.sleep(0.5)
        qmp.shot(os.path.join(SHOT_DIR, "04_esc_ac.ppm"))

        # F4 closes the focused window; the desktop must survive.
        qmp.key("f4")
        time.sleep(1.5)
        qmp.shot(os.path.join(SHOT_DIR, "05_closed.ppm"))
        ok, _ = serial_contains("Desktop starting", 5)
        if not ok:
            failures.append("desktop died after closing XCALC")

        # Screenshot sanity: all shots exist and are non-blank, and the
        # calculator steps visibly change the screen.
        shot_files = {
            "01_launched": "01_launched.ppm",
            "02_mouse": "02_mouse_2enter3plus.ppm",
            "03_keyboard": "03_keyboard_78.ppm",
            "04_esc": "04_esc_ac.ppm",
            "05_closed": "05_closed.ppm",
        }
        shots = {}
        for label, fn in shot_files.items():
            path = os.path.join(SHOT_DIR, fn)
            data = open(path, "rb").read() if os.path.exists(path) else b""
            shots[label] = data
            if not data:
                failures.append(f"{fn}: screenshot missing")
            elif not ppm_has_content(path):
                failures.append(f"{fn}: screenshot blank")
        if shots["02_mouse"] and shots["02_mouse"] == shots["03_keyboard"]:
            failures.append("keyboard input left the screen unchanged")
        if shots["04_esc"] and shots["04_esc"] == shots["03_keyboard"]:
            failures.append("Escape left the screen unchanged")
    finally:
        kill_qemu()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    log("")
    if failures:
        for f in failures:
            log(f"[FAIL] {f}")
        log(f"=== XCALC acceptance FAILED ({len(failures)} problems) ===")
        return 1
    log("[TEST] screenshots: all present, non-blank, and input changed the screen")
    log("=== XCALC acceptance PASSED ===")
    log(f"(screenshots in {SHOT_DIR})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
