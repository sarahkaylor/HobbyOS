#!/usr/bin/env python3
"""tools/cp/hunt/soaktrend.py LOGS RUN [--bucket-min 10] -- a long boot's trends
(lane H9-hunt4, docs/cp-hunt.md round 4, item 3): one row per bucket of the
active session (boot_p9 host seconds), from run-hunt.sh's copies in LOGS:

  fps     median / p10 of the world-like fps rows (fps.csv phases walk, drive, world)
  ghit    glasswatch gaps >= 250 ms in world-like drive states (RUN.glass.txt + drive-states)
  gmax    the longest such gap (ms)
  thr / regs / res   the game's [THRS] row last in the bucket (threads, regions, resident MiB)
  nts     [NTSYNC] live objects (Wine's sync objects: the handle proxy), last in the bucket
  free    the launcher census' guest free MiB ([P8MEM]), last in the bucket

The serial rows have no host time: a [THRS] / [NTSYNC] / [P8MEM] row gets the
guest time of the last Wine `+timestamp` row before it, and guest time is
mapped to boot_p9's host seconds through fps.csv (host_s, game_s).
"""
import argparse
import csv
import datetime
import os
import re
import statistics
import sys

WL = ("walk", "drive", "world")
TS_RX = re.compile(r"^\[X\]\s+(\d+\.\d+):")
THRS_RX = re.compile(r"\[THRS\] pid=\d+ live=\d+ gt=(\d+) .*? regs=(\d+) .*?res_mib=(\d+) .*Cyberpunk2077\.e")
NTS_RX = re.compile(r"\[NTSYNC\] stats .*? live=(\d+)")
MEM_RX = re.compile(r"\[P8MEM\] frames_free=\d+ free_mib=(\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs")
    ap.add_argument("run")
    ap.add_argument("--bucket-min", type=float, default=10)
    a = ap.parse_args()
    P = lambda ext: os.path.join(a.logs, f"{a.run}.{ext}")  # noqa: E731
    fps = list(csv.DictReader(open(P("fps.csv"))))
    g2h = [(float(r["game_s"]), float(r["host_s"])) for r in fps if float(r["game_s"]) > 0]

    def host_of(gs):
        best = None
        for g, h in g2h:
            if g <= gs:
                best = h + (gs - g)
            else:
                break
        return best
    # boot_p9's t0 as an epoch: the first timeline row "+  N s  HH:MM:SS"
    t0 = None
    day = None
    for ln in open(P("log"), errors="replace"):
        m = re.search(r"start=(\d{4}-\d\d-\d\d)T", ln)
        if m and not day:
            day = m.group(1)
        m = re.match(r"\[P9W\] \+\s+([0-9.]+) s\s+(\d\d:\d\d:\d\d)\s", ln)
        if m and day:
            t0 = datetime.datetime.fromisoformat(f"{day}T{m.group(2)}").timestamp() - float(m.group(1))
            break
    states = []
    if os.path.exists(P("drive-states.txt")):
        for ln in open(P("drive-states.txt")):
            p = ln.split()
            if len(p) == 2:
                states.append((float(p[0]), p[1]))

    def st(t):
        cur = "?"
        for e, n in states:
            if e > t:
                break
            cur = n
        return cur
    gaps = []
    if t0 and os.path.exists(P("glass.txt")):
        for ln in open(P("glass.txt")):
            p = ln.split()
            if p[:1] == ["G"] and len(p) >= 3:
                e, g = float(p[1]), float(p[2])
                s0, s1 = st(e - g / 1000), st(e)
                if g >= 250 and s0 == s1 and s0.startswith(WL):
                    gaps.append((e - g / 1000 - t0, g))
    rows = {}           # host_s -> (thr, regs, res) / nts / free
    gt = None
    for ln in open(P("serial.txt"), "rb").read().decode("utf-8", "replace").replace("\r", "").split("\n"):
        m = TS_RX.match(ln)
        if m:
            gt = float(m.group(1))
            continue
        if gt is None:
            continue
        for k, rx in (("thr", THRS_RX), ("nts", NTS_RX), ("free", MEM_RX)):
            m = rx.search(ln)
            if m:
                h = host_of(gt)
                if h is not None:
                    rows.setdefault(k, []).append((h, m.groups()))
    play = [float(r["host_s"]) for r in fps if r["phase"].startswith(WL)]
    if not play:
        print("soaktrend: no world-like fps rows")
        return 1
    start, end = min(play), max(play)
    B = a.bucket_min * 60
    print(f"== soaktrend {a.run}: world-like play {start:.0f}..{end:.0f} s ({(end - start) / 60:.1f} min), "
          f"{a.bucket_min:g}-minute buckets")
    print("| min | fps med | fps p10 | rows | ghit | gmax ms | game thr | regs | res MiB | ntsync live | free MiB |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    b = start
    while b < end:
        e = b + B
        v = sorted(float(r["approx_fps"]) for r in fps
                   if r["phase"].startswith(WL) and b <= float(r["host_s"]) < e)
        gg = [g for t, g in gaps if b <= t < e]

        def last(k):
            x = [g for h, g in rows.get(k, []) if h < e]
            return x[-1] if x else None
        th, nt, fr = last("thr"), last("nts"), last("free")
        print(f"| {(b - start) / 60:.0f} | {round(statistics.median(v), 1) if v else '-'} | "
              f"{v[int(len(v) * 0.1)] if v else '-'} | {len(v)} | {len(gg)} | {max(gg) if gg else 0:.0f} | "
              f"{th[0] if th else '-'} | {th[1] if th else '-'} | {th[2] if th else '-'} | "
              f"{nt[0] if nt else '-'} | {fr[0] if fr else '-'} |")
        b = e
    return 0


if __name__ == "__main__":
    sys.exit(main())
