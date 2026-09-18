#!/usr/bin/env python3
"""End-to-end test: CONSOLE.BIN launches from the Apps menu and the
right-click context menu, opens a PERSISTENT interactive console window,
and closing the window ends the session cleanly.

Original bug: the console window vanished within the same frame the process
exited (often before anything rendered), so it looked like the app crashed
on launch. Now CONSOLE hands the window's pipes to SH.BIN and the window
lives as long as the shell does.

Asserts (framebuffer diffs against a baseline desktop frame, taskbar strip
excluded so the clock never affects the numbers):
  1. launching opens a window that persists            (big diff vs baseline)
  2. typed 'ls' output renders in the window           (diff between frames)
  3. window still there several seconds later          (still big diff, idle-small)
  4. F4 (close) ends the session, window gone          (small diff vs baseline)
  5. right-click menu launch behaves the same way
Result: exit 0 on success; screenshots in /tmp/hobbyos_console/."""
import json, os, re, socket, subprocess, time

ROOT = os.path.expanduser("~/Documents/GitHub/HobbyOS")
os.chdir(ROOT)
W, H = 1024, 768
TASKBAR_Y = H - 26
SHOT_DIR = "/tmp/hobbyos_console"
SERIAL = "/tmp/console_test_serial.log"
QMP = "./qmp-console-test"
BIG, MED, SMALL = 150000, 3000, 8000

os.makedirs(SHOT_DIR, exist_ok=True)
fails = []

def log(msg):
    print(f"[TEST] {msg}", flush=True)

def fail(msg):
    fails.append(msg)
    log(f"FAIL: {msg}")

class Q:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX); s.s.settimeout(15)
        for _ in range(160):
            try:
                s.s.connect(p); break
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
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)
    def rclick(s, x, y):
        s.mv(x, y)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "right", "down": True}}]})
        time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "right", "down": False}}]})
        time.sleep(0.5)
    def key(s, q, pause=0.15):
        s.c("send-key", {"keys": [{"type": "qcode", "data": q}]})
        time.sleep(pause)
    def shot(s, name):
        path = os.path.join(SHOT_DIR, name)
        s.c("screendump", {"filename": path})
        time.sleep(0.3)
        return path

def parse_ppm(path):
    raw = open(path, "rb").read()
    vals, i = [], 0
    while len(vals) < 4:
        while raw[i:i+1].isspace():
            i += 1
        if raw[i:i+1] == b"#":
            while raw[i:i+1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not raw[j:j+1].isspace():
            j += 1
        vals.append(raw[i:j]); i = j
    return raw, i + 1, int(vals[1]), int(vals[2])  # data, offset, w, h

def region_diff(fa, fb):
    """Count differing pixels above the taskbar strip."""
    ra, oa, wa, ha = parse_ppm(fa)
    rb, ob, wb, hb = parse_ppm(fb)
    assert (wa, ha) == (wb, hb), "screen size changed"
    row = wa * 3
    diffs = 0
    for y in range(0, ha - 30):
        ra_row = ra[oa + y*row : oa + (y+1)*row]
        rb_row = rb[ob + y*row : ob + (y+1)*row]
        if ra_row == rb_row:
            continue
        for x in range(0, row, 3):
            if ra_row[x:x+3] != rb_row[x:x+3]:
                diffs += 1
    return diffs

def check(cond, what, detail=""):
    if cond:
        log(f"PASS: {what} ({detail})")
    else:
        fail(f"{what} ({detail})")

# ---- boot ----
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
for f in (SERIAL, QMP):
    if os.path.exists(f):
        os.remove(f)
serial = open(SERIAL, "w")
subprocess.Popen(["stdbuf", "-oL", "make", "run", "ARCH=arm",
                  f"QEMU_ARGS=-display none -qmp unix:{QMP},server,nowait"],
                 stdout=serial, stderr=subprocess.STDOUT)
q = Q(QMP)
t0 = time.time()
while time.time() - t0 < 90:
    if "Desktop starting" in open(SERIAL, errors="ignore").read():
        break
    time.sleep(0.5)
time.sleep(3)

# menu index of CONSOLE.BIN from the load-time dump
txt = open(SERIAL, errors="ignore").read()
entries = dict((int(a), b) for a, b in re.findall(
    r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)(?:\[CONSOLE\]\s*)*=(?:\[CONSOLE\]\s*)*([A-Za-z0-9_.]+)", txt))
idx = next((i for i, n in entries.items() if n == "CONSOLE.BIN"), None)
log(f"CONSOLE.BIN menu index: {idx}")
if idx is None:
    fail("menu dump did not contain CONSOLE.BIN")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
    raise SystemExit(1)

# ---- 1) Apps menu launch: window must appear and persist ----
base = q.shot("00_baseline.ppm")
q.click(38, TASKBAR_Y + 13)
time.sleep(0.8)
q.key("home", 0.3)
for _ in range(idx):
    q.key("down", 0.06)
q.key("ret", 0.3)
time.sleep(1.6)
a1 = q.shot("01_window.ppm")
d = region_diff(base, a1)
check(d > BIG, "console window opened and is visible", f"diff={d}")

# ---- 2) shell is interactive: type 'ls', output must render ----
for ch in ("l", "s"):
    q.key(ch, 0.12)
q.key("ret", 0.2)
time.sleep(1.6)
a2 = q.shot("02_ls_output.ppm")
d2 = region_diff(a1, a2)
check(d2 > MED, "typed 'ls' output rendered in the console", f"diff={d2}")

# ---- 3) window persists (the original bug: it did not) ----
time.sleep(3.0)
a3 = q.shot("03_persist.ppm")
d3 = region_diff(a2, a3)
d3b = region_diff(base, a3)
check(d3 < SMALL, "console window idle (no flicker/close)", f"diff={d3}")
check(d3b > BIG, "console window still present after 3s", f"diff={d3b}")

# ---- 4) close (F4): session ends, window gone ----
q.key("f4", 0.3)
time.sleep(2.0)
a4 = q.shot("04_closed.ppm")
d4 = region_diff(base, a4)
check(d4 < SMALL, "closing the window ends the session", f"diff={d4}")

# ---- 5) right-click context menu path ----
q.rclick(100, 100)
time.sleep(0.4)
q.shot("05_rclick_menu.ppm")
q.click(140, 100 + idx * 20 + 10)
time.sleep(1.6)
r1 = q.shot("06_rclick_window.ppm")
d5 = region_diff(base, r1)
check(d5 > BIG, "right-click launch opens the same console window", f"diff={d5}")
q.key("f4", 0.3)
time.sleep(2.0)
r2 = q.shot("07_rclick_closed.ppm")
d6 = region_diff(base, r2)
check(d6 < SMALL, "right-click session closes cleanly", f"diff={d6}")

# ---- serial evidence ----
txt = open(SERIAL, errors="ignore").read()
launches = txt.count("[LAUNCH] [CONSOLE] CONSOLE.BIN") + txt.count("[LAUNCH] CONSOLE.BIN")
log(f"serial [LAUNCH] CONSOLE.BIN count: {launches}")

subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)

if fails:
    log(f"CONSOLE TEST FAILED ({len(fails)} failures)")
    for f in fails:
        log(f"  - {f}")
    raise SystemExit(1)
log("CONSOLE TEST PASSED (window persists, shell interactive, both menu paths)")
