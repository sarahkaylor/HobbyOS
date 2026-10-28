#!/usr/bin/env python3
"""GB-117-V3 clause 6 / clause 7-of-V2: name a borderline comment and show what the
tool does with it.

Two candidates:
  (a) the docstring's own borderline: a *claim-of-work* post with DoD text;
  (b) a WORKER content post that quotes a lane move -- `_is_bookkeeping`'s lane rule
      is `"LANE" in up and ("→" or "->")`, a body-substring test that does not look at
      the post's kind, so a worker EVIDENCE/VERDICT comment mentioning a lane move may
      be classified bookkeeping (and dropped from Clock B).

Also scans the LIVE board for real comments in case (b).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import claim_age as C  # noqa: E402

print("== synthetic probes ==")
cases = [
    ("claim-of-work with DoD (docstring's borderline)",
     "**GB-108 claimed by wabi (FLASH, swarm-farm-2).** DoD: the device leg runs."),
    ("bare claim notch", "claimed by `wabi` on swarm-farm-1 — lane: Ready → In Progress"),
    ("lane move", "GB-117: lane Backlog → **Ready**"),
    ("worker EVIDENCE that QUOTES a lane move",
     "**EVIDENCE — GB-117.** The lane move `GB-117: lane Backlog → **Ready**` is bookkeeping; here is the run."),
    ("worker VERDICT that mentions a lane and an arrow",
     "**VERDICT — GREEN.** Measured: lane Ready → **In Progress** at 17:04:42Z."),
]
for name, body in cases:
    print(f"  {'BOOKKEEPING' if C._is_bookkeeping({'body': body}) else 'ACTIVITY   '}  <- {name}")

print("\n== live board: worker-content comments classified bookkeeping by the lane rule ==")
issues, comments, now = C.load_live()
hits = []
for i in issues:
    for c in comments.get(i["number"], []):
        b = c.get("body") or ""
        up = b.lstrip("*# \t\n").upper()
        if C._is_bookkeeping(c) and not (up.startswith(C._BOOKKEEPING_PREFIXES)
                                         or up.startswith(C._BOOKKEEPING_MARK)):
            hits.append((i["number"], b.strip().splitlines()[0][:88], c.get("created_at")))
for n, first, ts in hits[:12]:
    print(f"  #{n:<4} {ts}  {first!r}")
print(f"  count: {len(hits)}")