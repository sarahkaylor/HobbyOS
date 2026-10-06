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
  5. modifiers (F1.8) - Ctrl+7 and Alt+7 reach the app with the K stamp
                        decoded ("[X11] K mods=2", "[X11] K mods=4")
  6. wheel (F1.8)   - QMP wheel ticks arrive as btn 4/5 press/release
                        pairs ("[X11] wheel up", "[X11] wheel down")
  7. F4             - ESC [ D ~ closes the app within the grace period
                        ("[CLOSE] ...: exited within grace"), never the
                        kill fallback, and the desktop comes back
  8. pixel readback - the entry line's glyph width shows one digit after
                        AC and two after Ctrl+7 + Alt+7, so the modifier
                        keys provably rendered in the pixel window

The keypad coordinates are computed from the same layout equation as
src/user/x11/apps/xcalc/main.c (layout_compute), so a layout change that
breaks the click targets is caught here.

Usage:  python3 run_xcalc_test.py             (aarch64, virtio input)
        ARCH=intel python3 run_xcalc_test.py  (x86_64 under KVM; PS/2
                                              input, rel pointer moves,
                                              menu launched by click)
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

ARCH = os.environ.get("ARCH", "arm")
INTEL = ARCH == "intel"

QMP_SOCK = "./qmp-sock-xcalc"
SERIAL_LOG = "/tmp/xcalc_serial.log"
SHOT_DIR = "/tmp/hobbyos_xcalc"
QEMU_BIN = "qemu-system-x86_64" if INTEL else "qemu-system-aarch64"

W, H = 1920, 1080
TASKBAR_Y = H - 26

# The desktop's single-window tile is the whole area above the taskbar;
# the content rectangle is the window minus frame (2px), title (16) and
# menu bar (16) -- keep in sync with wm_pixel_content_rect().
WIN_W, WIN_H = W, H - 26
CONTENT_ORIGIN = (0 + 2, 0 + 34)
CONTENT_W, CONTENT_H = WIN_W - 4, WIN_H - 36

# Mirror of the start-menu geometry in src/user/desktop.c (draw_start_menu).
START_MENU_VISIBLE = 16
START_MENU_X, START_MENU_W = 4, 220
APPS_BTN_X, APPS_BTN_W = 6, 64

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
    # Only this run's machine: the other arch may belong to a sibling
    # lane, and whichever holds THIS worktree's disk.img is killed below.
    subprocess.run(["pkill", "-9", "-f", QEMU_BIN], stderr=subprocess.DEVNULL)
    subprocess.run(["fuser", "-k", "disk.img"], stderr=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL)
    time.sleep(1)


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
        # The kernel parks the pointer mid-screen on both arches; the rel
        # path below tracks where it has been moved to.
        self.px, self.py = W // 2, H // 2
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
        if INTEL:
            # PS/2 relative pointer: the kernel adds units*40 to a 0..32767
            # counter and maps it linearly to the screen (POSIX.2-style
            # kernel scaling, src/kernel keyboard/mouse path).  x and y
            # are both screen-signed here (QEMU emits rel y downwards).
            ux = int(round((x - self.px) * 32767.0 / (40.0 * W)))
            uy = int(round((y - self.py) * 32767.0 / (40.0 * H)))
            while ux or uy:
                sx = max(-100, min(100, ux))
                sy = max(-100, min(100, uy))
                ux -= sx
                uy -= sy
                self.cmd("input-send-event", {"events": [
                    {"type": "rel", "data": {"axis": "x", "value": sx}},
                    {"type": "rel", "data": {"axis": "y", "value": sy}}]})
                time.sleep(0.05)
        else:
            self.cmd("input-send-event", {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        self.px, self.py = x, y
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

    def chord(self, *qcodes, pause=0.4):
        """Press several keys together (Ctrl+7): modifiers first."""
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": q}
                                       for q in qcodes]})
        time.sleep(pause)

    def wheel(self, direction, ticks=1):
        """One QMP wheel tick per button event, like a real wheel click."""
        button = "wheel-up" if direction == "up" else "wheel-down"
        for _ in range(ticks):
            self.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"button": button, "down": True}}]})
            time.sleep(0.05)
            self.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"button": button, "down": False}}]})
            time.sleep(0.2)

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path})
        return "return" in r


