#!/usr/bin/env python3
"""Boot RimWorld on the staged image and timestamp every load milestone.

Measures, relative to the click that launches it:
  spawn          RWLAUNCH.BIN window
  dll25/dll50    N distinct managed assemblies loaded
  engine         "Initialize engine version:"
  gfx            "GfxDevice: creating device client"
  defs           first def-database / cross-reference line
  defs_done      last cross-reference before death, or the crash line
  end            fault / process exit, and how long it survived

Usage: rw_perf.py <label> [seconds]
"""
import sys, time, re, json
sys.path.insert(0, "tools/rw")
from rw_desktop_drive import Qmp

label = sys.argv[1] if len(sys.argv) > 1 else "run"
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 300
LOG = f"/tmp/rw-perf-{label}.log"
QMP = "/tmp/hb-qmp.sock"

q = Qmp(QMP)


def txt():
    try:
        return open(LOG, errors="ignore").read()
    except Exception:
        return ""


# wait for the desktop, then click the taskbar entry that launches RWLAUNCH
for _ in range(80):
    if "Mode: DESKTOP" in txt():
        break
    time.sleep(3)
time.sleep(45)

spawned = False
for _ in range(3):
    q.click(120, 1058, 1920, 1080, hold=0.25)
    for _ in range(10):
        if "[HBHOST] RWLAUNCH.BIN pid" in txt():
            spawned = True
            break
        time.sleep(2)
    if spawned:
        break
if not spawned:
    print(json.dumps({"label": label, "error": "no spawn"}))
    sys.exit(0)

t0 = time.time()
marks = {}
dlls_seen = set()
while time.time() - t0 < limit:
    s = txt()
    for m in re.findall(r"(/[^\r\n]*?\.dll)\r?$", s, re.M):
        dlls_seen.add(m)
    n = len(dlls_seen)
    if "dll25" not in marks and n >= 25:
        marks["dll25"] = round(time.time() - t0, 1)
    if "dll50" not in marks and n >= 50:
        marks["dll50"] = round(time.time() - t0, 1)
    if "engine" not in marks and "Initialize engine version:" in s:
        marks["engine"] = round(time.time() - t0, 1)
    if "gfx" not in marks and "GfxDevice: creating device client" in s:
        marks["gfx"] = round(time.time() - t0, 1)
    if "defs" not in marks and "cross-reference" in s:
        marks["defs"] = round(time.time() - t0, 1)
    # A no-handler fault kills ONE thread; Unity keeps loading (measured: the
    # engine banner is logged AFTER the fault).  Only a process exit ends a run.
    if "fault-no-handler" in s and "fault" not in marks:
        marks["fault"] = round(time.time() - t0, 1)
    if "[HBHOST] RWLAUNCH.BIN exited" in s:
        marks["exit"] = round(time.time() - t0, 1)
        break
    time.sleep(1)

s = txt()
marks["dlls"] = len(dlls_seen)
marks["xrefs"] = s.count("cross-reference")
marks["wedges"] = s.count("YLOCK-WEDGE")
marks["fatram"] = 1 if "FAT resident in RAM" in s else 0
m = re.search(r"fault-no-handler[^\r\n]*", s)
if m:
    marks["fault_line"] = m.group(0)[:120]
out = {"label": label, **marks}
print(json.dumps(out))
with open(f"/tmp/rw-perf-{label}.json", "w") as f:
    json.dump(out, f)
