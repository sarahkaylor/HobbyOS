#!/usr/bin/env python3
"""End-to-end test: FILES arrow-key navigation against the REAL framebuffer.

Boots the desktop headless with a QMP socket, launches FILES.BIN from the
Apps menu, then:
  1. screenshot A (settled, pointer parked on the taskbar).
  2. three Down presses -> screenshot B.  The listing selection marker is
     the only thing that may change on screen: the '>' glyph moves three
     listing rows down (~14 lit pixels leaving one row, ~14 appearing in
     another).  Every other pixel - window chrome, path line, all the other
     rows - must be byte-identical between A and B.
  3. three Up presses -> screenshot C.  The screen must return to A exactly
     (the marker is back where it started).

This is the visual counterpart of the APPS_T responsiveness line: it catches
stale pixels / cracks / full-screen flashes that text-level host tests
cannot see.  28 px of change for three arrow presses is the expected order
of magnitude (2 markers); anything in the thousands means the compositor is
repainting (and visibly disturbing) rows it should have left alone.

Side effect being monitored: the FILES window opened and rendered, and the
serial log shows the app started.
Result: exit 0 on success; screenshots in /tmp/hobbyos_files_nav/.
"""
import json, os, re, socket, subprocess, time

ROOT = os.path.expanduser("~/Documents/GitHub/HobbyOS")
os.chdir(ROOT)
W, H = 1024, 768
TASKBAR_Y = H - 26
SHOT_DIR = "/tmp/hobbyos_files_nav"
SERIAL = "/tmp/files_nav_serial.log"
QMP = "./qmp-files-nav"
os.makedirs(SHOT_DIR, exist_ok=True)
fails = []

def log(m): print(f"[NAV] {m}", flush=True)
def fail(m):
    fails.append(m); log(f"FAIL: {m}")

class Q:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX); s.s.settimeout(20)
        for _ in range(200):
            try:
                s.s.connect(p); break
            except Exception:
                time.sleep(0.5)
        s.s.recv(65536)
        s.c("qmp_capabilities")
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
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.1)
    def click(s, x, y):
        s.mv(x, y)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)
    def key(s, q, pause=0.25):
        s.c("send-key", {"keys": [{"type": "qcode", "data": q}]})
        time.sleep(pause)
    def shot(s, name):
        p = os.path.join(SHOT_DIR, name)
        s.c("screendump", {"filename": p})
        time.sleep(0.35)
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

def diff_rows(fa, fb, y0, y1):
    """Rows (y -> changed pixel count) with differences in [y0, y1)."""
    ra, oa, wa, ha = parse_ppm(fa)
    rb, ob, wb, hb = parse_ppm(fb)
    row = wa * 3
    out = {}
    for y in range(y0, min(y1, ha)):
        ra_row = ra[oa + y*row : oa + (y+1)*row]
        rb_row = rb[ob + y*row : ob + (y+1)*row]
        if ra_row == rb_row: continue
        n = 0
        for x in range(0, row, 3):
            if ra_row[x:x+3] != rb_row[x:x+3]: n += 1
        out[y] = n
    return out

def check(cond, what, detail=""):
    log(f"{'PASS' if cond else 'FAIL'}: {what} ({detail})")
    if not cond: fail(what)

subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
for f in (SERIAL, QMP):
    if os.path.exists(f): os.remove(f)
serial = open(SERIAL, "w")
subprocess.Popen(["stdbuf", "-oL", "make", "run", "ARCH=arm",
                  f"QEMU_ARGS=-display none -qmp unix:{QMP},server,nowait"],
                 stdout=serial, stderr=subprocess.STDOUT)
q = Q(QMP)
t0 = time.time()
while time.time() - t0 < 120:
    if "Desktop starting" in open(SERIAL, errors="ignore").read(): break
    time.sleep(0.5)
time.sleep(3)

txt = open(SERIAL, errors="ignore").read()
entries = dict((int(a), b) for a, b in re.findall(
    r"\[MENU\]\s*(?:\[[A-Z_]+\]\s*)*(\d+)(?:\[[A-Z_]+\]\s*)*=(?:\[[A-Z_]+\]\s*)*([A-Za-z0-9_.]+)", txt))
idx = next((i for i, n in entries.items() if n == "FILES.BIN"), None)
log(f"FILES.BIN menu index: {idx}")
if idx is None:
    fail("menu dump did not contain FILES.BIN")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
    raise SystemExit(1)

base = q.shot("00_baseline.ppm")
q.click(38, TASKBAR_Y + 13)
time.sleep(0.8)
q.key("home", 0.3)
for _ in range(idx): q.key("down", 0.06)
q.key("ret", 0.3)
time.sleep(2.5)

# Park the pointer on the taskbar so cursor damage never lands in the window.
q.mv(500, TASKBAR_Y + 13)
time.sleep(0.6)

a = q.shot("01_files_settled.ppm")
for _ in range(3): q.key("down", 0.35)
time.sleep(1.2)
b = q.shot("02_after_3down.ppm")
for _ in range(3): q.key("up", 0.35)
time.sleep(1.2)
c = q.shot("03_after_3up.ppm")

# The FILES window tiles the whole desktop above the taskbar; its content
# starts at y=44 and each text row is 10px tall.  The selection marker is a
# 8x8 glyph in column 0, so the expected visible change is a couple of ~14
# pixel glyphs, confined to the listing rows; the window chrome, path line
# and every other row must be untouched.
rows_ab = diff_rows(a, b, 34, TASKBAR_Y)
rows_ca = diff_rows(c, a, 34, TASKBAR_Y)
n_ab = sum(rows_ab.values())
n_ca = sum(rows_ca.values())
log(f"diff A->B: {n_ab} px over {len(rows_ab)} rows; C->A: {n_ca} px over {len(rows_ca)} rows")
if rows_ab:
    log("  rows (y: px): " + ", ".join(f"{y}:{v}" for y, v in sorted(rows_ab.items())[:16]))
if rows_ca:
    log("  C->A rows: " + ", ".join(f"{y}:{v}" for y, v in sorted(rows_ca.items())[:16]))

# The listing starts at content row 3 (y = 44 + 3*10 = 74); the legend row
# is content row 21 (y = 254).  Everything the arrows may touch lives in
# between; the chrome, path line and taskbar must be untouched.
outside = [y for y in rows_ab if not (74 <= y <= 280)]

check(n_ab > 0, "Down arrows moved the selection marker", f"{n_ab} px")
check(n_ab < 2000, "visible change stays glyph-sized (no row/screen repaint)",
      f"{n_ab} px")
check(len(rows_ab) <= 24, "change confined to the marker glyphs' rows",
      f"{len(rows_ab)} rows")
check(not outside, "no pixels changed outside the listing area",
      f"{outside[:5]}")
check(n_ca == 0, "Up arrows restored the screen exactly", f"{n_ca} px")

# FILES window opened: whole-region change vs the empty desktop.
d_open = sum(v for v in diff_rows(base, a, 34, TASKBAR_Y).values())
check(d_open > 100000, "FILES window opened and rendered", f"{d_open} px")

txt = open(SERIAL, errors="ignore").read()
check("FILES started" in txt, "serial shows [APP] FILES started")

subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
if fails:
    log(f"FILES NAV CHECK FAILED ({len(fails)} failures)")
    raise SystemExit(1)
log("FILES NAV CHECK PASSED (marker-only repaints, screen restored)")
