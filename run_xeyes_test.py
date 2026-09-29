#!/usr/bin/env python3
"""
Boot the real HobbyOS desktop, launch XEYES (the xeyes port) from the
start menu through the pixel-window path, and verify that the pupils
follow the mouse, capturing a screenshot at every step.

What it verifies:
  1. launch   - serial marker "[APP] XEYES started" + a visible window
                (rims and pupils in ink on the paper background)
  2. idle     - two shots seconds apart with NO input are identical:
                xeyes is input-driven, so a still pointer must draw
                nothing (the drawEye() pixel check + poll ladder)
  3. tracking - move the pointer to the left / right / top / bottom of
                the window; the pupils' centroid inside each eye must
                move the same way (and by a real distance), on screen
  4. menu     - with XEYES up, the Apps menu stays visible above the
                window (the app must not blit over it), survives pointer
                motion, and its items are still clickable; once it closes
                the window repaints what the menu had covered
  5. F4       - closes the window and the desktop comes back

Usage:  python3 run_xeyes_test.py   (or: make xeyes_test)
Result: exit 0 on success; screenshots in /tmp/hobbyos_xeyes/.
"""
import json
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

QMP_SOCK = "./qmp-sock-xeyes"
SERIAL_LOG = "/tmp/xeyes_serial.log"
SHOT_DIR = "/tmp/hobbyos_xeyes"
QEMU_BIN = "qemu-system-aarch64"

W, H = 1024, 768
TASKBAR_Y = H - 26

# The desktop's single-window tile is the whole area above the taskbar;
# the content rectangle is the window minus frame (2px), title (16) and
# menu bar (16) -- keep in sync with wm_pixel_content_rect().
CONTENT_ORIGIN = (2, 34)
CONTENT_W, CONTENT_H = W - 4, (H - 26) - 36

# Palette of src/user/x11/apps/xeyes/main.c.
PAPER = (0xF2, 0xEF, 0xE9)
INK = (0x26, 0x2B, 0x33)

# The Apps menu (draw_start_menu): a 220px panel of this fill, 16 rows.
MENU_PANEL = (232, 234, 240)
MENU_VISIBLE_ROWS = 16

# The port's eye geometry (thousandths of an eye unit): centers at 0 and
# 2000, the window spanning x in [-900, 2900]; BALL_DIST and the pupil
# diameter bound how far the pupil can travel from its eye's center.
EYE_X_MILLI = (0, 2000)
EYE_W_SPAN = 3800
EYE_W_MIN = -900
BALL_DIST = 400
BALL_DIAM = 300


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


def pix_at(pixels, x, y):
    """One screen pixel as an (r,g,b) tuple."""
    i = (y * W + x) * 3
    return (pixels[i], pixels[i + 1], pixels[i + 2])


