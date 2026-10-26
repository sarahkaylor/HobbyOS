#!/usr/bin/env python3
"""t3_three_outcome_test.py - GB-103 DoD clause 3: the three-outcome judgement.

The instrument-failure status must be a DISTINCT outcome, distinguishable from a
tier PASS and a tier FAIL.  clean -> EXIT 0 PASS; genuine tier red -> EXIT 1 FAIL
(FAIL line quoted); instrument failure -> EXIT 4 INSTRUMENT-FAILURE (forced, so the
distinction is real, not "the tier just went red").
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
T3 = os.path.join(HERE, "..", "t3_tier.sh")

fails = 0
def check(cond, msg):
    global fails
    if cond: print("  PASS:", msg)
    else: fails += 1; print("  FAIL:", msg)

def clean_log():
    f = os.path.join(tempfile.gettempdir(), "t3_three_%d.log" % os.getpid())
    with open(f, "w") as fh:
        for n in range(1, 61):
            fh.write("Process %d: APP%d.BIN exited\n" % (n, n))
        i = 0
        while os.path.getsize(f) < 1 or i < 1200:
            fh.write("benign console chatter line %d\n" % i); i += 1
        fh.write("System halt\n")
    return f

env = dict(os.environ); env["W0_GATE"] = "0"
# 1. clean -> PASS (EXIT 0)
f = clean_log()
r = subprocess.run(["bash", T3, "police", f], capture_output=True, text=True, env=env)
check(r.returncode == 0 and "PASS" in r.stdout, "clean run -> EXIT 0 PASS")

# 2. genuine tier red -> FAIL (EXIT 1) with the FAIL line
g = f + "_fail"
with open(g, "w") as fh:
    fh.write(open(f).read()); fh.write("SOMETEST: FAIL injected genuine red\n")
r = subprocess.run(["bash", T3, "police", g], capture_output=True, text=True, env=env)
check(r.returncode == 1 and "FAIL" in r.stdout, "genuine tier red -> EXIT 1 FAIL (FAIL line quoted)")

# 3. instrument failure (forced) -> INSTRUMENT-FAILURE (EXIT 4), neither PASS nor FAIL
env["T3_FORCE_INSTRUMENT_FAIL"] = "1"
r = subprocess.run(["bash", T3, "police", f], capture_output=True, text=True, env=env)
check(r.returncode == 4 and "INSTRUMENT-FAILURE" in r.stdout,
      "instrument failure -> EXIT 4 INSTRUMENT-FAILURE (distinct, got rc=%d)" % r.returncode)

if fails:
    print("T3_THREE_OUTCOME_TEST: FAIL (%d)" % fails); sys.exit(1)
print("T3_THREE_OUTCOME_TEST: PASS - clean=PASS, tier-red=FAIL, instrument=INSTRUMENT-FAILURE")
sys.exit(0)
