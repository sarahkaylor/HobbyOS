#!/usr/bin/env python3
"""tools/cp/hunt/tables.py LOGDIR -- the P8 and P9 tables of docs/cp-hunt.md
from run-hunt.sh's per-boot files (RUN.class.json, RUN.fps.csv, RUN.host.txt,
RUN.thrs.csv).  Markdown on stdout."""
import csv
import glob
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify import q10  # noqa: E402

d = sys.argv[1]
runs = sorted(glob.glob(os.path.join(d, "*.class.json")), key=os.path.getmtime)
p8, p9 = [], []
for j in runs:
    r = json.load(open(j))
    rid = r["run"]
    kind = open(os.path.join(d, rid + ".log")).readline().split("kind=")[1].split()[0] if os.path.exists(os.path.join(d, rid + ".log")) else "?"
    rl, c, h = r["runlog"], r["classes"], r["host"] or {}
    newerr = sum(1 for v in c["wine_err"].values() if v[-1] == "new")
    newdx = sum(1 for v in c["dxvk"].values() if v[-1] == "new")
    exitpc = sum(v[0] for v in c["exitpc"].values())
    kf = sum(v[0] for v in c["kfault"].values())
    kt = {}
    for s, v in c["kstall"].items():
        kt[s.split(":")[0]] = kt.get(s.split(":")[0], 0) + v[0]
    ks = ", ".join(f"{k}×{v}" for k, v in kt.items()) or "0"
    gb = h.get("gpu_busy_all") or {}
    gw = h.get("gpu_busy_world") or {}
    red = "yes" if c["red"] else "no"
    verdict = rl.get("verdict", "?") + (" (EARLY EXIT %.1f s)" % (rl["game_ms"] / 1000) if rl.get("early_exit") else "")
    row = dict(run=rid, kind=kind, verdict=verdict, launch=rl.get("launch_s"), l2m=rl.get("launch_to_menu_s"),
               kf=kf, ks=ks, exitpc=exitpc, newerr=newerr, newdx=newdx, red=red,
               gpu=f"{gb.get('median', '-')}/{gb.get('p90', '-')}", gpuw=f"{gw.get('median', '-')}/{gw.get('p90', '-')}",
               guests=len(h.get("guests", [])) - 1, fail=rl.get("script_fail", ""))
    fp = os.path.join(d, rid + ".fps.csv")
    if os.path.exists(fp):
        rows = list(csv.DictReader(open(fp)))
        for ph in ("world",) + tuple(f"soak{i}" for i in range(1, 7)) + ("menu",):
            v = [float(x["approx_fps"]) for x in rows if x["phase"] == ph]
            if v:
                row[ph] = (round(statistics.median(v), 1), round(q10(v), 1), len(v))
        mem = r.get("mem", {}).get("Cyberpunk2077.e")
        row["mem"] = f"{mem['res_first']}→{mem['res_max']}" if mem else "-"
        p9.append(row)
    else:
        p8.append(row)
print("| boot | kind | verdict | launch s | launch→menu s | kernel faults | kernel stalls (rows) | EXITPC≠0 | new Wine err sigs | new DXVK/vkd3d sigs | REDEngineErrorReporter | 9070 XT busy med/p90 | other guests |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in p8:
    print(f"| {r['run']} | {r['kind']} | {r['verdict']} | {r['launch']} | {r['l2m']} | {r['kf']} | {r['ks']} | {r['exitpc']} | {r['newerr']} | {r['newdx']} | {r['red']} | {r['gpu']} | {r['guests']} |")
print()
print("| boot | kind | verdict | world median | world p10 | rows | menu median | 9070 XT busy (world) med/p90 | game res MiB first→max | kernel stalls | other guests | script |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in p9:
    w = r.get("world") or (None, None, 0)
    m = r.get("menu") or (None, None, 0)
    print(f"| {r['run']} | {r['kind']} | {r['verdict']} | {w[0]} | {w[1]} | {w[2]} | {m[0]} | {r['gpuw']} | {r['mem']} | {r['ks']} | {r['guests']} | {r['fail'][:60]} |")
