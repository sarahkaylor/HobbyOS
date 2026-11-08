#!/usr/bin/env python3
"""l87-boots.py -- summarize one L87 boot from its serial log.

The question a boot log has to answer is not "did a futex row look odd" but
"did the front end reach the menu, and was any wake actually lost".  This
prints one row per file:

    file  presents=<max [XSW] n>  xsw_t=<last t>  menu=<New colony present>
          lostwake=<n>  cwv=<n dumps>  xo=<n opens>  futex_ops=<last [FUTXR]>

and, with --keys, the per-key ledger rows ([CWV] KEY) for the handshake keys.

Usage:
    tools/rw/l87-boots.py obj/rw-desktop/serial-*.log
"""
import argparse
import re
import sys

HANDSHAKE = ("0x113000A460", "0x1100000334", "0x1083B1B624", "0x10E080EBB0",
             "0x10E089D218")


def scan(path):
    txt = open(path, errors="replace").read()
    ns = [int(m.group(1)) for m in re.finditer(r"\[XSW\] n=(\d+)", txt)]
    ts = [int(m.group(1)) for m in
          re.finditer(r"\[XSW\] n=\d+[^\n]*?\bt=(-?\d+)", txt)]
    ops = [int(m.group(1)) for m in re.finditer(r"\[FUTR\] t=\d+s ops=(\d+)", txt)]
    keys = {}
    for m in re.finditer(
            r"\[CWV\] KEY key=(0x[0-9A-Fa-f]+) w=(\d+) k=(\d+) park=(\d+) "
            r"lw=(\d+) lk=(\d+) lwp=(-?\d+) lkp=(-?\d+) verdict=(\S+)", txt):
        # keep the LAST snapshot of each key (the state at the last dump)
        keys[m.group(1)] = m.groups()[1:]
    return {
        "path": path,
        "presents": max(ns) if ns else None,
        "xsw_t": ts[-1] if ts else None,
        "menu": "New colony" in txt,
        "lostwake": len(re.findall(r"\[LOSTWAKE\]", txt)),
        "cwv": len(re.findall(r"\[CWV\] ledger", txt)),
        "xo": len(re.findall(r"^\[XO\] ", txt, re.M)),
        "ops": ops[-1] if ops else None,
        "keys": keys,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--keys", action="store_true")
    args = ap.parse_args()
    bad = 0
    for p in args.logs:
        r = scan(p)
        print("%-52s presents=%-5s t=%-7s menu=%-5s lostwake=%d cwv=%-3d "
              "xo=%-4d ops=%s" %
              (p.split("/")[-1], r["presents"], r["xsw_t"], r["menu"],
               r["lostwake"], r["cwv"], r["xo"], r["ops"]))
        if args.keys:
            for k in HANDSHAKE:
                v = r["keys"].get(k)
                if v:
                    print("    key=%s w=%s k=%s park=%s lw=%s lk=%s "
                          "lwp=%s lkp=%s verdict=%s" % ((k,) + v))
        if not r["menu"]:
            bad += 1
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
