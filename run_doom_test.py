#!/usr/bin/env python3
"""T5 playthrough harness — DOOM (doomgeneric) native port on HobbyOS, QEMU.

Boots the desktop under QEMU (virtio-gpu), launches DOOM.BIN from the Apps
menu (launcher slot 12), and asserts a real raycaster frame appears on screen
(via QMP screendump) within the 30-second rule, that the frame is a raycaster
(horizon signature: top/ceiling region differs from the bottom/floor region,
and there are many distinct nonzero pixels), and that forwarded arrow-key
input moves the view (two screendumps differ → deterministic movement).

Auto-fail on: no raycaster frame within 30 s, no DOOM process on the serial
log, or "IWAD not found" (asset missing → run tools/fetch_doom_wad.sh).

Usage: ARCH=arm python3 run_doom_test.py     (ARCH in {arm, intel})
Result: exit 0 on success; screendumps + serial in /tmp/hobbyos_doom/.

Tier: T5 (QMP input + screendump assertions).  Needs a built boot image
(`make disk.img`) — the harness itself only drives the already-built OS.
"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.expanduser("~/Documents/GitHub/HobbyOS")
os.chdir(ROOT)
ARCH = os.environ.get("ARCH", "arm")
W, H = 1920, 1080
SHOT_DIR = "/tmp/hobbyos_doom"
SER = "/tmp/doom_test_serial.log"
QMP = "/tmp/qmp-doom-test"
DOOM_SLOT = 18            # 16 pinned apps (0..15) + PONG=16 MILLIPED=17 + DOOM=18
MAX_WAIT_S = 30           # the 30-second rule
TASKBAR_Y = H - 26

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

def _region_diff(fa, fb):
    """Count differing pixels between two PPM frames (same geometry)."""
    ra, oa, wa, ha = parse_ppm(fa); rb, ob, wb, hb = parse_ppm(fb)
    if (wa, ha) != (wb, hb): return -1
    row = wa * 3
    diffs = 0
    for y in range(0, ha):
        a = ra[oa+y*row: oa+(y+1)*row]; b = rb[ob+y*row: ob+(y+1)*row]
        if a == b: continue
        for x in range(0, row, 3):
            if a[x:x+3] != b[x:x+3]: diffs += 1
    return diffs

def center_region_stats(path):
    """Return (nonzero_pixels, ncolors, top_mean_g, bot_mean_g) for the
    centred DOOM area (a 1280x800 region on the 1920x1080 frame)."""
    raw, off, w, h = parse_ppm(path)
    # DOOM buffer: 640x400 scaled 2x => 1280x800 centred.
    rx0, ry0, rw, rh = (W - 1280) // 2, (H - 800) // 2, 1280, 800
    row = w * 3
    nonzero = 0; colors = set(); top_sum = 0; bot_sum = 0; n = 0
    for y in range(ry0, ry0 + rh):
        r = raw[off + y*row : off + (y+1)*row]
        for x in range(rx0*3, (rx0+rw)*3, 3):
            R, G, B = r[x], r[x+1], r[x+2]
            if R or G or B:
                nonzero += 1
                colors.add((R >> 4, G >> 4, B >> 4))
            if y < ry0 + rh//3: top_sum += G
            elif y > ry0 + 2*rh//3: bot_sum += G
        n += 1
    half = rh//3 * rw
    return nonzero, len(colors), top_sum / max(half,1), bot_sum / max(half,1)

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
doom_i = next((i for i, n in entries.items() if n.strip() == "DOOM.BIN"), None)
log(f"menu: DOOM.BIN idx={doom_i}")
check(doom_i == DOOM_SLOT, "DOOM.BIN pinned at launcher slot 12", f"idx={doom_i}")
if doom_i is None:
    # fall back to parsing ':DOOM.BIN' tokens if the menu regex is brittle
    if "DOOM.BIN" in txt: doom_i = DOOM_SLOT
    else:
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
        log("FATAL: DOOM.BIN not found in start-menu dump")
        sys.exit(1)

# ---- launch ------------------------------------------------------------
q.click(38, TASKBAR_Y + 13)
time.sleep(0.8)
q.key("home", 0.3)
for _ in range(doom_i): q.key("down", 0.06)
q.key("ret", 0.3)

# ---- T5: assert a raycaster frame within the 30-second rule ------------
t0 = time.time()
seen = None; last = None
while time.time() - t0 < MAX_WAIT_S:
    shot = q.shot("doom_frame.ppm")
    nz, nc, tmean, bmean = center_region_stats(shot)
    last = (shot, nz, nc, tmean, bmean)
    # A real raycaster: lots of nonzero pixels, many colours, and a horizon
    # (ceiling region != floor region).
    if nz > 200000 and nc > 32 and abs(tmean - bmean) > 6:
        seen = (shot, nz, nc, tmean, bmean)
        break
    if serial_has(r"\bIWAD not found\b|couldn't find IWAD|IWAD.*(not found|couldn't)") and re.search(r"\[LAUNCH\].*DOOM|/DOOM\.BIN", open(SER, errors="ignore").read()):
        fail("IWAD not found in image; run tools/fetch_doom_wad.sh and rebuild")
        break
    time.sleep(1.0)

(shot, nz, nc, tmean, bmean) = seen if seen else last
check(seen is not None, f"raycaster frame within {MAX_WAIT_S}s rule",
      f"t={time.time()-t0:.1f}s nonzero={nz} colors={nc} ceilG={tmean:.0f} floorG={bmean:.0f}")
check(nz > 200000, "DOOM rendered a populated screen", f"nonzero={nz}")
check(nc > 32, "many distinct colours (textures/HUD)", f"colors={nc}")
check(abs(tmean - bmean) > 6, "horizon signature (ceiling != floor mean green)",
      f"{tmean:.0f} vs {bmean:.0f}")

# ---- T5: forwarded arrow-key input must move the view -------------------
if seen is not None:
    for _ in range(6): q.key("up", 0.1)   # walk forward
    time.sleep(1.0)
    moved = q.shot("doom_after_move.ppm")
    nz2, nc2, _, _ = center_region_stats(moved)
    # two screendumps at different times/locations must differ
    diff = _region_diff(shot, moved)
    check(diff > 0, "view changed (game is live / input forwarded)", f"diffs={diff}")

if fails:
    log(f"RESULT: FAIL ({len(fails)})")
    for f in fails: log("  - " + f)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(1)
log("RESULT: PASS (T5 playthrough harness green)")
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
sys.exit(0)
