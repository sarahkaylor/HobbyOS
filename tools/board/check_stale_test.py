#!/usr/bin/env python3
"""check_stale_test.py - GB-098 host (T1) test for tools/board/check_stale.py.

INDEPENDENT assertions (the GB-080/GB-007 discipline): this test does NOT just
run the tool's own --self-test and trust its self-reported count.  It drives
the module directly with a case table that lives IN THE TEST, so a whole-file
revert to origin/main's pre-GB-098 check_stale.py (dropped in place of this
branch's) makes the test exit non-zero.

What GB-098 does: the stale clock is the last NON-COORDINATOR COMMENT only.
Every lane move is itself a comment, so the comment's created_at captures both
real activity and every lane transition.  The issue's raw `updated_at` is
deliberately never used -- a board-wide sync (import_seed / sync_gaming_md /
a label sweep) bumps `updated_at` on every issue at once, so a fresh
`updated_at` must NOT re-arm a genuinely parked card.

The headline case is exactly the one that failed before GB-098:
    In Progress, 40h-old last comment, but `updated_at` was just bulk-touched
    -> must be reported STALE (the pre-fix tool said NOT stale because it
       gated on a fresh `updated_at`, silently clearing genuinely parked
       cards).  The verifier measured 3 vs 37 on this.
"""
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "check_stale.py")

# The tool's --self-test must keep its full GB-098 case table.  origin/main's
# pre-GB-098 tool has only the 6 case-clock cases (it reports them as
# fails=0 with a fresh updated_at), so an exact-count check catches a
# whole-file revert cleanly.
EXPECTED_SELFTEST_CASES = 9

fails = 0


def bad(msg):
    global fails
    fails += 1
    print("  FAIL: " + msg)


# --- 1. tool --self-test must run clean AND keep its full case table ------
r = subprocess.run([sys.executable, TOOL, "--self-test"],
                   capture_output=True, text=True)
out = (r.stdout or "") + (r.stderr or "")
for line in out.splitlines():
    if line.startswith("  "):
        print(line)
if "self-test result: fails=0" not in out:
    bad("tool --self-test did not report fails=0")
cases = sum(1 for l in out.splitlines() if "[OK" in l or "[FAIL" in l)
if cases != EXPECTED_SELFTEST_CASES:
    bad(f"tool --self-test case table shrank to {cases} "
        f"(expected {EXPECTED_SELFTEST_CASES} -- the GB-098 comment-clock cases); "
        f"a pre-GB-098 tool was dropped in?")

# --- 2. independent case table driving classify() directly ----------------
spec = importlib.util.spec_from_file_location("check_stale", TOOL)
if spec is None or spec.loader is None:
    bad(f"cannot load module from {TOOL}")
    print(f"CHECK_STALE_TEST: FAIL (module load)")
    sys.exit(1)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
HOURS = mod.DEFAULT_HOURS

# (lane, comment_age_h, expected_stale).  The stale clock is the COMMENT only;
# these cases are authored in the TEST, so the tool cannot assert itself green.
INDEPENDENT = [
    # old last comment + bulk-touched updated_at -> STALE (the GB-098 case)
    ("In Progress", 40.0, True),
    # fresh comment clears the card, whatever updated_at says
    ("In Progress", 0.5, False),
    ("Ready", 99.0, False),
    ("In Review", 99.0, False),
    ("Icebox", 99.0, False),
]
for lane, comment_age, want in INDEPENDENT:
    try:
        got = mod.classify(lane, comment_age, HOURS)
        if got == want:
            print(f"  [OK  ] lane={lane:<11} comment_age={comment_age:<5} "
                  f"-> stale={got} (want {want})")
        else:
            bad(f"classify({lane!r}, {comment_age}, {HOURS}) = {got}, want {want}")
    except TypeError as e:
        # origin/main's pre-GB-098 classify takes (lane, comment_age, issue_age,
        # hours) -- the old updated_at clock -- so this call fails loudly when a
        # pre-fix tool is dropped in.
        bad(f"classify({lane!r}, {comment_age}, {HOURS}) raised TypeError: {e} "
            "-- pre-GB-098 (updated_at) signature? old tool dropped in?")

# --- verdict ----------------------------------------------------------------
if fails:
    print(f"CHECK_STALE_TEST: FAIL ({fails} failure(s))")
    sys.exit(1)
print("CHECK_STALE_TEST: PASS -- comment-only stale clock, independent case table green")
sys.exit(0)
