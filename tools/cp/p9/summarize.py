#!/usr/bin/env python3
"""tools/cp/p9/summarize.py OUT -- the numbers of one P9 boot (lane C9-play,
docs/cp-p9.md) from boot_p9.py's session record: winevulkan +fps rows per
phase (n, median, p10, min, max of the "approx" column; each row averages the
frames since the previous row, ~1-5 s), frame-pool use from the launcher's
[P8MEM] rows, the game's thread count / anon MiB from kernel [THRS] rows, and
the timeline's phase boundaries."""
import csv
import os
import statistics
import sys

out = sys.argv[1]


def q(v, p):
    v = sorted(v)
    if not v:
        return float("nan")
    k = (len(v) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


rows = list(csv.DictReader(open(os.path.join(out, "fps.csv"))))
phases = []
for r in rows:
    if r["phase"] not in phases:
        phases.append(r["phase"])
print(f"== fps (winevulkan trace:fps 'approx' rows) in {out}")
print(f"{'phase':<14}{'rows':>6}{'game s':>16}{'median':>9}{'p10':>8}{'min':>8}{'max':>8}{'mean':>8}")
for ph in phases:
    v = [float(r["approx_fps"]) for r in rows if r["phase"] == ph]
    t = [float(r["game_s"]) for r in rows if r["phase"] == ph]
    print(f"{ph:<14}{len(v):>6}{f'{min(t):.0f}-{max(t):.0f}':>16}{statistics.median(v):>9.1f}{q(v, 0.1):>8.1f}"
          f"{min(v):>8.1f}{max(v):>8.1f}{statistics.fmean(v):>8.1f}")
mem = list(csv.DictReader(open(os.path.join(out, "mem.csv"))))
if mem:
    print("== frame pool free MiB ([P8MEM]) by phase: first / min / last")
    for ph in dict.fromkeys(r["phase"] for r in mem):
        v = [int(r["free_mib"]) for r in mem if r["phase"] == ph]
        print(f"   {ph:<14} {v[0]:>7} {min(v):>7} {v[-1]:>7}  ({len(v)} rows)")
thr = os.path.join(out, "thrs.csv")
if os.path.exists(thr):
    t = [r for r in csv.DictReader(open(thr)) if r["name"].startswith("Cyberpunk")]
    if t:
        print("== game process ([THRS]): max threads %s, max anon %s MiB, max res %s MiB (%d rows)" % (
            max(int(r["threads"]) for r in t), max(int(r["anon_mib"]) for r in t),
            max(int(r["res_mib"]) for r in t), len(t)))
ph = os.path.join(out, "phases.txt")
if os.path.exists(ph):
    print("== phases (host s since boot, game s since launch, name)")
    for ln in open(ph):
        print("   " + ln.rstrip())
hbp = os.path.join(out, "hbp.csv")
if os.path.exists(hbp):
    h = list(csv.DictReader(open(hbp)))
    if h:
        print("== present path ([HBP] rows, C6-p7's instrument; per phase: rows, frames by path, avg flip/sub/wait us)")
        for ph in dict.fromkeys(r["phase"] for r in h):
            rs = [r for r in h if r["phase"] == ph]
            sc = sum(int(r["scanout"]) for r in rs)
            pu = sum(int(r["putimage"]) for r in rs)
            rf = sum(int(r["refused"]) for r in rs)
            n = max(sum(int(r["n"]) for r in rs), 1)
            avg = lambda k: sum(int(r[k]) * int(r["n"]) for r in rs) / n  # noqa: E731
            print(f"   {ph:<18} rows {len(rs):4d}  scanout {sc:6d}  putimage {pu:6d}  refused {rf:4d}  "
                  f"flip {avg('flip_us'):7.0f}  sub {avg('sub_us'):7.0f}  wait {avg('wait_us'):7.0f}")
