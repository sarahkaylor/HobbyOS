#!/usr/bin/env python3
"""tools/rw/l65-timeline.py -- merge the L65 soak samples into 30 s timelines.

Reads one sampler CSV per run (tools/rw/l65-soak-sample.py) and the final
serial log (to backfill the rows the sampler missed before it was pointed at
the run) and writes:

  obj/l65-timeline.csv          one long table: run,t_s,nfs_mib,xsw,live,res_mib,last_xo,phase,retr,blkq,nfspipe,nfsrd,txtzero,kvm
  docs/l65-variance-timelines.md  three markdown tables at 30 s

A row's `phase` is derived the same way for every run so the three columns are
comparable: the last Player.log marker if one exists, else the class of the
last XML opened (defs / language), else `post-xml` (the asset burst: reads
continue with no further XML open), else `parked` (no new NFS bytes and no new
present for 60 s).
"""
import csv
import re
import sys
from collections import OrderedDict

RUNS = [
    ("run1", "obj/l65-run1.csv", "obj/l65-run1.serial.log", "obj/l65-run1.Player.log"),
    ("run2", "obj/l65-run2.csv", "obj/l65-run2.serial.log", "obj/l65-run2.Player.log"),
    ("run3", "obj/l65-run3.csv", "obj/l65-run3.serial.log", "obj/l65-run3.Player.log"),
]


def read(p):
    try:
        return open(p, "rb").read().decode("latin-1")
    except OSError:
        return ""


def xo_series(serial):
    """t -> last [XO] path, where t is the launcher's own 10 s clock."""
    t = 0
    out = OrderedDict()
    for line in serial.splitlines():
        m = re.search(r"child still running after (\d+)s", line)
        if m:
            t = int(m.group(1))
        m = re.search(r"\[XO\] (\S+)", line)
        if m:
            out[t] = m.group(1)
    return out


def serial_series(serial):
    """t -> (nfs_bytes_max_so_far, live, res_mib) from 5 s [THRS] and 1 MiB
    [NFSRTT] rows."""
    t = 0
    nfs = {}
    thr = {}
    for line in serial.splitlines():
        m = re.search(r"child still running after (\d+)s", line)
        if m:
            t = int(m.group(1))
        m = re.search(r"\[NFSRTT\] periodic .*?bytes=(\d+)", line)
        if m:
            nfs[t] = max(nfs.get(t, 0), int(m.group(1)))
        m = re.search(r"\[THRS\] pid=4 .*?live=(\d+).*?res_mib=(\d+)", line)
        if m:
            thr[t] = (int(m.group(1)), int(m.group(2)))
    return nfs, thr


def at(d, t, default=0):
    ks = [k for k in d if k <= t]
    return d[max(ks)] if ks else default


def phase_of(last_xo, nfs_now, nfs_prev, xsw_now, xsw_prev):
    if last_xo:
        if "/Defs/" in last_xo:
            return "defs"
        if "/Languages/" in last_xo:
            return "language"
    if nfs_now == nfs_prev and xsw_now == xsw_prev and nfs_now:
        return "parked"
    if nfs_now > nfs_prev:
        return "post-xml"
    return "-"


def build(run, csv_path, serial_path, player_path):
    rows = []
    try:
        with open(csv_path) as f:
            for r in csv.DictReader(f):
                rows.append({k: r[k] for k in r})
    except OSError:
        return []
    if not rows:
        return []
    serial = read(serial_path)
    nfs_s, thr_s = serial_series(serial)
    xo = xo_series(serial)
    # One row per 30 s bucket; keep the LAST sample in the bucket (the sampler
    # may emit duplicates when its clock source stalls).
    per = {}
    for r in rows:
        t = int(r["t_s"])
        if t < 0:
            continue
        per[t // 30 * 30] = r
    # Backfill the head of the run when the sampler was pointed at the log
    # late (run 1 lost its first 140 s to a restart): the serial log's 5 s
    # [THRS] and 1 MiB [NFSRTT] rows are enough for every column but xsw.
    if per and min(per) > 0:
        for t in range(0, min(per), 30):
            n = at(nfs_s, t, 0)
            lr = at(thr_s, t, (0, 0))
            per[t] = dict(t_s=str(t), nfs_bytes=str(n), xsw_n="", live=str(lr[0]),
                          res_mib=str(lr[1]), last_xo=at(xo, t, ""),
                          retr="", blkq="", nfspipe="", nfsrdfail="",
                          txtzero="", kvm="")
    out = []
    times = sorted(per)
    for i, t in enumerate(times):
        r = per[t]
        nfs = int(r["nfs_bytes"] or 0)
        prev = int(per[times[i - 1]]["nfs_bytes"] or 0) if i else 0
        xsw = int(r["xsw_n"] or 0)
        xsw_prev = int(per[times[i - 1]]["xsw_n"] or 0) if i else 0
        last_xo = r["last_xo"] or at(xo, t, "")
        phase = phase_of(last_xo, nfs, prev, xsw, xsw_prev)
        out.append(dict(run=run, t_s=t, nfs_mib=f"{nfs/1048576:.1f}", xsw=xsw,
                        live=r["live"] or str(at(thr_s, t, (0, 0))[0]),
                        res_mib=r["res_mib"] or str(at(thr_s, t, (0, 0))[1]),
                        last_xo=last_xo, phase=phase,
                        retr=r["retr"], blkq=r["blkq"], nfspipe=r["nfspipe"],
                        nfsrd=r["nfsrdfail"], txtzero=r["txtzero"],
                        kvm=r["kvm"]))
    return out


def main():
    allrows = []
    for run, c, s, p in RUNS:
        rs = build(run, c, s, p)
        print(f"{run}: {len(rs)} rows", file=sys.stderr)
        allrows += rs
    with open("obj/l65-timeline.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(allrows[0].keys()))
        w.writeheader()
        w.writerows(allrows)

    doc = ["# L65 variance: three 1800 s timelines (30 s resolution)", ""]
    doc.append("`nfs` is cumulative READ bytes from `[NFSRTT] periodic`; `xsw` is")
    doc.append("presents from `[XSW] n=`; `live`/`res` are `[THRS] pid=4`; `xo` is")
    doc.append("the last XML the game opened. `post-xml` = NFS still growing with no")
    doc.append("new XML (asset/art load); `parked` = neither NFS nor presents moved.")
    doc.append("`t` is the launcher's own `child still running after Ns` clock, so a")
    doc.append("row is ±10 s even though the sampler runs on a 30 s grid.")
    doc.append("")
    for run, _, _, _ in RUNS:
        rs = [r for r in allrows if r["run"] == run]
        if not rs:
            continue
        doc.append(f"## {run}")
        doc.append("")
        doc.append("| t | NFS MiB | xsw | live | res MiB | last XO | phase | stalls (retr/blkq/pipe) |")
        doc.append("|---:|---:|---:|---:|---:|---|---|---|")
        for r in rs:
            xo = r["last_xo"].rsplit("/", 1)[-1][:34]
            stall = "/".join(str(r[k] or "-") for k in ("retr", "blkq", "nfspipe"))
            doc.append(f"| {r['t_s']} | {r['nfs_mib']} | {r['xsw']} | {r['live']} | "
                       f"{r['res_mib']} | {xo} | {r['phase']} | {stall} |")
        doc.append("")
    with open("docs/l65-variance-timelines.md", "w") as f:
        f.write("\n".join(doc) + "\n")
    print("wrote obj/l65-timeline.csv and docs/l65-variance-timelines.md",
          file=sys.stderr)


if __name__ == "__main__":
    main()
