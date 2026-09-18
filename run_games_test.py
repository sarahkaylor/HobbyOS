#!/usr/bin/env python3
"""End-to-end: the games in the Apps menu.

PONG.BIN and MILLIPED.BIN must sit right below the ten apps (indices 10/11),
launch from the menu as full-screen games, receive keyboard input, and hand
the screen back to the desktop when they exit.

Asserts:
  1. menu dump: PONG.BIN == index 10, MILLIPED.BIN == index 11.
  2. launching PONG takes over the screen and draws its field.
  3. PONG's paddle MOVES for forwarded arrow keys (8x down -> >= 40 px) —
     the desktop forwards keys to the window's stdin, which the game reads.
  4. 'q' quits (serial-confirmed, within a few tries).
  5. the desktop is fully back afterwards (diff vs baseline small).
  6. same for MILLIPED: takeover, green ship, ship moves right on arrows,
     q quits, desktop restored.
Result: exit 0 on success; screenshots in /tmp/hobbyos_games/."""
import json, os, re, socket, subprocess, time

ROOT = os.path.expanduser("~/Documents/GitHub/HobbyOS")
os.chdir(ROOT)
W, H = 1024, 768
TASKBAR_Y = H - 26
SHOT_DIR = "/tmp/hobbyos_games"
SER = "/tmp/games_test_serial.log"
QMP = "./qmp-games-test"

os.makedirs(SHOT_DIR, exist_ok=True)
fails = []

def log(m): print(f"[TEST] {m}", flush=True)
def fail(m): fails.append(m); log(f"FAIL: {m}")

class Q:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX); s.s.settimeout(15)
        for _ in range(160):
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
        s.c("screendump", {"filename": p}); time.sleep(0.3)
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

def region_diff(fa, fb):
    ra, oa, wa, ha = parse_ppm(fa); rb, ob, wb, hb = parse_ppm(fb)
    row = wa * 3
    diffs = 0
    for y in range(0, ha - 30):
        a = ra[oa+y*row: oa+(y+1)*row]; b = rb[ob+y*row: ob+(y+1)*row]
        if a == b: continue
        for x in range(0, row, 3):
            if a[x:x+3] != b[x:x+3]: diffs += 1
    return diffs

def color_stats(path):
    raw, off, w, h = parse_ppm(path)
    row = w * 3
    black = green = bright = 0
    for y in range(0, h - 30):
        r = raw[off+y*row: off+(y+1)*row]
        for x in range(0, row, 3):
            R, G, B = r[x], r[x+1], r[x+2]
            if R < 16 and G < 16 and B < 16: black += 1
            elif G > 150 and G > R + 40 and G > B + 40: green += 1
            elif R > 160 or G > 160 or B > 160: bright += 1
    return black, green, bright, (h-30)*w

def paddle_top(path):
    """Top y of the white paddle: longest white vertical run in x 1..30."""
    raw, off, w, h = parse_ppm(path)
    rows = []
    for y in range(0, h - 30):
        rowa = raw[off+y*w*3 : off+y*w*3 + 31*3]
        white = any(rowa[x] > 180 and rowa[x+1] > 180 and rowa[x+2] > 180
                    for x in range(3, 90, 3))
        rows.append(white)
    best = cur = 0; bs = 0; cs = 0
    for y, val in enumerate(rows):
        if val:
            if cur == 0: cs = y
            cur += 1
            if cur > best: best, bs = cur, cs
        else:
            cur = 0
    return bs

