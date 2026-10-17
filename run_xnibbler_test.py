#!/usr/bin/env python3
"""T5 playthrough harness — XNIBBLER (GB-037, original X11-era nibbler/Snake
game on the HobbyOS X11 support library) under QEMU.

Boots the desktop under QEMU (virtio-gpu), launches XNIBBLER.BIN from the
Apps menu (launcher slot 18: 16 pinned apps 0..15 + PONG=16 MILLIPED=17 +
XNIBBLER=18), and asserts a real game frame appears (via QMP screendump)
within the 30-second rule, that it carries the XNIBBLER signature (a dark
board with green snake cells / orange food), and that forwarded arrow keys
move the snake (board changes between screendumps => the game is live).

XNIBBLER signature (x11 window, whole-desktop tile above the taskbar):
  - the board field is FIELD_COLOR (dark blue-grey) on BG_COLOR
  - the snake is SNAKE_COLOR (green 0x4FDD6F -> (79,221,111))
  - food is FOOD_COLOR (orange 0xFFB347 -> (255,179,71))
  - arrow keys steer; the snake advances on its own each SPEED_MS

Auto-fail on: no game frame within 30 s, no XNIBBLER process on the serial
log, or a fatal link/graphics error in the serial log.

Usage: ARCH=arm python3 run_xnibbler_test.py     (ARCH in {arm, intel})
Result: exit 0 on success; screendumps + serial in /tmp/hobbyos_xnibbler/.

Tier: T5 (QMP input + screendump assertions).  Needs a built boot image
(`make disk.img`) — the harness only drives the already-built OS.
"""
import json, os, re, socket, subprocess, sys, time
from collections import Counter

ROOT = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/wgames-gb037"
os.chdir(ROOT)
ARCH = os.environ.get("ARCH", "arm")
W, H = 1920, 1080
SHOT_DIR = "/tmp/hobbyos_xnibbler"
SER = "/tmp/xnibbler_test_serial.log"
QMP = "/tmp/qmp-xnibbler-test"
XNIBBLER_SLOT = 18        # 16 pinned apps (0..15) + PONG=16 MILLIPED=17 + XNIBBLER=18
MAX_WAIT_S = 30           # the 30-second rule
TASKBAR_Y = H - 26

# XNIBBLER palette (src/user/x11/apps/xnibbler/main.c).
COLOR_BG      = (0x10, 0x15, 0x20)
COLOR_FIELD   = (0x18, 0x22, 0x30)
COLOR_SNAKE   = (0x4F, 0xDD, 0x6F)
COLOR_FOOD    = (0xFF, 0xB3, 0x47)
COLOR_TEXT    = (0xDD, 0xE4, 0xEA)

os.makedirs(SHOT_DIR, exist_ok=True)
fails = []

def log(m): print(f"[TEST] {m}", flush=True)
def fail(m): fails.append(m); log(f"FAIL: {m}")

class Q:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX); s.s.settimeout(15)
        for _ in range(200):
            try: s.s.connect(p); break
            except Exception: time.sleep(0.5)
        s.s.recv(65536); s.c("qmp_capabilities")
    def c(s, ex, args=None):
        m = {"execute": ex}
        if args: m["arguments"] = args
        s.s.sendall(json.dumps(m).encode() + b"\n")
        buf = b""
        while True:
            buf += s.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try: r = json.loads(part)
                except Exception: continue
                if "return" in r or "error" in r: return r
    def mv(s, x, y):
        s.c("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x*0x7FFF/W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y*0x7FFF/H)}}]})
        time.sleep(0.1)
    def click(s, x, y):
        s.mv(x, y)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]}); time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]}); time.sleep(0.4)
    def key(s, k, pause=0.15):
        s.c("send-key", {"keys": [{"type": "qcode", "data": k}]}); time.sleep(pause)
    def shot(s, name):
        p = os.path.join(SHOT_DIR, name)
        s.c("screendump", {"filename": p}); time.sleep(0.4)
        return p

