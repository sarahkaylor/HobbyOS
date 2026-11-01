#!/usr/bin/env python3
"""Capture the README screenshots from a live HobbyOS IceWM desktop.

`make run` boots HobbyOS into an X11 session (XHOBBY.BIN + IceWM) by default;
this script reproduces the screenshots in `docs/screenshots/` from that
desktop:

  icewm-desktop     FILES + CONSOLE (with real command output) + SYSMON +
                    CLOCK + the X11 xeyes arranged on the IceWM desktop
  icewm-browser     the WebKit browser on a live Wikipedia article (HTTPS)
  icewm-games       PONG, STARDUST and XNIBBLER running in IceWM windows
  icewm-apps-menu   the start menu with the Apps submenu open
  icewm-games-menu  the start menu with the Games submenu open
  icewm-x11         the real-Xlib clients XCALC and XEYES plus ANTFARM

Every scene boots its own headless QEMU (intel + KVM) with a QMP socket, drives
the UI with QMP `input-send-event`/`send-key`, and screendumps the framebuffer.
Each new window is located by diffing a before/after screendump and is then
dragged into place by its title bar, so the layout does not depend on IceWM's
placement policy. Nothing is asserted: the operator picks the frames.

    python3 capture_screenshots.py                  # all scenes -> docs/screenshots
    python3 capture_screenshots.py --only browser    # one scene
    python3 capture_screenshots.py --out /tmp/shots  # somewhere else

Requires a built `disk.img` with the IceWM desktop:

    make DESKTOP=icewm disk.img

For the older native HobbyOS desktop (`make run DESKTOP=hobby`), use
`capture_screenshots_hobby.py`.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
W, H = 1920, 1080
CONVERT = os.path.join(REPO, "skills/hobbyos-screenshot/scripts/ppm2png.py")
# Where the pointer is parked before any measurement screenshot, so the drawn
# cursor does not land inside the window being measured.
NEUTRAL = (1700, 60)

# Start-menu geometry of the generated /ICEWM/menu on this 1920x1080 screen --
# the same numbers run_icewm_test.py drives: the start button sits bottom-left,
# the Apps submenu rows are 21 px apart starting at y=750, and the Games
# submenu's first row lines up with the "Games" row (y=857).
START_BTN = (25, H - 13)
APPS_ROW = (60, 836)
GAMES_ROW = (60, 857)
APPS_Y0, GAMES_Y0, ROW_H = 750, 857, 21

APPS = ["BROWSER", "CONSOLE", "FILES", "CALC", "XCALC", "ANTFARM", "XEYES",
        "CLOCK", "SYSMON", "HEX", "TASKS", "FIND", "DIFF", "NOTES", "NANO", "UNIT"]
GAMES = ["PONG", "MILLIPED", "DOOM", "STARDUST", "XNIBBLER", "XCRYPT", "NES"]


def log(msg):
    print(msg, flush=True)


# ------------------------------------------------------------------ QMP ---
class Qmp:
    def __init__(self, path, deadline_s=60):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(30)
        end = time.time() + deadline_s
        while True:
            try:
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise RuntimeError("QMP socket never appeared")
                time.sleep(0.3)
        self.s.recv(65536)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})

    def button(self, down):
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}}]})

    def click(self, x, y, settle=0.4):
        self.move(x, y)
        time.sleep(0.15)
        self.button(True)
        time.sleep(0.09)
        self.button(False)
        time.sleep(settle)

    def drag(self, x1, y1, x2, y2, steps=14, settle=0.5):
        self.move(x1, y1)
        time.sleep(0.25)
        self.button(True)
        time.sleep(0.25)
        for i in range(1, steps + 1):
            self.move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
            time.sleep(0.05)
        time.sleep(0.25)
        self.button(False)
        time.sleep(settle)

    def key(self, q):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": k} for k in q.split("+")]})
        time.sleep(0.12)

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path})
        time.sleep(0.7)
        if "error" in r:
            raise RuntimeError(f"screendump failed: {r}")


KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal",
    ".": "dot", ",": "comma", "/": "slash", ";": "semicolon", "'": "apostrophe",
    "[": "bracket_left", "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
    ":": "shift+semicolon", "_": "shift+minus", '"': "shift+apostrophe",
    "!": "shift+1", "@": "shift+2", "#": "shift+3", "$": "shift+4", "%": "shift+5",
    "^": "shift+6", "&": "shift+7", "*": "shift+8", "(": "shift+9", ")": "shift+0",
    "?": "shift+slash", ">": "shift+dot", "<": "shift+comma", "+": "shift+equal",
    "|": "shift+backslash", "{": "shift+bracket_left", "}": "shift+bracket_right",
    "~": "shift+grave_accent",
}


def type_text(q, text):
    for ch in text:
        if ch in KEYMAP:
            q.key(KEYMAP[ch])
        elif ch.isupper():
            q.key("shift+" + ch.lower())
        else:
            q.key(ch)


# --------------------------------------------------------------- images ---
def ppm_pixels(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        return None, 0, 0
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h = fields[0], fields[1]
    return data[i:i + w * h * 3], w, h


def to_png(ppm, png):
    subprocess.run([sys.executable, CONVERT, ppm, png], check=True)


def diff_bbox(a_path, b_path, tol=14):
    """Bounding box (x0, y0, x1, y1) of the *new window* between two shots.

    Only pixels that were bare wallpaper in `a` and are not wallpaper in `b`
    count: everything else on the desktop (the taskbar's new task button,
    animating games inside older windows) changes without being the new
    window. IceWM places a new window in free space, so this is exactly the
    window's rectangle.
    """
    a, w, h = ppm_pixels(a_path)
    b, _, _ = ppm_pixels(b_path)
    if not a or not b:
        return None
    WALL = (80, 80, 120)
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            o = row + x * 3
            if (abs(a[o] - WALL[0]) > 18 or abs(a[o + 1] - WALL[1]) > 18
                    or abs(a[o + 2] - WALL[2]) > 22):
                continue                      # not wallpaper before: not the new window
            if (abs(a[o] - b[o]) > tol or abs(a[o + 1] - b[o + 1]) > tol
                    or abs(a[o + 2] - b[o + 2]) > tol):
                x0 = min(x0, x)
                x1 = max(x1, x)
                y0 = min(y0, y)
                y1 = max(y1, y)
    if x1 < 0:
        return None
    return (x0, y0, x1 + 2, y1 + 2)


def find_xeyes(s):
    """The session launcher starts XEYESR.BIN at boot; its window is the small
    cream-coloured one in the top-left corner. Locate it by its background."""
    ppm = os.path.join(s.work, "xeyes-locate.ppm")
    s.qmp.shot(ppm)
    data, w, h = ppm_pixels(ppm)
    if not data:
        return None
    for y in range(0, 400, 3):
        o = (y * w + 8) * 3
        if data[o] > 220 and data[o + 1] > 210 and data[o + 2] > 190:
            right = 0
            for x in range(4, w // 3):
                oo = (y * w + x) * 3
                if data[oo] > 200 and data[oo + 1] > 190:
                    right = x
            if right > 60:
                return (0, 0, right + 4, 220)
    return None


# ---------------------------------------------------------------- scene ---
class Session:
    """One headless HobbyOS boot: QMP control, serial reading, screenshots."""

    def __init__(self, workdir):
        self.work = workdir
        os.makedirs(workdir, exist_ok=True)
        self.disk = os.path.join(workdir, "disk.img")
        shutil.copy(os.path.join(REPO, "disk.img"), self.disk)
        fw = os.path.join(workdir, "ovmf.fd")
        shutil.copy(os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"), fw)
        self.qmp_path = os.path.join(workdir, "qmp-sock")
        self.serial_path = os.path.join(workdir, "serial.log")
        for stale in (self.qmp_path, self.serial_path):
            try:
                os.unlink(stale)
            except OSError:
                pass
        cmd = ["qemu-system-x86_64", "-M", "q35", "-smp", "4", "-m", "8192M",
               "-drive", f"if=pflash,format=raw,file={fw}",
               "-display", "none", "-serial", f"file:{self.serial_path}",
               "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
               "-drive", f"file={self.disk},format=raw,id=disk0,if=none",
               "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
               "-device", "edu", "-netdev", "user,id=net0",
               "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
               "-device", "virtio-gpu-pci", "-vga", "none",
               "-action", "shutdown=poweroff",
               "-qmp", f"unix:{self.qmp_path},server,nowait"]
        if os.path.exists("/dev/kvm") and not os.environ.get("SKIP_KVM"):
            cmd.append("-enable-kvm")
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL,
                                     stderr=open(os.path.join(workdir, "qemu.err"), "w"),
                                     start_new_session=True)
        self.qmp = Qmp(self.qmp_path)
        self.nshot = 0

    def serial(self):
        try:
            return open(self.serial_path, "rb").read().decode(errors="replace")
        except OSError:
            return ""

    def wait_serial(self, needle, deadline_s=120, start=0):
        end = time.time() + deadline_s
        while time.time() < end:
            i = self.serial().find(needle, start)
            if i >= 0:
                return i
            time.sleep(0.4)
        return -1

    def wait_session(self, deadline_s=180):
        if self.wait_serial("[XSESS] session running", deadline_s) < 0:
            raise RuntimeError("IceWM session never came up (see serial.log)")
        if not self.wait_desktop():
            raise RuntimeError("desktop never painted (no taskbar / wallpaper)")

    def wait_desktop(self, timeout=90):
        """Wait until IceWM has actually painted: the start button is light and
        the top-right corner is bare wallpaper. Without this the first window
        diff can compare a black framebuffer with a painted desktop and report
        the whole screen as the new window."""
        ppm = os.path.join(self.work, "desktop-probe.ppm")
        end = time.time() + timeout
        while time.time() < end:
            self.qmp.shot(ppm)
            d, w, h = ppm_pixels(ppm)
            if d:
                o = ((h - 13) * w + 25) * 3          # start button
                c = (5 * w + (w - 5)) * 3            # top-right corner
                taskbar = d[o] > 170 and d[o + 1] > 165
                corner = abs(d[c] - 80) < 18 and abs(d[c + 2] - 120) < 22
                if taskbar and corner:
                    return True
            time.sleep(2)
        return False

    def shot(self, name, png_dir=None):
        self.nshot += 1
        ppm = os.path.join(self.work, f"{self.nshot:02d}-{name}.ppm")
        self.qmp.shot(ppm)
        png = os.path.join(png_dir or self.work, f"{name}.png")
        to_png(ppm, png)
        log(f"[shot] {png}")
        return ppm, png

    # --- desktop actions -------------------------------------------------
    def close_menu(self):
        self.qmp.key("esc")
        self.qmp.key("esc")
        time.sleep(0.6)

    def light_pixels(self, ppm, x0, y0, x1, y1, step=4):
        """Count menu-panel pixels (light grey) in a region above the taskbar."""
        d, w, _ = ppm_pixels(ppm)
        if not d:
            return 0
        n = 0
        for y in range(y0, y1, step):
            row = y * w * 3
            for x in range(x0, x1, step):
                o = row + x * 3
                if d[o] > 170 and d[o + 1] > 165 and d[o + 2] > 165:
                    n += 1
        return n

    def wait_menu(self, x0, y0, x1, y1, min_light, timeout=60):
        """Poll until a menu panel covers the region (IceWM's first menu open
        scans fontconfig and can take many seconds)."""
        cur = os.path.join(self.work, "menu-probe.ppm")
        end = time.time() + timeout
        while time.time() < end:
            self.qmp.shot(cur)
            if self.light_pixels(cur, x0, y0, x1, y1) >= min_light:
                return True
            time.sleep(1.5)
        return False

    def open_menu(self, row):
        # The very first start-menu open can swallow the click while IceWM
        # scans fontconfig (seconds, and long on a cold boot), so click, look,
        # and click again if nothing came up.
        opened = False
        for _ in range(6):
            self.qmp.click(*START_BTN)
            if self.wait_menu(2, 700, 168, 1048, 400, timeout=8):
                opened = True
                break
            time.sleep(1.0)
        if not opened:
            raise RuntimeError("start menu did not open")
        self.qmp.move(*row)
        time.sleep(1.2)
        if not self.wait_menu(174, 700, 330, 1048, 300, timeout=30):
            raise RuntimeError("start-menu submenu did not open")
        time.sleep(0.8)

    def launch(self, name, wait_s=90):
        """Launch a native app from the start menu (Apps or Games submenu)."""
        if name in GAMES:
            i, row, y0 = GAMES.index(name), GAMES_ROW, GAMES_Y0
        else:
            i, row, y0 = APPS.index(name), APPS_ROW, APPS_Y0
        self.close_menu()
        # Park the pointer off every window before both frames: the cursor is
        # drawn into the framebuffer, and a diff would otherwise report it as
        # part of the new window.
        self.qmp.move(*NEUTRAL)
        time.sleep(0.5)
        self.qmp.shot(os.path.join(self.work, "settle.ppm"))   # force a flush
        time.sleep(1.5)
        before = os.path.join(self.work, "before-launch.ppm")
        self.qmp.shot(before)
        self.open_menu(row)
        # Take the log position BEFORE the click: HBHOST writes its launch
        # line within milliseconds, so a position read afterwards can already
        # be past the marker we are waiting for.
        start = len(self.serial())
        self.qmp.click(200, y0 + ROW_H * i)
        mark = f"[HBHOST] {name}.BIN pid="
        if self.wait_serial(mark, wait_s, start) < 0 and mark not in self.serial():
            raise RuntimeError(f"{name} did not start from the start menu")
        time.sleep(10 if name == "BROWSER" else 6)
        self.qmp.move(*NEUTRAL)
        time.sleep(0.6)
        after = os.path.join(self.work, "after-launch.ppm")
        self.qmp.shot(after)
        rect = diff_bbox(before, after)
        if rect and (rect[2] - rect[0]) * (rect[3] - rect[1]) > 0.7 * W * H:
            # A whole-screen diff means the two shots are of different desktop
            # states, not of a new window -- re-measure against a fresh frame.
            log(f"[launch] {name}: diff covered the screen, re-measuring")
            time.sleep(3)
            self.qmp.shot(after)
            rect = diff_bbox(before, after)
        log(f"[launch] {name} rect={rect}")
        return rect

    def place(self, rect, x, y):
        """Drag a window by its title bar so its top-left lands on (x, y)."""
        if not rect:
            return
        x0, y0, x1, _ = rect
        gx, gy = (x0 + x1) // 2, y0 + 9
        self.qmp.drag(gx, gy, gx + (x - x0), gy + (y - y0))
        time.sleep(0.8)

    def resize(self, rect, w, h):
        """Drag the bottom-right corner so the window becomes w x h."""
        if not rect:
            return rect
        x0, y0, x1, y1 = rect
        self.qmp.drag(x1 - 2, y1 - 2, x1 - 2 + (w - (x1 - x0)), y1 - 2 + (h - (y1 - y0)), steps=16)
        time.sleep(1.8)      # the app repaints at the new size
        return (x0, y0, x0 + w, y0 + h)

    def fit(self, rect, x, y, w, h):
        """Resize a window to w x h and move it so its top-left is (x, y)."""
        rect = self.resize(rect, w, h)
        self.place(rect, x, y)
        time.sleep(1.2)

    def focus(self, rect):
        x0, y0, x1, y1 = rect
        self.qmp.click((x0 + x1) // 2, (y0 + y1) // 2)
        time.sleep(1.0)

    def close_window(self, rect):
        """Focus a window by its title bar and close it with F4."""
        if not rect:
            return
        x0, y0, x1, _ = rect
        self.qmp.click((x0 + x1) // 2, y0 + 9)
        time.sleep(1.0)
        self.qmp.key("f4")
        time.sleep(2.0)

    def type_command(self, rect, text):
        self.focus(rect)
        type_text(self.qmp, text)
        self.qmp.key("ret")
        time.sleep(2.2)


# ---------------------------------------------------------------- scenes ---
# Natural window sizes IceWM/HBHOST hand out on this screen (measured from a
# live session, `tools/measure_apps.py`): text apps 808x588, PONG/STARDUST
# 968x568, XNIBBLER 528x404, XCALC 328x524, ANTFARM 568x444, xeyes 330x227.
TEXT_W, TEXT_H = 900, 480


def scene_desktop(s, png_dir):
    """FILES + CONSOLE + SYSMON + CLOCK on the IceWM desktop."""
    s.close_window(find_xeyes(s))        # the boot-time Xlib demo makes way
    files = s.launch("FILES")
    s.fit(files, 20, 20, TEXT_W, TEXT_H)
    console = s.launch("CONSOLE")
    s.fit(console, 960, 20, TEXT_W, TEXT_H)
    for cmd in ("ls /home", "free", "ps"):
        s.type_command(console, cmd)
    sysmon = s.launch("SYSMON")
    s.fit(sysmon, 20, 530, TEXT_W, TEXT_H)
    clock = s.launch("CLOCK")
    s.fit(clock, 960, 530, TEXT_W, TEXT_H)
    time.sleep(3)
    return s.shot("icewm-desktop", png_dir)[1]


def scene_browser(s, png_dir):
    """The WebKit browser on a live Wikipedia article."""
    rect = s.launch("BROWSER", wait_s=120)
    s.place(rect, 0, 0)
    time.sleep(2)
    s.qmp.click(900, 82)          # address bar
    time.sleep(1)
    s.qmp.key("ctrl+a")
    type_text(s.qmp, "https://en.wikipedia.org/wiki/Hobbyist_operating_system")
    s.qmp.key("ret")
    start = len(s.serial())
    if s.wait_serial("[WIN] load-ok", 180, start) < 0:
        raise RuntimeError("Wikipedia did not load (see serial.log)")
    time.sleep(15)                # CSS + subresources
    s.qmp.move(*NEUTRAL)
    time.sleep(0.5)
    return s.shot("icewm-browser", png_dir)[1]


def scene_games(s, png_dir):
    """PONG, STARDUST and XNIBBLER side by side."""
    pong = s.launch("PONG")
    s.fit(pong, 10, 10, 940, 520)
    star = s.launch("STARDUST")
    s.fit(star, 960, 10, 940, 520)
    nib = s.launch("XNIBBLER")
    s.fit(nib, 10, 545, 528, 400)
    time.sleep(5)                 # let the games animate
    s.qmp.move(*NEUTRAL)
    time.sleep(0.5)
    return s.shot("icewm-games", png_dir)[1]


def scene_apps_menu(s, png_dir):
    """Start menu with the Apps submenu open, over a working desktop."""
    files = s.launch("FILES")
    s.place(files, 330, 0)
    console = s.launch("CONSOLE")
    s.place(console, 1104, 0)
    s.type_command(console, "ls /home")
    s.close_menu()
    s.open_menu(APPS_ROW)
    time.sleep(2)
    return s.shot("icewm-apps-menu", png_dir)[1]


def scene_games_menu(s, png_dir):
    """Start menu with the Games submenu open, PONG and STARDUST running."""
    pong = s.launch("PONG")
    s.fit(pong, 0, 170, 940, 620)
    star = s.launch("STARDUST")
    s.fit(star, 960, 170, 940, 620)
    s.close_menu()
    s.open_menu(GAMES_ROW)
    time.sleep(2)
    return s.shot("icewm-games-menu", png_dir)[1]


def scene_x11(s, png_dir):
    """The real-Xlib clients XCALC and XEYES plus the ANTFARM native app."""
    xeyes = find_xeyes(s)
    s.place(xeyes, 1290, 240)                    # the boot-time Xlib demo
    xcalc = s.launch("XCALC")
    s.place(xcalc, 20, 170)
    time.sleep(2)
    # The first click only focuses the window under IceWM's click-to-focus,
    # so spend it on the display before pressing any button.
    s.qmp.click(170, 230)
    time.sleep(1.0)
    # 2 ENTER 3 +  on the calculator's button grid. The grid lines measured in
    # the 328x524 window are x = 28/105/183/261/339 and y = 138/201/265/329/
    # 393/457/521, so the buttons are 4 columns x 6 rows: 2 = (144,425),
    # ENTER = (222,489), 3 = (222,425), + = (300,297) -- relative to the frame.
    for (bx, by) in ((144, 425), (222, 489), (222, 425), (300, 297)):
        s.qmp.click(bx + 20, by + 170)
        time.sleep(0.8)
    ant = s.launch("ANTFARM")
    s.fit(ant, 380, 120, 880, 700)
    time.sleep(4)
    s.qmp.move(*NEUTRAL)
    time.sleep(0.5)
    return s.shot("icewm-x11", png_dir)[1]


SCENES = {
    "desktop": scene_desktop,
    "browser": scene_browser,
    "games": scene_games,
    "apps-menu": scene_apps_menu,
    "games-menu": scene_games_menu,
    "x11": scene_x11,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--only", default=None, choices=sorted(SCENES),
                    help="capture one scene (default: all of them)")
    ap.add_argument("--out", default=os.path.join(REPO, "docs/screenshots"))
    ap.add_argument("--work", default="/tmp/hobbyos_readme_shots")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    names = [args.only] if args.only else list(SCENES)
    failed = []
    for name in names:
        work = os.path.join(args.work, name)
        shutil.rmtree(work, ignore_errors=True)
        log(f"[scene] {name}")
        s = Session(work)
        t0 = time.time()
        try:
            s.wait_session()
            png = SCENES[name](s, args.out)
            log(f"[scene] {name} -> {png}  ({time.time() - t0:.0f}s)")
        except Exception as e:      # bounded harness: report, never hang
            log(f"[scene] {name} FAILED: {e}")
            failed.append(name)
        finally:
            if s.proc.poll() is None:
                s.proc.terminate()
                try:
                    s.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    s.proc.kill()
    if failed:
        log(f"[scene] failed: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
