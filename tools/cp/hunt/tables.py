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
        sv = [float(x["approx_fps"]) for x in rows if x["phase"].startswith("soak")]
        if sv:            # the soak walks in soak1..soak6: its "world" is all of them
            row["world"] = (round(statistics.median(sv), 1), round(q10(sv), 1), len(sv))
        fg = (r.get("fps_gaps") or {})
        row["hitch"] = sum(v.get("hitches", 0) for k, v in fg.items() if k == "world" or k.startswith("soak"))
        row["wmin"] = min((v["min"] for k, v in fg.items() if k == "world" or k.startswith("soak")), default="-")
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
print("| boot | kind | verdict | world median | world p10 | world min row | rows | hitches (>1 s frame) | menu median | 9070 XT busy (world) med/p90 | game res MiB first→max | kernel stalls | other guests | script |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in p9:
    w = r.get("world") or (None, None, 0)
    m = r.get("menu") or (None, None, 0)
    print(f"| {r['run']} | {r['kind']} | {r['verdict']} | {w[0]} | {w[1]} | {r.get('wmin', '-')} | {w[2]} | {r.get('hitch', '-')} | {m[0]} | {r['gpuw']} | {r['mem']} | {r['ks']} | {r['guests']} | {r['fail'][:60]} |")

# the soak: per phase fps (decay) and the game's memory (growth), from
# RUN.fps.csv / RUN.thrs.csv of every boot that has soak phases
for r in p9:
    fp = os.path.join(d, r["run"] + ".fps.csv")
    tp = os.path.join(d, r["run"] + ".thrs.csv")
    rows = list(csv.DictReader(open(fp)))
    if not any(x["phase"].startswith("soak") for x in rows):
        continue
    th = [x for x in csv.DictReader(open(tp)) if x["name"].startswith("Cyberpunk")] if os.path.exists(tp) else []
    print()
    print(f"Soak {r['run']}: per phase")
    print()
    print("| phase | game s | fps rows | median | p10 | min | max gap s | game res MiB (last) | anon MiB (last) | threads (last) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for ph in ["world"] + [f"soak{i}" for i in range(1, 7)]:
        v = [x for x in rows if x["phase"] == ph]
        if not v:
            continue
        f = [float(x["approx_fps"]) for x in v]
        t = [float(x["game_s"]) for x in v]
        gap = max((b - a for a, b in zip(t, t[1:])), default=0)
        m = [x for x in th if x["phase"] == ph]
        last = m[-1] if m else {}
        print(f"| {ph} | {t[0]:.0f}-{t[-1]:.0f} | {len(f)} | {statistics.median(f):.1f} | {q10(f):.1f} | {min(f):.1f} | {gap:.1f} | "
              f"{last.get('res_mib', '-')} | {last.get('anon_mib', '-')} | {last.get('threads', '-')} |")
    s = [x for x in th if x["phase"].startswith("soak")]
    if len(s) >= 2:
        g0, g1 = float(s[0]["game_s"]), float(s[-1]["game_s"])
        r0, r1 = int(s[0]["res_mib"]), int(s[-1]["res_mib"])
        print(f"\nresident growth over the soak phases: {r0} -> {r1} MiB in {(g1 - g0) / 60:.1f} min = "
              f"{(r1 - r0) / max((g1 - g0) / 60, 1e-9):.1f} MiB/min")
