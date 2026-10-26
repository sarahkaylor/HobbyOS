# GB-081-V — gate evidence (VERDICT: CMPTEST deliverable GREEN; the absolute clause is unmet)

Ticket: GB-081-V (#188) — verify ARM `CMPTEST` on `main`
Verifies: GB-081 (#127). Graded tree: this worktree = `origin/main` @ `20084d8` + evidence only.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). I authored neither GB-081 nor the
cmp port. Local gate only — no CI/check result appears in this evidence.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | `make cmp_parity_strict` → `55/55 byte-exact PASS`, exit 0 | **PASS** |
| 2 | ARM `CMPTEST` green at `T3_SMP=1` **and** default | **PASS** — 16/16 both, 0 CMPTEST FAIL lines |
| 3 | the police control fails, then 55/55 restored | **PASS** — reproduced exactly (52/55, 3 FAILED, SEGV 139) |
| 4 | residual ARM FAIL lines attributed one by one | **done below** — none is CMPTEST's |

## Clause 1 PASS
```
$ make cmp_parity_strict
cmp parity: 55/55 byte-exact PASS
PARITY_EXIT=0
```

## Clause 2 PASS — both CPU counts, CMPTEST green
```
T3_SMP=1:  T3_arm: 8548 console lines, 3 FAIL lines, …, halted=1
           [CMPTEST] all 16 cases passed          CMPTEST FAIL lines: 0
default :  T3_arm: 9396 console lines, 1 FAIL lines, …, halted=1
           [CMPTEST] all 16 cases passed          CMPTEST FAIL lines: 0
```
Both runs reached CMPTEST (`[WAVE-TRY] CMPTEST.BIN` … `[ARRIVED] CMPTEST.BIN rc>0` … `Process N:
CMPTEST.BIN exited`), so neither is a "never reached the suite" run. The 1-vs-8 difference the card
worries about does not exist: 16/16 at both counts.

## Clause 3 PASS — the control reproduces the worker's numbers exactly
`control.py` deletes the first `%3o` from the `-b` first-diff printf (`src/user/cmp_gnu.c:645`), rebuilds,
runs the parity suite, then restores the file from git and re-runs:

```
[control] mutant written: first %3o deleted in the -b first-diff printf
case 6 FAIL (differ -b): stdout differs
case 6 FAIL (differ -b): exit status 139 != 1
case 7 FAIL (differ -c): stdout differs
case 7 FAIL (differ -c): exit status 139 != 1
case 14 FAIL (differ binary -b): stdout differs
case 14 FAIL (differ binary -b): exit status 139 != 1
cmp parity: 52/55 passed, 3 FAILED          (exit 2)

[control] file restored from git; diff: (empty)
cmp parity: 55/55 byte-exact PASS           (exit 0)
```
Same three cases, same SEGV 139, same 52/55 as the worker reported. A weaker mutation (keeping the
conversion but dropping its `%3` width) also fails, at 1 case — recorded in `02_control.log`; the card's
"delete the octal" form is the one quoted above.

## Clause 4 — the residual ARM FAILs, attributed line by line

`T3_SMP=1` (3 FAIL lines):
```
1481:  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): FAIL   -> POLLTST, the select-timeout
2234:  1POLLTST FAILED:  check(s)                                       -> POLLTST summary (same suite)
7637: SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST           -> GB-058's watchdog
```
default `T3_SMP=8` (1 FAIL line):
```
7171: SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST           -> GB-058's watchdog
```
None is CMPTEST's: `grep -c 'CMPTEST.*FAIL'` is 0 in both logs, and CMPTEST prints
`all 16 cases passed`.

## The two-part verdict the card asks for

1. **CMPTEST deliverable — GREEN.** Host parity 55/55 byte-exact (exit 0), on-device 16/16 at both CPU
   counts with zero CMPTEST FAIL lines, and the control proves the parity rows are real assertions (a
   one-token deletion turns 3 of them red, including a SEGV). The 2026-10-23 red does not reproduce.
2. **The absolute clause — UNMET, and not this work's fault.** The card's first clause is "`T3_arm` on
   `main` prints 0 FAIL lines". Today it prints 3 at `T3_SMP=1` and 1 at default, all from **GB-058**
   (`SHELLTEST` watchdog) and **POLLTST** (the select-timeout family). That is unsatisfiable while
   GB-058 is open — the same structural defect GB-012 had, and the same remedy applies: re-scope the
   card to the CMPTEST-scoped DoD (as GB-012's DoD was re-scoped on 2026-10-26), or hold it open on
   GB-058 explicitly rather than leaving it ambiguous.

I am not calling the card green by ignoring the absolute clause, and not calling the CMPTEST work red
because of GB-058's reds. The recommendation is a re-scope to "no CMPTEST FAIL line in a completed ARM
run at both CPU counts", which is met and measurable.