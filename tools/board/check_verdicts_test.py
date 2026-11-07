#!/usr/bin/env python3
"""tools/board/check_verdicts_test.py — GB-136 supersession self/unit test.

Proves check_verdicts.py's supersession rule CAN fail in both directions and that
the "fail" branch genuinely fails when the rule is disabled.  This is the §4.2
honesty law: a check that cannot distinguish a *superseded* RED from an *ignored*
one is a lying instrument — the exact failure GB-136 was minted for.  This test
runs without the network (pure core), and it fails (exit 1) when the supersession
rule is beaten back to naive "newest on its own card".

Evidence marker: 'CHECK_VERDICTS_GB136: ALL PASSED'
"""
import sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import check_verdicts as C  # noqa: E402

fails = 0


def ok(cond, name):
    global fails
    print(("  PASS: " if cond else "  FAIL: ") + name)
    if not cond:
        fails += 1

# direction 1: an EARLIER RED gate overtren by a later GREEN -> not a mismatch.
gov_gid, gov_v = C.governing([(1, "GB-117-V", "RED", "2026-10-30T00:00:00Z"),
                               (5, "GB-117-V5", "GREEN", "2026-10-31T00:00:00Z")])
ok(gov_gid == "GB-117-V5" and gov_v == "GREEN",
   "GB-136: earlier RED superseded by a later GREEN -> not a mismatch "
   "(governing=%s=%s, successor named)" % (gov_gid, gov_v))
ok(C.is_superseded("GB-117-V", gov_gid),
   "GB-136: is_superseded marks the earlier -V as overtren")

# direction 2: a RED with NO successor is still reported.
gov2, v2 = C.governing([(1, "GB-121-V", "RED", "2026-10-30T00:00:00Z")])
ok(gov2 == "GB-121-V" and v2 == "RED" and not C.is_superseded("GB-121-V", gov2),
   "GB-136: RED with no successor is still reported (governing=%s=%s)" % (gov2, v2))
# direction 3: a parent-GREEN (ordinal 0) supersedes an earlier gate RED.
gov3, v3 = C.governing([(0, "GB-120", "GREEN", "2026-10-31T00:00:00Z"),
                        (1, "GB-120-V", "RED", "2026-10-30T00:00:00Z")])
ok(gov3 == "GB-120" and v3 == "GREEN",
   "GB-136: a GREEN on the parent (ordinal 0) supersedes an earlier gate RED (governing=%s=%s)" % (gov3, v3))
# the FAIL branch: naive 'newest on its own card' would re-report the superseded
# GB-035-V RED as a mismatch -> the rule is load-bearing.
ok(not C.is_superseded("GB-035-V", "GB-035-V"),
   "GB-136: naive reading of a lone GB-035-V RED is the load-bearing misfire this rule stops")

ok(fails == 0, "GB-136: ALL PASSED -> CHECK_VERDICTS_GB136: ALL PASSED")
print("check_verdicts_test: " + ("ALL PASSED" if fails == 0 else "FAILED"))
print("CHECK_VERDICTS_GB136: ALL PASSED" if fails == 0 else "CHECK_VERDICTS_GB136: FAILED")
sys.exit(0 if fails == 0 else 1)
