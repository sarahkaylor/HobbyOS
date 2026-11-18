#!/usr/bin/env python3
"""tools/cp/ntsync/ab_summary.py RUN_DIR... -- one row per P9 fpsprobe boot
(C2-ntsync, docs/cp-ntsync.md §5): the world phase's winevulkan fps (median,
p10, rows, from run-p9's summary.txt), and from the kernel's [THRW] rows
(tools/cp/present/hbv_summary.py's per-phase means) the wineserver's share of
a vCPU and its kernel part, the game group's running / in-kernel threads, its
two hottest threads (run%/kern% of wall time), and whether the run used
/dev/ntsync (the last [NTSYNC] stats row of serial.log)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HBV = os.path.join(HERE, "..", "present", "hbv_summary.py")


def world_fps(run):
    p = os.path.join(run, "summary.txt")
    if not os.path.exists(p):
        return None
    for l in open(p, errors="replace"):
        f = l.split()
        if f and f[0] == "world" and len(f) >= 5:
            return float(f[3]), float(f[4]), int(f[1])
    return None


def thrw(run, phase="world"):
    try:
        out = subprocess.run([sys.executable, "-I", HBV, run], capture_output=True,
                             text=True, timeout=120).stdout
    except Exception:
        return {}
    res = {}
    cur = None
    for l in out.splitlines():
        m = re.match(r"== (\S+)\s+pid(\d+):(\S+)\s+rows\s+(\d+)\s+(.*)", l)
        if m:
            cur = None
            if m.group(1) != phase:
                continue
            name = m.group(3)
            key = "ws" if "wineserver" in name else ("game" if "Cyberpunk" in name else None)
            if not key:
                continue
            v = dict(re.findall(r"(\w+) ([\d.]+)", m.group(5)))
            res[key] = {k: float(x) for k, x in v.items()}
            cur = key
            continue
        if cur == "game" and l.strip().startswith("top"):
            res["top"] = re.findall(r"t(\d+) (\d+)/(\d+)/(\d+)/(\d+)%", l)[:2]
    return res


def ntsync_row(run):
    p = os.path.join(run, "serial.log")
    last = ""
    if os.path.exists(p):
        for l in open(p, errors="replace"):
            if "[NTSYNC] stats" in l or "[NTSYNC] device-closed" in l:
                last = l.strip()
    return last


print("%-10s %-22s %-18s %-26s %s" % ("run", "world fps med (p10) n", "wineserver run/kern",
                                      "game run/kern (threads)", "hot threads run%/kern%"))
for run in sys.argv[1:]:
    name = os.path.basename(run.rstrip("/"))
    w = world_fps(run)
    t = thrw(run)
    ws = t.get("ws", {})
    g = t.get("game", {})
    top = " ".join("t%s %s/%s" % (tid, r, k) for tid, r, _, k, _ in t.get("top", []))
    print("%-10s %-22s %-18s %-26s %s" % (
        name,
        "%.1f (%.1f) %d" % w if w else "-",
        "%.2f / %.2f" % (ws.get("run", 0), ws.get("kern", 0)) if ws else "-",
        "%.2f / %.2f" % (g.get("run", 0), g.get("kern", 0)) if g else "-",
        top or "-"))
    n = ntsync_row(run)
    if n:
        print("           " + n[:230])
