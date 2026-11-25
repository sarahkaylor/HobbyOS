#!/usr/bin/env python3
"""fence_map.py -- per-window classification of the committed NVIDIA-580 BAR0 dumps.

Evidence used (committed in the tree, from a card running NVIDIA 580.178.04):
  docs/nv-gsp-reference-evidence/dump-{pre,post}init-gsp-sweep.txt  (0x110000..0x112000)
  docs/nv-gsp-reference-evidence/dump-{pre,post}init-named.txt      (named windows)

Sentinels are the driver's own (src/kernel/nv_regs.h):
  0xBADF5040 = unreadable-by-design (a normal answer; the card renders with it)
  0xBADF5620 = fenced / not initialised
  0xBADF5720 = exists but not readable here, after a fence lifted
  0xFFFFFFFF = absent
Anything else is data.

Why this table exists: a no-RM path needs to know which apertures the host can
address. That answer is per window, not per card, and this prints it for the one
state we have measured from both stacks: pre-bind and with GSP-RM running.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEF = os.path.join(HERE, "..", "..", "..", "docs", "nv-gsp-reference-evidence")
EV = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(DEF)

SENT = {0xBADF5040: "unreadable-by-design", 0xBADF5620: "fenced",
        0xBADF5720: "exists-not-readable", 0xFFFFFFFF: "absent"}

WINDOWS = [
    ("PMC (boot/intr/enable)", 0x000000, 0x001000),
    ("PBUS 0x009400", 0x009400, 0x009800),
    ("PDISP 0x100000", 0x100000, 0x101000),
    ("GSP falcon 0x110000", 0x110000, 0x110800),
    ("GSP fenced tail", 0x110800, 0x111000),
    ("PRI 0x111000 (RISC-V)", 0x111000, 0x112000),
    ("PFB/WPR2 0x1FA800", 0x1FA800, 0x1FA840),
    ("SEC2 0x840000", 0x840000, 0x840800),
]


def read_rows(p):
    rows = []
    for line in open(p):
        m = re.match(r"^(?:#\s*)?0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s*$", line.strip())
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2), 16)))
    return rows


def build(tag):
    sweep = read_rows(os.path.join(EV, "dump-%s-gsp-sweep.txt" % tag))
    named = read_rows(os.path.join(EV, "dump-%s-named.txt" % tag))
    m = {}
    for a, v in sweep + named:
        m[a] = SENT.get(v, "data")
    return m, len(sweep), len(named)


def counts(m, lo, hi):
    c = {}
    for a, k in m.items():
        if lo <= a < hi:
            c[k] = c.get(k, 0) + 1
    return c


def fmt(c):
    if not c:
        return "(no rows)"
    return ", ".join("%s=%d" % (k, c[k]) for k in sorted(c))


pre, ps, pn = build("preinit")
post, qs, qn = build("postinit")
print("pre-init dump: sweep=%d named=%d | post-init dump: sweep=%d named=%d"
      % (ps, pn, qs, qn))
print()
print("| window | range | pre-init | post-init (GSP-RM running) |")
print("|---|---|---|---|")
for name, lo, hi in WINDOWS:
    print("| %s | 0x%06x..0x%06x | %s | %s |"
          % (name, lo, hi - 1, fmt(counts(pre, lo, hi)), fmt(counts(post, lo, hi))))