def ship_centroid_x(path):
    """x centroid of green pixels in the bottom third (the player ship)."""
    raw, off, w, h = parse_ppm(path)
    xsum = n = 0
    for y in range(2 * h // 3, h - 30):
        r = raw[off+y*w*3: off+(y+1)*w*3]
        for x in range(0, w*3, 3):
            R, G, B = r[x], r[x+1], r[x+2]
            if G > 150 and G > R + 40 and G > B + 40:
                xsum += x // 3; n += 1
    return (xsum / n) if n else None

def serial_has(pat):
    return re.search(pat, open(SER, errors="ignore").read()) is not None

def check(cond, what, detail=""):
    if cond: log(f"PASS: {what} ({detail})")
    else: fail(f"{what} ({detail})")

# ---- boot ----
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
for f in (SER, QMP):
    if os.path.exists(f): os.remove(f)
subprocess.Popen(["stdbuf", "-oL", "make", "run", "ARCH=arm",
                  f"QEMU_ARGS=-display none -qmp unix:{QMP},server,nowait"],
                 stdout=open(SER, "w"), stderr=subprocess.STDOUT)
q = Q(QMP)
t0 = time.time()
while time.time() - t0 < 120:
    if "Desktop starting" in open(SER, errors="ignore").read(): break
    time.sleep(0.5)
time.sleep(3)

txt = open(SER, errors="ignore").read()
entries = dict((int(a), b) for a, b in re.findall(
    r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)(?:\[CONSOLE\]\s*)*=(?:\[CONSOLE\]\s*)*([A-Za-z0-9_.]+)", txt))
pong_i = next((i for i, n in entries.items() if n == "PONG.BIN"), None)
milli_i = next((i for i, n in entries.items() if n == "MILLIPED.BIN"), None)
log(f"menu: PONG.BIN idx={pong_i}, MILLIPED.BIN idx={milli_i}")
check(pong_i == 10, "PONG.BIN pinned at index 10", f"idx={pong_i}")
check(milli_i == 11, "MILLIPED.BIN pinned at index 11", f"idx={milli_i}")
if None in (pong_i, milli_i):
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
    raise SystemExit(1)

base = q.shot("00_baseline.ppm")

def launch(idx, menu_shot):
    q.click(38, TASKBAR_Y + 13)
    time.sleep(0.8)
    q.shot(menu_shot)
    q.key("home", 0.3)
    for _ in range(idx): q.key("down", 0.06)
    q.key("ret", 0.3)

def quit_and_confirm(name, tries=5):
    for n in range(1, tries + 1):
        q.key("q", 0.5)
        if serial_has(rf"Process \d+: {name} exited"):
            return f"q x{n}"
        time.sleep(0.4)
    for n in range(1, 5):  # cleanup fallback so the run can finish; counts as FAIL above
        q.key("f4", 0.5)
        if serial_has(rf"Process \d+: {name} exited"):
            return f"f4 x{n} (q never reached the game!)"
        time.sleep(0.4)
    return None

# ---- PONG: takeover, input, quit, restore ----
launch(pong_i, "00_menu_open.ppm")
time.sleep(2.5)
p1 = q.shot("01_pong.ppm")
blk, grn, brt, total = color_stats(p1)
check(blk / total > 0.85, "PONG took over the screen (black field)", f"black={blk/total:.2%}")
check(brt > 500, "PONG drew its paddles/line/score", f"bright={brt}")
y0 = paddle_top(p1)
check(y0 > 40, "paddle detected in left strip", f"top_y={y0}")
for _ in range(8): q.key("down", 0.3)
time.sleep(0.8)
p1b = q.shot("01b_pong_moved.ppm")
y1 = paddle_top(p1b)
moved = y1 - y0
check(40 <= moved <= 72, "PONG paddle moves on forwarded arrow keys", f"moved={moved}px for 8 keys (8px each)")
how = quit_and_confirm("PONG.BIN")
check(how is not None and how.startswith("q"), "PONG exits on q", f"{how}")
time.sleep(2.0)
p2 = q.shot("02_pong_closed.ppm")
d = region_diff(base, p2)
check(d < 8000, "desktop fully restored after PONG exits", f"diff={d}")

# ---- MILLIPED: takeover, ship input, quit, restore ----
launch(milli_i, "00_menu_open_milli.ppm")
time.sleep(2.5)
m1 = q.shot("03_milli.ppm")
blk, grn, brt, total = color_stats(m1)
check(blk / total > 0.85, "MILLIPED took over the screen (black field)", f"black={blk/total:.2%}")
check(grn > 20, "MILLIPED drew its ship/HUD", f"green={grn}")
cx0 = ship_centroid_x(m1)
check(cx0 is not None, "ship detected in bottom third", f"x={cx0 and round(cx0)}")
for _ in range(6): q.key("right", 0.3)
time.sleep(0.8)
m1b = q.shot("03b_milli_moved.ppm")
cx1 = ship_centroid_x(m1b)
if cx0 is not None and cx1 is not None:
    smoved = cx1 - cx0
    check(60 <= smoved <= 150, "MILLIPED ship moves on forwarded arrow keys", f"moved={smoved:.0f}px for 6 keys (20px each)")
else:
    fail("ship centroid not measurable after move")
how = quit_and_confirm("MILLIPED.BIN")
check(how is not None and how.startswith("q"), "MILLIPED exits on q", f"{how}")
time.sleep(2.0)
m2 = q.shot("04_milli_closed.ppm")
d = region_diff(base, m2)
check(d < 8000, "desktop fully restored after MILLIPED exits", f"diff={d}")

subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)

if fails:
    log(f"GAMES TEST FAILED ({len(fails)})")
    for f in fails: log(f"  - {f}")
    raise SystemExit(1)
log("GAMES TEST PASSED (menu pinning, full-screen launch, key input, quit, desktop restore — both games)")
