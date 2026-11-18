#!/usr/bin/env python3
"""tools/cp/hunt/inputcheck.py -- were menu key presses dropped, doubled or
late?  (lane H1-hunt, docs/cp-hunt.md)

    inputcheck.py RUNLOG [RUNLOG ...]

boot_p9.py's `seek` / `listseek` take a screendump before every tap and log
the highlighted row ("focus <shot>: rows ... centre Y -> LABEL", "list focus
<shot>: ... centre Y").  Each tap (an up/down key, 200 ms down, 800 ms gap)
should move the focus exactly one row; so, per pair of consecutive readings
with a visible focus:
   one      moved one row in the tap's direction        (expected)
   dropped  did not move                                 (lost or late key)
   extra    moved two or more rows                       (repeat / doubled key)
The first tap of a seek, which only reveals the hidden focus, is counted
apart ("reveal"), as are readings with no visible focus ("none").  Main-menu
rows are 37-38 px apart, pause-menu rows ~40, save-list entries ~64.
"""
import re
import sys

FOCUS_RX = re.compile(r"\[P9W\] focus (s\d+-t\d+-(\S+?)(\d+)): (?:rows \d+\.\.\d+ centre (\d+) -> (\S+(?: \S+)*)|no highlighted row)")
LIST_RX = re.compile(r"\[P9W\] list focus (s\d+-t\d+-(\S+?)(\d+)): frame rows .*? centre (\S+)")
DONE_RX = re.compile(r"\[P9W\] \+\s+[0-9.]+ s\s+\S+\s+(seek|listseek|SEEK FAILED|LISTSEEK FAILED) (\S+?):")


def steps(seq, pitch):
    """seq: list of y or None per reading -> list of classifications per tap"""
    out = []
    prev = None
    revealed = False
    for y in seq:
        if prev is None:
            out.append("reveal" if (y is not None and not revealed) else "none")
            revealed = revealed or y is not None
        else:
            if y is None:
                out.append("none")
            else:
                n = round(abs(y - prev) / pitch)
                out.append("dropped" if n == 0 else "one" if n == 1 else "extra")
        prev = y if y is not None else prev
    return out[1:]                       # the first reading precedes any tap


def check(path):
    seeks = {}
    order = []
    for ln in open(path, errors="replace"):
        m = FOCUS_RX.search(ln)
        kind = "main"
        if not m:
            m = LIST_RX.search(ln)
            kind = "list"
        if m:
            tag = m.group(2)
            if tag not in seeks:
                seeks[tag] = {"kind": kind, "ys": []}
                order.append(tag)
            yv = m.group(4)
            y = float(yv) if yv not in (None, "None") else None
            seeks[tag]["ys"].append(y)
            continue
        m = DONE_RX.search(ln)
        if m and m.group(2) in seeks:
            seeks[m.group(2)]["result"] = m.group(1)
    tot = {}
    print(f"== inputcheck {path}")
    for tag in order:
        s = seeks[tag]
        pitch = 64.0 if s["kind"] == "list" else 38.0
        if "pause" in tag:
            pitch = 40.0
        st = steps(s["ys"], pitch)
        for k in st:
            tot[k] = tot.get(k, 0) + 1
        ys = " ".join("-" if y is None else f"{y:.0f}" for y in s["ys"])
        print(f"   {tag:<20} {s['kind']:<5} {s.get('result', '?'):<15} taps={len(st):2d} focus y: {ys}  -> {' '.join(st)}")
    print("   totals: " + " ".join(f"{k}={v}" for k, v in sorted(tot.items())))
    return tot


def main():
    grand = {}
    for p in sys.argv[1:]:
        for k, v in check(p).items():
            grand[k] = grand.get(k, 0) + v
    if len(sys.argv) > 2:
        print("== all: " + " ".join(f"{k}={v}" for k, v in sorted(grand.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
