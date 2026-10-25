#!/usr/bin/env python3
"""groom_test.py — GB-049 grooming bot unit test.

Proves each grooming check CAN fail and, when fed a clean board, stays quiet —
the honesty discipline the board demands (§4.2: no pass without a check that can
provably fail).  Exercises the PURE core of tools/board/groom.py (no network):
the blockers deadlock rule, the verify-lane lint (unverified-in-review /
stale-verify / orphan-verify), and the quantitative-DoD Done hard stop.

Self-registering: the GB-064 host suite (tools/run_t5_tests.py) auto-discovers
tools/*/*_test.py, so no Makefile edit is needed.

Run:  python3 tools/board/groom_test.py   (the host suite runs it too)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groom  # noqa: E402

checks = 0
failed = 0


def check(ok, name):
    global checks, failed
    checks += 1
    print(("  PASS: " if ok else "  FAIL: ") + name)
    if not ok:
        failed += 1


# ---- blockers lint (deadlock rule) -----------------------------------------
acyclic = [
    {"title": "GB-001 a", "body": "| Depends on | GB-002 |"},
    {"title": "GB-002 b", "body": "| Depends on | — |"},
    {"title": "GB-003 c", "body": "| Depends on | |"},
]
check(groom.dependency_cycles(acyclic) == [],
      "acyclic dependencies find no cycle")

cyc = [
    {"title": "GB-010 x", "body": "| Depends on | GB-011 |"},
    {"title": "GB-011 y", "body": "| Depends on | GB-010 |"},
]
c = groom.dependency_cycles(cyc)
check(len(c) == 1 and set(c[0][:-1]) == {"GB-010", "GB-011"},
      "two-ticket dependency cycle is detected")
check(len(c[0]) == 3 and c[0][0] == c[0][-1],
      "reported cycle is closed (A -> B -> A)")

check(groom.dependency_cycles(
    [{"title": "GB-020 p", "body": "| Depends on | GB-021 |"}]) == [],
    "an edge to a ticket outside the open set cannot close a cycle")

# ---- dependency field parsing (no guessing on a board that lies by omission) -
check(groom.expand_dep_field("GB-044/45/46") == {"GB-044", "GB-045", "GB-046"},
      "compact shorthand GB-044/45/46 expands to three ids")
check(groom.expand_dep_field("GB-003/004") == {"GB-003", "GB-004"},
      "compact shorthand GB-003/004 expands to two ids")
check(groom.expand_dep_field("GB-077, GB-060") == {"GB-077", "GB-060"},
      "comma-separated full ids parse")
check(groom.deps_of("| Depends on | turtles |") == set(),
      "a non-GB dependency token is ignored, not guessed")

# ---- verify-lane lint (PLAN-006) -------------------------------------------
def _cm(d): return lambda n: d.get(n, [])

good = [
    {"title": "GB-030 impl", "state": "open",
     "labels": [{"name": "In Review"}], "number": 1},
    {"title": "GB-030-V gate", "state": "open",
     "labels": [{"name": "Ready"}], "number": 2},
]
check(groom.verify_lint_findings(good, _cm({2: [{"body": "VERDICT: GREEN"}]})) == [],
      "a review card with a GREEN gate is not flagged unverified")

bad = [
    {"title": "GB-031 impl", "state": "open",
     "labels": [{"name": "In Review"}], "number": 3},
    {"title": "GB-031-V gate", "state": "open",
     "labels": [{"name": "Ready"}], "number": 4},
]
f = groom.verify_lint_findings(bad, _cm({4: [{"body": "started the run"}]}))
check(any(k == "unverified-in-review" and g == "GB-031" for k, g, _ in f),
      "a review-lane card whose gate has no GREEN is flagged unverified-in-review")

orphan = [
    {"title": "GB-040-V gate", "state": "open",
     "labels": [{"name": "Ready"}], "number": 5},
]
f = groom.verify_lint_findings(orphan, _cm({}))
check(any(k == "orphan-verify" and g == "GB-040-V" for k, g, _ in f),
      "a -V with no parent on the board is flagged orphan-verify")

stale = [
    {"title": "GB-041 impl", "state": "closed",
     "labels": [{"name": "Done"}], "number": 6},
    {"title": "GB-041-V gate", "state": "open",
     "labels": [{"name": "Ready"}], "number": 7},
]
f = groom.verify_lint_findings(stale, _cm({}))
check(any(k == "stale-verify" and g == "GB-041-V" for k, g, _ in f),
      "an open -V whose parent is closed is flagged stale-verify")

# ---- quantitative-DoD Done hard stop (2026-10-19) ---------------------------
check(groom.quant_dod_ok('verdict prints "0 user memory faults"', []) is False,
      "numeric-threshold DoD without EVIDENCE is refused a Done close (GB-068 shape)")
check(groom.quant_dod_ok(
    'verdict prints "0 user memory faults"',
    ["**EVIDENCE — ** 0 user memory faults on a full wave."]) is True,
    "the same DoD is allowed once an EVIDENCE comment quotes 0")
check(groom.quant_dod_ok("docs pass human read", []) is True,
      "a descriptive (non-numeric) DoD is not gated")

print()
if failed:
    print(f"GROOM_TEST: FAIL — {failed} of {checks} checks failed")
    sys.exit(1)
print(f"GROOM_TEST: PASS — {checks} checks, all can fail and all held")
sys.exit(0)
