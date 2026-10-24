#!/usr/bin/env python3
"""Host (T1) self-test for evidence_pack aggregation (GB-047)."""
import sys, os, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "t5"))
from evidence_pack import sha256, find_reports, resolve_marker

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

d = tempfile.mkdtemp()
os.makedirs(os.path.join(d, "tools", "t5", "replays"), exist_ok=True)
# a fake evidence tree
open(os.path.join(d, "GAME-EVIDENCE-REPORT.md"), "w").write("x" * 100)
open(os.path.join(d, "tools", "evidence_rig.py"), "w").write("rig" * 10)
open(os.path.join(d, "tools", "t5", "replays", "xnibbler.jsonl"), "w").write("{}")

check(sha256(os.path.join(d, "GAME-EVIDENCE-REPORT.md")) == sha256(os.path.join(d, "GAME-EVIDENCE-REPORT.md")),
      "sha256 deterministic")
rep = find_reports(d)
check(any("GAME-EVIDENCE-REPORT.md" in r for r in rep), "find_reports picks up EVIDENCE-REPORT")
hz = resolve_marker(d, "evidence rig", "tools/evidence_rig.py")
check(len(hz) == 1 and hz[0][0].endswith("evidence_rig.py"), "resolve_marker finds rig")
hzr = resolve_marker(d, "replays", "tools/t5/replays/*.jsonl")
check(len(hzr) == 1 and "xnibbler" in hzr[0][0], "resolve_marker globs replay scripts")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
