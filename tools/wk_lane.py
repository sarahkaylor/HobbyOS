#!/usr/bin/env python3
"""wk_lane.py - parallel-safe HobbyOS ARM browser test lane harness.

Each lane gets its OWN working dir (default ~/.hermes/cache/scratch/wklane-<id>)
containing a clonefile copy of disk.img, its own qmp-sock, serial.log and a
shots/ dir, so N lanes can boot concurrently without fighting over the shared
image, the socket name, or the serial log.

Typical use from a scenario script:

    from wk_lane import Lane
    L = Lane("wiki")              # boots qemu, waits for the desktop
    L.quit()
    L.launch_browser()            # Apps menu -> BROWSER.BIN
    L.navigate("https://en.wikipedia.org/wiki/Cat")
    ...

Everything the lane proves comes from the guest's own serial receipts
([WIN] geom/inputs/buttons/page-key/load-ok/doc ...) plus QMP screenshots.
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
REPO = os.path.join(HOME, "Documents/GitHub/HobbyOS")
DISK_SRC = os.path.join(REPO, "disk.img")
SCRATCH = os.path.join(HOME, ".hermes/cache/scratch")
PPM2PNG = os.path.join(HOME, ".hermes/skills/hobbyos/hobbyos-screenshot/scripts/ppm2png.py")

SCREEN_W, SCREEN_H = 1920, 1080
# Desktop geometry (src/user/desktop.c)
TASKBAR_Y = 1054
APPS_BTN = (35, 1062)
BROWSER_ROW = (110, 760)   # Apps menu row 1
CHROME_H = 84

SPECIAL = {
    " ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon", "-": "minus",
    "_": "shift-minus", "@": "shift-2", "?": "shift-slash", "=": "equal",
    "&": "shift-7", "%": "shift-5", "#": "shift-3", "+": "shift-equal",
    ",": "comma", ";": "semicolon", "'": "apostrophe", '"': "shift-apostrophe",
    "\\": "backslash", "~": "shift-grave", "(": "shift-9", ")": "shift-0",
    "|": "shift-backslash", "!": "shift-1", "*": "shift-8", "$": "shift-4",
    "[": "bracket_left", "]": "bracket_right", "`": "grave", "<": "shift-comma",
    ">": "shift-dot", "{": "shift-bracket_left", "}": "shift-bracket_right",
}


def qcode_for(ch):
    if ch.isalnum() and ch.isascii():
        return ch.lower() if not ch.isupper() else "shift-" + ch.lower()
    if ch in SPECIAL:
        return SPECIAL[ch]
    raise SystemExit("no qcode for %r" % ch)


CA_BUNDLES = [
    os.path.join(REPO, "certs/cacert.pem"),
    "/etc/ssl/cert.pem",
]


def ca_bundle_path():
    for p in CA_BUNDLES:
        if os.path.exists(p) and os.path.getsize(p) > 10000:
            return p
    raise SystemExit("no CA bundle found (expected %s)" % CA_BUNDLES[0])


def stage_ca_bundle(disk, bundle=None):
    """Create ::/CERTS/CA.PEM on a disk image (idempotent)."""
    bundle = bundle or ca_bundle_path()
    subprocess.run(["mmd", "-i", disk, "::/CERTS"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["mcopy", "-o", "-i", disk, bundle, "::/CERTS/CA.PEM"], check=True)
    return bundle


class Qmp:
    """QMP client bound to one lane's socket."""

    def __init__(self, sock_path, width=SCREEN_W, height=SCREEN_H):
        self.width, self.height = width, height
        self.sock_path = sock_path
        s = None
        for _ in range(160):
            cand = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                cand.connect(sock_path)
            except OSError:
                cand.close()
                time.sleep(0.25)
                continue
            s = cand
            break
        if s is None:
            raise RuntimeError("no QMP socket at %s" % sock_path)
        self.sock = s
        self.f = s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            obj = json.loads(line)
            if "return" in obj or "error" in obj:
                return obj

    def _abs(self, v, extent):
        return max(0, min(32767, int(round(float(v) / float(extent) * 32767))))

    def move(self, x, y):
        return self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": self._abs(x, self.width)}},
            {"type": "abs", "data": {"axis": "y", "value": self._abs(y, self.height)}},
        ])

    def btn(self, down, button="left"):
        return self.cmd("input-send-event",
                        events=[{"type": "btn", "data": {"button": button, "down": down}}])

    def click(self, x, y, button="left", settle=0.10):
        self.move(x, y)
        time.sleep(settle)
        self.btn(True, button)
        time.sleep(0.08)
        self.btn(False, button)

    def wheel(self, x, y, updown=-1, ticks=1):
        """Mouse wheel over a point.  updown=-1 == scroll down (button 5)."""
        self.move(x, y)
        time.sleep(0.10)
        name = "wheel-down" if updown > 0 else "wheel-up"
        for _ in range(ticks):
            self.btn(True, name)
            time.sleep(0.05)
            self.btn(False, name)
            time.sleep(0.12)

    def key(self, qcode):
        keys = qcode.split("-")
        if len(keys) > 1 and keys[0] == "shift":
            keys = ["shift", keys[1]]
        return self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys])

    def keys(self, text, delay=0.05):
        for ch in text:
            self.key(qcode_for(ch))
            time.sleep(delay)

    def shot(self, out):
        ppm = out + ".ppm"
        if os.path.exists(ppm):
            os.remove(ppm)
        r = self.cmd("screendump", filename=ppm)
        if "error" in r:
            raise RuntimeError("screendump error: %s" % r)
        for _ in range(80):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                break
            time.sleep(0.1)
        subprocess.run([sys.executable, PPM2PNG, ppm, out], check=True)
        return out


