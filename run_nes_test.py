#!/usr/bin/env python3
"""T5 playthrough harness — NES emulator (GB-035) on HobbyOS, under QEMU.

Boots the desktop, launches NES.BIN from the Apps menu, and asserts the
compact NES emulator boots the hand-authored homebrew ROM and renders its
recognizable frame within the 30-second rule: a bright field with a dark 8px
band across the top of the centred 4x-upscaled NES buffer. Then it asserts
the screen responds to input (Space inverts the frame -> many pixels flip),
which demonstrates the emulator is running and input reaches it.

De-magic-indexed launcher detection (parsed [MENU] dump, presence + adjacency
to the other pinned games). Needs a built boot image (`make disk.img`).

Usage: ARCH=arm python3 run_nes_test.py   (ARCH in {arm, intel})
Result: exit 0 on success.
"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.environ.get("HOBBYOS_ROOT",
                      "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/wgames-gb035")
os.chdir(ROOT)
print(f"[TEST] HOBBYOS_ROOT={ROOT}", flush=True)
ARCH = os.environ.get("ARCH", "arm")
W, H = 1920, 1080
SHOT_DIR = "/tmp/hobbyos_nes"
SER = "/tmp/nes_test_serial.log"
QMP = "/tmp/qmp-nes-test"
MAX_WAIT_S = 30
TASKBAR_Y = H - 26
# NES buffer is 256x240 at 4x = 1024x960, centred on 1920x1080:
NX0, NY0, NW, NH = (W-1024)//2, (H-960)//2, 1024, 960

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
        s.s.sendall(json.dumps(m).encode() + b"\n"); buf = b""
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
        s.c("input-send-event", {"events": [{"type":"btn","data":{"button":"left","down":True}}]}); time.sleep(0.08)
        s.c("input-send-event", {"events": [{"type":"btn","data":{"button":"left","down":False}}]}); time.sleep(0.4)
    def key(s, k, pause=0.15):
        s.c("send-key", {"keys": [{"type":"qcode","data":k}]}); time.sleep(pause)
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

def frame(shot):
    """Return (bright_px, dark_top_px). bright_px = count of light-grey pixels
    in the NES area; dark_top_px = per-column checks of the top NES band."""
    raw, off, w, h = parse_ppm(shot)
    row = w * 3
    bright = 0; n = 0
    topband_dark = 0; topband_n = 0
    top_end = NY0 + 8*4                      # 8 NES rows of band at 4x
    for y in range(NY0, NY0 + NH, 3):
        r = raw[off + y*row : off + (y+1)*row]
        for x in range(NX0*3, (NX0+NW)*3, 6):
            R, G, B = r[x], r[x+1], r[x+2]
            if R > 120 and G > 120 and B > 120: bright += 1
            n += 1
            if y < top_end and R < 60 and G < 60 and B < 60:
                topband_dark += 1
            if y < top_end: topband_n += 1
    return bright, n, topband_dark, topband_n

def diff(pa, pb):
    ra, oa, wa, ha = parse_ppm(pa); rb, ob, wb, hb = parse_ppm(pb)
    row = wa * 3; d = 0
    for y in range(0, ha, 3):
        a = ra[oa+y*row:oa+(y+1)*row]; b = rb[ob+y*row:ob+(y+1)*row]
        for x in range(0, row, 6):
            if a[x:x+3] != b[x:x+3]: d += 1
    return d

def serial_has(p): return re.search(p, open(SER, errors="ignore").read()) is not None

def check(c, what, detail=""):
    if c: log(f"PASS: {what} ({detail})")
    else: fail(f"{what} ({detail})")

subprocess.run(["pkill","-9","-f","qemu-system[-]" + ("aarch64" if ARCH=="arm" else "x86_64")], stderr=subprocess.DEVNULL)
for f in (SER, QMP):
    if os.path.exists(f): os.remove(f)
qemu = "qemu-system-aarch64" if ARCH == "arm" else "qemu-system-x86_64"
subprocess.Popen(["stdbuf","-oL","make","run",f"ARCH={ARCH}",
                  f"QEMU_ARGS=-display none -qmp unix:{QMP},server,nowait"],
                 stdout=open(SER,"w"), stderr=subprocess.STDOUT)
q = Q(QMP)
t0 = time.time()
while time.time()-t0 < 180:
    if "Desktop starting" in open(SER, errors="ignore").read(): break
    time.sleep(0.5)
time.sleep(3)

txt = open(SER, errors="ignore").read()
entries = dict((int(a), b) for a,b in re.findall(
    r"\[MENU\]\s*(?:\s*\[\s*\d+\]\s*)*(\d+)(?:\s*[\[\]A-Z0-9 ]*\s*)*=(\s*[A-Za-z0-9_.]+)", txt))
name_idx = {n.strip(): i for i, n in entries.items()}
nes_i = name_idx.get("NES.BIN")
log(f"menu: (informational) parsed NES.BIN idx={nes_i}")
if nes_i is None:
    subprocess.run(["pkill","-9","-f","qemu-system[-]"], stderr=subprocess.DEVNULL)
    log("FATAL: NES.BIN not found in start-menu dump")
    sys.exit(1)
check(nes_i is not None, "NES.BIN present in launcher menu")
# adjacency: NES next to the newest pinned game (XCRYPT)
xcr_i = name_idx.get("XCRYPT.BIN")
check(xcr_i is not None and nes_i == xcr_i + 1,
      "NES.BIN pinned adjacent after XCRYPT.BIN", f"XCRYPT={xcr_i} NES={nes_i}")

q.click(38, TASKBAR_Y+13); time.sleep(0.8)
q.key("home", 0.3)
for _ in range(nes_i): q.key("down", 0.06)
q.key("ret", 0.3)

# T5: assert an NES frame within the 30-second rule
t0 = time.time(); seen = None; last = None
while time.time()-t0 < MAX_WAIT_S:
    shot = q.shot("nes_frame.ppm")
    bright, n, tbd, tbn = frame(shot)
    last = (shot, bright, n, tbd, tbn)
    # homebrew: bright field dominates + a dark top band present
    if bright > n//2 and tbd > 0.15 * tbn:
        seen = last; break
    if serial_has(r"NES: graphics_init failed|FATAL"):
        fail("graphics init failed in serial log"); break
    time.sleep(1.0)
(shot, bright, n, tbd, tbn) = seen if seen else last
check(seen is not None, f"NES frame within {MAX_WAIT_S}s (bright dominates + dark top band)",
      f"t={time.time()-t0:.1f}s bright_frac={bright/max(n,1):.2f} topband_dark_frac={tbd/max(tbn,1):.2f}")
check(bright > n//2, "bright field dominates the NES area", f"bright_frac={bright/max(n,1):.2f}")
check(tbn > 0 and tbd > 0.15*tbn, "dark band present across the top row",
      f"topband_dark_frac={tbd/max(tbn,1):.2f}")

# T5: input reaches the emulator -> the picture visibly changes (emulator live)
shot2 = q.shot("nes_before.ppm")
q.key("left", 0.4); time.sleep(4.0)   # one controller edge -> invert once (slow renderer)
shot3 = q.shot("nes_after.ppm")
d = diff(shot2, shot3)
check(d > 5000, "input reaches the emulator (frame visibly changes / game live)",
      f"diffs={d}")

if fails:
    log(f"RESULT: FAIL ({len(fails)})")
    subprocess.run(["pkill","-9","-f","qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(1)
log("RESULT: PASS (NES T5 playthrough harness green)")
subprocess.run(["pkill","-9","-f","qemu-system[-]"], stderr=subprocess.DEVNULL)
sys.exit(0)
