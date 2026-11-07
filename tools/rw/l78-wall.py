#!/usr/bin/env python3
"""l78-wall.py — the rows that name the next wall.

Given a desktop-harness serial log (and optionally the extracted Player.log),
print the last state of every instrument the ladder uses:

  XSW   last present row (n, crc, changed, box)
  XO    count and the last def/keyed file the loader opened
  FUTR  the last three futex round windows (rounds/s)
  GDC   per-directory census totals: any dup / any mismatch left
  THRS  the last thread table row (the process at the stop)
  SIGST / ALIVE-SET   the last thread states
  FAULT the last kernel/user fault row, if any
  EXC   the last exceptions from Player.log

Usage: tools/rw/l78-wall.py obj/l78/serial-M1.log [--player obj/l78/Player-M1.log]
"""
import argparse
import re
import sys


def last(pat, txt, n=1, flags=0):
    m = list(re.finditer(pat, txt, flags))
    return m[-n:] if m else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--player")
    a = ap.parse_args()
    txt = open(a.log, "r", errors="replace").read()

    x = last(r"\[XSW\][^\n]*", txt)
    print("XSW  last:", x[0].group(0)[:220].rstrip() if x else "(none)")
    print("XSW  rows:", len(re.findall(r"\[XSW\]", txt)))

    xo = re.findall(r"\[XO\] (\S+)", txt)
    print("XO   count:", len(xo), " last:", xo[-1] if xo else "(none)")

    f = last(r"\[FUTR\][^\n]*", txt, 3)
    print("FUTR last 3:")
    for m in f:
        print("   ", m.group(0)[:200].rstrip())
    if not f:
        print("    (none)")

    gdc = re.findall(r"\[GDC\] rel=(\S*) .*?total=(-?\d+)(?: dup=(\d+))?", txt)
    dups = [g for g in gdc if g[2] and g[2] != "0"]
    print("GDC  windows:", len(gdc), " with dup:", len(dups))
    if dups:
        for g in dups[:5]:
            print("    dup:", g)

    t = last(r"\[THRS\][^\n]*", txt)
    print("THRS last:", t[0].group(0)[:400].rstrip() if t else "(none)")

    s = last(r"\[SIGST\][^\n]*", txt, 6)
    print("SIGST last %d:" % len(s))
    for m in s:
        print("   ", m.group(0)[:180].rstrip())

    al = last(r"\[ALIVE-SET\][^\n]*", txt, 6)
    if al:
        print("ALIVE-SET last %d:" % len(al))
        for m in al:
            print("   ", m.group(0)[:200].rstrip())

    fa = last(r"\[(KERNEL|UDX|SIGDUMP|RWMAIN)\][^\n]*", txt, 6)
    print("FAULT rows (last 6):")
    for m in fa:
        print("   ", m.group(0)[:200].rstrip())
    if not fa:
        print("    (none)")

    for pat, tag in ((r"\[BLKD\][^\n]*", "BLKD"), (r"\[BLKQ\][^\n]*", "BLKQ")):
        m = last(pat, txt, 2)
        for mm in m:
            print(tag, " ", mm.group(0)[:240].rstrip())

    if a.player:
        pl = open(a.player, "r", errors="replace").read()
        exc = [l.strip() for l in pl.splitlines() if "Exception" in l or
               "error" in l.lower() and "Could not" in l]
        print("EXC  Player.log exception lines:", len(exc))
        for e in exc[-6:]:
            print("   ", e[:200])
        tail = [l.strip() for l in pl.splitlines() if l.strip()][-6:]
        print("EXC  Player.log tail:")
        for e in tail:
            print("   ", e[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main())
