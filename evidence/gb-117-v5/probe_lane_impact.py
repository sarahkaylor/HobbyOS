#!/usr/bin/env python3
"""How much does the lane-substring rule move Clock B on the live board?

Compares the shipped Clock B (lane rule = `"LANE" in up and arrow anywhere in the body`)
with a Clock B where a lane move must be a *move-shaped* post: one short line that
starts with the id (or `NNN`) and contains "lane" + arrow — i.e. what board.py writes.
Prints which cards change age and the two splits.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import claim_age as C  # noqa: E402

MOVE_RE = re.compile(r"^\W*(?:[A-Za-z0-9_.-]{1,24}:\s*)?lane\b.*(?:→|->)", re.I)


def shipped_book(c):
    return C._is_bookkeeping(c)


def narrow_book(c):
    body = (c.get("body") or "")
    head = body.lstrip("*# \t\n")
    up = head.upper()
    if up.startswith(C._BOOKKEEPING_PREFIXES) or up.startswith(C._BOOKKEEPING_MARK):
        return True
    first = head.strip().splitlines()[0] if head.strip() else ""
    if len(head.strip().splitlines()) == 1 and len(first) <= 160 and MOVE_RE.match(first):
        return True
    return False


issues, comments, now = C.load_live()


def clock(cs, pred):
    act = [c for c in cs if not pred(c)]
    return C.age_hours(act[-1]["created_at"], now) if act else None


changed = []
s1 = f1 = s2 = f2 = 0
for i in issues:
    if i.get("state") != "open":
        continue
    cs = comments.get(i["number"], [])
    a1, a2 = clock(cs, shipped_book), clock(cs, narrow_book)
    if C.stale(a1):
        s1 += 1
    else:
        f1 += 1
    if C.stale(a2):
        s2 += 1
    else:
        f2 += 1
    if a1 != a2:
        changed.append((i["number"], (i.get("title") or "")[:44], a1, a2))

print(f"open cards: {sum(1 for i in issues if i.get('state') == 'open')}")
print(f"shipped Clock B : stale/fresh = {s1}/{f1}")
print(f"narrow  Clock B : stale/fresh = {s2}/{f2}")
print(f"\ncards whose Clock-B age changes: {len(changed)}")
for n, t, a1, a2 in changed:
    fa = "None(stale)" if a1 is None else f"{a1:.1f}h"
    fb = "None(stale)" if a2 is None else f"{a2:.1f}h"
    print(f"  #{n:<4} shipped={fa:>12}  narrow={fb:>12}  {t}")