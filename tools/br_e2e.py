#!/usr/bin/env python3
"""E2E address-bar driver for the HobbyOS windowed browser (lane WC).

Drives a real ARM QEMU session over QMP + {serial file} and exposes the
browser address-bar hooks -- `type-url <url>` and `go <url>` -- plus boot,
Apps-menu launch (F1 preselected), console receipts and screenshots.

Two modes:

1. `--serve <ctrl.sock>` (spawns QEMU itself).  Boots the ARM disk, waits for
   the desktop + Apps menu, launches BROWSER.BIN via the F1 path (F1 opens the
   menu preselected on the browser; Enter launches), then serves a small JSON
   command socket:
       {"action":"type-url","url":"..."}   F2 -> [WIN] url-prompt -> type
       {"action":"go","url":"..."}         Enter -> url-entry -> load-ok assert
       {"action":"shot","name":"..."}      QMP screendump into the evidence dir
       {"action":"console","cmd":"..."}    launch CONSOLE, run a shell command
       {"action":"status"}                 serial marker summary
       {"action":"poweroff"}               clean QEMU shutdown, end the session
   One command per line (JSON in, JSON reply out).  Evidence markers land in
   <evdir> (screenshots, ctrl transcript, serial copy).

2. Action mode against a QEMU instance started separately by this same tool
   (--qmp/--serial/--evdir given, no --serve):
       br_e2e.py --qmp S --serial L --evdir D type-url <url>
       br_e2e.py --qmp S --serial L --evdir D go <url>
   (The br_step.py-style hook surface lane WA can drive once the fork's
   address bar lands: every action asserts the `[WIN]` receipt markers.)

Evidence convention (browser.md §8.4): every claim is backed by markers read
back from the real serial stream and a QMP screendump into <evdir>.
"""
import argparse
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

W, H = 1920, 1080
TASKBAR_Y = H - 26
APPS_BTN_X, APPS_BTN_W = 6, 64

CHAR_QCODE = {
    ' ': 'spc', '.': 'dot', ',': 'comma', '/': 'slash', ';': 'semicolon',
    "'": 'apostrophe', '-': 'minus', '=': 'equal', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent',
    '!': ['shift', '1'], '@': ['shift', '2'], '#': ['shift', '3'],
    '$': ['shift', '4'], '%': ['shift', '5'], '^': ['shift', '6'],
    '&': ['shift', '7'], '*': ['shift', '8'], '(': ['shift', '9'],
    ')': ['shift', '0'], '_': ['shift', 'minus'], '+': ['shift', 'equal'],
    ':': ['shift', 'semicolon'], '"': ['shift', 'apostrophe'],
    '<': ['shift', 'comma'], '>': ['shift', 'dot'], '?': ['shift', 'slash'],
    '{': ['shift', 'bracket_left'], '}': ['shift', 'bracket_right'],
    '|': ['shift', 'backslash'], '~': ['shift', 'grave_accent'],
}


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------- QMP ------

