#!/usr/bin/env python3
"""tools/cp/present/hbv_summary.py RUN_DIR [venus-protocol defines .h]

Per phase of a P9 run: the [HBV] rows (C6-present's venus wait instrument,
tools/mesa/venus/hb_present_stats.h) as per-frame counts and per-frame
milliseconds, next to the [HBP] present split.  Each [HBV] row follows its
[HBP] row in serial.log (same window); hbp.csv gives each [HBP] row's phase.
"""
import csv, os, re, sys
run = sys.argv[1]
names = {}
if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
    for l in open(sys.argv[2], errors="replace"):
        m = re.match(r"\s*VK_COMMAND_TYPE_(\w+)_EXT\s*=\s*(\d+)", l)
        if m:
            names.setdefault(int(m.group(2)), m.group(1))
phases = [r["phase"] for r in csv.DictReader(open(os.path.join(run, "hbp.csv")))]
rows = []  # (hbp_line, hbv_line)
thrw = []  # (phase, row): the kernel's [THRW] thread-state rows (process.c)
last = None
nhbp = 0
for l in open(os.path.join(run, "serial.log"), errors="replace"):
    if "[THRW]" in l:
        thrw.append((phases[nhbp - 1] if 0 < nhbp <= len(phases) else "pre", l.strip()))
        continue
    if "[HBP]" in l:
        nhbp += 1
        last = l
    elif "[HBV]" in l and last is not None:
        rows.append((last, l))
        last = None
KEYS = ["ring", "tls", "space", "slpR", "slpO", "yield", "ntf", "subF", "subU", "wait", "fwait", "qsub", "pwait"]
agg = {}
for i, (p, v) in enumerate(rows):
    ph = phases[i] if i < len(phases) else "?"
    a = agg.setdefault(ph, {"n": 0, "win": 0.0, "k": {k: [0, 0.0] for k in KEYS}, "cmd": {},
                            "sub": 0.0, "flip": 0.0, "np": 0, "tid": {}})
    n = int(re.search(r" n=(\d+)", v).group(1))
    a["n"] += n
    a["win"] += float(re.search(r"win_ms=([\d.]+)", v).group(1))
    for k in KEYS:
        m = re.search(r" %s=(\d+)/([\d.]+)/([\d.]+)/" % k, v)
        if m:
            a["k"][k][0] += int(m.group(1))
            a["k"][k][1] += float(m.group(3))
    tail = v.split(" cmd", 1)[1] if " cmd" in v else ""
    for t, c, avg, mx in re.findall(r" (\d+):(\d+)/([\d.]+)/([\d.]+)", tail):
        e = a["cmd"].setdefault(int(t), [0, 0.0])
        e[0] += int(c)
        e[1] += int(c) * float(avg) / 1000.0
    if " tid" in v:
        for t, wc, wm, qc, qm, pc, pm in re.findall(
                r" (\d+):w=(\d+)/([\d.]+),q=(\d+)/([\d.]+),p=(\d+)/([\d.]+)", v.split(" tid", 1)[1]):
            e = a["tid"].setdefault(t, [0, 0.0, 0, 0.0, 0, 0.0])
            for j, x in enumerate((wc, wm, qc, qm, pc, pm)):
                e[j] += float(x)
    ms = re.search(r" sub=(\d+)/", p); mf = re.search(r" flip=(\d+)/", p)
    if ms and mf:
        a["sub"] += int(ms.group(1)) * n; a["flip"] += int(mf.group(1)) * n; a["np"] += n
