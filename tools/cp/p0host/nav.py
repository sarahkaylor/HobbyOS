#!/usr/bin/env python3
"""tools/cp/p0host/nav.py <tag> [--bench] [--menu-log S] [--timeout S]

Drives one launched run (see launch.sh) by gamescope screenshots + tesseract OCR and
xdotool key presses into gamescope's Xwayland.  Appends timestamped events to
runs/<tag>/events.log (epoch ns, event).  Exits 0 when done, 2 on timeout.

Sequence (measured on game 2.5 / Proton Experimental 11, 2026-11-14):
  boot video "PRESS [Space] TO CONTINUE"  -> Space
  main menu (CONTINUE/NEW GAME/LOAD GAME/SETTINGS/MODS/CREDITS/QUIT GAME)
  [--menu-log S] MangoHud Shift_L+F2 on, S seconds, off
  [--bench] Left (focus CONTINUE), Down x3 (SETTINGS, verified by the yellow focus box),
            Return, '3' x3 (GRAPHICS tab), 'b' (RUN BENCHMARK), Return on "Are you sure
            you would like to run a benchmark?"; wait for a new
            Documents/.../benchmarkResults/<dir>/summary.json
"""
import os, sys, time, subprocess, glob, argparse
from PIL import Image

P0 = "/home/sarah/cp/p0host"
ap = argparse.ArgumentParser()
ap.add_argument("tag"); ap.add_argument("--bench", action="store_true")
ap.add_argument("--menu-log", type=int, default=0)
ap.add_argument("--menu-settle", type=int, default=10)
ap.add_argument("--timeout", type=int, default=900)
ap.add_argument("--pfx", default=P0 + "/pfx-proton/1091500")
ap.add_argument("--stop-at-menu", action="store_true")
a = ap.parse_args()
R = f"{P0}/runs/{a.tag}"
BR = a.pfx + "/pfx/drive_c/users/steamuser/Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults"
t_start = time.time()
n = 0

def ev(s):
    with open(R + "/events.log", "a") as f:
        f.write(f"{time.time_ns()} {s}\n")
    print(f"[{time.time()-t_start:7.1f}] {s}", flush=True)

def disp():
    return open(R + "/display").read().strip()

def key(*ks, hold=0.08):
    env = dict(os.environ, DISPLAY=disp())
    for k in ks:
        subprocess.run(["xdotool", "keydown", k], env=env)
        time.sleep(hold)
        subprocess.run(["xdotool", "keyup", k], env=env)
        time.sleep(0.45)

def mango_toggle():
    env = dict(os.environ, DISPLAY=disp())
    subprocess.run(["xdotool", "keydown", "Shift_L", "keydown", "F2"], env=env)
    time.sleep(0.15)
    subprocess.run(["xdotool", "keyup", "F2", "keyup", "Shift_L"], env=env)

def shot(name=None):
    global n
    n += 1
    name = name or f"n{n:04d}"
    r = subprocess.run([P0 + "/tools/shot.sh", a.tag, name], capture_output=True, text=True)
    return name, r.stdout.upper()

def check_timeout():
    if time.time() - t_start > a.timeout:
        ev("TIMEOUT"); sys.exit(2)

def alive():
    gp = open(R + "/gamescope.pid").read().strip()
    return os.path.exists(f"/proc/{gp}")

MENU_Y = {"CONTINUE": 289, "NEW GAME": 326, "LOAD GAME": 364, "SETTINGS": 400,
          "MODS": 437, "CREDITS": 475, "QUIT GAME": 512}   # at 1280x720; scaled by H/720

def focus_item(png):
    im = Image.open(png).convert("RGB"); W, H = im.size; s = H / 720
    px = im.load(); ys = []
    for y in range(int(260*s), int(540*s)):
        c = 0
        for x in range(int(118*s), int(385*s), 2):
            r, g, b = px[x, y]
            if r > 200 and g > 170 and b < 110: c += 1
        if c > 40: ys.append(y)
    if not ys: return None
    yc = (min(ys) + max(ys)) / 2 / s
    return min(MENU_Y, key=lambda k: abs(MENU_Y[k] - yc))

def game_pid():
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try: a0 = open(f"/proc/{p}/cmdline").read().split("\0", 1)[0]
        except OSError: continue
        if "Cyberpunk2077.exe" in a0 and not a0.startswith("/"): return int(p)
    return None

