#!/usr/bin/env python3
"""drift_check_test.py - unit test for the board drift finder (GB-049 territory).

Tests the ATTRIBUTION rule rather than the network calls, because the rule is
where the judgement lives and where the first run of the tool went wrong: it
attributed GB-007 to PR #25 purely because that PR's *body* mentioned it.  A tool
that over-reports drift is worse than no tool -- it trains the reader to ignore
it -- so the title-vs-body distinction is pinned here.

Run:  python3 tools/board/drift_check_test.py   (the host suite runs it too)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drift_check as dc  # noqa: E402

checks = 0
failed = 0


def check(ok, name):
    global checks, failed
    checks += 1
    print(("  PASS: " if ok else "  FAIL: ") + name)
    if not ok:
        failed += 1


PRS = [
    {"number": 25, "merged_at": "2026-10-18T02:16Z",
     "title": "GB-017: T4 differential lock for getrandom + /dev/urandom + /dev/null",
     "body": "Extends the probe. Also references GB-016, GB-019, GB-020 and GB-007 for context."},
    {"number": 40, "merged_at": "2026-10-18T03:38Z",
     "title": "infra(tests): GB-064 self-registering host tests",
     "body": "GB-005 asked for this."},
    {"number": 99, "merged_at": None, "title": "GB-021: still open, not merged", "body": ""},
    {"number": 41, "merged_at": "2026-10-18T03:57Z",
     "title": "tests(games): GB-053 S0/S1 final acceptance run", "body": ""},
]

titles = dc.landed_ids(PRS)
bodies = dc.landed_ids(PRS, use_bodies=True)

check(titles.get("GB-017") == [25], "title attribution: GB-017 -> PR #25")
check(titles.get("GB-064") == [40], "title attribution: GB-064 -> PR #40")
check(titles.get("GB-053") == [41], "title attribution: GB-053 -> PR #41")

check("GB-007" not in titles,
      "a ticket MENTIONED only in a body is not attributed (the first run's false positive)")
check("GB-016" not in titles and "GB-019" not in titles and "GB-020" not in titles,
      "the same for every ticket PR #25 merely mentioned")
check(bodies.get("GB-007") == [25],
      "--bodies finds the mention, so the imprecision is available on request, not by default")

check("GB-021" not in titles and "GB-021" not in bodies,
      "an UNMERGED PR never counts, whatever it is about")
check(all(n != 99 for v in titles.values() for n in v),
      "and its number never appears in any list")

# A single merged PR can legitimately carry several tickets in its title.
multi = dc.landed_ids([{"number": 7, "merged_at": "x",
                        "title": "fix GB-001 and GB-002 together", "body": ""}])
check(multi.get("GB-001") == [7] and multi.get("GB-002") == [7],
      "one title naming two tickets attributes both")

print(f"drift_check_test: {checks} checks, {failed} failed -- "
      + ("ALL PASSED" if failed == 0 else "FAILED"))
sys.exit(0 if failed == 0 else 1)
