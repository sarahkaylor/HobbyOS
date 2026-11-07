#!/usr/bin/env python3
"""l78-bks.py — the L78 block-stress probe's survivability table.

Reads one desktop-harness serial log and prints, in order:

  BKS   every `[BKS] alive` heartbeat (the probe's own counters)
  BLKD  resets / stalls / devoff / cc0 from the 5 s device row
  XSW   present count and the last present's CRC
  END   the last [XO], the last [FUTR] round rate, any exception rows

The point is to prove the probe was APPLIED for the whole window (its op
count rises monotonically, its heartbeat never stops) and what it saw
(bad/mismatch/busy), next to the game's own liveness on the same timeline.

Usage: tools/rw/l78-bks.py obj/l78/serial-P1.log [--json out.json] [--md]
"""
import argparse
import json
import re
import sys

RE_BKS = re.compile(
    r"\[BKS\] alive t=(\d+)s ops=(\d+) reads=(\d+) write_verify=(\d+) "
    r"bad=(\d+) mismatch=(\d+) busy=(\d+) skip=(\d+) slow=(\d+) inflight=(\d+)")
RE_START = re.compile(
    r"\[BKS\] start ops_per_s=(\d+) token_wait_ms=(\d+) max_inflight=(\d+) "
    r"wv_every=(\d+)")
RE_ROW = re.compile(r"\[BKS\] (read failed|write failed|verify MISMATCH|slow) (.*)")
RE_BLKD = re.compile(
    r"\[BLKD\] cc=(\S+) csts=(\S+).*? resets=(\d+) pre=(\d+).*? fr_used=(\d+)")
RE_XSW = re.compile(
    r"\[XSW\] n=(\d+) swap=(\d+) put=(\d+) putok=(\d+).*?crc=(\S+) "
    r"changed=(\d+)")
RE_XO = re.compile(r"\[XO\] (\S+)")
RE_FUTR = re.compile(r"\[FUTR\] t=(\d+)s ops=(\d+) d=(\d+) rate=(\d+)/s dt=(\d+)ms")
RE_SELF = re.compile(r"\[BLKQ\] selfcheck .*?match=(\d+)")
RE_EXC = re.compile(r"Exception reading|FileNotFoundException|NullReferenceException|"
                    r"SIGSEGV|SIGDUMP|System\.IO\.IOException")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--json")
    ap.add_argument("--md", action="store_true")
    a = ap.parse_args()
    rows = []
    with open(a.log, "r", errors="replace") as f:
        txt = f.read()

    start = RE_START.search(txt)
    for m in RE_BKS.finditer(txt):
        rows.append(dict(t=int(m.group(1)), ops=int(m.group(2)),
                         reads=int(m.group(3)), wv=int(m.group(4)),
                         bad=int(m.group(5)), mismatch=int(m.group(6)),
                         busy=int(m.group(7)), skip=int(m.group(8)),
                         slow=int(m.group(9)), inflight=int(m.group(10))))
    faults = [dict(kind=m.group(1), detail=m.group(2).strip())
              for m in RE_ROW.finditer(txt)]
    blkd = []
    for m in RE_BLKD.finditer(txt):
        blkd.append(dict(cc=m.group(1), csts=m.group(2), resets=int(m.group(3)),
                         pre=int(m.group(4)), fr_used=int(m.group(5))))
    xsw = [dict(n=int(m.group(1)), swap=int(m.group(2)), put=int(m.group(3)),
                putok=int(m.group(4)), crc=m.group(5), changed=int(m.group(6)))
           for m in RE_XSW.finditer(txt)]
    selfc = [int(m.group(1)) for m in RE_SELF.finditer(txt)]
    xo = [m.group(1) for m in RE_XO.finditer(txt)]
    futr = [dict(t=int(m.group(1)), ops=int(m.group(2)), rate=int(m.group(4)))
            for m in RE_FUTR.finditer(txt)]
    exc = [line.strip() for line in txt.splitlines() if RE_EXC.search(line)][:12]

    if a.md:
        print("| t (s) | ops | reads | write-verify | bad | mismatch | busy |"
              " skip | slow | inflight |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in rows:
            print("| {t} | {ops} | {reads} | {wv} | {bad} | {mismatch} | {busy}"
                  " | {skip} | {slow} | {inflight} |".format(**r))
    else:
        print("PROBE START:", start.groups() if start else "no [BKS] start row")
        print("BKS heartbeats: %d" % len(rows))
        for r in rows:
            print("  t=%(t)4ds ops=%(ops)5d reads=%(reads)5d wv=%(wv)5d "
                  "bad=%(bad)d mismatch=%(mismatch)d busy=%(busy)d "
                  "skip=%(skip)d slow=%(slow)d" % r)
        if faults:
            print("BKS fault rows: %d" % len(faults))
            for f in faults[:12]:
                print("  %s %s" % (f["kind"], f["detail"][:120]))
        print("BLKD rows: %d  resets_max=%s  pre_max=%s  fr_used_max=%s" % (
            len(blkd), max([b["resets"] for b in blkd], default=0),
            max([b["pre"] for b in blkd], default=0),
            max([b["fr_used"] for b in blkd], default=0)))
        print("selfcheck rows: %d match=%s" % (len(selfc), selfc))
        print("XSW rows: %d  presents=%s  last_crc=%s changed=%s" % (
            len(xsw), xsw[-1]["n"] if xsw else 0,
            xsw[-1]["crc"] if xsw else "-",
            xsw[-1]["changed"] if xsw else "-"))
        print("last [XO]: %s" % (xo[-1] if xo else "-"))
        print("[XO] rows: %d" % len(xo))
        if futr:
            print("last [FUTR]: t=%ds ops=%d rate=%d/s" %
                  (futr[-1]["t"], futr[-1]["ops"], futr[-1]["rate"]))
        if exc:
            print("exception rows (first %d):" % len(exc))
            for e in exc:
                print("  " + e[:160])

    if a.json:
        with open(a.json, "w") as f:
            json.dump(dict(start=start.groups() if start else None,
                           bks=rows, faults=faults, blkd=blkd, xsw=xsw,
                           selfcheck=selfc, last_xo=xo[-1] if xo else None,
                           xo_count=len(xo),
                           futr=futr[-1] if futr else None,
                           exceptions=exc), f, indent=1)
        print("json: " + a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