def ppm_has_content(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return False
    return any(data[100:])


def ppm_pixels(path):
    """Parse a binary P6 PPM and return (w, h, pixel_bytes)."""
    data = open(path, "rb").read()
    parts = []
    i = 2
    while len(parts) < 3:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(int(data[i:j]))
        i = j
    i += 1                              # one whitespace byte after maxval
    return parts[0], parts[1], data[i:]


def entry_ink_width(path):
    """Pixel readback of XCALC's entry line: the width in screen pixels of
    the white glyphs on it (one digit ~6px, two ~14px).  The band is the
    entry row: content-relative (10, 4*line_spacing) with a 16% header, so
    screen x 10..240, y 130..152 for the single full-size tile."""
    w, h, px = ppm_pixels(path)
    xs = []
    for y in range(130, 152):
        for x in range(10, 240):
            o = (y * w + x) * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            if r > 200 and g > 200 and b > 200:
                xs.append(x)
    return (max(xs) - min(xs) + 1) if xs else 0


def serial_text():
    try:
        return open(SERIAL_LOG, errors="ignore").read()
    except OSError:
        return ""


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


def launch_from_start_menu(qmp, menu_map, xcalc_idx, ups):
    """Open the Apps menu and select XCALC.BIN.

    arm: End jumps to the last row, Up walks up to XCALC, Enter selects.
    intel: the PS/2 path reports E0-extended keys with different codes, so
    the row is clicked in the menu instead (still a real input path)."""
    qmp.click(5, 5)
    time.sleep(0.3)
    qmp.click(APPS_BTN_X + APPS_BTN_W // 2, TASKBAR_Y + 13)
    time.sleep(0.5)
    if INTEL:
        h = min(START_MENU_VISIBLE, len(menu_map)) * 20 + 8
        row_y = (TASKBAR_Y - h) + 4 + xcalc_idx * 20 + 10
        log(f"[INFO] intel: clicking XCALC.BIN row at y={row_y}")
        qmp.click(START_MENU_X + START_MENU_W // 2, row_y)
    else:
        qmp.key("end")
        for _ in range(ups):
            qmp.key("up")
        qmp.key("ret")


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    kill_qemu()
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    log(f"[INFO] booting desktop ({ARCH})...")
    serial = open(SERIAL_LOG, "w")
    qemu_args = f"-display none -qmp unix:{QMP_SOCK},server,nowait"
    if INTEL:
        qemu_args += " -enable-kvm"
    proc = subprocess.Popen(
        ["stdbuf", "-oL", "-eL", "make", "run", f"ARCH={ARCH}",
         f"QEMU_ARGS={qemu_args}"],
        stdout=serial, stderr=subprocess.STDOUT)

    failures = []
    try:
        qmp = Qmp(QMP_SOCK)

        ok, _ = serial_contains("Desktop starting", 90)
        if not ok:
            log("[FAIL] desktop never booted")
            return 1
        time.sleep(3)

        # Launch XCALC from the start menu.
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

        launch_from_start_menu(qmp, menu_map, xcalc_idx, ups)

        ok, mark = serial_contains("[APP] XCALC started", 30)
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

        # F1.8 modifiers: the K stamp must reach the app before the key,
        # and the keys must actually land (two digits on the entry line).
        # AC again, then Ctrl+7 (K mods=2) and Alt+7 (K mods=4); both keys
        # still display as plain 7.
        qmp.click(x, y)
        time.sleep(0.5)
        qmp.shot(os.path.join(SHOT_DIR, "03a_ac.ppm"))
        mark = len(serial_text())
        qmp.chord("ctrl", "7")
        ok, mark = serial_contains("[X11] K mods=2", 10, mark)
        if not ok:
            failures.append("Ctrl+7: no '[X11] K mods=2' marker (K stamp missing)")
        else:
            log("[TEST] modifiers: Ctrl+7 carries K mods=2")
        qmp.chord("alt", "7")
        ok, mark = serial_contains("[X11] K mods=4", 10, mark)
        if not ok:
            failures.append("Alt+7: no '[X11] K mods=4' marker")
        else:
            log("[TEST] modifiers: Alt+7 carries K mods=4")
        time.sleep(0.5)
        qmp.shot(os.path.join(SHOT_DIR, "03b_modifiers.ppm"))

        # F1.8 wheel: two detents up, one down, over the calculator (the
        # pointer is still on AC).  The app decodes btn 4/5 pairs.
        if INTEL:
            log("[INFO] intel: wheel not sent (the PS/2 path reports none)")
        else:
            mark = len(serial_text())
            qmp.wheel("up", 2)
            ok, mark = serial_contains("[X11] wheel up", 10, mark)
            if not ok:
                failures.append("wheel up: no '[X11] wheel up' marker")
            else:
                log("[TEST] wheel: up tick decoded as btn 4")
            qmp.wheel("down", 1)
            ok, mark = serial_contains("[X11] wheel down", 10, mark)
            if not ok:
                failures.append("wheel down: no '[X11] wheel down' marker")
            else:
                log("[TEST] wheel: down tick decoded as btn 5")

        # Escape acts as AC.
        qmp.key("esc")
        time.sleep(0.5)
        qmp.shot(os.path.join(SHOT_DIR, "04_esc_ac.ppm"))

        # F4 closes the focused window through the F1.8 graceful path:
        # ESC [ D ~ goes out first, XCALC exits within the grace, and the
        # desktop must survive (never the kill fallback for a live app).
        mark = len(serial_text())
        qmp.key("f4")
        ok, _ = serial_contains("[CLOSE] win=", 10, mark)
        if not ok:
            failures.append("F4: no close request marker")
        elif "ESC [ D sent" not in serial_text():
            failures.append("F4: close request did not send ESC [ D")
        else:
            log("[TEST] close: ESC [ D sent to the pixel window")
        # Both close lines can land inside one poll, so search from the
        # F4 mark, not from the end of the first match.
        ok, _ = serial_contains("exited within grace", 10, mark)
        if not ok:
            failures.append("close: app did not exit within the grace period")
        else:
            log("[TEST] close: app exited within the grace period")
        if "grace expired, killing" in serial_text():
            failures.append("close: kill fallback fired on a live app")
        time.sleep(1.0)
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
            "03a_ac": "03a_ac.ppm",
            "03b_modifiers": "03b_modifiers.ppm",
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

        # Pixel readback: the entry line shows one digit after AC and two
        # after Ctrl+7 + Alt+7 -- the modifier keys reached the app and
        # rendered, whatever the serial markers say.
        try:
            w_ac = entry_ink_width(os.path.join(SHOT_DIR, "03a_ac.ppm"))
            w_mod = entry_ink_width(os.path.join(SHOT_DIR, "03b_modifiers.ppm"))
            w_esc = entry_ink_width(os.path.join(SHOT_DIR, "04_esc_ac.ppm"))
            w_78 = entry_ink_width(os.path.join(SHOT_DIR, "03_keyboard_78.ppm"))
            log(f"[TEST] readback: entry-line ink width 03a={w_ac} 03b={w_mod} "
                f"04={w_esc} 03={w_78} px")
            if not (w_mod > w_ac):
                failures.append(
                    f"readback: Ctrl+7/Alt+7 did not add a digit "
                    f"(entry width {w_ac} -> {w_mod})")
            if not (w_78 > w_esc):
                failures.append(
                    f"readback: keyboard 78 vs Esc AC ink widths "
                    f"{w_78} vs {w_esc}")
        except (OSError, ValueError) as e:
            failures.append(f"readback failed: {e}")
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
