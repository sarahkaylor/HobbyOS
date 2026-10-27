#!/usr/bin/env python3
"""GB-114-V probes: clause 3 (ambiguity) and clause 4 (one helper, false positives).
Read-only: no board or GitHub writes."""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, "..", "..", "tools", "board")
sys.path.insert(0, BOARD)
import groom  # noqa: E402
import merge_pr  # noqa: E402

print("== clause 3: ticket_from(branch, title, body) ==")
cases = [
    ("games/gb112 + title GB-112", ("games/gb112", "board: GB-112 fixed", "")),
    ("games/gb112 + title GB-113", ("games/gb112", "GB-113 the wait word", "")),
    ("ticket/222-waitstatus + title GB-113 (PR #222's real shape)",
     ("ticket/222-waitstatus", "abi(t3): GB-113 — quote the raw wait status word", "")),
    ("no id anywhere", ("fix/typo", "spelling in README", "")),
]
for name, args in cases:
    print(f"  {str(merge_pr.ticket_from(*args)):12} <- {name}")

print("\n== clause 4: verdict_in on non-verdict bodies ==")
for name, body in [
    ("Greenfield", "Greenfield is not a verdict"),
    ("Green eggs and ham (line starts with Green)", "Green eggs and ham"),
    ("Green, and then some.", "Green, and then some."),
    ("RED herring", "RED herring"),
    ("mid-sentence", "the build is green today"),
    ("real GB-113-V opening", "**GREEN — GB-113-V, verified on this VM, code PR #222 @ `d21759c`.**"),
]:
    print(f"  {str(groom.verdict_in(body)):6} <- {name}")

print("\n== clause 4: one helper, three call sites ==")
for mod in ("merge_pr", "check_verdicts", "groom"):
    path = os.path.join(BOARD, mod + ".py")
    hits = [f"{i}:{l.strip()}" for i, l in enumerate(open(path).read().splitlines(), 1)
            if ("verdict_in" in l and "def verdict_in" not in l)]
    print(f"  {mod}.py: {hits[:3]}")

print("\n== clause 6: graded_sha_from_board on an unresolvable PR ==")
print("  pr_gb_id(9999) =", merge_pr.pr_gb_id(9999))
print("  graded_sha_from_board(9999) =", merge_pr.graded_sha_from_board(9999))