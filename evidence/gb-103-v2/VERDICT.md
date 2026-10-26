# GB-103-V2 — VERDICT: **RED** (clauses 1, 2, 4 pass; clause 3 fails — the tests are blind to the run-path classifier)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 14:0x–14:4xZ.
Verifies GB-103 (#189) / ship **PR #194 @ `7de6dde`** ("INSTRUMENT-FAILURE is now a DISTINCT verdict, not
an annotation"), which supersedes the head I RED'd twice (`92e999f`). Local gate only; no CI consulted.
I did not author any of this code — re-grading my own earlier RED against a new head.

## One line

The three-outcome design now works on a **real killed guest** (my previous RED's exact failing case), and
5/5 monitored ARM runs still produce verdicts — but the self-tests the card relies on for clause 3 are
**insensitive to the run-path classifier**: mapping a real run's instrument failure to `PASS`/`rc=0`
leaves both tests green, so the regression they are meant to catch would ship.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | three distinguishable outcomes, each demonstrated (incl. **killing the guest under the monitor**) | **PASS** — real kill → `INSTRUMENT-FAILURE`, `EXIT=4`, rc=4 |
| 2 | original symptom fixed: 5/5 monitored ARM runs produce a verdict; `T3_SMP=1` same | **PASS** — 5/5 + the SMP=1 run |
| 3 | new tests are **executed** and **can fail** on a deliberate classifier break | **FAIL** — run-path break goes undetected |
| 4 | a real tier red is not swallowed into an instrument failure, and vice versa | **PASS** — side-by-side quoted |

## Clause 1 — the killed-guest case, which failed twice before (`03-killed-guest*`)

Driver: `T3_MONITOR=1 T3_TIMEOUT=150 bash tools/t3_tier.sh arm`, watch the serial for the sample header,
then `SIGTERM` the live QEMU immediately:

```
SIGTERM -> qemu 212723 at 307.2 s
tier rc = 4
sample header present: True
INSTRUMENT-FAILURE in serial log: True
INSTRUMENT-FAILURE in tier stdout: True
  > === T3 guest sample: INSTRUMENT-FAILURE — monitor read ended abnormally (rc=0; deadline approaching with no halt marker) — sample truncated; tier verdict still graded below ===
  > T3_arm: INSTRUMENT-FAILURE — the monitor could not produce a sample (DoD clause 3); this is neither PASS nor a tier FAIL
  > T3_arm EXIT=4
```

So on a real run: the name fires **and** the verdict is a distinct `INSTRUMENT-FAILURE`, rc=4 — not
`PASS`, not a tier `FAIL`. This is exactly what my `92e999f` RED said could not happen, and the fix is
real. Cosmetic note: the line quotes `rc=0` (the subshell's status) while the trigger was the missing
socket (`[ ! -S "$MON_SOCK" ]`); it is correct but reads confusingly.

The other two outcomes, from the shipped test (`01-three-outcome-tests.log`):
```
PASS: clean run -> EXIT 0 PASS
PASS: genuine tier red -> EXIT 1 FAIL (FAIL line quoted)
PASS: instrument failure -> EXIT 4 INSTRUMENT-FAILURE (distinct, got rc=4)
```

## Clause 2 — 5/5 and `T3_SMP=1` (`04-five-runs-and-smp1.log`)

```
run 1 exit=1  T3_arm: 9214 console lines, 1 FAIL lines, 5 user memory fault records across 5 processes, 85 programs exited, halted=1   T3_arm EXIT=1
run 2 exit=1  T3_arm: 9199 console lines, 1 FAIL lines, 5 user memory fault records across 3 processes, 84 programs exited, halted=1   T3_arm EXIT=1
run 3 exit=1  T3_arm: 9079 console lines, 1 FAIL lines, 5 user memory fault records across 4 processes, 85 programs exited, halted=1   T3_arm EXIT=1
run 4 exit=1  T3_arm: 9293 console lines, 1 FAIL lines, 5 user memory fault records across 2 processes, 84 programs exited, halted=1   T3_arm EXIT=1
run 5 exit=1  T3_arm: 9359 console lines, 1 FAIL lines, 5 user memory fault records across 2 processes, 84 programs exited, halted=1   T3_arm EXIT=1
T3_SMP=1     T3_arm: 8624 console lines, 3 FAIL lines, 4 user memory fault records across 2 processes, 78 programs exited, halted=1   T3_arm EXIT=1
```
Five verdicts + the SMP=1 verdict; no exit 141, no death inside `probe_guest()`.

## Clause 3 — the failing cell: the tests do not guard the path a real run takes

The tests **are** executed (`05-runner.log` — `run_t5_tests.py` discovers `tools/t3/*_test.py`, and
`make host_tests` calls that runner):
```
[host-tests] self-registering suite: 20 python tests
  PASS  tools/t3/t3_instrument_failure_test.py
  PASS  tools/t3/t3_three_outcome_test.py
[host-tests] done: 25/25 passed
```

**Deliberate break A — the run-path classifier** (the code a killed guest goes through). Mapped the
instrument-failure verdict to `PASS`/`rc=0`:

```diff
 if [ "$INSTRUMENT_FAILED" = "1" ]; then
-  echo "… INSTRUMENT-FAILURE — the monitor could not produce a sample …"; rc=4
+  echo "… PASS (0 FAIL lines, halted) [break: no-verdict mapped to PASS]"; rc=0
```
Result (`06-break-test.log`): **both tests still PASS**, `rc=0` each:
```
PASS: instrument failure -> EXIT 4 INSTRUMENT-FAILURE (distinct, got rc=4)
T3_THREE_OUTCOME_TEST: PASS …
T3_INSTRUMENT_FAILURE_TEST: PASS …
```
That is the whole defect: a change that makes every real instrument failure read as `PASS` ships green.

**Deliberate break B — for contrast**, the *police* force-hook the tests actually drive, mapped to
`PASS`/`0` (`07-break2.log`):
```
FAIL: instrument failure -> EXIT 4 INSTRUMENT-FAILURE (distinct, got rc=0)
T3_THREE_OUTCOME_TEST: FAIL (1)      rc=1
```
So the tests have teeth — on `police` plus the `T3_FORCE_INSTRUMENT_FAIL` hook. They never exercise
"a run whose guest was killed", which is the path the card exists for. Both breaks were reverted and the
suite re-run green (`06`/`07` tails; `git status --short tools/t3_tier.sh` empty).

**Fix:** extract the verdict decision into one shell function used by both paths (or have the test drive a
`police --simulate-instrument-failure` that goes through the *run* branch), and add the case "monitor dies
⇒ rc=4 and the word INSTRUMENT-FAILURE, never PASS". The test then fails on break A, which is the
property the card asks for.

## Clause 4 — both directions, side by side (`02-side-by-side.log`)

```
A) tier red, monitor healthy
T3_intel: FAIL — 1 FAIL line(s); first 5:
1261:SOMETEST: FAIL injected genuine red
T3_intel EXIT=1                                        rc=1

B) same red log + forced instrument failure
T3_intel: FAIL — 1 FAIL line(s); first 5:
1261:SOMETEST: FAIL injected genuine red
T3_intel: INSTRUMENT-FAILURE — forced (T3_FORCE_INSTRUMENT_FAIL) — neither PASS nor a tier FAIL
T3_intel EXIT=4                                        rc=4
```
A real red is reported as `FAIL` and never as an instrument failure; when both are present **both lines
are printed**, so the red is not swallowed into the instrument failure — although the exit code is 4
precedence, the FAIL block above it names the red. That is the distinction the design exists for.

## Card hygiene (not a verdict cell)

`tools/t3/gb103_clause3_repro.sh` hardcodes another agent's worktree:
```
cd /home/sarah/Documents/GitHub/HobbyOS/.worktrees/hermes-830a858d
```
so it cannot run on any other VM (that directory does not exist here). I used my own driver; the script
should take the repo root from `$(git rev-parse --show-toplevel)` like the other gates do.

## Reproduce

```
git worktree add /tmp/gb103v2 7de6dde && cd /tmp/gb103v2 && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
python3 tools/t3/t3_three_outcome_test.py && python3 tools/t3/t3_instrument_failure_test.py
# kill test: T3_MONITOR=1 T3_TIMEOUT=150 bash tools/t3_tier.sh arm; SIGTERM qemu at the sample header
# break test: map the INSTRUMENT_FAILED block to PASS/rc=0 -> both tests still PASS  (the RED)
```