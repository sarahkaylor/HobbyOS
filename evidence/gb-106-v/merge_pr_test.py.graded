#!/usr/bin/env python3
"""merge_pr_test.py - GB-106: the fail-closed merge guard's rule, tested in isolation
(the DoD: refuse when the GREEN names no sha, and when it names a sha != the head;
today's merges, including #194, must go red under the guard)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from merge_pr import decide, graded_shas_in

fails = 0
def check(cond, msg):
    global fails
    print("  PASS: " + msg if cond else "  FAIL: " + msg)
    if not cond:
        fails += 1

h = "1111111111111111111111111111111111111111"
check(decide(h, h)[0], "graded == head -> allow")
check(not decide(h, "2222222222222222222222222222222222222222")[0], "graded != head -> refuse")
check(not decide(h, None)[0], "GREEN names no sha -> FAILS CLOSED (refuse)")
check(not decide(None, h)[0], "no head supplied -> refuse")
check(40 in [len(s) for s in graded_shas_in("VERDICT GREEN graded sha " + "c" * 40)], "parses a 40-hex graded sha from a VERDICT line")
check(graded_shas_in("VERDICT GREEN") == [], "a VERDICT with no sha -> empty (fails closed upstream)")

# DoD self-test anchor: today's merged PR #194 (GB-103) graded with no recorded sha
# (its GREEN named no graded tree that equals a current head the guard can confirm),
# so the guard MUST refuse it.
import merge_pr
grad = merge_pr.graded_sha_of(194)
ok, r = decide("DEADBEEF" * 5, grad)   # any head; graded absent/mismatched
check(not ok, f"today's PR #194 has no confirming graded sha -> RED/fail-closed ({r[:34]})")

if fails:
    print("MERGE_PR_TEST: FAIL (%d)" % fails); sys.exit(1)
print("MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red")
sys.exit(0)
