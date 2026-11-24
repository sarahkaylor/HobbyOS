#!/usr/bin/env python3
"""Recompute the two fps windows from the r1 serial log, stream 1 only.

The log carries a second [XSW] stream that begins at guest t=23732 ms and
presents the loader-art CRC; its rows interleave with the run-long stream, so
this filters to the run-long stream (t > 600000) before differencing.
"""
import re
import sys

path = sys.argv[1]
rows = []
for l in open(path, errors="ignore"):
    if "[XSW] n=" in l:
        m = re.search(r"n=(\d+)", l)
        t = re.search(r" t=(\d+)", l)
        c = re.search(r" crc=(0x[0-9a-f]+)", l)
        if m and t:
            rows.append((int(t.group(1)), int(m.group(1)), c.group(1) if c else ""))

s1 = [r for r in rows if r[0] > 600000]
s2 = [r for r in rows if r[0] <= 600000]
print("rows=%d stream1=%d stream2=%d" % (len(rows), len(s1), len(s2)))
if s2:
    print("stream2 first=%s last=%s" % (s2[0], s2[-1]))
print("stream2 crcs=%s" % sorted({r[2] for r in s2}))


def stats(seq, label):
    rates = []
    for a, b in zip(seq, seq[1:]):
        if b[0] > a[0]:
            rates.append(((b[1] - a[1]) * 1000.0 / (b[0] - a[0]), b[0] - a[0]))
    v = sorted(r for r, dt in rates if dt < 2000)
    if not v:
        print(label + ": no intervals")
        return
    n = len(v)
    print("%s: intervals=%d median=%.2f p10=%.2f p90=%.2f min=%.2f max=%.2f"
          % (label, n, v[n // 2], v[int(.1 * n)], v[int(.9 * n)], v[0], v[-1]))


modal = [r for r in s1 if r[2] == "0x0e40610827f1e4d3"]
if modal:
    stats(modal, "MODAL")
    print("modal presents=%d guest_ms=%d" % (modal[-1][1] - modal[0][1],
                                             modal[-1][0] - modal[0][0]))
menu_all = [r for r in s1 if r[2] == "0xcb920d0f8c49a201"]
if menu_all:
    lo = menu_all[0][0] + 5000
    hi = lo + 150000
    menu = [r for r in menu_all if lo <= r[0] <= hi]
    stats(menu, "MENU(150s,+5s)")
    print("menu presents=%d guest_ms=%d rows=%d" % (menu[-1][1] - menu[0][1],
                                                    menu[-1][0] - menu[0][0], len(menu)))
    stats(menu_all, "MENU(whole)")
whole = [r for r in s1]
stats(whole, "RUN(stream1)")
