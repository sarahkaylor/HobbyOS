#!/usr/bin/env python3
"""check_stale_test.py - GB-098 host (T1) self-test for tools/board/check_stale.py.

Runs the tool's own --self-test, which proves both directions the DoD names:
  * an issue with an OLD last comment and a FRESH updated_at is reported
    STALE (this is the case that failed before GB-098 - a bulk touch on
    `updated_at` cleared genuinely parked cards), and
  * an issue with a fresh comment is NOT stale, whatever updated_at says.

The stale clock is the last non-coordinator comment (which every lane move
also posts), so `updated_at` never re-arms a parked card.  The test needs NO
network (FORGEJO_TOKEN is not read during --self-test), so it runs inside
make host_tests.  The verdict is the exit status plus this script's own
PASS line - it exits non-zero if --self-test finds a failure.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "check_stale.py")

r = subprocess.run([sys.executable, TOOL, "--self-test"],
                   capture_output=True, text=True)
out = (r.stdout or "") + (r.stderr or "")

result_line = [l for l in out.splitlines() if l.startswith("self-test result:")]
fails = 0
if result_line and "fails=0" in result_line[-1]:
    print("check_stale.py --self-test: PASS (comment-clock stale detector)")
    print(out)
    sys.exit(0)

print("  FAIL: check_stale.py --self-test did not pass for zero failures")
print(out)
sys.exit(1)