print("# [HBV] per phase: per frame = count/frame, ms/frame (sum over all threads)")
for ph, a in agg.items():
    n = max(a["n"], 1)
    fps = a["n"] / (a["win"] / 1000.0) if a["win"] else 0
    print("== %-14s frames %6d  fps %.1f  [HBP] sub %.2f ms flip %.2f ms (avg/frame)" % (
        ph, a["n"], fps, a["sub"] / max(a["np"], 1) / 1000.0, a["flip"] / max(a["np"], 1) / 1000.0))
    print("   " + "  ".join("%s %.2f/%.2fms" % (k, a["k"][k][0] / n, a["k"][k][1] / n) for k in KEYS))
    top = sorted(a["cmd"].items(), key=lambda kv: -kv[1][1])[:8]
    tt = sorted(a["tid"].items(), key=lambda kv: -(kv[1][1] + kv[1][3] + kv[1][5]))[:6]
    if tt:
        print("   threads (per frame: wait n/ms, submit n/ms, present-wait n/ms): " + "  ".join(
            "t%s w%.1f/%.1f q%.1f/%.1f p%.2f/%.1f" % (t, e[0] / n, e[1] / n, e[2] / n, e[3] / n, e[4] / n, e[5] / n)
            for t, e in tt))
    print("   replies: " + "  ".join("%s %.2f/%.2fms" % (names.get(t, str(t)), c / n, ms / n) for t, (c, ms) in top))

# [THRW]: per phase and process (name), the mean of each state and the
# threads that ran most (tid: run%/ready%/kern% averaged over the rows).
if thrw:
    print("# [THRW] per phase (mean threads: run ready futex kern; blk/ksys by syscall nr; top threads run/ready/kern %)")
    by = {}
    for ph, l in thrw:
        name = l.rsplit(" ", 1)[1]
        pid = re.search(r" pid=(\d+)", l).group(1)
        # keyed by pid: a group leader's name can flip (pid 92 reads
        # Cyberpunk2077.e and services.exe in alternate [THRS]/[THRW] rows)
        e = by.setdefault((ph, pid), {"n": 0, "v": {}, "blk": {}, "ks": {}, "top": {}, "names": set()})
        e["names"].add(name)
        e["n"] += 1
        for k in ("run", "ready", "futex", "kern", "trap"):
            m = re.search(r" %s=([\d.]+)" % k, l)
            if m:
                e["v"][k] = e["v"].get(k, 0) + float(m.group(1))
        for sec, key in (("blk", "blk"), ("ksys", "ks")):
            m = re.search(r" %s((?: -?\d+:[\d.]+)*)" % sec, l)
            if m:
                for nr, v in re.findall(r" (-?\d+):([\d.]+)", m.group(1)):
                    e[key][nr] = e[key].get(nr, 0) + float(v)
        m = re.search(r" top((?: \d+:\d+/\d+(?:/\d+)?(?:/\d+)?(?:\([-\d=,]+\))?)*)", l)
        if m:
            for t, a, b, c, d, ks in re.findall(r" (\d+):(\d+)/(\d+)(?:/(\d+))?(?:/(\d+))?(?:\(([-\d=,]+)\))?", m.group(1)):
                x = e["top"].setdefault(t, [0, 0, 0, 0, {}, 0])
                x[0] += int(a); x[1] += int(b); x[2] += int(c or 0); x[3] += 1; x[5] += int(d or 0)
                for kv in filter(None, ks.split(",")):
                    nr, v = kv.split("=")
                    x[4][nr] = x[4].get(nr, 0) + int(v)
    for (ph, pid), e in by.items():
        name = "pid%s:%s" % (pid, "/".join(sorted(e["names"])))
        n = e["n"]
        if e["v"].get("run", 0) / n < 0.1 and ph != "world":
            continue
        top = sorted(e["top"].items(), key=lambda kv: -kv[1][0])[:4]
        print("== %-14s %-34s rows %3d  " % (ph, name, n) + " ".join("%s %.2f" % (k, e["v"].get(k, 0) / n) for k in ("run", "ready", "futex", "kern", "trap")))
        print("   blk " + " ".join("%s:%.2f" % (k, v / n) for k, v in sorted(e["blk"].items(), key=lambda kv: -kv[1])[:8]))
        if e["ks"]:
            print("   ksys " + " ".join("%s:%.2f" % (k, v / n) for k, v in sorted(e["ks"].items(), key=lambda kv: -kv[1])[:8]))
        print("   top (run/ready/kern/trap %) " + " ".join("t%s %d/%d/%d/%d%%%s" % (t, x[0] / n, x[1] / n, x[2] / n, x[5] / n,
              ("(" + ",".join("%s=%d" % (k, v / n) for k, v in sorted(x[4].items(), key=lambda kv: -kv[1])) + ")") if x[4] else "")
              for t, x in top))
