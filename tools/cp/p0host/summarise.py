#!/usr/bin/env python3
"""tools/cp/p0host/summarise.py <rundir> — one run's process/memory/VRAM/CPU summary.

Inputs (written by run.sh/launch.sh/nav.py/sampler.py): t0_ns, events.log, samples.csv,
vram_before_b.  Prints key=value lines; every number is derived from those raw files.
  launch_to_prompt_s  t0 -> first "PRESS ... TO CONTINUE"
  launch_to_menu_s    t0 -> main menu recognised by OCR (screenshot cadence ~1.3 s)
  *_menu              sample nearest menu+10 s (the menu fps window start)
  *_peak              max over the whole run
  cpu_game_cores_*    game process utime+stime per wall second / CLK_TCK(100)
  host_busy_pct_*     /proc/stat busy share of all 32 CPUs over the same window
"""
import sys, os

R = sys.argv[1]
t0 = int(open(R + "/t0_ns").read())
ev = []
for l in open(R + "/events.log"):
    ts, _, s = l.strip().partition(" ")
    if not ts.isdigit(): continue           # e.g. a pre-hook's own output line
    if "lock held=" in s: ev = []          # a re-used run dir: keep only the last attempt
    ev.append((int(ts), s))
def first(sub):
    for ts, s in ev:
        if sub in s: return ts
    return None
rows = []
for l in list(open(R + "/samples.csv"))[1:]:
    f = l.strip().split(",")
    rows.append(f)
g = [r for r in rows if r[1]]
num = lambda r, i: int(r[i]) if r[i] else 0
out = {}
tp, tm = first("press-to-continue"), first("main menu seen")
out["launch_to_prompt_s"] = f"{(tp-t0)/1e9:.1f}" if tp else "n/a"
out["launch_to_menu_s"] = f"{(tm-t0)/1e9:.1f}" if tm else "n/a"
vb = int(open(R + "/vram_before_b").read()) if os.path.exists(R + "/vram_before_b") else None
if g:
    out["samples"] = len(g)
    out["rss_peak_mib"] = max(num(r, 3) for r in g) // 1024           # VmHWM
    out["threads_peak"] = max(num(r, 4) for r in g)
    out["fds_peak"] = max(num(r, 5) for r in g)
    out["game_vram_peak_mib"] = max(num(r, 10) for r in g) // 1024
    out["sys_vram_peak_mib"] = max(int(r[8]) for r in rows if r[8]) >> 20
    if vb is not None:
        out["sys_vram_before_mib"] = vb >> 20
        out["sys_vram_delta_peak_mib"] = (max(int(r[8]) for r in rows if r[8]) - vb) >> 20
    def window(a, b, label):
        w = [r for r in g if a <= int(r[0]) <= b]
        if len(w) < 2: return
        dt = (int(w[-1][0]) - int(w[0][0])) / 1e9
        out[f"cpu_game_cores_{label}"] = f"{(num(w[-1],6)-num(w[0],6))/100/dt:.2f}"
        out[f"cpu_wine_all_cores_{label}"] = f"{(num(w[-1],7)-num(w[0],7))/100/dt:.2f}"
        hb = int(w[-1][11]) - int(w[0][11]); ht = int(w[-1][12]) - int(w[0][12])
        out[f"host_busy_pct_{label}"] = f"{100*hb/ht:.1f}"
        mid = w[len(w)//2]
        out[f"rss_mib_{label}"] = num(mid, 2) // 1024
        out[f"threads_{label}"] = num(mid, 4)
        out[f"fds_{label}"] = num(mid, 5)
        out[f"game_vram_mib_{label}"] = num(mid, 10) // 1024
        out[f"sys_vram_mib_{label}"] = int(mid[8]) >> 20
    on, off = first("mangohud log ON (menu)"), first("mangohud log OFF (menu)")
    if on and off: window(on, off, "menu")
    elif tm: window(tm + 10**10, tm + 4 * 10**10, "menu")
    br = first("benchmark confirm"); bd = first("benchmark result")
    if br and bd: window(br + 25 * 10**9, bd - 5 * 10**9, "bench")
for k, v in out.items():
    print(f"{k}={v}")
