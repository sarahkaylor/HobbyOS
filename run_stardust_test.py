#!/usr/bin/env python3
"""T5 playthrough harness — STARDUST (GB-038, original arcade dodge game)
on HobbyOS, under QEMU.

Boots the desktop under QEMU (virtio-gpu), launches STARDUST.BIN from the
Apps menu (launcher slot 12, right after PONG=10 / MILLIPED=11), and asserts
a real game frame appears on screen (via QMP screendump) within the
30-second rule, that the frame has STARDUST's visual signature, and that
forwarded Left/Right input moves the ship.

STARDUST signature (full-screen, 1920x1080):
  - dominant dark background COLOR_BG (4,4,12)
  - a cyan ship (COLOR_SHIP 80,220,255) in the bottom band
    (PLAYER_BASE_Y=1036 .. +PLAYER_H=16) — the ship row
  - orange meteors (COLOR_HAZARD 255,120,60) somewhere above the ship
  - pressing Left then Right shifts the bottom-band cyan centroid
    (deterministic movement => input is forwarded and the game is live)

Auto-fail on: no game frame within 30 s, no STARDUST process on the serial
log, or a fatal link/graphics error in the serial log.

Usage: ARCH=arm python3 run_stardust_test.py     (ARCH in {arm, intel})
Result: exit 0 on success; screendumps + serial in /tmp/hobbyos_stardust/.

Tier: T5 (QMP input + screendump assertions).  Needs a built boot image
(`make disk.img`) — the harness itself only drives the already-built OS.
"""
import json, os, re, socket, subprocess, sys, time

ROOT = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/wgames-stardust"
os.chdir(ROOT)
ARCH = os.environ.get("ARCH", "arm")
W, H = 1920, 1080
SHOT_DIR = "/tmp/hobbyos_stardust"
SER = "/tmp/stardust_test_serial.log"
QMP = "/tmp/qmp-stardust-test"
STARDUST_SLOT = 18        # 16 pinned apps (0..15) + PONG=16 MILLIPED=17 + STARDUST=18
MAX_WAIT_S = 30           # the 30-second rule
TASKBAR_Y = H - 26

# STARDUST geometry (matches src/user/stardust.c)
SHIP_Y0, SHIP_H, SHIP_W = 1036, 16, 42
COLOR_BG = (4, 4, 12)
COLOR_SHIP = (80, 220, 255)
COLOR_HAZARD = (255, 120, 60)

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

def frame_stats(path):
    """Return (nonzero, ncolors, bg_fraction, ship_centroid_x, hazard_px).
    - ship_centroid_x: mean x of cyan pixels in the ship's bottom band,
      or -1 if none found.
    - hazard_px: count of orange pixels anywhere in the frame.
    """
    raw, off, w, h = parse_ppm(path)
    row = w * 3
    nonzero = 0; colors = set(); n = 0; bg = 0
    shx = 0.0; shn = 0
    haz = 0
    for y in range(0, h):
        r = raw[off + y*row : off + (y+1)*row]
        for x in range(0, w*3, 3):
            R, G, B = r[x], r[x+1], r[x+2]
            if R or G or B: nonzero += 1
            colors.add((R >> 4, G >> 4, B >> 4))
            n += 1
            if (R, G, B) == COLOR_BG: bg += 1
            if SHIP_Y0 <= y < SHIP_Y0 + SHIP_H and abs(R-80) < 40 and abs(G-220) < 40 and abs(B-255) < 40:
                shx += x//3; shn += 1
            if y < SHIP_Y0 and abs(R-255) < 40 and abs(G-120) < 40 and abs(B-60) < 40:
                haz += 1
    return nonzero, len(colors), bg/n, (shx/shn) if shn else -1, haz

def serial_has(pat):
    return re.search(pat, open(SER, errors="ignore").read()) is not None

def check(cond, what, detail=""):
    if cond: log(f"PASS: {what} ({detail})")
    else: fail(f"{what} ({detail})")

# ---- boot ---------------------------------------------------------------
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]" + ("aarch64" if ARCH=="arm" else "x86_64")], stderr=subprocess.DEVNULL)
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
    r"\[MENU\]\s*(?:\s*\[\s*\d+\]\s*)*(\d+)(?:\s*[\[\]A-Z0-9 ]*\s*)*=(\s*[A-Za-z0-9_.]+)", txt))
sd_i = next((i for i, n in entries.items() if n.strip() == "STARDUST.BIN"), None)
log(f"menu: STARDUST.BIN idx={sd_i}")
check(sd_i == STARDUST_SLOT, "STARDUST.BIN pinned at launcher slot 18", f"idx={sd_i}")
if sd_i is None:
    if "STARDUST.BIN" in txt: sd_i = STARDUST_SLOT
    else:
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
        log("FATAL: STARDUST.BIN not found in start-menu dump")
        sys.exit(1)

# ---- launch -------------------------------------------------------------
q.click(38, TASKBAR_Y + 13)
time.sleep(0.8)
q.key("home", 0.3)
for _ in range(sd_i): q.key("down", 0.06)
q.key("ret", 0.3)

# ---- T5: assert a STARDUST frame within the 30-second rule --------------
t0 = time.time()
seen = None; last = None
while time.time() - t0 < MAX_WAIT_S:
    shot = q.shot("stardust_frame.ppm")
    nz, nc, bgf, shx, haz = frame_stats(shot)
    last = (shot, nz, nc, bgf, shx, haz)
    # STARDUST signature: populated, mostly dark, cyan ship in row, orange meteors.
    if nz > 50000 and bgf > 0.5 and shx >= 0 and haz > 0:
        seen = (shot, nz, nc, bgf, shx, haz)
        break
    if serial_has(r"Failed to initialize graphics|FATAL|STARDUST: Failed"):
        fail("graphics init failed in serial log")
        break
    time.sleep(1.0)

(shot, nz, nc, bgf, shx, haz) = seen if seen else last
check(seen is not None, f"STARDUST frame within {MAX_WAIT_S}s rule",
      f"t={time.time()-t0:.1f}s nonzero={nz} bg={bgf:.2f} ship_x={shx:.0f} hazards={haz}")
check(nz > 50000, "STARDUST rendered a populated screen", f"nonzero={nz}")
check(bgf > 0.5, "dark background dominates (COLOR_BG)", f"bg_frac={bgf:.2f}")
check(shx >= 0, "cyan ship present in bottom band", f"ship_centroid_x={shx:.0f}")
check(haz > 0, "orange meteors present above the ship", f"hazard_px={haz}")

# ---- T5: forwarded Left/Right input must move the ship -------------------
if seen is not None:
    # nudge far right, read ship x; far left, read ship x.
    for _ in range(20): q.key("right", 0.05)
    time.sleep(0.8)
    rshot = q.shot("stardust_right.ppm")
    _, _, _, rx, _ = frame_stats(rshot)
    for _ in range(60): q.key("left", 0.05)
    time.sleep(0.8)
    lshot = q.shot("stardust_left.ppm")
    _, _, _, lx, _ = frame_stats(lshot)
    check(rx >= 0 and lx >= 0, "ship visible during input test", f"rx={rx:.0f} lx={lx:.0f}")
    check(lx < rx, "ship moved LEFT after left-arrow input (game is live / input forwarded)",
          f"lx={lx:.0f} vs rx={rx:.0f}")

if fails:
    log(f"RESULT: FAIL ({len(fails)})")
    for f in fails: log("  - " + f)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(1)
log("RESULT: PASS (T5 playthrough harness green)")
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
sys.exit(0)