class Qmp:
    def __init__(self, path, connect_timeout_s=240):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        deadline = time.time() + connect_timeout_s
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("QMP socket never appeared: %s" % path)
                time.sleep(0.5)
        self.sock.recv(1 << 20)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None, timeout=90):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        deadline = time.time() + timeout
        while True:
            if len(buf) > (1 << 22):
                raise RuntimeError("QMP response too large")
            try:
                chunk = self.sock.recv(1 << 20)
            except socket.timeout:
                if time.time() > deadline:
                    raise RuntimeError("QMP timeout waiting for %s" % execute)
                continue
            if not chunk:
                raise RuntimeError("QMP socket closed during %s" % execute)
            buf += chunk
            text = buf.decode(errors="ignore")
            parts = text.strip().split("\n")
            if not text.endswith("\n"):
                parts = parts[:-1]
            for part in parts:
                part = part.strip()
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
            buf = b""

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.15)

    def click(self, x, y, settle=0.3):
        self.move(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(settle)

    def key(self, codes, pause=0.35):
        if isinstance(codes, str):
            codes = [codes]
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": c} for c in codes]})
        time.sleep(pause)

    def type_text(self, text, pause=0.12):
        for ch in text:
            if ch == "\n":
                self.key("ret", pause=0.5)
                continue
            if ch.isalpha() and ch.isupper():
                code = ["shift", ch.lower()]
            elif ch.isalnum():
                code = ch
            else:
                code = CHAR_QCODE.get(ch)
                if code is None:
                    raise RuntimeError("no qcode for char %r" % ch)
            self.key(code, pause=pause)

    def shot(self, path):
        try:
            r = self.cmd("screendump", {"filename": path, "format": "png"}, timeout=60)
            if "return" in r and os.path.exists(path):
                return True
        except Exception:
            pass
        ppm = path[:-4] + ".ppm" if path.endswith(".png") else path
        try:
            r = self.cmd("screendump", {"filename": ppm}, timeout=60)
            if "return" not in r:
                return False
            png = ppm[:-4] + ".png" if ppm.endswith(".ppm") else ppm + ".png"
            ppm_to_png(ppm, png)
            os.unlink(ppm)
            return True
        except Exception:
            return os.path.exists(ppm)


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, w, h, rgb_rows):
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_png_chunk(b"IHDR", ihdr))
        f.write(_png_chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(_png_chunk(b"IEND", b""))


def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM")
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _maxv = fields
    pix = data[idx:idx + w * h * 3]
    if len(pix) != w * h * 3:
        raise ValueError("short PPM payload")
    rows = [pix[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    write_png(png_path, w, h, rows)


# -------------------------------------------------------------- serial -----

class Serial:
    def __init__(self, path):
        self.path = path

    def text(self):
        try:
            with open(self.path, "rb") as f:
                return f.read().decode("utf-8", errors="ignore")
        except OSError:
            return ""

    def count(self, needle):
        return self.text().count(needle)

    def wait_count(self, needle, want_at_least, timeout, poll=0.4):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.count(needle) >= want_at_least:
                return True
            time.sleep(poll)
        return False

    def wait_any(self, needles, timeout, poll=0.4):
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.text()
            for n in needles:
                if n in text:
                    return n
            time.sleep(poll)
        return None

    def lines(self, needle):
        return [ln.strip() for ln in self.text().splitlines() if needle in ln]


# ----------------------------------------------------------- qemu boot -----

QEMU_ARGS = [
    "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53",
    "-smp", os.environ.get("WC_QEMU_SMP", "1"),
    "-m", os.environ.get("WC_QEMU_MEM", "2048M"),
    "-accel", "tcg,thread=multi",
    "-bios", os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd"),
    "-display", "none",
    "-device", "virtio-blk-device,drive=hd0",
    "-device", "virtio-gpu-device",
    "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
    "-netdev", "user,id=net0",
    "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56",
    "-semihosting", "-action", "shutdown=poweroff",
]


def boot_attempts(max_attempts=3):
    """The TCG lost-wakeup boot class is stochastic (browser.md / WF1B
    receipts): a boot can fault mid-desktop.  Retry with a fresh QEMU."""
    return list(range(max_attempts))


# --------------------------------------------------------- browser session --

class BrowserSession:
    def __init__(self, args):
        self.a = args
        self.serial = Serial(args.serial_log)
        self.qmp = None
        self.proc = None
        self.ctrl = None
        self._t0 = time.time()
        os.makedirs(args.evdir, exist_ok=True)

    # ---- lifecycle ---------------------------------------------------
    def spawn_qemu(self):
        for f in (self.a.serial_log, self.a.qmp_sock):
            try:
                os.remove(f)
            except OSError:
                pass
        argv = list(QEMU_ARGS)
        argv += ["-serial", "file:" + self.a.serial_log,
                 "-drive", "if=none,file=%s,format=raw,id=hd0" % self.a.disk]
        argv += ["-qmp", "unix:%s,server,nowait" % self.a.qmp_sock]
        log("[E2E] qemu: %s" % " ".join(argv[:8]) + " ...")
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.qmp = Qmp(self.a.qmp_sock)

    def wait_desktop(self, timeout=360):
        if not self.serial.wait_count("Desktop starting", 1, timeout):
            raise RuntimeError("desktop never started (serial tail: %s)"
                               % self.serial.text()[-400:])
        time.sleep(3)
        self.serial.wait_count("[MENU]", 1, 60)
        if "disposing pid=1 DESKTOP.BIN" in self.serial.text():
            raise RuntimeError("desktop lost to the TCG lost-wakeup class "
                               "(LOSTWAKE disposing pid=1 DESKTOP.BIN)")
        ip = [l for l in self.serial.lines("Network configured")]
        log("[E2E] desktop up; network: %s" % (ip[-1] if ip else "n/a"))
        return True

    def apps_launch(self, target_name, wait_marker, timeout=180):
        """Open the Apps menu (F1), which the desktop opens preselected on
        BROWSER* (a name scan, so the index varies per disk), move to
        `target_name` (a shell-style prefix match against the [MENU] map),
        Enter launches.  The wait-marker base count is taken BEFORE the keys
        (the browser's lazy loader reaches [WIN] BOOT in <1 s, so a post-action
        base would race the launch).  Returns the menu map."""
        menu = self.menu_map()
        if not menu:
            raise RuntimeError("no [MENU] map in serial")
        preselect = None
        for k, v in menu.items():
            if v.upper().startswith("BROWSER"):
                preselect = k
                break
        if preselect is None:
            preselect = 0
        target = None
        tup = target_name.upper()
        for k, v in menu.items():
            if v.upper() == tup or v.upper().startswith(tup[:8]):
                target = k
                break
        if target is None:
            raise RuntimeError("%s not in [MENU] map" % target_name)
        base = self.serial.count(wait_marker)
        self.qmp.key("f1", pause=1.0)
        time.sleep(0.4)
        delta = target - preselect
        if delta > 0:
            for _ in range(delta):
                self.qmp.key("down", pause=0.35)
        elif delta < 0:
            for _ in range(-delta):
                self.qmp.key("up", pause=0.35)
        self.qmp.key("ret", pause=2.5)
        if not self.serial.wait_count(wait_marker, base + 1, timeout):
            raise RuntimeError("launch marker %s not seen (target=%s, base=%d)"
                               % (wait_marker, target_name, base))
        return menu

    def menu_map(self, timeout=60):
        out = {}
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in self.serial.text().splitlines():
                if "[MENU]" in line and "=" in line:
                    seg = line.split("[MENU]", 1)[1].replace("[CONSOLE]", "")
                    for tok in seg.split():
                        if "=" in tok:
                            k, v = tok.split("=", 1)
                            try:
                                out[int(k)] = v.strip()
                            except ValueError:
                                pass
            if out:
                return out
            time.sleep(0.5)
        return out

    def launch_browser(self):
        menu = self.apps_launch("BROWSER.BIN", "[WIN] BOOT")
        ok = self.serial.wait_count("[WIN] created", 1, 60)
        self.serial.wait_count("[WIN] geom", 1, 90)
        self.serial.wait_count("[WIN] load-ok", 1, 180)
        self.record("launch-browser", {
            "ok": ok, "menu_items": len(menu),
            "boot_lines": [l for l in self.serial.lines("[WIN] BOOT")][-1:]})
        self.shot("browser-launch")
        return ok

    # ---- address-bar hooks -------------------------------------------
    def type_url(self, url):
        base_prompt = self.serial.count("[WIN] url-prompt")
        self.qmp.key("f2", pause=0.6)
        ok = self.serial.wait_count("[WIN] url-prompt", base_prompt + 1, 30)
        if not ok:
            raise RuntimeError("F2 did not open the URL prompt (url=%s)" % url)
        self.qmp.type_text(url)
        time.sleep(0.6)
        r = {"ok": True, "url": url, "url_prompt": True}
        self.record("type-url", r)
        return r

    def go(self, url, timeout=240):
        base_entry = self.serial.count("[WIN] url-entry")
        base_ok = self.serial.count("[WIN] load-ok url=%s" % url)
        base_openfail = self.serial.count("[WIN] load open-fail")
        base_frames = self.serial.count("[WIN] frame")
        self.qmp.key("ret", pause=1.0)
        entry_ok = self.serial.wait_count("[WIN] url-entry", base_entry + 1, 40)
        r = {"ok": True, "url": url, "entry_echo": None, "load_ok": False,
             "load_ms": None, "open_fail": None, "frames": 0, "checksum": None,
             "net": None}
        if entry_ok:
            m = re.findall(r"\[WIN\] url-entry=(\S+)", self.serial.text())
            r["entry_echo"] = m[-1] if m else None
            # prefer the exact-url load-ok ("http://10.0.2.2:8800/robots.txt")
            dl = time.time() + timeout
            while time.time() < dl:
                if self.serial.count("[WIN] load-ok url=%s" % url) > base_ok:
                    r["load_ok"] = True
                    ms = re.findall(r"\[WIN\] load-ok url=%s ms=(\d+)" % re.escape(url),
                                    self.serial.text())
                    r["load_ms"] = int(ms[-1]) if ms else None
                    break
                if self.serial.count("[WIN] load open-fail") > base_openfail:
                    m = [l for l in self.serial.lines("[WIN] load open-fail")]
                    r["open_fail"] = m[-1] if m else "open-fail"
                    break
                if "[WIN] load-ok" in self.serial.text() and \
                   [l for l in self.serial.lines("[WIN] net fetch")]:
                    r["net"] = [l for l in self.serial.lines("[WIN] net fetch")][-1]
                time.sleep(0.5)
        r["frames"] = self.serial.count("[WIN] frame") - base_frames
        ck = re.findall(r"\[WIN\] frame .*checksum=(0x[0-9a-fA-F]+)", self.serial.text())
        r["checksum"] = ck[-1] if ck else None
        net = [l for l in self.serial.lines("[WIN] net fetch")]
        if net:
            r["net"] = net[-1]
        persist = [l for l in self.serial.lines("[WIN] net persist")]
        r["persist"] = persist[-1] if persist else None
        shot = self.shot("go-%s" % re.sub(r"[^A-Za-z0-9_.-]", "_", url)[-40:])
        r["screenshot"] = shot
        self.record("go", r)
        return r

    # ---- console/shell receipts ---------------------------------------
    def console_cmd(self, cmd, settle=4.0, wait_launch=True, type_pause=0.14):
        if wait_launch:
            self.apps_launch("CONSOLE.BIN", "[LAUNCH] CONSOLE.BIN", timeout=120)
        self.serial.wait_count("[LAUNCH] CONSOLE.BIN", 1, 60)
        time.sleep(4.0)  # banner + SH.BIN handoff (keystrokes must not race it)
        self.qmp.type_text(cmd, pause=type_pause)
        time.sleep(0.4)
        self.qmp.key("ret", pause=settle)
        return True

    # ---- evidence -----------------------------------------------------
    def shot(self, tag):
        name = "e2e-%s.png" % tag
        path = os.path.join(self.a.evdir, name)
        try:
            ok = self.qmp.shot(path)
        except Exception as e:
            log("screenshot %s failed: %s" % (tag, e))
            return None
        return name if (ok and os.path.exists(path)) else None

    def record(self, step, r):
        r["step"] = step
        r["t_wall_s"] = round(time.time() - self._t0, 1)
        log("[E2E] %s %s" % (step, json.dumps(r)))

    def serial_snapshot(self):
        txt = self.serial.text()
        keep = [ln for ln in txt.splitlines() if any(
            k in ln for k in ("[WIN]", "[MENU]", "[LAUNCH]", "[KERNEL]", "FATAL",
                              "IDLESTUCK", "LOSTWAKE", "Network configured", "DNS"))]
        return keep[-400:]

    # ---- control socket ------------------------------------------------
    def serve(self, ctrl_path):
        """JSON command socket server. One command per line -> one JSON reply."""
        try:
            os.remove(ctrl_path)
        except OSError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(ctrl_path)
        srv.listen(4)
        srv.settimeout(1.0)
        log("[E2E] READY ctrl=%s evdir=%s" % (ctrl_path, self.a.evdir))
        while True:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                if self.proc and self.proc.poll() is not None:
                    raise RuntimeError("qemu died: rc=%s" % self.proc.returncode)
                continue
            with conn:
                conn.settimeout(600)
                buf = b""
                try:
                    while True:
                        c = conn.recv(4096)
                        if not c:
                            break
                        buf += c
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            line = line.strip()
                            if not line:
                                continue
                            resp = self.handle_command(json.loads(line))
                            conn.sendall(json.dumps(resp).encode() + b"\n")
                except (socket.timeout, ValueError, OSError) as e:
                    log("[E2E] ctrl conn end: %r" % e)

    def handle_command(self, cmd):
        action = cmd.get("action")
        try:
            if action == "type-url":
                return self.type_url(cmd["url"])
            if action == "go":
                return self.go(cmd["url"], timeout=cmd.get("timeout", 240))
            if action == "shot":
                name = cmd.get("name", "manual")
                n = self.shot(name)
                return {"ok": bool(n), "screenshot": n}
            if action == "console":
                launch = bool(cmd.get("launch", True))
                self.console_cmd(cmd["cmd"], settle=cmd.get("settle", 4.0),
                                 wait_launch=launch,
                                 type_pause=cmd.get("type_pause", 0.14))
                return {"ok": True, "cmd": cmd["cmd"]}
            if action == "status":
                return {"ok": True, "markers": self.serial_snapshot()[-40:]}
            if action == "poweroff":
                self.poweroff()
                return {"ok": True, "powered": True}
            return {"ok": False, "error": "unknown action %r" % action}
        except Exception as e:
            return {"ok": False, "error": repr(e)}

    def poweroff(self):
        try:
            if self.qmp:
                self.qmp.cmd("system_powerdown", timeout=30)
        except Exception:
            pass
        time.sleep(3)
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=10)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
        return True


# ------------------------------------------------------------- action mode --

def action_mode(args):
    """Connect to a running session and perform exactly one action (the
    br_step.py-style hook surface for lane WA)."""
    sess = BrowserSession(args)
    sess.qmp = Qmp(args.qmp_sock, connect_timeout_s=60)
    sess._t0 = time.time()
    act = args.action
    if act == "type-url":
        print(json.dumps(sess.type_url(args.url)))
    elif act == "go":
        print(json.dumps(sess.go(args.url)))
    elif act == "shot":
        print(json.dumps({"ok": True, "screenshot": sess.shot(args.name)}))
    elif act == "status":
        print(json.dumps({"ok": True, "markers": sess.serial_snapshot()[-40:]}))
    elif act == "poweroff":
        print(json.dumps({"ok": sess.poweroff()}))
    else:
        print(json.dumps({"ok": False, "error": "unknown action %s" % act}))
        return 2
    return 0


def main():
    ap = argparse.ArgumentParser(description="HobbyOS browser E2E driver (lane WC)")
    ap.add_argument("--disk", default=os.path.join(os.path.dirname(__file__),
                                                   "..", "disk.img"))
    ap.add_argument("--qmp", default="/tmp/br-wc-qmp.sock", dest="qmp_sock")
    ap.add_argument("--serial", default="/tmp/br-wc-serial.log", dest="serial_log")
    ap.add_argument("--evdir", default="evidence")
    ap.add_argument("--serve", default=None, help="control socket path for --serve mode")
    ap.add_argument("--boot-timeout", type=int, default=360)
    ap.add_argument("action", nargs="?", default=None)
    ap.add_argument("url", nargs="?")
    ap.add_argument("--name", default="manual")
    args = ap.parse_args()

    if args.action:
        args.evdir = os.path.abspath(args.evdir)
        return action_mode(args)

    if not args.serve:
        print("serve mode needs --serve", file=sys.stderr)
        return 2

    args.disk = os.path.abspath(args.disk)
    args.evdir = os.path.abspath(args.evdir)
    sess = BrowserSession(args)
    sess._t0 = time.time()
    last_err = None
    for attempt in boot_attempts():
        try:
            log("[E2E] boot attempt %d/%d" % (attempt + 1, len(boot_attempts())))
            sess = BrowserSession(args)
            sess._t0 = time.time()
            sess.spawn_qemu()
            sess.wait_desktop(timeout=args.boot_timeout)
            sess.launch_browser()
            last_err = None
            break
        except Exception as e:
            last_err = e
            log("[E2E] boot attempt %d failed: %r" % (attempt + 1, e))
            try:
                sess.poweroff()
            except Exception:
                pass
            # fresh serial/qmp files for the next attempt
            for f in (args.serial_log, args.qmp_sock):
                try:
                    os.remove(f)
                except OSError:
                    pass
            time.sleep(2)
    if last_err is not None:
        log("[E2E] all boot attempts failed: %r" % last_err)
        try:
            sess.poweroff()
        except Exception:
            pass
        return 1
    try:
        sess.serve(args.serve)
    finally:
        sess.poweroff()
    return 0


if __name__ == "__main__":
    sys.exit(main())
