#!/usr/bin/env python3
"""GB-117-V2 clause 5: which way does a lane-move comment fall under the shipped
`_is_bookkeeping`, and does classifying it the other way change the count?

Read-only against the live board.  Prints:
  * how the shipped predicate classifies a real lane-move comment and a real
    EVIDENCE/VERDICT comment,
  * every card whose NEWEST activity comment is a lane-move line (so the
    classification is the difference between two ages),
  * the stale/fresh split under the shipped rule, and under the alternative
    where a lane move is treated as bookkeeping.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import claim_age as C  # noqa: E402

LANE_RE = re.compile(r"^\W*[A-Za-z0-9_.-]+\s*:\s*lane\b")

issues, comments, now = C.load_live()
print(f"open cards: {len(issues)}   now={now.isoformat()}")

print("\n== shipped classification of representative comments ==")
samples = []
for n, cs in comments.items():
    for c in cs:
        b = (c.get("body") or "")
        if LANE_RE.match(b.strip()) and len(samples) < 2:
            samples.append(("lane move", b.strip()[:70], C._is_bookkeeping(c)))
        elif b.lstrip("*# \t\n").upper().startswith(("EVIDENCE", "VERDICT", "**GREEN", "**RED")) and len(samples) < 6:
            samples.append(("worker content", b.strip().splitlines()[0][:70], C._is_bookkeeping(c)))
for kind, text, booked in samples:
    print(f"  {'BOOKKEEPING' if booked else 'ACTIVITY   '}  <- {kind}: {text!r}")

print("\n== cards whose newest ACTIVITY comment is a lane move (shipped rule) ==")
affected = []
for i in issues:
    n = i["number"]
    act = [c for c in comments.get(n, []) if not C._is_bookkeeping(c)]
    if act and LANE_RE.match((act[-1].get("body") or "").strip()):
        affected.append((n, i["title"][:52], C.age_hours(act[-1]["created_at"], now)))
for n, t, age in sorted(affected, key=lambda x: x[2]):
    print(f"  #{n:<4} newest 'activity' is a lane move at {age:6.1f}h   {t}")
print(f"  count: {len(affected)}")


def split(exclude_lane):
    buckets = {"under6h": 0, "6-12h": 0, "12-24h": 0, "over24h": 0, "nocomments": 0}
    st = fr = 0
    for i in issues:
        n = i["number"]
        act = [c for c in comments.get(n, [])
               if not C._is_bookkeeping(c)
               and not (exclude_lane and LANE_RE.match((c.get("body") or "").strip()))]
        age = C.age_hours(act[-1]["created_at"], now) if act else None
        if age is None:
            buckets["nocomments"] += 1
        elif age < 6:
            buckets["under6h"] += 1
        elif age < 12:
            buckets["6-12h"] += 1
        elif age < 24:
            buckets["12-24h"] += 1
        else:
            buckets["over24h"] += 1
        if C.stale(age):
            st += 1
        else:
            fr += 1
    return buckets, st, fr


b1, s1, f1 = split(False)
b2, s2, f2 = split(True)
print(f"\nshipped rule          : buckets={b1} stale={s1} fresh={f1}")
print(f"lane move = bookkeeping: buckets={b2} stale={s2} fresh={f2}")
print(f"\ndelta stale: {s2 - s1:+d}   delta fresh: {f2 - f1:+d}")