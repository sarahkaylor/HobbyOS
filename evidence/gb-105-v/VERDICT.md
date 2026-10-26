# GB-105-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-105-V (#199) · verifies GB-105 (#197) · ship = PR #202 head `d706c8c`
Gate tree `9572b52` = `origin/main` `fea33b0` × `d706c8c` (`ort` merge, no conflicts)
Run window 2026-10-26T18:58Z–19:39Z · worktree `hermes-36c16b0a` (swarm-farm-1)
Local only — no CI consulted, no check/status result cited.

## VERDICT: GREEN

| # | Clause | Result |
|---|---|---|
| 1 | reproduce the coupling myself: REAL classifier + FORCE → rc=4; BROKEN classifier + FORCE → rc=0, with the edit shown | **PASS** |
| 2 | coupling test executed by a runner, and **fails** when the coupling is broken | **PASS** — 27/27 in the suite; hardcode → `FAIL (1)` rc=1 |
| 3 | real run path unchanged: ARM `T3_MONITOR=1` 5/5 verdicts, and a clean log still PASSes | **PASS** — 5/5; clean `PASS rc=0` |
| 4 | no new third state in the police path: `bash -n`, red log → FAIL with its line, clean → PASS | **PASS** |
| 5 | the old unconditional `exit 4` is gone from the force path | **PASS** — both sites call the classifier |

### 1 — the coupling, reproduced by me (not from the worker's transcript)

    ### (a) REAL classifier
    $ T3_FORCE_INSTRUMENT_FAIL=1 bash tools/t3_tier.sh police <clean.log> intel
    T3_intel (police …): INSTRUMENT-FAILURE — forced (T3_FORCE_INSTRUMENT_FAIL) — neither PASS nor a tier FAIL
    T3_intel EXIT=4                                            rc=4

    ### (b) BROKEN classifier — the exact edit
    instrument_failure_evident() {
      [ "$3" = "1" ] && { [ "$1" != "0" ] || [ ! -S "$2" ]; }
    }
    -> instrument_failure_evident() { return 1; }        # maps the failure to PASS

    $ T3_FORCE_INSTRUMENT_FAIL=1 bash /tmp/gb105_broken.sh police <clean.log> intel
    T3_intel: PASS (0 FAIL lines, halted)
    T3_intel EXIT=0                                            rc=0

The flag can no longer manufacture the third outcome once the decision code is broken — which is the
whole card.

### 2 — the coupling test is executed, and can fail

    $ python3 tools/t5/run_t5_tests.py
      PASS  tools/t3/t3_classifier_coupling_test.py
      PASS  tools/t3/t3_instrument_failure_test.py
      PASS  tools/t3/t3_three_outcome_test.py
    [host-tests] done: 27/27 passed

(The card's note says the PR adds no wiring and the tests must be run by hand. That is wrong on this
tree: the GB-064 self-registering suite globs `tools/*/*_test.py`, so all three run inside `host_tests`
— 27/27 — without any Makefile edit. Worth correcting so the next verifier does not skip them.)

Deliberate break of the *coupling* — the police force path hardcoded back to the old short-circuit,
`if [ "${T3_FORCE_INSTRUMENT_FAIL:-0}" = "1" ] && instrument_failure_evident 1 "" 1; then` →
`if [ "${T3_FORCE_INSTRUMENT_FAIL:-0}" = "1" ]; then`:

    PASS: REAL classifier + FORCE -> rc=4 (got 4)
    FAIL: BROKEN classifier + FORCE -> rc=0 PASS, NOT 4 (got 4) — forced case feeds the real classifier
    T3_CLASSIFIER_COUPLING_TEST: FAIL (1)                      rc=1

restored → `T3_CLASSIFIER_COUPLING_TEST: PASS`, rc=0.

### 3 — the real run path is unchanged

Five consecutive ARM monitored runs, each with its own verdict line:

    run 1: rc=0  T3_arm: PASS (0 FAIL lines, halted, no kernel fault, 9506 lines)  EXIT=0
    run 2: rc=0  T3_arm: PASS (0 FAIL lines, halted, no kernel fault, 9275 lines)  EXIT=0
    run 3: rc=1  T3_arm: FAIL — 2 FAIL line(s)                                    EXIT=1
    run 4: rc=0  T3_arm: PASS (0 FAIL lines, halted, no kernel fault, 9492 lines)  EXIT=0
    run 5: rc=0  T3_arm: PASS (0 FAIL lines, halted, no kernel fault, 9409 lines)  EXIT=0
    ARM_VERDICTS: 5/5

Run 3's red is `SQLTEST insert-wide: FAIL` / `SQLTEST FAILED: 1` — a userspace test red, not the monitor
dying. `T3_SMP=1`: at a 200 s budget it hit the deadline (`FAIL — no 'System halt' line`) — a budget
artifact of a uniprocessor wave, so I re-ran at 420 s:

    rc=0  T3_arm: PASS (0 FAIL lines, halted, no kernel fault, 8701 lines)  EXIT=0

Clean log through the police path: `T3_intel: PASS (0 FAIL lines, halted)` `EXIT=0`, rc=0.

### 4 / 5 — nothing leaked into the police path, and the short-circuit is gone

    $ bash -n tools/t3_tier.sh          -> clean
    red log:   T3_intel: FAIL — 1 FAIL line(s); 1262:SOMETEST: FAIL injected genuine red   rc=1
    clean log: T3_intel: PASS (0 FAIL lines, halted)                                       rc=0

Both force sites now route through the classifier, and the only `exit 4`s are inside those guarded
blocks:

    86:  if [ "${T3_FORCE_INSTRUMENT_FAIL:-0}" = "1" ] && instrument_failure_evident 1 "" 1; then
    89:      exit 4                                   # police path, classifier-guarded
    143: if [ "${T3_FORCE_INSTRUMENT_FAIL:-0}" = "1" ] && instrument_failure_evident 1 "" 1; then
    144:     echo "… INSTRUMENT-FAILURE — forced …"; echo "T3_${ARCH} EXIT=4"; exit 4
    510: if [ "${T3_FORCE_INSTRUMENT_FAIL:-0}" = "1" ]; then
         instrument_failure_evident 1 "" 1 && INSTRUMENT_FAILED=1     # run-path hook, classifier-coupled

## Board note (not a verdict cell)

`GB-105-V` was minted **twice** for the same PR — #198 (18:38Z) and #199 (18:40Z) — and both went
Ready. I gated #199 because its clause set is the superset (it demands the deliberate break and the
5/5); farm-3 claimed #198 at 18:58Z, so the same tree is being gated twice. This evidence covers both
cards' clauses; the Coordinator should close one of them rather than reconcile two verdicts.