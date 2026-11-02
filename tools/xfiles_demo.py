#!/usr/bin/env python3
"""Drive the HobbyOS IceWM desktop, launch Files (X11) and record a demo.

Boots the built disk.img under qemu (intel/KVM), waits for the IceWM session,
launches /XFILES.BIN from the start menu, then walks the browser (selection,
path prompt, sorting, help, opening a file) while capturing screenshots and
an MP4 of the framebuffer.

  python3 tools/xfiles_demo.py --work /tmp/xfiles-demo

Outputs: <work>/shots/*.png, <work>/xfiles-demo.mp4, <work>/serial.log
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCREEN = (1920, 1080)


def log(m):
    print(f"[demo] {m}", flush=True)


def img(path):
    from PIL import Image
    return Image.open(path).convert("RGB")


class Qmp:
    def __init__(self, path, deadline=40, screen=SCREEN):
        self.screen = screen
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(15)
        end = time.time() + deadline
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > end:
                    raise RuntimeError("QMP socket never appeared")
                time.sleep(0.2)
        self.buf = b""
        self.lock = threading.Lock()
        self._line()
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, execute, arguments=None):
        # One QMP socket: the recorder thread and the driver both use it, so
        # every request/response pair must be atomic.
        with self.lock:
            msg = {"execute": execute}
            if arguments:
                msg["arguments"] = arguments
            self.sock.sendall(json.dumps(msg).encode() + b"\n")
            while True:
                r = self._line()
                if "return" in r or "error" in r:
                    return r

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path})
        if "error" in r:
            raise RuntimeError(f"screendump failed: {r}")

    def move(self, x, y):
        # QMP absolute axes are 0..32767, not pixels.
        w, h = self.screen
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": x * 32767 // w}},
            {"type": "abs", "data": {"axis": "y", "value": y * 32767 // h}}]})

    def click(self, x, y, button="left"):
        self.move(x, y)
        time.sleep(0.12)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": False}}]})

    def key(self, *qcodes):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": q} for q in qcodes]})

    def type_text(self, text):
        shift = {c: f"shift+{c}" for c in "abcdefghijklmnopqrstuvwxyz"}
        shift["_"] = "shift+minus"
        plain = {"/": "slash", ".": "dot", "-": "minus", " ": "spc"}
        for ch in text:
            low = ch.lower()
            if ch != low and low in shift:
                a, b = shift[low].split("+")
                self.key(a, b)
            elif low in "abcdefghijklmnopqrstuvwxyz0123456789":
                self.key(low)
            elif ch in plain:
                self.key(plain[ch])
            else:
                raise ValueError(f"no qcode for {ch!r}")
            time.sleep(0.03)


class Recorder(threading.Thread):
    """Framebuffer capture at ~4 fps into frames/fNNNN.png."""

    def __init__(self, qmp, outdir, fps=4.0):
        super().__init__(daemon=True)
        self.qmp, self.outdir, self.dt = qmp, outdir, 1.0 / fps
        self.stop = threading.Event()
        self.n = 0
        os.makedirs(outdir, exist_ok=True)

    def run(self):
        while not self.stop.is_set():
            ppm = os.path.join(self.outdir, "cur.ppm")
            try:
                self.qmp.shot(ppm)
                subprocess.run(["convert", ppm, "-resize", "1280x720",
                                os.path.join(self.outdir, f"f{self.n:04d}.png")],
                               stderr=subprocess.DEVNULL)
                self.n += 1
            except Exception:
                pass
            time.sleep(self.dt)


def build_qemu(work, disk):
    fw_src = os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd")
    fw = os.path.join(work, "ovmf.fd")
    shutil.copy(fw_src, fw)
    qmp = os.path.join(work, "qmp-sock")
    serial = os.path.join(work, "serial.log")
    cmd = ["qemu-system-x86_64", "-M", "q35", "-smp", "4", "-m", "8192M",
           "-drive", f"if=pflash,format=raw,file={fw}",
           "-display", "none", "-serial", f"file:{serial}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", f"file={disk},format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-device", "virtio-gpu-pci", "-vga", "none",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{qmp},server,nowait"]
    if os.path.exists("/dev/kvm") and not os.environ.get("SKIP_KVM"):
        cmd.append("-enable-kvm")
    return cmd, qmp, serial


def wait_serial(path, needle, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            txt = open(path, "rb").read().decode(errors="replace")
        except FileNotFoundError:
            txt = ""
        if needle in txt:
            return True
        time.sleep(0.3)
    return False


def find_menu(ppm):
    """Top-left of the light-grey menu panel above the taskbar."""
    im = img(ppm)
    w, h = im.size
    for y in range(400, h - 60, 4):
        hits = 0
        first = None
        for x in range(0, min(w, 700), 4):
            r, g, b = im.getpixel((x, y))
            if r > 150 and g > 150 and b > 145 and abs(r - g) < 12 and abs(g - b) < 16:
                hits += 1
                if first is None:
                    first = x
        if hits > 25:
            return (first, y)
    return None


def find_window(ppm, bg=(36, 36, 46), tol=4):
    """Bounding box of the app's client area (its dark background colour),
    found by requiring long runs of the colour per row and per column."""
    im = img(ppm)
    w, h = im.size
    rows = []
    for y in range(h):
        n = 0
        for x in range(0, w, 4):
            r, g, b = im.getpixel((x, y))
            if abs(r - bg[0]) <= tol and abs(g - bg[1]) <= tol and abs(b - bg[2]) <= tol:
                n += 1
        if n > 30:
            rows.append(y)
    if len(rows) < 40:
        return None
    y0, y1 = rows[0], rows[-1]
    cols = []
    for x in range(w):
        n = 0
        for y in range(y0, y1, 4):
            r, g, b = im.getpixel((x, y))
            if abs(r - bg[0]) <= tol and abs(g - bg[1]) <= tol and abs(b - bg[2]) <= tol:
                n += 1
        if n > 8:
            cols.append(x)
    if len(cols) < 40:
        return None
    return (cols[0], y0, cols[-1], y1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="/tmp/xfiles-demo")
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--no-video", action="store_true")
    ap.add_argument("--keep-open", action="store_true")
    args = ap.parse_args()

    work = args.work
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shots = os.path.join(work, "shots")
    os.makedirs(shots)
    frames = os.path.join(work, "frames")

    if not os.path.exists(args.disk):
        log(f"missing {args.disk}; run: make ARCH=intel DESKTOP=icewm disk.img")
        return 2

    cmd, qmp_path, serial = build_qemu(work, args.disk)
    log("booting qemu (intel, KVM)")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    failures = []
    try:
        if not wait_serial(serial, "IceWM session launcher starting", 120):
            log("FAIL: session launcher never started")
            print(open(serial, errors="replace").read()[-2000:])
            return 2
        q = Qmp(qmp_path)
        log("QMP up")

        last = {"ppm": os.path.join(work, "probe.ppm")}

        def snap(name, delay=0.35):
            ppm = os.path.join(work, "snap.ppm")
            q.shot(ppm)
            last["ppm"] = ppm
            subprocess.run(["convert", ppm, "-resize", "1600x900",
                            os.path.join(shots, name + ".png")],
                           stderr=subprocess.DEVNULL)
            log(f"shot {name}")
            time.sleep(delay)

        # ---- IceWM draws its taskbar a few seconds in
        log("waiting for the taskbar")
        H = SCREEN[1]
        for _ in range(80):
            q.shot(os.path.join(work, "probe.ppm"))
            r, g, b = img(os.path.join(work, "probe.ppm")).getpixel((25, H - 8))
            if r > 150 and g > 150:
                break
            time.sleep(0.5)
        snap("01-desktop")

        rec = None
        if not args.no_video:
            rec = Recorder(q, frames, fps=5)
            rec.start()

        # ---- start menu -> Files (X11) (first row)
        log("opening the start menu")
        menu = None
        # Check before clicking: a retry click on an already-open menu closes
        # it, which is how a slow first open turns into a flapping loop.
        for attempt in range(6):
            q.shot(os.path.join(work, "probe.ppm"))
            menu = find_menu(os.path.join(work, "probe.ppm"))
            if menu:
                break
            if attempt:
                log(f"  menu not open yet (attempt {attempt})")
            q.click(25, H - 13)
            time.sleep(2.0)
        if not menu:
            log("FAIL: start menu did not open")
            snap("02-menu-missing")
            return 2
        snap("02-menu")
        log(f"menu at x={menu[0]} y={menu[1]}; clicking the first row")
        q.click(menu[0] + 45, menu[1] + 12)
        time.sleep(4.0)
        snap("03-files-root")

        # ---- the app logs every navigation and file operation on stderr
        #      (the session relays client stderr to the serial console), so
        #      each step below is verified against what the app did, not
        #      against pixels.
        class Log:
            def here(self):
                try:
                    return os.path.getsize(serial)
                except OSError:
                    return 0

            def since(self, off):
                try:
                    with open(serial, "rb") as f:
                        f.seek(off)
                        return f.read().decode(errors="replace")
                except OSError:
                    return ""

            def wait(self, off, needle, timeout=8.0):
                end = time.time() + timeout
                while time.time() < end:
                    if needle in self.since(off):
                        return True
                    time.sleep(0.12)
                return False

        log_ = Log()
        problems = []

        def step(name, ok, detail=""):
            if ok:
                log(f"  ok: {name}")
            else:
                log(f"  FAIL: {name} {detail}".rstrip())
                problems.append(name)
            return ok

        def rect():
            q.shot(os.path.join(work, "probe.ppm"))
            return find_window(os.path.join(work, "probe.ppm"))

        step("app started in /", log_.wait(0, "xfiles: cwd /", 25))

        win = rect()
        if not win:
            log("FAIL: Files (X11) window not found")
            snap("04-no-window")
            return 2
        x0, y0, x1, y1 = win
        log(f"client area {x0},{y0} .. {x1},{y1}")

        def focus():
            q.click(x0 + 200, y0 + 140)
            time.sleep(0.4)

        def goto(path):
            """Type a path into the prompt until the app reports that cwd."""
            for attempt in range(3):
                q.key("slash")
                time.sleep(0.5)
                for _ in range(60):
                    q.key("backspace")
                    time.sleep(0.012)
                q.type_text(path)
                time.sleep(0.3)
                off = log_.here()
                q.key("ret")
                if log_.wait(off, f"xfiles: cwd {path}", 8):
                    return True
                log(f"  goto {path} attempt {attempt + 1} did not register")
            return False

        # ---- selection with the keyboard (Home, then Down x3)
        focus()
        q.key("home")
        time.sleep(0.2)
        for _ in range(3):
            q.key("down")
            time.sleep(0.2)
        snap("04-selection")

        # ---- path prompt -> /X11/FONTS
        q.key("slash")
        time.sleep(0.5)
        for _ in range(60):
            q.key("backspace")
            time.sleep(0.012)
        q.type_text("/X11/FONTS")
        time.sleep(0.4)
        snap("05-path-typed")
        off = log_.here()
        q.key("ret")
        step("navigated to /X11/FONTS", log_.wait(off, "xfiles: cwd /X11/FONTS", 8))
        time.sleep(0.6)
        snap("06-fonts-dir")

        # ---- sort by size, then the help overlay
        q.key("s")
        time.sleep(0.9)
        snap("07-sorted-by-size")
        q.key("f1")
        time.sleep(0.9)
        snap("08-help")
        q.key("esc")
        time.sleep(0.5)

        # ---- / then /HOME
        step("navigated to /", goto("/"))
        snap("09-root")
        step("navigated to /HOME", goto("/HOME"))
        snap("10-home")

        # ---- new folder ("n"), verified by the app's own log line
        q.key("n")
        time.sleep(0.5)
        q.type_text("DEMO")
        time.sleep(0.3)
        snap("11-newfolder-typed")
        off = log_.here()
        q.key("ret")
        step("mkdir DEMO", log_.wait(off, "xfiles: mkdir DEMO ok", 8))
        time.sleep(0.6)
        snap("12-newfolder")

        # ---- open the text file in EDITOR.BIN (rows: .., DEMO, SHTEST.TXT)
        q.key("home")
        time.sleep(0.2)
        q.key("down")
        time.sleep(0.2)
        q.key("down")
        time.sleep(0.3)
        snap("13-file-selected")
        off = log_.here()
        q.key("ret")
        step("opened SHTEST.TXT in the editor",
             log_.wait(off, "xfiles: open /HOME/SHTEST.TXT in editor", 8))
        time.sleep(6.0)
        snap("14-editor")

        # ---- raise the browser again and rename DEMO -> DEMO2
        q.click(x0 + 200, y0 - 10)
        time.sleep(0.8)
        q.key("home")
        time.sleep(0.2)
        q.key("down")
        time.sleep(0.4)
        snap("15-browser-raised")
        q.key("f2")
        time.sleep(0.7)
        for _ in range(40):
            q.key("backspace")
            time.sleep(0.012)
        q.type_text("DEMO2")
        time.sleep(0.3)
        snap("16-rename-typed")
        off = log_.here()
        q.key("ret")
        step("renamed DEMO to DEMO2",
             log_.wait(off, "xfiles: rename DEMO -> DEMO2 ok", 8))
        time.sleep(0.8)
        snap("17-renamed")

        # ---- delete it again, leaving the disk as we found it
        q.key("d")
        time.sleep(0.7)
        snap("18-delete-confirm")
        off = log_.here()
        q.key("y")
        time.sleep(0.3)
        q.key("ret")
        step("deleted DEMO2", log_.wait(off, "xfiles: delete DEMO2 ok", 8))
        time.sleep(0.9)
        snap("19-deleted")

        # ---- close from the title bar: WM_DELETE_WINDOW must exit cleanly
        q.click(x0 + 200, y0 + 140)
        time.sleep(0.4)
        off = log_.here()
        q.key("alt", "f4")          # IceWM sends WM_DELETE_WINDOW
        time.sleep(1.2)
        snap("20-closed")
        step("window closed cleanly", log_.wait(off, "/XFILES.BIN exited", 6))

        if problems:
            failures.extend(problems)

        if rec:
            rec.stop.set()
            rec.join(timeout=6)
            log(f"captured {rec.n} frames")
            if rec.n > 4:
                subprocess.run(["ffmpeg", "-y", "-framerate", "5",
                                "-i", os.path.join(frames, "f%04d.png"),
                                "-c:v", "libx264", "-pix_fmt", "yuv420p",
                                "-crf", "20", "-movflags", "+faststart",
                                os.path.join(work, "xfiles-demo.mp4")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                log("video: " + os.path.join(work, "xfiles-demo.mp4"))

        log("done" if not failures else f"finished with failures: {failures}")
        return 1 if failures else 0
    finally:
        if not args.keep_open:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())