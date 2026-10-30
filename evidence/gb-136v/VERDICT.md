# GB-136-V — gate PR #259 at 721650e1 (verifier run, 2026-10-30)

Graded head **`721650e1`** (`hermes/winf-gb135` — note the branch name in the card,
`ticket/check-verdicts-supersession`, does not exist on the remote; the PR's head ref is
`hermes/winf-gb135`, which is where I fetched it from). Base `177c4b7`, 2 files, +188/−35.

## Clause 1 — self-tests

```
$ python3 tools/board/check_verdicts.py --self-test
  [OK  ] GB-136: earlier RED superseded by a later GREEN -> not a mismatch; governing=GB-117-V5=GREEN
  [OK  ] GB-136: RED with no successor is still reported (governing=GB-121-V=RED)
  [OK  ] GB-136: supersession is load-bearing (a re-verified RED would otherwise misfire)
self-test result: fails=0

$ python3 tools/board/check_verdicts_test.py
  PASS: GB-136: earlier RED superseded by a later GREEN -> not a mismatch (governing=GB-117-V5=GREEN, successor named)
  PASS: GB-136: is_superseded marks the earlier -V as overtren
  PASS: GB-136: RED with no successor is still reported (governing=GB-121-V=RED)
  PASS: GB-136: a GREEN on the parent (ordinal 0) supersedes an earlier gate RED (governing=GB-120=GREEN)
  PASS: GB-136: naive reading of a lone GB-035-V RED is the load-bearing misfire this rule stops
check_verdicts_test: ALL PASSED
CHECK_VERDICTS_GB136: ALL PASSED
```

Both directions and the parent-GREEN (ordinal 0) case are present and green.

## Clause 2 — the supersession rule provably fails

Neutered `is_superseded` (`return o1 < o2` → `return False`), on the graded head:

```
$ python3 tools/board/check_verdicts.py --self-test
  [FAIL] GB-136: earlier RED superseded by a later GREEN -> not a mismatch; governing=GB-117-V5=GREEN
  [OK  ] GB-136: RED with no successor is still reported (governing=GB-121-V=RED)
  [FAIL] GB-136: supersession is load-bearing (a re-verified RED would otherwise misfire)
self-test result: fails=1                                   exit=1

$ python3 tools/board/check_verdicts_test.py
  FAIL: GB-136: is_superseded marks the earlier -V as overtren
  FAIL: GB-136: ALL PASSED -> CHECK_VERDICTS_GB136: ALL PASSED
check_verdicts_test: FAILED                                 exit=1
```

Restored (`cp /tmp/cv_orig.py tools/board/check_verdicts.py`, `git status --short` clean), and
both went back to `fails=0` / `ALL PASSED`. So the rule is load-bearing, not decoration.

**Defect found here (see "required fix" below):** the sabotage prints **two** `[FAIL]` lines
but the summary says **`fails=1`**.

## Clause 3 — the live board

```
$ python3 tools/board/check_verdicts.py     (BOARD_REPO=sarah/gaming-program, FORGEJO_TOKEN set)
verdicts checked: 65, mismatched parents: 3, superseded REDs: 58, orphaned REDs in Merge Gate: 0
exit=1
```

All nine named parents resolve, each with its successor:

```
SUPERSEDED GB-035-V2#218 = RED, overtaken by GB-035 (successor sha ee98d31) for GB-035#35
SUPERSEDED GB-035-V#109  = RED, overtaken by GB-035 (successor sha None)    for GB-035#35
SUPERSEDED GB-060-V#170  = RED, overtaken by GB-060 (successor sha 9db1388) for GB-060#62
SUPERSEDED GB-066-V2#195 = RED, overtaken by GB-066 (successor sha fbd3cf3) for GB-066#74
SUPERSEDED GB-066-V#96   = RED, overtaken by GB-066 (successor sha None)    for GB-066#74
SUPERSEDED GB-071-V#284  = RED, overtaken by GB-071 (successor sha None)    for GB-071#99
SUPERSEDED GB-074-V#110  = RED, overtaken by GB-074 (successor sha 6636741) for GB-074#100
SUPERSEDED GB-106-V#202  = RED, overtaken by GB-106 (successor sha 68982dc) for GB-106#200
SUPERSEDED GB-110-V#212..#221 (5 cards) = RED, overtaken by GB-110 (sha 04bc956/0633743/937a517)
SUPERSEDED GB-117-V#239 / -V2#240 = RED, overtaken by GB-117 (successor sha None)
SUPERSEDED GB-120-V#262 / #263    = RED, overtaken by GB-120 (sha None / 2271d1c)
```

**0 of 9 remain mismatched.** The three that remain are genuine lane errors, reported and
not suppressed:

```
MISMATCH GB-024-V = RED but GB-024#25 is in 'In Review' (want 'In Progress')
MISMATCH GB-101   = GREEN but GB-101#179 is in 'Backlog' (want 'Merge Gate')
MISMATCH GB-136   = GREEN but GB-136#292 is in 'In Review' (want 'Merge Gate')
```

- **GB-024-V** — a standing RED with no successor gate, card in `In Review`: exactly the
  §4.2 case (RED → In Progress). Not a supersession miss.
- **GB-101** — a GREEN whose card sits in `Backlog`. No later gate overtakes it; a real
  lane error, and the commit message names it as staying reported.
- **GB-136** — this card's own parent: it reads GREEN because a **review comment** on #292
  carries the verdict; the card has not yet been moved. My verdict is what moves it, so this
  line should disappear on the next run.

## Clause 4 — host_tests, lane semantics

```
[host-tests] done: 35/35 passed                       HOST_EXIT=0
  PASS  tools/board/check_verdicts_test.py
```

Lane semantics unchanged — from the self-test:

```
  [OK  ] verdict=RED   parent in 'In Review'   -> ok=False (expected False, want-lane 'In Progress')
  [OK  ] verdict=RED   parent in 'In Progress' -> ok=True  (expected True,  want-lane 'In Progress')
  [OK  ] verdict=GREEN parent in 'In Review'   -> ok=False (expected False, want-lane 'Merge Gate')
```

## Clause 5 — builds

```
INTEL_RC=0
ARM_RC=0
make ARCH=intel MODE=unit_tests hobbyos.elf   UNIT_RC=0
```

## Required fix before merge (not a re-gate)

`self_test()` prints the `dir1` verdict a second time as a separate "supersession is
load-bearing" line **without incrementing `bad`**, and assigns an unused `ok_load`:

```python
    if dir1:
        ok_load = dir1
    print(f"  [{'OK  ' if dir1 else 'FAIL'}] GB-136: supersession is load-bearing ...")

    print(f"self-test result: fails={bad}")
```

So under sabotage the output has two `[FAIL]` lines and a summary of `fails=1`. The count and
the marks disagree — the same class of defect the card was bounced for, in a smaller place.
It does not weaken the rule (both directions do fail correctly), but a self-test whose summary
undercounts its own failures is the kind of instrument this program keeps re-learning not to
trust. Delete the duplicate line or count it.