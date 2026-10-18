#!/usr/bin/env python3
"""Host (T1) self-test for flaky_triage core (GB-050)."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "t5"))
from flaky_triage import classify_line, parse_test_names, FlakyClassifier

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

# classify_line buckets the marker kinds
check(classify_line("make: *** [foo] Error 1 FAIL")[0] == "FAIL", "FAIL line -> FAIL")
check(classify_line("wexec timeout 30s exceeded")[0] == "TIMEOUT", "timeout -> TIMEOUT")
check(classify_line("SIGSEGV in run_task")[0] == "CRASH", "SIGSEGV -> CRASH")
check(classify_line("all green, 42 passed")[0] is None, "green line -> no marker")

# parse_test_names picks the test name before a FAIL/timeout marker
txt = "case_a: FAIL (2)\ncase_b: timeout\n[OK] case_c"
names = parse_test_names(txt)
check("case_a" in names and "case_b" in names and "case_c" not in names,
      "parse_test_names names the failing/timeout tests")

# FlakyClassifier separates BROKEN / FLAKY / PASSING
fc = FlakyClassifier()
for name, failed in [("broken", True), ("broken", True), ("broken", True),
                     ("flaky", True), ("flaky", False), ("flaky", True),
                     ("ok", False), ("ok", False)]:
    fc.observe(name, failed)
check(fc.classify("broken") == "BROKEN", "always-fail -> BROKEN")
check(fc.classify("flaky") == "FLAKY", "some-fail-some-pass -> FLAKY")
check(fc.classify("ok") == "PASSING", "always-pass -> PASSING")
check(fc.classify("unseen") == "UNSEEN", "unknown -> UNSEEN")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
