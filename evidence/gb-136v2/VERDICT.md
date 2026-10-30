# GB-136-V (#301) — board-level gate at head 721650e1 (verifier run, 2026-10-30)

Everything below is from this VM, with the tree named for each number.

- **before**: worktree at `origin/main` = **`c530864`** (old tool, no `is_superseded`)
- **after**: worktree at the graded head **`721650e1`** (PR #259)

## Clause 3 — before / after

```
BEFORE  main c530864 : verdicts checked: 62, mismatched parents: 10, orphaned REDs in Merge Gate: 0
AFTER   head 721650e1: verdicts checked: 66, mismatched parents:  1, superseded REDs: 60, orphaned REDs: 0
```

The ten on `main`, each accounted for by successor + sha (from the AFTER run):

```
SUPERSEDED GB-005-V#80  = RED, overtaken by GB-005 (successor sha None)    for GB-005#10
SUPERSEDED GB-035-V#109 = RED, overtaken by GB-035 (successor sha None)    for GB-035#35
SUPERSEDED GB-035-V2#218= RED, overtaken by GB-035 (successor sha ee98d31) for GB-035#35
SUPERSEDED GB-060-V#170 = RED, overtaken by GB-060 (successor sha 9db1388) for GB-060#62
SUPERSEDED GB-066-V#96  = RED, overtaken by GB-066 (successor sha None)    for GB-066#74
SUPERSEDED GB-066-V2#195= RED, overtaken by GB-066 (successor sha fbd3cf3) for GB-066#74
SUPERSEDED GB-071-V#284 = RED, overtaken by GB-071 (successor sha None)    for GB-071#99
SUPERSEDED GB-074-V#110 = RED, overtaken by GB-074 (successor sha 6636741) for GB-074#100
SUPERSEDED GB-106-V#202 = RED, overtaken by GB-106 (successor sha 68982dc) for GB-106#200
SUPERSEDED GB-110-V#212..#221 (5) = RED, overtaken by GB-110 (sha 04bc956/0633743/937a517) for GB-110#211
SUPERSEDED GB-117-V#239 / -V2#240 = RED, overtaken by GB-117 (successor sha None) for GB-117#238
SUPERSEDED GB-120-V#262 / #263    = RED, overtaken by GB-120 (sha None / 2271d1c) for GB-120#261
```

**0 of 10 remain mismatched.** The single remaining mismatch is a genuine lane error, reported:

```
MISMATCH GB-101 = GREEN but GB-101#179 is in 'Backlog' (want 'Merge Gate')
```

## Clause 1 — successor named, not silently suppressed

Every superseded line carries the successor's id **and** its sha where the board has one
(`ee98d31`, `9db1388`, `fbd3cf3`, `6636741`, `68982dc`, `937a517`, `2271d1c`). Where the parent
carries the GREEN itself, the successor is the parent and the sha field is `None` — named, not hidden.

## Clause 2 — the rule can be turned off, and the ten come back

**Knob 1 (insufficient, and worth recording):** neutering only `is_superseded`
(`return o1 < o2` → `return False`) leaves the board reading unchanged —

```
verdicts checked: 66, mismatched parents: 2, superseded REDs: 60
```

because the suppression lives in `governing()`'s highest-ordinal / parent-GREEN resolution, not in
`is_superseded` alone. A falsifier aimed at `is_superseded` alone proves only that the unit test
notices; it does not move the board.

**Knob 2 (the real demolition):** `governing()` made naive — any RED on any gate card governs:

```
verdicts checked: 66, mismatched parents: 22, superseded REDs: 36

MISMATCH GB-005-V2 = RED but GB-005#10  is in 'Done' (want 'In Progress')
MISMATCH GB-035-V2 = RED but GB-035#35  is in 'Done' (want 'In Progress')
MISMATCH GB-060-V  = RED but GB-060#62  is in 'Done' (want 'In Progress')
MISMATCH GB-066-V2 = RED but GB-066#74  is in 'Done' (want 'In Progress')
MISMATCH GB-071-V  = RED but GB-071#99  is in 'Done' (want 'In Progress')
MISMATCH GB-074-V  = RED but GB-074#100 is in 'Done' (want 'In Progress')
MISMATCH GB-106-V  = RED but GB-106#200 is in 'Done' (want 'In Progress')
MISMATCH GB-110-V4 = RED but GB-110#211 is in 'Done' (want 'In Progress')
MISMATCH GB-117-V2 = RED but GB-117#238 is in 'Done' (want 'In Progress')
MISMATCH GB-120-V  = RED but GB-120#261 is in 'Done' (want 'In Progress')
```

All ten return, plus twelve more that the rule legitimately suppresses. Restored from the
committed file; `git status --short tools/board/` clean; clean run back to 1 mismatch.

**An unsuperseded RED is still reported** — measured on this head at 15:41Z, before the board
moved:

```
MISMATCH GB-024-V = RED but GB-024#25 is in 'In Review' (want 'In Progress')
```

and the unit-level direction is in the self-test: `RED with no successor is still reported
(governing=GB-121-V=RED)`.

## Clause 4 — lane semantics unchanged

```
  [OK  ] verdict=RED   parent in 'In Review'   -> ok=False (expected False, want-lane 'In Progress')
  [OK  ] verdict=RED   parent in 'In Progress' -> ok=True  (expected True,  want-lane 'In Progress')
  [OK  ] verdict=GREEN parent in 'In Review'   -> ok=False (expected False, want-lane 'Merge Gate')
  [OK  ] verdict=RED   parent in 'Done'        -> ok=False (expected False, want-lane 'In Progress')
self-test result: fails=0
```

## Clause 5 — host_tests, self-registered

```
[host-tests] done: 35/35 passed
  PASS  tools/board/check_verdicts_test.py
```

Discovered by `tools/t5/run_t5_tests.py`'s `tools/*/*_test.py` glob — no Makefile line was added.
The test's own failure case is the falsifier above (`check_verdicts_test: FAILED` with the rule
neutered).

## Defect found — the verdict parser reads prose as a verdict (and I tripped it myself)

At 16:2xZ the live board reported:

```
MISMATCH GB-136-V = RED but GB-136#292 is in 'Merge Gate' (want 'In Progress')
```

for a card whose newest real verdict is GREEN. Cause, by applying the tool's own regex
(`VERDICT[^\n]{0,40}?\b(GREEN|RED)\b`, case-insensitive) to the comments:

```
#292 c17988 -> RED    match: 'verdicts supersession (RED'
#292 c18302 -> GREEN  match: 'verdict, except a **parent-GREEN'
#292 c18343 -> RED    match: 'verdict=RED'
#292 c18343 -> GREEN  match: 'verdict=GREEN'
#292 c18343 -> GREEN  match: "verdict_in` next: GB-136's governing GREEN"
```

`c18343` is **my own #300 verdict comment** — it quotes the self-test transcript, and
`verdict=RED` from a quoted `[OK ] verdict=RED parent in 'In Review'` line is read as the card's
verdict. The first match wins, so a GREEN evidence comment became a RED.

I am reporting it rather than quietly dropping it because of the blast radius: `GB-005`'s Merge
Gate enforcement consumes this parser, so "pasting a test transcript into a verdict comment can
flip a card's governing verdict" is a merge-adjacent defect, not a cosmetic one. It reads clean
again on the current board (the newest comment wins), which makes it worse, not better — the
failure is order-dependent.

Suggested hardening: anchor on a line-start marker (`^VERDICT:`), and ignore fenced code blocks.
Not a re-gate: this card's DoD is supersession, and supersession works.