"""GB-005-V2 clause 3: merge_pr.decide() refuses without a GREEN naming the head.

Run by the verifier, on the graded head's own code (no mocks): the three cases the
DoD names, plus the two boundary cases that decide whether the rule is real.
"""
import sys

sys.path.insert(0, "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb005v2/tools/board")
import merge_pr  # noqa: E402

cases = [
    ("no graded GREEN ref at all", "c08913fdeadbeef", None),
    ("GREEN naming a DIFFERENT head", "c08913fdeadbeef", "7e78ff2"),
    ("GREEN naming the head (allow arm)", "7e78ff2aabbcc", "7e78ff2"),
    ("graded sha shorter than 7", "7e78ff2aabbcc", "7e78"),
    ("no head at all (unreachable API)", None, "7e78ff2"),
]
for name, head, graded in cases:
    ok, reason = merge_pr.decide(head, graded)
    print(f"[{'ALLOW ' if ok else 'REFUSE'}] {name}")
    print(f"         head={head!r} graded={graded!r}")
    print(f"         {reason}")