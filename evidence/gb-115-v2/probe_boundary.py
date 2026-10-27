#!/usr/bin/env python3
"""GB-115-V2 clause 6 probe: what does `>=` let through?

Case: a gate minted in the SAME second as the lane move that has ALREADY posted RED.
`gate_is_pending()` is consulted when verdict != GREEN, so a same-second gate with a
RED is treated as 'has not run yet' and the card is not flagged."""
import importlib.util
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
spec = importlib.util.spec_from_file_location("groom", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board", "groom.py"))
groom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(groom)

T = "2026-10-27T17:04:42Z"
issues = [
    {"number": 900, "title": "GB-900 a card in review", "state": "open",
     "created_at": T, "labels": [{"name": "In Review"}]},
    {"number": 901, "title": "GB-900-V its gate", "state": "open",
     "created_at": T, "labels": [{"name": "In Progress"}]},
]
comments = {
    900: [{"created_at": T, "body": "GB-900: lane In Progress \u2192 **In Review**"}],
    901: [{"created_at": "2026-10-27T17:04:47Z", "body": "**RED \u2014 GB-900-V, 5 seconds after the move.**"}],
}

print("fixture: lane move at", T, "/ gate created at", T, "/ gate posted RED at 17:04:47Z")
print("  gate_is_pending ->", groom.gate_is_pending(issues[0], issues[1], lambda n: comments[n]))
print("  findings        ->", groom.verify_lint_findings(issues, lambda n: comments[n]))

# control: the same RED five seconds later than the move still gets masked?
comments[901][0]["created_at"] = "2026-10-27T17:09:00Z"
print("\ncontrol: gate created at T, RED posted at 17:09:00Z (4 min later)")
print("  findings        ->", groom.verify_lint_findings(issues, lambda n: comments[n]))

# control 2: gate created one second BEFORE the move -> must be flagged
issues[1]["created_at"] = "2026-10-27T17:04:41Z"
print("\ncontrol 2: gate created one second BEFORE the move (17:04:41Z)")
print("  findings        ->", groom.verify_lint_findings(issues, lambda n: comments[n]))