def eye_centers():
    """The eye centers (screen coords) for the tile-sized window, from
    the port's transform: x_px(v) = round((v + 900) * width / 3800)."""
    cx, cy = [], None
    for v in EYE_X_MILLI:
        cx.append(CONTENT_ORIGIN[0] + ((v - EYE_W_MIN) * CONTENT_W + EYE_W_SPAN // 2) // EYE_W_SPAN)
    cy = CONTENT_ORIGIN[1] + ((0 - EYE_W_MIN) * CONTENT_H + 1800 // 2) // 1800
    return cx, cy


def pupil_centroid(pixels, cx, cy):
    """Centroid of the ink pixels inside (not on) the rim ring.  The rim
    is an ellipse (the port scales per axis), so the inner radii come
    from the eye diameter 1450 in eye units: 1450/3800 of the content
    width and 1450/1800 of its height.  Ink within 0.9 of those radii
    is pupil: the ring's inner edge cannot reach in there, and the pupil
    (capped at BALL_DIST travel plus its own radius) cannot escape."""
    a_in = (1450 * CONTENT_W) // 3800 // 2
    b_in = (1450 * CONTENT_H) // 1800 // 2
    a = (0.9 * a_in) ** 2
    b = (0.9 * b_in) ** 2
    lim = a * b
    sx = sy = n = 0.0
    y0, y1 = max(0, cy - b_in), min(H - 1, cy + b_in)
    x0, x1 = max(0, cx - a_in), min(W - 1, cx + a_in)
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            dx, dy = x - cx, y - cy
            if dx * dx * b + dy * dy * a > lim:
                continue
            if pix_at(pixels, x, y) == INK:
                sx += x
                sy += y
                n += 1
    if n == 0:
        return None
    return (sx / n, sy / n, n)


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

        xeyes_idx = None
        for k, v in menu_map.items():
            if v == "XEYES.BIN":
                xeyes_idx = k
        if xeyes_idx is None:
            log("[FAIL] XEYES.BIN missing from the desktop menu (apps launcher)")
            return 1
        ups = max(menu_map.keys()) - xeyes_idx
        log(f"[INFO] XEYES.BIN at menu index {xeyes_idx} ({ups} ups from the end)")

        # Launch XEYES from the start menu.
        qmp.click(5, 5)
        time.sleep(0.3)
        qmp.click(38, TASKBAR_Y + 13)
        time.sleep(0.5)
        qmp.key("end")
        for _ in range(ups):
            qmp.key("up")
        qmp.key("ret")

        ok, mark = serial_contains("[APP] XEYES started", 20)
        if not ok:
            log("[FAIL] XEYES never started (no serial marker)")
            return 1
        log("[TEST] launch: marker OK")

        time.sleep(1.5)
        shot1 = os.path.join(SHOT_DIR, "01_launched.ppm")
        qmp.shot(shot1)
        if not ppm_has_content(shot1):
            failures.append("01_launched: screenshot blank")
        px1 = crop(ppm_pixels(shot1), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        n_paper, n_ink = count_color(px1, PAPER), count_color(px1, INK)
        log(f"[INFO] palette: paper={n_paper} ink={n_ink}")
        if n_paper < 10000:
            failures.append(f"paper background missing (paper={n_paper})")
        if n_ink < 20000:
            failures.append(f"eyes missing (ink={n_ink}; rims alone are tens of thousands)")
        else:
            log("[TEST] launch: eyes drawn (paper + ink on screen)")

        eye_cx, eye_cy = eye_centers()
        log(f"[INFO] eye centers on screen: eye0=({eye_cx[0]},{eye_cy}) "
            f"eye1=({eye_cx[1]},{eye_cy})")

        # Idle: with no input xeyes must redraw nothing; the window's
        # content is byte-identical seconds later.
        time.sleep(2.5)
        shot2 = os.path.join(SHOT_DIR, "02_idle.ppm")
        qmp.shot(shot2)
        px2 = crop(ppm_pixels(shot2), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        if px2 != px1:
            failures.append("idle: content changed with no input (idle churn)")
        else:
            log("[TEST] idle: no input, no redraw (content identical)")

        # Tracking: move the pointer around the window; both pupils must
        # follow.  Shot pairs at opposite sides of each axis.
        cx_mid = CONTENT_ORIGIN[0] + CONTENT_W // 2
        cy_mid = CONTENT_ORIGIN[1] + CONTENT_H // 2
        positions = {
            "left": (CONTENT_ORIGIN[0] + 20, cy_mid),
            "right": (CONTENT_ORIGIN[0] + CONTENT_W - 20, cy_mid),
            "top": (cx_mid, CONTENT_ORIGIN[1] + 12),
            "bottom": (cx_mid, CONTENT_ORIGIN[1] + CONTENT_H - 12),
        }
        centroids = {}
        for name, (mx, my) in positions.items():
            qmp.move(mx, my)
            time.sleep(1.0)  # hover (30ms) + poll (<=400ms) + flush
            shot = os.path.join(SHOT_DIR, f"03_track_{name}.ppm")
            qmp.shot(shot)
            px = ppm_pixels(shot)
            for eye in (0, 1):
                c = pupil_centroid(px, eye_cx[eye], eye_cy)
                if c is None:
                    failures.append(f"tracking/{name}: no pupil pixels in eye {eye}")
                    centroids[(name, eye)] = None
                    continue
                centroids[(name, eye)] = c
            for eye in (0, 1):
                c = centroids[(name, eye)]
                if c:
                    log(f"[INFO] {name:6s} eye{eye}: pupil centroid=({c[0]:.1f},{c[1]:.1f}) "
                        f"n={int(c[2])}")

        for eye in (0, 1):
            l, r = centroids.get(("left", eye)), centroids.get(("right", eye))
            t, b = centroids.get(("top", eye)), centroids.get(("bottom", eye))
            if l and r:
                if r[0] - l[0] > 30:
                    log(f"[TEST] tracking: eye {eye} follows the mouse left/right "
                        f"(dx={r[0] - l[0]:.0f}px)")
                else:
                    failures.append(f"tracking: eye {eye} did not follow left->right "
                                    f"(dx={r[0] - l[0]:.1f}px)")
            if t and b:
                if b[1] - t[1] > 30:
                    log(f"[TEST] tracking: eye {eye} follows the mouse up/down "
                        f"(dy={b[1] - t[1]:.0f}px)")
                else:
                    failures.append(f"tracking: eye {eye} did not follow top->bottom "
                                    f"(dy={b[1] - t[1]:.1f}px)")

        # A far mouse parks the pupil away from the center: magnitude.
        for eye in (0, 1):
            l = centroids.get(("left", eye))
            if l:
                off = abs(l[0] - eye_cx[eye])
                if off > 60:
                    log(f"[TEST] tracking: eye {eye} pupil parked off-center "
                        f"({off:.0f}px from the eye center)")
                else:
                    failures.append(f"tracking: eye {eye} pupil barely left the "
                                    f"center with the mouse far left ({off:.1f}px)")

        # --- Menu over the window ------------------------------------------
        # With XEYES up, the Apps menu must stay visible above the app's
        # pixels: the app blits its shadow over the region the menu
        # occupies as soon as the menu opens unless the desktop holds the
        # repair requests back.  And a menu item must still be clickable.
        log("[STEP] menu over the window")
        menu_h = MENU_VISIBLE_ROWS * 20 + 8
        menu_y = TASKBAR_Y - menu_h
        mr_y0 = menu_y - 14

        def menu_region(px):
            return crop(px, 0, mr_y0, 240, TASKBAR_Y - mr_y0)

        qmp.click(38, TASKBAR_Y + 13)          # open the Apps menu
        time.sleep(0.8)
        shot5 = os.path.join(SHOT_DIR, "05_menu_open.ppm")
        qmp.shot(shot5)
        n_menu1 = count_color(menu_region(ppm_pixels(shot5)), MENU_PANEL)
        if n_menu1 < 20000:
            failures.append(f"menu: the Apps menu is covered with XEYES up "
                            f"(panel pixels={n_menu1})")
        else:
            log(f"[TEST] menu: the Apps menu stays above the window "
                f"(panel={n_menu1})")

        # Move across the menu and the window: the menu must stay put.
        for (mx, my) in ((110, menu_y + 30), (110, menu_y + 150),
                         (110, menu_y + 300), (700, 300), (300, 500),
                         (110, menu_y + 80)):
            qmp.move(mx, my)
            time.sleep(0.2)
        time.sleep(0.5)
        shot6 = os.path.join(SHOT_DIR, "06_menu_held.ppm")
        qmp.shot(shot6)
        n_menu2 = count_color(menu_region(ppm_pixels(shot6)), MENU_PANEL)
        if n_menu2 < int(n_menu1 * 0.9):
            failures.append(f"menu: the Apps menu decayed under pointer motion "
                            f"({n_menu1} -> {n_menu2})")
        else:
            log(f"[TEST] menu: held under pointer motion (panel={n_menu2})")

        # Click the FILES row: the menu routes the click and launches it.
        files_idx = next((k for k, v in menu_map.items() if v == "FILES.BIN"),
                         None)
        if files_idx is None or files_idx >= MENU_VISIBLE_ROWS:
            failures.append("menu: FILES.BIN not on the first page of the menu")
        else:
            _, mark_before = serial_contains("", 0.1)
            qmp.click(100, menu_y + 4 + files_idx * 20 + 10)
            ok, _ = serial_contains("[APP] FILES started", 20, since=mark_before)
            if ok:
                log("[TEST] menu: the item click launched FILES")
            else:
                failures.append("menu: clicking the FILES item did nothing")
            time.sleep(0.8)
            shot7 = os.path.join(SHOT_DIR, "07_menu_clicked.ppm")
            qmp.shot(shot7)
            n_menu3 = count_color(menu_region(ppm_pixels(shot7)), MENU_PANEL)
            if n_menu3 > 2000:
                failures.append(f"menu: menu still on screen after the click "
                                f"({n_menu3})")

            # Close FILES (F4); XEYES retiles to the full tile.
            qmp.key("f4")
            time.sleep(1.5)

        # Focus XEYES through its taskbar button; its content must be back
        # where the menu and FILES had covered it.
        qmp.click(100, TASKBAR_Y + 13)
        time.sleep(1.2)
        shot8 = os.path.join(SHOT_DIR, "08_restored.ppm")
        qmp.shot(shot8)
        px8 = ppm_pixels(shot8)
        px_last = crop(px8, CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                       CONTENT_W, CONTENT_H)
        n_ink8 = count_color(px_last, INK)
        if n_ink8 < 20000:
            failures.append(f"menu: window content not restored after the menu "
                            f"closed (ink={n_ink8})")
        else:
            log("[TEST] menu: window content restored after the menu closed")
        n_restore = (count_color(menu_region(px8), PAPER) +
                     count_color(menu_region(px8), INK))
        if n_restore < 20000:
            failures.append(f"menu: the area the menu covered is not repainted "
                            f"({n_restore} content pixels)")
        else:
            log(f"[TEST] menu: the covered area is repainted "
                f"({n_restore} content pixels)")

        # F4 closes the focused window; the desktop must survive.
        qmp.key("f4")
        time.sleep(1.5)
        shot4 = os.path.join(SHOT_DIR, "04_closed.ppm")
        qmp.shot(shot4)
        px4 = crop(ppm_pixels(shot4), CONTENT_ORIGIN[0], CONTENT_ORIGIN[1],
                   CONTENT_W, CONTENT_H)
        if px4 == px_last:
            failures.append("F4: content rectangle unchanged after closing the window")
        if count_color(px4, INK) > 1000:
            failures.append("F4: eyes still on screen after closing")
        if count_color(px4, PAPER) > 10000:
            failures.append("F4: window content still on screen after closing")
        ok, _ = serial_contains("Desktop starting", 5)
        if not ok:
            failures.append("desktop died after closing XEYES")
        else:
            log("[TEST] F4: window closed, desktop alive")

        # Sanity: every shot exists and is non-blank.
        for label in (["01_launched", "02_idle", "04_closed", "05_menu_open",
                       "06_menu_held", "07_menu_clicked", "08_restored"] +
                      [f"03_track_{name}" for name in positions]):
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
        log(f"=== XEYES acceptance FAILED ({len(failures)} problems) ===")
        return 1
    log("[TEST] screenshots: all present, non-blank; launch, idle thrift and "
        "pupil tracking verified")
    log("=== XEYES acceptance PASSED ===")
    log(f"(screenshots in {SHOT_DIR})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
