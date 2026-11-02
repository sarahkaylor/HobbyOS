#!/usr/bin/env python3
"""Host (T1) self-test for replay.py's ReplayParser + replay scripts (GB-043)."""
import sys, os, json, glob, tempfile, shutil
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
        # GB-116: F1 opens the Apps menu on both arches; a click cannot land on
        # intel (no pointer device), so a replay that opens the menu with `f1`
        # is as valid a nav as one that clicks.
        has_f1 = any(e["t"] == "key" and e["k"] == "f1" for e in ev)
        has_launch = any(e["t"] == "key" and e["k"] == "ret" for e in ev)
        check(n >= 20 and (has_click or has_f1) and has_launch,
              os.path.basename(rp) + f" parses ({n} events), navigates + launches")
    except Exception as ex:
        check(False, os.path.basename(rp) + f" FAILED: {ex}")

# schema errors must raise (GB-085: per-process fixture -- two concurrent
# `make host_tests` must not share a path, or one unlinks _bad.jsonl while the
# other is parsing it and the loser throws FileNotFoundError)
_bad_dir = tempfile.mkdtemp(prefix="gb085_replay_")
try:
    bad = os.path.join(_bad_dir, "_bad.jsonl")
    open(bad, "w").write(json.dumps({"t": "teleport"}) + "\n")
    try:
        ReplayParser(bad).validate()
        check(False, "unknown event type rejected")
    except ValueError:
        check(True, "unknown event type rejected")
finally:
    shutil.rmtree(_bad_dir, ignore_errors=True)

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
