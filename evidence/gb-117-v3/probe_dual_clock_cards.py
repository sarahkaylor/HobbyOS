#!/usr/bin/env python3
"""GB-117-V3 clause 2: does the live report ever print a card's BOTH clocks?

Finds the cards where Clock A and Clock B disagree (the round-1 shape: a fresh
bookkeeping/lane-move notch on top of older substantive work), prints their two
ages as the tool's own functions compute them, and checks whether the report's
text mentions those ages together.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import claim_age as C  # noqa: E402

issues, comments, now = C.load_live()
rep = os.path.join(os.path.dirname(os.path.abspath(__file__)), "claim_age_live.txt")
text = open(rep, errors="ignore").read() if os.path.exists(rep) else ""

rows = []
for i in issues:
    if i.get("state") != "open":
        continue
    cs = comments.get(i["number"], [])
    ac, wc = C._clock(cs, now, "any"), C._clock(cs, now, "working")
    if ac is None or wc is None:
        continue
    if C.stale(ac) != C.stale(wc):
        rows.append((i["number"], (i.get("title") or "")[:46], ac, wc))

print(f"open cards: {sum(1 for i in issues if i.get('state') == 'open')}   clocks disagree on: {len(rows)}")
for n, t, ac, wc in sorted(rows, key=lambda r: r[2]):
    has_both = bool(re.search(rf"#{n}\b", text)) and (f"{ac:.1f}h" in text and f"{wc:.1f}h" in text)
    print(f"  #{n:<4} clockA={ac:6.1f}h ({'STALE' if C.stale(ac) else 'fresh'})  "
          f"clockB={wc:6.1f}h ({'STALE' if C.stale(wc) else 'fresh'})  both-ages-in-report={has_both}  {t}")

print("\nLive report lines that name a card id and an age (per-card rows):")
for line in text.splitlines():
    if re.search(r"^\s+#\d+", line):
        print("  " + line.strip()[:100])