class Lane:
    def __init__(self, name, smp=4, mem="6144M", boot_timeout=300, display_none=True,
                 disk_src=None, stage_certs=False):
        self.name = name
        self.dir = os.path.join(SCRATCH, "wklane-" + name)
        os.makedirs(os.path.join(self.dir, "shots"), exist_ok=True)
        self.disk = os.path.join(self.dir, "disk.img")
        self.sock = os.path.join(self.dir, "qmp-sock")
        self.serial = os.path.join(self.dir, "serial.log")
        for p in (self.sock, self.serial):
            if os.path.exists(p):
                os.remove(p)
        src = disk_src or DISK_SRC
        force = bool(disk_src)  # an explicit source always re-clones
        if force or not os.path.exists(self.disk) or \
                os.path.getsize(self.disk) != os.path.getsize(src):
            if os.path.exists(self.disk):
                os.remove(self.disk)
            subprocess.run(["cp", "-c", src, self.disk], check=True)
        if stage_certs:
            stage_ca_bundle(self.disk)
        args = [
            "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53",
            "-smp", str(smp), "-m", mem, "-accel", "tcg,thread=multi",
            "-bios", "/opt/homebrew/share/qemu/edk2-aarch64-code.fd",
            "-display", "none",
            "-serial", "file:" + self.serial,
            "-drive", "if=none,file=%s,format=raw,id=hd0" % self.disk,
            "-device", "virtio-blk-device,drive=hd0",
            "-device", "virtio-gpu-device",
            "-device", "virtio-keyboard-device",
            "-device", "virtio-tablet-device",
            "-netdev", "user,id=net0",
            "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:%02x:%02x"
                       % (abs(hash(name)) % 250, 0x56),
            "-semihosting", "-action", "shutdown=poweroff",
            "-qmp", "unix:%s,server,nowait" % self.sock,
        ]
        self.log_path = os.path.join(self.dir, "qemu.log")
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        self.q = Qmp(self.sock)
        self.t0 = time.time()
        self.wait_for(r"desktop", timeout=boot_timeout, label="desktop up")
        self.boot_secs = time.time() - self.t0

    # ---------- serial receipts ----------
    def text(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except FileNotFoundError:
            return ""

    def mark(self):
        return len(self.text())

    def wait_for(self, pattern, timeout=180, since=0, label=None):
        rx = re.compile(pattern)
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = rx.search(self.text()[since:])
            if m:
                return m
            time.sleep(0.25)
            if self.proc.poll() is not None:
                raise RuntimeError("qemu died waiting for %s" % (label or pattern))
        raise RuntimeError("timeout waiting for %s (%s)" % (label or pattern, pattern))

    def geom(self):
        gs = re.findall(r"\[WIN\] geom x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", self.text())
        if not gs:
            raise RuntimeError("no [WIN] geom yet")
        return tuple(int(v) for v in gs[-1])

    def inputs(self):
        ls = re.findall(r"\[WIN\] inputs (.*)", self.text())
        if not ls:
            return []
        field = ls[-1].strip()
        if ";" in field:
            field = field.split(";", 1)[1]
        out = []
        for item in field.split("|"):
            if "@" not in item:
                continue
            name, rect = item.rsplit("@", 1)
            xy, wh = rect.split(":")
            x, y = xy.split(",")
            w, h = wh.split("x")
            out.append((name, int(x), int(y), int(w), int(h)))
        return out

    def buttons(self):
        ls = re.findall(r"\[WIN\] buttons (.*)", self.text())
        if not ls:
            return []
        out = []
        for item in ls[-1].strip().split("|"):
            if "@" not in item:
                continue
            label, rect = item.rsplit("@", 1)
            xy, wh = rect.split(":")
            x, y = xy.split(",")
            w, h = wh.split("x")
            out.append((label, int(x), int(y), int(w), int(h)))
        return out

    def doc(self):
        """Last doc census -> dict(title, bodyLen, inputs, links, images, ...)."""
        ls = re.findall(r"\[WIN\] doc (.*)", self.text())
        if not ls:
            return None
        raw = ls[-1].strip()
        d = {"raw": raw}
        mt = re.search(r"title=(.*?)(?:\||$)", raw)
        if mt:
            d["title"] = mt.group(1)
        for k in ("bodyLen", "inputs", "links", "images", "forms", "text"):
            m = re.search(k + r"=(\d+)", raw)
            if m:
                d[k] = int(m.group(1))
        return d

    def links(self):
        """Last links census -> [(href, x, y, w, h), ...] (viewport pixels).

        The census is `"<count>;href@x,y:WxH|..."` for the first 12 <a href>.
        Long hrefs can be truncated by the driver's fixed emit line, so a
        dropped tail item is normal; parse what is well-formed and stop at the
        first malformed chunk.
        """
        ls = re.findall(r"\[WIN\] links (.*)", self.text())
        if not ls:
            return []
        field = ls[-1].strip()
        if ";" in field:
            field = field.split(";", 1)[1]
        out = []
        for item in field.split("|"):
            m = re.match(r"^(.*)@(-?\d+),(-?\d+):(\d+)x(\d+)$", item)
            if not m:
                continue
            href = m.group(1)
            out.append((href, int(m.group(2)), int(m.group(3)),
                        int(m.group(4)), int(m.group(5))))
        return out

    def scroll_pos(self):
        ps = re.findall(r"\[WIN\] scroll pos=(-?\d+)", self.text())
        return int(ps[-1]) if ps else None

    # ---------- chrome geometry (mirrors WK5Chrome::wk5ChromeLayout) ----------
    @staticmethod
    def title_from_url(url):
        """Python port of WK5UI::bkTitleFromUrl (tab chip / bookmark label)."""
        if not url:
            return "untitled"
        u = url
        m = re.match(r"^([A-Za-z]{1,16})://", u)
        has_scheme = bool(m)
        rest = u[m.end():] if has_scheme else u
        is_file = u.startswith("file:")
        no_dots = "/" not in rest
        if is_file or (not has_scheme and no_dots):
            base = rest.rsplit("/", 1)[-1]
            return (base or "untitled")[:255].upper()
        if not has_scheme:
            rest = u
        host = rest.split("/", 1)[0][:127]
        if host.startswith("www."):
            host = host[4:]
        if "/" in rest:
            seg = rest.split("/", 1)[1].split("/", 1)[0][:63]
            if seg:
                combined = host + "/" + seg
                if len(combined) <= 24:
                    return combined
        return host or u

    def tab_boxes(self, urls):
        """Tab chip boxes (content-relative) for the given per-tab URLs."""
        boxes, x = [], 4
        for u in urls:
            title = self.title_from_url(u)[:47]
            w = max(88, min(240, 8 * len(title) + 44))
            boxes.append((x, 0, x + w, 26))
            x += w + 4
        newtab = (x + 6, 1, x + 6 + 24, 25)
        return boxes, newtab

    @staticmethod
    def bookmark_boxes(urls):
        boxes, x = [], 6
        for u in urls:
            title = Lane.title_from_url(u)[:39]
            w = max(60, min(200, 8 * len(title) + 14 + 18))
            boxes.append((x, 60, x + w, 78))
            x += w + 6
        return boxes

    def click_content(self, cx, cy, label="chrome click"):
        """Click a content-relative point (adds the window origin)."""
        x, y, w, h = self.geom()
        m = self.mark()
        self.q.click(x + cx, y + cy)
        time.sleep(0.8)
        return m

    # ---------- driving ----------
    def shot(self, tag):
        return self.q.shot(os.path.join(self.dir, "shots", tag + ".png"))

    def launch_browser(self, timeout=300):
        m = self.mark()
        self.q.click(*APPS_BTN)
        time.sleep(1.2)
        self.q.click(*BROWSER_ROW)
        self.wait_for(r"\[WIN\] created pref=", timeout=timeout, since=m, label="browser window")
        self.wait_for(r"\[WIN\] (load-ok|frame view=)", timeout=timeout, since=m, label="browser first paint")
        time.sleep(2.0)
        return self.geom()

    def chrome(self):
        x, y, w, h = self.geom()
        omni_x1 = w - 38 - 40
        return {
            "geom": (x, y, w, h),
            "back": (x + 6 + 13, y + 43),
            "fwd": (x + 34 + 13, y + 43),
            "reload": (x + 62 + 13, y + 43),
            "omni": (x + (94 + omni_x1) // 2, y + 43),
            "go": (x + omni_x1 + 6 + 17, y + 43),
            "star": (x + w - 36 + 13, y + 43),
        }

    def navigate(self, url, timeout=600, expect_back=True):
        """Type url into the omnibox, press GO, wait for the page to render.

        Returns (mark_before_go, doc_census)."""
        c = self.chrome()
        x, y, w, h = c["geom"]
        m = self.mark()
        self.q.click(*c["omni"])
        self.wait_for(r"\[WIN\] addr click .*focus", timeout=30, since=m, label="omnibox focus")
        # An omnibox click resets the edit buffer (driver: m_urlLen = 0), so
        # typing here always starts from empty.
        self.q.keys(url)
        time.sleep(0.4)
        m = self.mark()
        self.q.click(*c["go"])
        self.wait_for(r"\[WIN\] go-click url=", timeout=30, since=m, label="GO click")
        # Real load receipts: ok / fetch open-fail / driver load-timeout.
        mt = self.wait_for(r"\[WIN\] (load-ok url=|load open-fail|FAIL load-timeout)",
                           timeout=timeout, since=m, label="page load")
        failed = "open-fail" in mt.group(0) or "load-timeout" in mt.group(0)
        if not failed:
            self.wait_for(r"\[WIN\] frame view=", timeout=timeout, since=m, label="first frame")
        time.sleep(2.5)
        return m, self.doc(), failed

    def page_point(self, px, py):
        x, y, w, h = self.geom()
        return (x + px, y + CHROME_H + py)

    def click_page(self, px, py, label="page click"):
        """Click a page-viewport point; parse the driver's own echo and re-click
        with the offset if the first click arrived in screen space."""
        m = self.mark()
        self.q.click(*self.page_point(px, py))
        time.sleep(0.6)
        mt = re.search(r"\[WIN\] click x=(-?\d+) y=(-?\d+) px=(-?\d+) py=(-?\d+)",
                       self.text()[m:])
        if not mt:
            return None
        sx, sy, epx, epy = (int(v) for v in mt.groups())
        if abs(epx - px) > 2 or abs(epy - py) > 2:
            dx, dy = px - epx, py - epy
            m2 = self.mark()
            self.q.click(sx + dx, sy + dy)
            time.sleep(0.6)
            mt2 = re.search(r"\[WIN\] click x=(-?\d+) y=(-?\d+) px=(-?\d+) py=(-?\d+)",
                            self.text()[m2:])
            return {"first": (sx, sy, epx, epy), "retry": mt2.groups() if mt2 else None}
        return {"first": (sx, sy, epx, epy), "retry": None}

    def quit(self):
        try:
            self.q.cmd("quit")
        except Exception:
            pass
        for _ in range(40):
            if self.proc.poll() is not None:
                break
            time.sleep(0.25)
        if self.proc.poll() is None:
            self.proc.terminate()
            time.sleep(0.5)
            if self.proc.poll() is None:
                self.proc.kill()
        self.log.close()

    # ---------- evidence ----------
    def report(self, path, payload):
        with open(path, "w") as f:
            json.dump(payload, f, indent=2)
        return path
