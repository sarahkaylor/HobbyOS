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


def _pr_id(title):
    m = C.PR_ID_RE.match(title)
    return m.group(1) if m else None


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

# GB-231: split-card gates.  The id must reach the check and map to the split
# parent; a RED on the split parent is a mismatch in Done and accepted in
# In Review (WORK POLICY §2: an open PR never moves back to In Progress).
m = C.GATE_RE.match("GB-146-A-V2")
ok(m is not None and m.group(1) == "GB-146-A" and C.gate_ordinal("GB-146-A-V2") == 2,
   "GB-231: GB-146-A-V2 parses as a gate on parent GB-146-A, ordinal 2")
ok(C.GATE_RE.match("GB-146-A") is None,
   "GB-231: the split parent GB-146-A is not itself a gate id")
ok(C.classify("RED", "Done", split=True)[0] is False,
   "GB-231: GB-146-A-V2 RED with parent GB-146-A in Done is a mismatch")
ok(C.classify("RED", "In Review", split=True)[0] is True,
   "GB-231: GB-146-A-V2 RED with parent GB-146-A in In Review is accepted (open PR, §2)")
ok(C.classify("RED", "In Review", split=False)[0] is False,
   "GB-231: a NON-split RED in In Review is still a mismatch (GB-010/GB-070 rule kept)")

# GB-236: a RED on a parent in `In Review` is clean ONLY with an observed open PR.
ok(C.classify("RED", "In Review", has_open_pr=True)[0] is True,
   "GB-236: GB-232-V RED + parent In Review + open PR #415 -> clean")
ok(C.classify("RED", "In Review", has_open_pr=False)[0] is False,
   "GB-236: the same pair with NO open PR is still a mismatch")
ok(C.classify("RED", "Done", has_open_pr=True)[0] is False,
   "GB-236: the same pair with the parent Done is still a mismatch")
ok(_pr_id("GB-232: stamp start_scheduler's stage and explain the breaker") == "GB-232",
   "GB-236: PR title maps to GB-232 (pr_sync.py's first-GB-token mapping)")
ok(_pr_id("GB-146-A: [BADVEC] names the fatal frame") == "GB-146-A",
   "GB-236: a split PR title keeps its suffix (GB-146-A)")
ok(_pr_id("no ticket id here") is None,
   "GB-236: a title with no GB id maps to nothing (no silent parent)")
ok(C.parse_open_pr_args(["--open-pr", "GB-232"]) == {"GB-232"}
   and C.parse_open_pr_args(["--open-pr=GB-146-A"]) == {"GB-146-A"}
   and C.parse_open_pr_args([]) is None,
   "GB-236: --open-pr is the explicit fallback when GITHUB_TOKEN is unavailable")

print("check_verdicts_test: " + ("ALL PASSED" if fails == 0 else "FAILED"))
print("CHECK_VERDICTS_GB136: ALL PASSED" if fails == 0 else "CHECK_VERDICTS_GB136: FAILED")
sys.exit(0 if fails == 0 else 1)
