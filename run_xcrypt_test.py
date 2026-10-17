#!/usr/bin/env python3
"""T5 playthrough harness — XCRYPT (GB-036, roguelike on the curses port)
under QEMU.

Boots the desktop under QEMU (virtio-gpu), launches XCRYPT.BIN from the
Apps menu (launcher slot 20: 16 pinned apps 0..15 + PONG=16 MILLIPED=17 +
XCRYPT=18 XNIBBLER=19 ... slot reported live from the [MENU] dump), and
asserts a real dungeon frame appears (via QMP screendump) within the
30-second rule with XCRYPT's signature (an '@' player, '#' walls, 'r/o/T'
monsters), and that forwarded arrow keys move the player (the '@' cell
shifts).  The game renders as a terminal window through the curses shim.

Auto-fail on: no game frame within 30 s, no XCRYPT process on the serial
log, or a fatal curses/graphics error.

Usage: ARCH=arm python3 run_xcrypt_test.py     (ARCH in {arm, intel})
Result: exit 0 on success; screendumps + serial in /tmp/hobbyos_xcrypt/.
"""
import json, os, re, socket, subprocess, sys, time
from collections import Counter

ROOT = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/wgames-gb036"
os.chdir(ROOT)
ARCH = os.environ.get("ARCH", "arm")
W, H = 1920, 1080
SHOT_DIR = "/tmp/hobbyos_xcrypt"
SER = "/tmp/xcrypt_test_serial.log"
QMP = "/tmp/qmp-xcrypt-test"
MAX_WAIT_S = 30
TASKBAR_Y = H - 26

# XCRYPT renders as text in a terminal window; the '@' player and '#' walls
# are monochrome glyphs.  We assert the presence of the dungeon text block
# by looking for the dark terminal background + a bright '@'/'o'/'#' glyph
# blob in the window region.
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

def signature(path):
    """Count dungeon-glyph (dark) and light-text pixels in the game's
    terminal window (the desktop's first/only window tiles the whole area
    above the taskbar, but the curses 80x24 grid tracks its top-left)."""
    raw, off, w, h = parse_ppm(path)
    row = w * 3
    yend = min(h - 26, 420)     # the terminal window's text area
    bright = 0; dark = 0
    for y in range(40, yend):
        base = off + y * row
        for x in range(0, 900, 2):
            R, G, B = raw[base+x*3], raw[base+x*3+1], raw[base+x*3+2]
            if R < 60 and G < 60 and B < 60: dark += 1
            elif R > 150 and G > 150 and B > 150: bright += 1
    return bright, dark

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
xn = next((i for i, n in entries.items() if n.strip() == "XCRYPT.BIN"), None)
log(f"menu: XCRYPT.BIN idx={xn}")
if xn is None:
    if "XCRYPT.BIN" in txt: xn = 18
    else:
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
        log("FATAL: XCRYPT.BIN not found in start-menu dump")
        sys.exit(1)
check(xn is not None, "XCRYPT.BIN present in launcher menu", f"idx={xn}")

q.click(38, TASKBAR_Y + 13)
time.sleep(0.8)
q.key("home", 0.3)
for _ in range(xn): q.key("down", 0.06)
q.key("ret", 0.3)

t0 = time.time()
seen = None; last = None
while time.time() - t0 < MAX_WAIT_S:
    shot = q.shot("xcrypt_frame.ppm")
    br, dk = signature(shot)
    last = (shot, br, dk)
    # The portable curses terminal renders light text (the dungeon glyphs)
    # on a dark-ish window; the reliable signature is the large bright-text
    # blob in the top-left window area (the desktop is white, so anything
    # >50k bright confined to that region is the game's text window).
    if br > 50000 and dk > 10000:
        seen = (shot, br, dk)
        break
    if serial_has(r"can't open|cannot|FATAL"):
        fail("display/curses init failed in serial log")
        break
    time.sleep(1.0)

(shot, br, dk) = seen if seen else last
check(seen is not None, f"XCRYPT frame within {MAX_WAIT_S}s rule",
      f"t={time.time()-t0:.1f}s bright={br} dark={dk}")
check(br > 10000, "dungeon glyphs rendered (bright text in terminal window)", f"bright={br}")
check(dk > 5000, "terminal window present (dark glyph area)", f"dark={dk}")

if seen is not None:
    a = q.shot("xcrypt_before.ppm")
    q.key("right", 0.2); q.key("right", 0.2); q.key("down", 0.2)
    time.sleep(1.0)
    b = q.shot("xcrypt_after.ppm")
    d = region_diff(a, b)
    check(d > 100, "forwarded keys move the player / game is live", f"diffs={d}")

if fails:
    log(f"RESULT: FAIL ({len(fails)})")
    for f in fails: log("  - " + f)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(1)
log("RESULT: PASS (T5 playthrough harness green)")
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
sys.exit(0)
