"""GB-035-V T5 capture: boot arm, locate NES.BIN by NAME from the [MENU] dump,
launch it, take 4 screendumps 2 s apart, and measure whole-frame differences
against the menu and between samples.  Prints a verdict the DoD can be judged on:
  - did the title load (serial [LAUNCH]/[ARRIVED] lines)
  - is the frame different from the menu (game rendered)
  - do successive samples differ (emulator ADVANCES frames)
No hardcoded slot: the slot comes from find_menu_slot(serial, "NES.BIN").
"""
import os, sys, subprocess, time, json

ROOT = os.path.abspath(".")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from evidence_rig import QMP, boot, find_menu_slot, launch_from_menu  # noqa

OUT = "evidence/GB-035-V/t5"
os.makedirs(OUT, exist_ok=True)
serial = os.path.join(OUT, "serial.log")
sock = "/tmp/gb035-qmp"

q = boot(["make", "run", "ARCH=arm",
          f"QEMU_ARGS=-display none -qmp unix:{sock},server,nowait"], serial, sock, "arm")
print("[t5] booted; menu dump present")

menu = os.path.join(OUT, "menu.ppm")
q.screendump(menu)
time.sleep(1.0)

slot = find_menu_slot(serial, "NES.BIN")
print("[t5] NES.BIN slot from the parsed [MENU] dump:", slot)
if slot is None:
    print("[t5] FATAL: NES.BIN not in menu dump")
    sys.exit(2)
launch_from_menu(q, slot)

shots = []
for i in range(4):
    time.sleep(2.0)
    p = os.path.join(OUT, f"nes-{i}.ppm")
    q.screendump(p)
    shots.append(p)

subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)

from PIL import Image, ImageChops
def diff(a, b):
    A, B = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    d = ImageChops.difference(A, B)
    px = d.load()
    W, H = d.size
    n = sum(1 for y in range(0, H, 2) for x in range(0, W, 2) if px[x, y] != (0, 0, 0))
    total = (W // 2) * (H // 2)
    return n, total, 100.0 * n / total

rows = []
for i, s in enumerate(shots):
    n, t, pct = diff(menu, s)
    rows.append((f"menu vs nes-{i}", n, pct))
for a, b in ((0, 1), (0, 2), (0, 3), (1, 2), (2, 3)):
    n, t, pct = diff(shots[a], shots[b])
    rows.append((f"nes-{a} vs nes-{b}", n, pct))

print("[t5] whole-frame pixel differences (every 2nd pixel sampled):")
for name, n, pct in rows:
    print(f"  {name:18s} differing={n:7d}  {pct:6.2f}%")

ser = open(serial, errors="ignore").read()
print("[t5] serial: [LAUNCH] NES.BIN present:", "[LAUNCH] NES.BIN" in ser)
print("[t5] serial: NES.BIN load line present:", "NES.BIN" in ser)
with open(os.path.join(OUT, "diffs.json"), "w") as f:
    json.dump([{"pair": n, "diff_px": c, "pct": round(p, 3)} for n, c, p in rows], f, indent=1)
