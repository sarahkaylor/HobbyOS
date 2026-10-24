#!/usr/bin/env python3
"""Host (T1) self-test for perf_report aggregation (GB-045)."""
import sys, os, tempfile, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "t5"))
from perf_report import load_jsonl, load_cycle_runs

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

d = tempfile.mkdtemp()
bj = os.path.join(d, "evidence", "perf", "boot_times.jsonl")
os.makedirs(os.path.dirname(bj))
open(bj, "w").write("\n".join([
    json.dumps({"arch": "arm", "boot_s": 13.03}),
    json.dumps({"arch": "arm", "boot_s": 12.53}),
    json.dumps({"arch": "arm", "boot_s": 12.53, "extra": True}),  # unknown key tolerated
]) + "\n")
boots = load_jsonl(bj)
check(len(boots) == 3, "loads all boot samples")
vals = [b["boot_s"] for b in boots if b.get("arch") == "arm"]
check(round(sum(vals) / len(vals), 2) == 12.70, "mean of [13.03,12.53,12.53] == 12.70")

# cycle aggregation with a freeze + growth to test PASS/FAIL plumbing
croot = os.path.join(d, "evidence", "perf_cycle", "ts")
os.makedirs(croot)
json.dump({"results": [
    {"game": "A.BIN", "cycle": 0, "frozen": False, "growth_mb": 0.27},
    {"game": "B.BIN", "cycle": 1, "frozen": True, "growth_mb": 0.1},
]}, open(os.path.join(croot, "report.json"), "w"))
rows = load_cycle_runs(d)
check(len(rows) == 2, "collects cycle rows")
check(any(fr for (_, _, fr, _) in rows), "detects a freeze for the FAIL path")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
