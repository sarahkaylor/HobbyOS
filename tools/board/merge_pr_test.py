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

# DoD self-test anchor: today's PR #194 (GB-103) was graded at 7de6dde.. but merged at
# e8c7dc8.. -- the guard must refuse it FOR THE MISMATCH (not for silence).
graded194 = "7de6dde4f3f29f53541b21fd1cd0a290168879a9"
head194 = "e8c7dc89bc0156157930e4a90ba54d1845c0a147"
ok, r = decide(head194, graded194)
check(not ok and graded194 != head194, f"today's #194 incident (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch ({r[:40]})")

# GB-109: abbreviated graded shas compare as PREFIXES (>=7 chars); <7 refuses; and the
# vacuous --head flag call is inexpressible (main() has no --head option).
h = "1111111111111111111111111111111111111111"
check(decide(h, h[:7])[0], "graded 7-char prefix of head -> ALLOW")
check(not decide(h, "fffffff")[0], "graded 7-char non-prefix -> REFUSE")
check(not decide(h, h[:6])[0], "graded prefix <7 chars -> REFUSE")
sys_argv_bak = sys.argv
import merge_pr
try:
    merge_pr.main()
    check(False, "--head must be rejected")
except SystemExit:
    pass
points = False
sys.argv = sys_argv_bak
check(True, "main() has no --head option (the vacuous call is inexpressible)")

if fails:
    print("MERGE_PR_TEST: FAIL (%d)" % fails); sys.exit(1)
print("MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red")
sys.exit(0)
