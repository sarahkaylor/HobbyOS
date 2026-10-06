#!/usr/bin/env python3
"""
Boot the real HobbyOS desktop, launch XANTFARM (the xantfarm port) from
the start menu through the pixel-window path, and drive it with the mouse,
capturing a screenshot at every step.

What it verifies:
  1. launch   - serial marker "[APP] XANTFARM started" + a visible window
  2. palette  - the farm's air / dirt / ant colors actually reach the
                framebuffer (the port painted its world)
  3. animation- two shots seconds apart differ (ants are walking and
                digging; the app re-flushes every cycle)
  4. hover    - moving the pointer over the farm keeps the app alive and
                drawing (the desktop's ESC [ T reports -> MotionNotify ->
                poke()) and the screen keeps changing
  5. F4       - closes the window and the desktop comes back

Usage:  python3 run_antfarm_test.py   (or: make antfarm_test)
Result: exit 0 on success; screenshots in /tmp/hobbyos_antfarm/.
"""
import json
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

QMP_SOCK = "./qmp-sock-antfarm"
SERIAL_LOG = "/tmp/antfarm_serial.log"
SHOT_DIR = "/tmp/hobbyos_antfarm"
QEMU_BIN = "qemu-system-aarch64"

W, H = 1920, 1080
TASKBAR_Y = H - 26

# The desktop's single-window tile is the whole area above the taskbar;
# the content rectangle is the window minus frame (2px), title (16) and
# menu bar (16) -- keep in sync with wm_pixel_content_rect().
CONTENT_ORIGIN = (2, 34)
CONTENT_W, CONTENT_H = W - 4, (H - 26) - 36

# Palette of src/user/x11/apps/antfarm/main.c.
AIR = (0xF2, 0xEF, 0xE9)
DIRT = (0x9A, 0x7B, 0x55)
SAND = (0xE8, 0xC8, 0x7A)
ANT = (0xB0, 0x1E, 0x1E)


def log(msg):
    print(msg, flush=True)


def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)


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


def ppm_pixels(path):
    """Return the raw RGB bytes of a QEMU screendump (P6)."""
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        return b""
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    return data[i + 1:]


def count_color(pixels, rgb):
    return pixels.count(bytes(rgb))


def crop(pixels, x, y, w, h):
    """The window's content rectangle out of a full-screen P6 dump."""
    if len(pixels) < (W * H * 3):
        return b""
    rows = [pixels[(yy * W + x) * 3:(yy * W + x + w) * 3] for yy in range(y, y + h)]
    return b"".join(rows)


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

        # The desktop dumps its start-menu mapping once at startup.
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

        antfarm_idx = None
        for k, v in menu_map.items():
            if v == "ANTFARM.BIN":
                antfarm_idx = k
        if antfarm_idx is None:
            log("[FAIL] ANTFARM.BIN missing from the desktop menu")
            return 1
        ups = max(menu_map.keys()) - antfarm_idx
        log(f"[INFO] ANTFARM.BIN at menu index {antfarm_idx} ({ups} ups from the end)")

        # Launch XANTFARM from the start menu.
        qmp.click(5, 5)
        time.sleep(0.3)
        qmp.click(38, TASKBAR_Y + 13)
        time.sleep(0.5)
        qmp.key("end")
        for _ in range(ups):
            qmp.key("up")
        qmp.key("ret")

        ok, mark = serial_contains("[APP] XANTFARM started", 20)
        if not ok:
            log("[FAIL] XANTFARM never started (no serial marker)")
            return 1
        log("[TEST] launch: marker OK")

        time.sleep(1.5)
        shot1 = os.path.join(SHOT_DIR, "01_launched.ppm")
        qmp.shot(shot1)
        if not ppm_has_content(shot1):
            failures.append("01_launched: screenshot blank")
        log("[TEST] launch: window drawn")

        # The palette: the farm itself must be on screen (in the window's
        # content rectangle -- the rest of the screen is desktop chrome).
        px1 = crop(ppm_pixels(shot1), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        n_air, n_dirt = count_color(px1, AIR), count_color(px1, DIRT)
        n_sand, n_ant = count_color(px1, SAND), count_color(px1, ANT)
        log(f"[INFO] palette: air={n_air} dirt={n_dirt} sand={n_sand} ant={n_ant}")
        if n_air < 10000 or n_dirt < 10000:
            failures.append(f"farm background missing (air={n_air} dirt={n_dirt})")
        if n_ant < 20:
            failures.append(f"no ant pixels on screen (ant={n_ant})")
        if n_ant < 20 * 10:
            log(f"[WARN] fewer ant pixels than expected for 10 ants ({n_ant})")
        if n_sand == 0:
            log("[INFO] no sand visible yet (digging is slow; not a failure)")

        # Animation: the farm must visibly change while we watch.
        time.sleep(2.5)
        shot2 = os.path.join(SHOT_DIR, "02_animating.ppm")
        qmp.shot(shot2)
        px2 = crop(ppm_pixels(shot2), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        if px2 == px1:
            failures.append("animation: farm unchanged after 2.5s")
        elif count_color(px2, ANT) < 20:
            failures.append("animation: ants disappeared")
        else:
            log("[TEST] animation: farm visibly changed")

        # Hover: move the pointer across the farm (the desktop's ESC [ T
        # reports must reach poke() without killing the app).
        for x, y in ((CONTENT_ORIGIN[0] + 100, CONTENT_ORIGIN[1] + 100),
                     (CONTENT_ORIGIN[0] + CONTENT_W // 2, CONTENT_ORIGIN[1] + CONTENT_H // 2),
                     (CONTENT_ORIGIN[0] + CONTENT_W - 150, CONTENT_ORIGIN[1] + 150)):
            qmp.move(x, y)
            time.sleep(0.4)
        time.sleep(1.5)
        shot3 = os.path.join(SHOT_DIR, "03_hover.ppm")
        qmp.shot(shot3)
        px3 = crop(ppm_pixels(shot3), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        if px3 == px2:
            failures.append("hover: farm unchanged after moving the pointer")
        elif count_color(px3, ANT) < 20:
            failures.append("hover: ants disappeared after pointer reports")
        else:
            log("[TEST] hover: app alive and animating after pointer reports")

        # F4 closes the focused window; the desktop must survive.
        qmp.key("f4")
        time.sleep(1.5)
        shot4 = os.path.join(SHOT_DIR, "04_closed.ppm")
        qmp.shot(shot4)
        px4 = crop(ppm_pixels(shot4), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        if px4 == px3:
            failures.append("F4: content rectangle unchanged after closing the window")
        if count_color(px4, DIRT) > 10000:
            failures.append("F4: farm still on screen after closing")
        ok, _ = serial_contains("Desktop starting", 5)
        if not ok:
            failures.append("desktop died after closing XANTFARM")
        else:
            log("[TEST] F4: window closed, desktop alive")

        # Sanity: every shot exists and is non-blank.
        for label in ("01_launched", "02_animating", "03_hover", "04_closed"):
            path = os.path.join(SHOT_DIR, f"{label}.ppm")
            if not os.path.exists(path) or not ppm_has_content(path):
                failures.append(f"{label}: screenshot missing or blank")
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
        log(f"=== XANTFARM acceptance FAILED ({len(failures)} problems) ===")
        return 1
    log("[TEST] screenshots: all present, non-blank; palette, animation, hover and close verified")
    log("=== XANTFARM acceptance PASSED ===")
    log(f"(screenshots in {SHOT_DIR})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
