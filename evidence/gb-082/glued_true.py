#!/usr/bin/env python3
"""True glued-line estimate: a SECOND record tag must look like a record start.

A record tag is `[A-Z0-9_]{3,}` that is NOT preceded by an identifier character —
`tf->regs[0]` is a subscript (preceded by `s`), `[KERNEL]` after `] ` is a record.
Counts the same lines the counter counts, and splits them.
"""
import re, sys, collections

TAG_ANY = re.compile(r"\[([A-Za-z0-9_-]{1,})\]")
RECORD = re.compile(r"(?<![A-Za-z0-9_>\-.])\[([A-Z][A-Z0-9_]{2,})\]")

def analyse(path):
    lines = [l.replace("\r", "") for l in open(path, errors="replace")]
    counter_hits = 0
    real = []
    subs = 0
    for l in lines:
        if "STACK[" in l:
            continue
        # replicate the shell predicates
        p1 = re.search(r"\[[A-Za-z0-9_-]+\].*\[[A-Za-z0-9_-]+", l)
        p2 = re.search(r"\][A-Za-z0-9_].*\[[A-Za-z0-9_-]+\]", l)
        if not (p1 or p2):
            continue
        counter_hits += 1
        recs = RECORD.findall(l)
        if len(recs) >= 2:
            real.append(l.strip())
        else:
            subs += 1
    print(f"{path}\n  counter says {counter_hits}, subscript-only {subs}, "
          f"two-record {len(real)}")
    for r in real[:6]:
        print("    REAL: " + r[:118])
    return counter_hits, subs, len(real)

for p in sys.argv[1:]:
    analyse(p)