def snap_proc(label):
    """copy /proc/<game>/{maps,status,smaps_rollup,limits} + the task names into the run dir"""
    pid = game_pid()
    if not pid: ev(f"snapshot {label}: no game pid"); return
    for f in ("maps", "status", "smaps_rollup", "limits"):
        try:
            open(f"{R}/proc_{label}_{f}.txt", "w").write(open(f"/proc/{pid}/{f}").read())
        except OSError as e:
            ev(f"snapshot {label} {f}: {e}")
    names, avx = [], []
    for t in os.listdir(f"/proc/{pid}/task"):
        try: names.append(open(f"/proc/{pid}/task/{t}/comm").read().strip())
        except OSError: pass
        try:   # x86 only: AVX512_elapsed_ms != -1 => this thread's XSAVE state had AVX-512 in use
            v = open(f"/proc/{pid}/task/{t}/arch_status").read().split(":")[1].strip()
            avx.append(f"{t} {names[-1] if names else '?'} AVX512_elapsed_ms={v}")
        except (OSError, IndexError): pass
    open(f"{R}/proc_{label}_arch_status.txt", "w").write("\n".join(avx) + "\n")
    open(f"{R}/proc_{label}_threads.txt", "w").write("\n".join(sorted(names)) + "\n")
    try: nfd = len(os.listdir(f"/proc/{pid}/fd"))
    except OSError: nfd = -1
    fds = []
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try: fds.append(os.readlink(f"/proc/{pid}/fd/{fd}"))
        except OSError: pass
    open(f"{R}/proc_{label}_fds.txt", "w").write("\n".join(sorted(fds)) + "\n")
    # who else is using the machine right now (the lock should make this only us)
    try:
        open(f"{R}/proc_{label}_top.txt", "w").write(
            subprocess.run(["top", "-b", "-n", "1", "-w", "180"], capture_output=True, text=True).stdout[:6000])
    except OSError:
        pass
    # every process of this run (environ carries our prefix path): name, threads, RSS
    tree, tot_thr = [], 0
    for q in os.listdir("/proc"):
        if not q.isdigit(): continue
        try:
            envb = open(f"/proc/{q}/environ", "rb").read()
            if a.pfx.encode() not in envb: continue
            st = dict(l.split(":", 1) for l in open(f"/proc/{q}/status").read().splitlines() if ":" in l)
            a0 = open(f"/proc/{q}/cmdline").read().split("\0")[0]
        except OSError:
            continue
        thr = int(st.get("Threads", "0").strip()); tot_thr += thr
        tree.append(f"{q}\t{thr}\t{st.get('VmRSS','').strip()}\t{st.get('Name','').strip()}\t{a0[:120]}")
    open(f"{R}/proc_{label}_tree.txt", "w").write("pid\tthreads\trss\tcomm\targv0\n" + "\n".join(sorted(tree)) +
                                                f"\n# processes={len(tree)} threads={tot_thr}\n")
    ev(f"snapshot {label} pid={pid} threads={len(names)} fds={nfd} run_processes={len(tree)} run_threads={tot_thr}")

ev("nav start")
# 1. boot -> press-to-continue (or straight to menu)
state = None
while True:
    check_timeout()
    if not alive(): ev("game gone before menu"); sys.exit(3)
    nm, t = shot()
    if "QUIT GAME" in t or ("NEW GAME" in t and "SETTINGS" in t):
        state = "menu"; break
    if "TO CONTINUE" in t or "TOCONTINUE" in t.replace(" ", ""):
        ev(f"press-to-continue seen ({nm})"); key("space"); state = "pressed"
        time.sleep(1.0)
        continue
    time.sleep(0.7)
ev(f"main menu seen ({nm})")
time.sleep(a.menu_settle)
snap_proc("menu")
if a.menu_log:
    shot("menu_before_log")
    mango_toggle(); ev("mangohud log ON (menu)")
    time.sleep(a.menu_log)
    mango_toggle(); ev("mangohud log OFF (menu)")
    shot("menu_after_log")
if a.stop_at_menu:
    ev("nav done (menu)"); sys.exit(0)
if not a.bench:
    ev("nav done"); sys.exit(0)

before = set(glob.glob(BR + "/*"))
# 2. main menu -> SETTINGS
key("Left")
for _ in range(8):
    nm, _t = shot()
    f = focus_item(f"{R}/shots/{nm}.png")
    ev(f"menu focus = {f}")
    if f == "SETTINGS": break
    order = list(MENU_Y)
    if f is None: key("Left"); continue
    key("Down" if order.index(f) < order.index("SETTINGS") else "Up")
key("Return"); time.sleep(2.0)
for _ in range(3): key("3")
time.sleep(1.0)
nm, t = shot("graphics_tab")
ev(f"settings tab ({nm}) benchmark-visible={'BENCHMARK' in t}")
key("b"); ev("benchmark requested (b)")
for _ in range(6):
    time.sleep(1.0)
    nm, t = shot()
    if "RUN A BENCHMARK" in t or "LIKE TO RUN" in t:
        key("Return"); ev(f"benchmark confirm dialog answered ({nm})"); break
# 3. wait for the result directory
snapped_bench = False
while True:
    check_timeout()
    if not alive(): ev("game gone during benchmark"); sys.exit(3)
    new = [d for d in glob.glob(BR + "/*") if d not in before and os.path.exists(d + "/summary.json")]
    if new:
        time.sleep(2)
        ev(f"benchmark result {os.path.basename(new[0])}")
        snap_proc("postbench")
        shot("bench_result")
        break
    nm, t = shot()
    if not snapped_bench and "FPS:" in t:
        time.sleep(20); snap_proc("bench"); snapped_bench = True
    time.sleep(2)
ev("nav done")