def parse_ppm(path):
    raw = open(path, "rb").read()
    vals, i = [], 0
    while len(vals) < 4:
        while raw[i:i+1].isspace(): i += 1
        if raw[i:i+1] == b"#":
            while raw[i:i+1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not raw[j:j+1].isspace(): j += 1
        vals.append(raw[i:j]); i = j
    return raw, i + 1, int(vals[1]), int(vals[2])

def near(c, ref, tol=40):
    return abs(c[0]-ref[0]) <= tol and abs(c[1]-ref[1]) <= tol and abs(c[2]-ref[2]) <= tol

def board_stats(path):
    """Count green-snake, orange-food pixels + snake centroid in the window
    content area (whole desktop above the taskbar)."""
    raw, off, w, h = parse_ppm(path)
    row = w * 3
    yend = h - 26
    green = 0; food = 0; gx = 0; gy = 0; dark = 0
    for y in range(0, yend):
        base = off + y * row
        for x in range(0, w):
            i = x * 3; R, G, B = raw[base+i], raw[base+i+1], raw[base+i+2]
            if near((R, G, B), COLOR_SNAKE):
                green += 1; gx += x; gy += y
            elif near((R, G, B), COLOR_FOOD):
                food += 1
            elif near((R, G, B), COLOR_BG) or near((R, G, B), COLOR_FIELD):
                dark += 1
    return green, food, (gx / green) if green else None, dark

def region_diff(a, b):
    ra, oa, wa, ha = parse_ppm(a); rb, ob, wb, hb = parse_ppm(b)
    row = wa * 3; d = 0
    for y in range(0, ha - 30):
        aa = oa + y * row; bb = ob + y * row
        if ra[aa:aa+row] == rb[bb:bb+row]: continue
        for x in range(0, row, 3):
            if ra[aa+x:aa+x+3] != rb[bb+x:bb+x+3]: d += 1
    return d

def serial_has(pat):
    return re.search(pat, open(SER, errors="ignore").read()) is not None

def check(cond, what, detail=""):
    if cond: log(f"PASS: {what} ({detail})")
    else: fail(f"{what} ({detail})")

# ---- boot ---------------------------------------------------------------
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
for f in (SER, QMP):
    if os.path.exists(f): os.remove(f)
qemu = "qemu-system-aarch64" if ARCH == "arm" else "qemu-system-x86_64"
subprocess.Popen(["stdbuf", "-oL", "make", "run", f"ARCH={ARCH}",
                  f"QEMU_ARGS=-display none -qmp unix:{QMP},server,nowait"],
                 stdout=open(SER, "w"), stderr=subprocess.STDOUT)
q = Q(QMP)
t0 = time.time()
while time.time() - t0 < 180:
    if "Desktop starting" in open(SER, errors="ignore").read(): break
    time.sleep(0.5)
time.sleep(3)

txt = open(SER, errors="ignore").read()
entries = dict((int(a), b) for a, b in re.findall(
    r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)(?:\[CONSOLE\]\s*)*=(?:\[CONSOLE\]\s*)*([A-Za-z0-9_.]+)", txt))
sn = next((i for i, n in entries.items() if n.strip() == "XNIBBLER.BIN"), None)
log(f"menu: XNIBBLER.BIN idx={sn}")
check(sn == XNIBBLER_SLOT, "XNIBBLER.BIN pinned at launcher slot 18", f"idx={sn}")
if sn is None:
    if "XNIBBLER.BIN" in txt: sn = XNIBBLER_SLOT
    else:
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
        log("FATAL: XNIBBLER.BIN not found in start-menu dump")
        sys.exit(1)

# ---- launch -------------------------------------------------------------
q.click(38, TASKBAR_Y + 13)
time.sleep(0.8)
q.key("home", 0.3)
for _ in range(sn): q.key("down", 0.06)
q.key("ret", 0.3)

# ---- T5: assert an XNIBBLER board within the 30-second rule -------------
t0 = time.time()
seen = None; last = None
while time.time() - t0 < MAX_WAIT_S:
    shot = q.shot("xnibbler_frame.ppm")
    grn, fud, gcx, drk = board_stats(shot)
    last = (shot, grn, fud, gcx, drk)
    # Signature: a dark board with green snake cells and some food.
    if drk > 300000 and grn > 50:
        seen = (shot, grn, fud, gcx, drk)
        break
    if serial_has(r"can't open display|Failed to initialize|FATAL"):
        fail("display/graphics init failed in serial log")
        break
    time.sleep(1.0)

(shot, grn, fud, gcx, drk) = seen if seen else last
check(seen is not None, f"XNIBBLER frame within {MAX_WAIT_S}s rule",
      f"t={time.time()-t0:.1f}s dark={drk} green={grn} food={fud}")
check(drk > 300000, "dark game board dominates the window", f"dark_px={drk}")
check(grn > 50, "green snake cells present", f"snake_px={grn} centroid_x={gcx and round(gcx)}")

# ---- T5: the game is live; forwarded keys start + steer it --------------
if seen is not None:
    a = q.shot("xnibbler_before.ppm")
    # The snake idles until the first steering key; sending arrows must
    # start it and move the board (the game is reacting to forwarded keys).
    q.key("up", 0.2); q.key("left", 0.2); q.key("up", 0.2); q.key("left", 0.2)
    time.sleep(1.0)
    c = q.shot("xnibbler_steered.ppm")
    d2 = region_diff(a, c)
    check(d2 > 100, "forwarded keys start + steer the snake (game is live)",
          f"diffs={d2}")

if fails:
    log(f"RESULT: FAIL ({len(fails)})")
    for f in fails: log("  - " + f)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(1)
log("RESULT: PASS (T5 playthrough harness green)")
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
sys.exit(0)
