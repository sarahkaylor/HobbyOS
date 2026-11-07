#!/usr/bin/env python3
"""l76-handshake.py — read the L76 rows out of a RimWorld serial log.

Three questions, three tables:

  FUTR  [FUTR] t=..s ops=.. d=.. rate=../s dt=..ms keys=<key>:<waits>w<wakes>k
        the futex op rate (the "handshake") per window, and which keys carry it
  XSW   present rate per window, from the client's own swap counter
  GDC   [GDC] rel=.. fresh=.. calls=.. ents=.. total=..   per-directory getdents
        census windows (fs.c l76_dir_note)

  rounds/s vs presents/s is the frame-count test: a handshake paced by the
  engine's frame loop moves 1:1 with presents; a loader-side loop does not.

Usage: tools/rw/l76-handshake.py obj/rw-desktop/serial-desktop.log [--json out]
"""
import argparse
import json
import re
import sys

RE_FUTR = re.compile(
    r"\[FUTR\] t=(\d+)s ops=(\d+) d=(\d+) rate=(\d+)/s dt=(\d+)ms keys=(\S*)")
RE_XSW = re.compile(
    r"\[XSW\] n=(\d+) swap=(\d+) put=(\d+) putok=(\d+).*?w=(\d+) h=(\d+)")
RE_GDC = re.compile(
    r"\[GDC\] rel=(\S*) fresh=(\d+) calls=(\d+) ents=(\d+) total=(-?\d+)(?: dup=(\d+))?")
RE_RDF = re.compile(
    r"\[RDF\] path=(\S*) reads=(\d+) bytes=(\d+) off=(\d+) len=(\d+)")
RE_GDT = re.compile(r"\[GDT\] rel=(\S*) total=(\d+) path=(\S*)")
RE_XO = re.compile(r"\[XO\] (\S+)")


def read(path):
    with open(path, "r", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--json")
    ap.add_argument("--top-dirs", type=int, default=12)
    a = ap.parse_args()
    txt = read(a.log)

    futr = [dict(t=int(m.group(1)), ops=int(m.group(2)), d=int(m.group(3)),
                 rate=int(m.group(4)), dt=int(m.group(5)),
                 keys=dict((k.split(":")[0], k.split(":")[1])
                           for k in m.group(6).split(",") if ":" in k))
            for m in RE_FUTR.finditer(txt)]
    # XSW rows: n is a present counter, t of the row is not in older builds.
    xsw = [dict(n=int(m.group(1)), swap=int(m.group(2)), put=int(m.group(3)),
                putok=int(m.group(4)), w=int(m.group(5)), h=int(m.group(6)))
           for m in RE_XSW.finditer(txt)]
    gdc = [dict(rel=m.group(1), fresh=int(m.group(2)), calls=int(m.group(3)),
                ents=int(m.group(4)), total=int(m.group(5)),
                dup=int(m.group(6) or 0)) for m in RE_GDC.finditer(txt)]
    gdt = [dict(rel=m.group(1), total=int(m.group(2)), path=m.group(3))
           for m in RE_GDT.finditer(txt)]
    xo = RE_XO.findall(txt)

    print("== FUTR: the futex op rate (the handshake), per window")
    if not futr:
        print("   (no [FUTR] rows: the kernel predates L76, or no futex op ran)")
    print("   t(s)   ops      d  rate/s   dt(ms)  keys")
    for r in futr:
        ks = ",".join("%s:%s" % (k, v) for k, v in r["keys"].items()) or "-"
        print("  %5d %7d %6d %6d %7d   %s" % (r["t"], r["ops"], r["d"],
                                              r["rate"], r["dt"], ks))
    # rounds/s: the 6-op round of L72 is the shape; report ops/s and ops/s/6.
    if len(futr) >= 2:
        tail = futr[-1]
        print("   last window: %d ops/s = %.1f rounds/s (6 ops/round)"
              % (tail["rate"], tail["rate"] / 6.0))

    print("\n== XSW: presents")
    if xsw:
        print("   n first=%d last=%d rows=%d size=%dx%d"
              % (xsw[0]["n"], xsw[-1]["n"], len(xsw), xsw[-1]["w"], xsw[-1]["h"]))
        print("   last row: n=%d swap=%d put=%d putok=%d"
              % (xsw[-1]["n"], xsw[-1]["swap"], xsw[-1]["put"], xsw[-1]["putok"]))
    else:
        print("   (no [XSW] rows)")

    print("\n== GDC: per-directory getdents64 census windows")
    if not gdc:
        print("   (no [GDC] rows)")
    agg = {}
    for r in gdc:
        e = agg.setdefault(r["rel"], dict(fresh=0, calls=0, ents=0, dup=0,
                                          total=r["total"], windows=0))
        e["fresh"] += r["fresh"]; e["calls"] += r["calls"]
        e["ents"] += r["ents"]; e["dup"] += r["dup"]
        e["total"] = r["total"]; e["windows"] += 1
    print("   %-52s %6s %7s %8s %6s %5s" % ("rel", "fresh", "calls", "ents",
                                            "total", "dup"))
    for rel, e in sorted(agg.items(), key=lambda kv: -kv[1]["calls"])[:a.top_dirs]:
        print("   %-52s %6d %7d %8d %6d %5d"
              % (rel, e["fresh"], e["calls"], e["ents"], e["total"], e["dup"]))
    tot_calls = sum(e["calls"] for e in agg.values())
    tot_dup = sum(e["dup"] for e in agg.values())
    print("   dirs=%d calls=%d dup=%d" % (len(agg), tot_calls, tot_dup))

    print("\n== XML opens ([XO])")
    print("   n=%d last=%s" % (len(xo), xo[-1] if xo else "-"))

    rdf = [dict(path=m.group(1), reads=int(m.group(2)), bytes=int(m.group(3)),
                off=int(m.group(4)), len=int(m.group(5)))
           for m in RE_RDF.finditer(txt)]
    print("\n== RDF: per-file NFS reads, last window of each file")
    if not rdf:
        print("   (no [RDF] rows)")
    else:
        agg = {}
        for r in rdf:
            e = agg.setdefault(r["path"], dict(reads=0, bytes=0, last_off=0,
                                               last_len=0, wins=0))
            e["reads"] += r["reads"]; e["bytes"] += r["bytes"]
            e["last_off"] = r["off"]; e["last_len"] = r["len"]; e["wins"] += 1
        print("   %-50s %6s %9s %8s %5s" % ("path", "reads", "bytes", "off", "len"))
        for p, e in sorted(agg.items(), key=lambda kv: -kv[1]["reads"])[:15]:
            print("   %-50s %6d %9d %8d %5d"
                  % (p, e["reads"], e["bytes"], e["last_off"], e["last_len"]))
        print("   total reads=%d bytes=%d in %d windows"
              % (sum(e["reads"] for e in agg.values()),
                 sum(e["bytes"] for e in agg.values()), len(rdf)))

    if a.json:
        with open(a.json, "w") as f:
            json.dump(dict(futr=futr, xsw=xsw, gdc=gdc, gdt=gdt, xo=len(xo)), f,
                      indent=1)
        print("\n   json -> %s" % a.json)


if __name__ == "__main__":
    sys.exit(main())
