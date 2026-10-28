#!/usr/bin/env python3
"""GB-117-V5 clause 2: is the new per-card row ADDITIVE — does it report the same two
clocks as the aggregate blocks?

Recomputes every card's Clock A/B with the tool's own functions, re-derives the bucket
counts and the split, and compares them with the numbers printed in the live report.
Also checks every printed per-card row against the recomputation.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import claim_age as C  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
text = open(os.path.join(HERE, "claim_age_live.txt"), errors="ignore").read()
issues, comments, now = C.load_live()

rows = {}
for i in issues:
    if i.get("state") != "open":
        continue
    cs = comments.get(i["number"], [])
    rows[i["number"]] = (C._clock(cs, now, "any"), C._clock(cs, now, "working"))

ab = {k: 0 for k in C.BUCKETS_ORDER}
wb = {k: 0 for k in C.BUCKETS_ORDER}
ast = afr = wst = wfr = 0
for n, (a, w) in rows.items():
    ab[C.bucket_for(a)] += 1
    wb[C.bucket_for(w)] += 1
    ast += 1 if C.stale(a) else 0
    afr += 0 if C.stale(a) else 1
    wst += 1 if C.stale(w) else 0
    wfr += 0 if C.stale(w) else 1

print(f"recomputed: A buckets={ab}  A split={ast}/{afr}")
print(f"recomputed: B buckets={wb}  B split={wst}/{wfr}")

m = re.search(r"CLOCK A.*?\n  buckets: ([^\n]+)\n.*?= (\d+)/(\d+)", text, re.S)
m2 = re.search(r"CLOCK B.*?\n  buckets: ([^\n]+)\n.*?split=(\d+)/(\d+)", text, re.S)
print(f"printed   : A buckets={m.group(1).strip()}  A split={m.group(2)}/{m.group(3)}")
print(f"printed   : B buckets={m2.group(1).strip()}  B split={m2.group(2)}/{m2.group(3)}")


def parse_bucket(s):
    d = {}
    for k, v in re.findall(r"([a-z0-9-]+)=(\d+)", s):
        d[k] = int(v)
    return d


a_ok = parse_bucket(m.group(1)) == {k: v for k, v in ab.items()}
b_ok = parse_bucket(m2.group(1)) == {k: v for k, v in wb.items()}
print(f"\nbuckets additive: A={a_ok}  B={b_ok}")
print(f"splits additive: A={m.group(2)}/{m.group(3)} vs {ast}/{afr} -> "
      f"{int(m.group(2)) == ast and int(m.group(3)) == afr}   "
      f"B={m2.group(2)}/{m2.group(3)} vs {wst}/{wfr} -> "
      f"{int(m2.group(2)) == wst and int(m2.group(3)) == wfr}")

print("\nper-card rows checked against recomputation:")
bad = 0
for line in text.splitlines():
    mm = re.match(r"\s+#(\d+)\s+\S.*?ClockA=([0-9.]+h|no-comments)\s+ClockB=([0-9.]+h|no-comments)?", line)
    if not mm:
        continue
    n = int(mm.group(1))
    def to_h(x):
        return None if (x is None or x == "no-comments") else float(x[:-1])
    ca, cb = to_h(mm.group(2)), to_h(mm.group(3))
    ra, rw = rows.get(n, (None, None))
    ok = ((ca is None and ra is None) or (ca is not None and ra is not None and abs(ca - ra) < 0.15)) and \
         ((cb is None and rw is None) or (cb is not None and rw is not None and abs(cb - rw) < 0.15))
    if not ok:
        bad += 1
        print(f"  MISMATCH #{n}: row A={ca} B={cb}  recomputed A={ra} B={rw}")
print(f"  rows checked; mismatches: {bad}")