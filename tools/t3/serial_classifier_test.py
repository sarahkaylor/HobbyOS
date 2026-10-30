#!/usr/bin/env python3
"""serial_classifier_test.py - GB-107 T1: classifies full / mid-cut / early-death logs
correctly and FAILS when the classifier breaks (a flipped branch must be caught)."""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib
import serial_classifier as sc

fails = 0
def check(cond, msg):
    global fails
    print("  PASS: " + msg if cond else "  FAIL: " + msg)
    if not cond:
        fails += 1

EXPECTED = 43  # the t4_device probe emits 43 row= lines in a straight-line main()

# 1. full log: end marker present -> FULL (blocked writing, not cut)
full = os.path.join(tempfile.mkdtemp(), "full.log")
with open(full, "w") as fh:
    fh.write("boot\n" * 30 + "row=%d\n" * 0 + "System halt\n")
rec = sc.capture_record(full, guest_alive_at_cutoff=False, expected_lines=EXPECTED)
check(rec["verdict"] == sc.FULL, "full log (end marker) -> FULL (got %s)" % rec["verdict"])

# 2. mid-cut: no end marker, GUEST ALIVE at cutoff, few lines -> TRUNCATED (distinct)
mid = os.path.join(tempfile.mkdtemp(), "midcut.log")
with open(mid, "w") as fh:
    fh.write("t4-probe\n" + "row=write3 rc=3 errno=0 out=68690a\n" * 17)  # 17 of 43 captured
rec = sc.capture_record(mid, guest_alive_at_cutoff=True, expected_lines=EXPECTED)
check(rec["verdict"] == sc.TRUNCATED, "mid-cut log (guest alive, no end marker) -> TRUNCATED (got %s)" % rec["verdict"])
check(sc.TRUNCATED != sc.GUEST_STOPPED, "TRUNCATED is a DISTINCT verdict from GUEST_STOPPED")

# 3. early death: no end marker, guest NOT alive -> GUEST_STOPPED (not cut)
dead = os.path.join(tempfile.mkdtemp(), "dead.log")
with open(dead, "w") as fh:
    fh.write("t4-probe\n" + "row=write3 rc=3 errno=0 out=68690a\n" * 17)
rec = sc.capture_record(dead, guest_alive_at_cutoff=False, expected_lines=EXPECTED)
check(rec["verdict"] == sc.GUEST_STOPPED, "early-death log (guest not alive) -> GUEST_STOPPED (got %s)" % rec["verdict"])

# record carries the four DoD facts
r = sc.capture_record(mid, guest_alive_at_cutoff=True, expected_lines=EXPECTED)
for k in ("lines", "bytes", "guest_alive_at_cutoff", "end_marker"):
    check(k in r, "monitored-run record carries %s (%s)" % (k, sc.record_line(r)))

# 4. the classifier FAILS when broken: flip the TRUNCATED branch -> mid-cut misclassified
mut = sc.classify
def broken(lines, alive, end, exp=None):
    # broken: a live guest + no end marker now yields GUEST_STOPPED (wrong)
    if end:
        return sc.FULL, "complete"
    return sc.GUEST_STOPPED, "WRONG: classified a live cut as stopped"
import serial_classifier as _s
_s.classify = broken
from serial_classifier import TRUNCATED as _T, GUEST_STOPPED as _G
wrong = _s.capture_record(mid, guest_alive_at_cutoff=True, expected_lines=EXPECTED)
_s.classify = sc.classify
broken_caught = wrong["verdict"] != sc.TRUNCATED
check(broken_caught, "a broken classifier (live-cut -> GUEST_STOPPED) FAILS the mid-cut case (got %s)" % wrong["verdict"])

if fails:
    print("SERIAL_CLASSIFIER_TEST: FAIL (%d)" % fails); sys.exit(1)
print("SERIAL_CLASSIFIER_TEST: PASS - full/mid-cut/early-death classified; broken classifier caught")
sys.exit(0)
