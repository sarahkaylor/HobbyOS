#!/usr/bin/env python3
"""t3_classifier_coupling_test.py - GB-105: the T3_FORCE_INSTRUMENT_FAIL forced case
must feed the REAL run-path classifier (instrument_failure_evident), never a hardcoded
rc=4.  Locks the coupling structurally: on a classifier-broken copy (remapped so it
always says "no failure" = maps the failure to PASS), a FORCE'd police run must yield
PASS, NOT rc=4.  If a future edit re-hardcodes the forced rc=4, this FAILs."""

import os, subprocess, sys, tempfile


def clean_log():
    f = os.path.join(tempfile.gettempdir(), "t3_coupling_%d.log" % os.getpid())
    with open(f, "w") as fh:
        for n in range(1, 61):
            fh.write("Process %d: APP%d.BIN exited\n" % (n, n))
        i = 0
        while os.path.getsize(f) < 1 or i < 1200:
            fh.write("benign console chatter line %d\n" % i); i += 1
        fh.write("System halt\n")
    return f


HERE = os.path.dirname(os.path.abspath(__file__))
T3 = os.path.join(HERE, "..", "t3_tier.sh")
fails = 0
def check(cond, msg):
    global fails
    if cond: print("  PASS:", msg)
    else: fails += 1; print("  FAIL:", msg)

src = open(T3).read()
broken = src.replace(
    'instrument_failure_evident() {\n  [ "$3" = "1" ] && { [ "$1" != "0" ] || [ ! -S "$2" ]; }\n}',
    'instrument_failure_evident() { return 1; }')
broken_path = "/tmp/t3_tier_broken_gb105.sh"
open(broken_path, "w").write(broken)
env = dict(os.environ); env["T3_FORCE_INSTRUMENT_FAIL"] = "1"; env["W0_GATE"] = "0"
f = clean_log()

r_real = subprocess.run(["bash", T3, "police", f], capture_output=True, text=True, env=env)
r_broken = subprocess.run(["bash", broken_path, "police", f], capture_output=True, text=True, env=env)
check(r_real.returncode == 4, "REAL classifier + FORCE -> rc=4 (got %d)" % r_real.returncode)
check(r_broken.returncode in (0,), "BROKEN classifier + FORCE -> rc=0 PASS, NOT 4 (got %d) — forced case feeds the real classifier" % r_broken.returncode)

if fails:
    print("T3_CLASSIFIER_COUPLING_TEST: FAIL (%d)" % fails); sys.exit(1)
print("T3_CLASSIFIER_COUPLING_TEST: PASS - forced case is coupled to instrument_failure_evident")
sys.exit(0)
