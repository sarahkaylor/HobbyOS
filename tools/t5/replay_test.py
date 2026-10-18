#!/usr/bin/env python3
"""Host (T1) self-test for replay.py's ReplayParser + replay scripts (GB-043)."""
import sys, os, json, glob
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "t5"))
from replay import ReplayParser

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

# every authored replay must parse and validate, and must launch a game
# (nav events: click + home + downs + ret)
rep_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "replays")
replays = sorted(glob.glob(os.path.join(rep_dir, "*.jsonl")))
check(len(replays) >= 5, f"{len(replays)} per-game replay scripts present (>=5 games)")

for rp in replays:
    try:
        p = ReplayParser(rp)
        n = p.validate()
        ev = list(p)
        has_click = any(e["t"] == "click" for e in ev)
        has_launch = any(e["t"] == "key" and e["k"] == "ret" for e in ev)
        check(n >= 20 and has_click and has_launch,
              os.path.basename(rp) + f" parses ({n} events), navigates + launches")
    except Exception as ex:
        check(False, os.path.basename(rp) + f" FAILED: {ex}")

# schema errors must raise
bad = os.path.join(rep_dir, "_bad_tmp.jsonl")
open(bad, "w").write(json.dumps({"t": "teleport"}) + "\n")
try:
    ReplayParser(bad).validate()
    check(False, "unknown event type rejected")
except ValueError:
    check(True, "unknown event type rejected")
os.remove(bad)

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
