# GB-105-V (#198) — VERDICT: **GREEN**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 18:5xZ.
Verifies GB-105 (#197) / ship **PR #202 @ `d706c8c`** ("route the T3_FORCE_INSTRUMENT_FAIL forced case
through the same classifier as the real probe_guest sample"). Host-side gate, as the card specifies (no
QEMU, no CI). I did not author GB-105 or PR #202.

## One line

The forced instrument-failure case now goes through the **same** `instrument_failure_evident()`
classifier as the real sample, so a broken classifier can no longer be masked by a hardcoded `rc=4` — and
the coupling test that locks this **fails** when I deliberately re-hardcoded the path, which is the
can-fail property the previous round (GB-103-V2) was missing.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | forced case coupled to the real classifier: REAL+FORCE → rc=4, BROKEN+FORCE → rc=0 | **PASS** — quoted, and re-proved by my own hardcode |
| 2 | three-outcome judgement intact (PASS / FAIL / INSTRUMENT-FAILURE) | **PASS** |
| 3 | real mid-sample monitor close still yields the named status | **PASS** |
| — | *(my own addition)* the coupling test can fail | **PASS** — hardcoding the forced `exit 4` makes it FAIL |

## Clause 1 — the coupling, quoted (`01-three-tests.log`, `02-can-fail.log`)

```
$ python3 tools/t3/t3_classifier_coupling_test.py
  PASS: REAL classifier + FORCE -> rc=4 (got 4)
  PASS: BROKEN classifier + FORCE -> rc=0 PASS, NOT 4 (got 0) — forced case feeds the real classifier
T3_CLASSIFIER_COUPLING_TEST: PASS
rc=0
```

The test builds a copy of `t3_tier.sh` with

```sh
instrument_failure_evident() { return 1; }
```
and shows the forced case then **falls through to ordinary grading** (rc=0) rather than emitting `rc=4`.
That is exactly the defect I RED'd in GB-103-V2 (`92e999f`: the force hook short-circuited before any
grading), now closed by structure rather than by assertion.

**My independent can-fail proof** — I re-introduced the hardcode on the real tree (forced path returning
`exit 4` without calling the classifier):

```
=== force path HARDCODED (classifier bypassed) — coupling test MUST fail ===
  PASS: REAL classifier + FORCE -> rc=4 (got 4)
  FAIL: BROKEN classifier + FORCE -> rc=0 PASS, NOT 4 (got 4) — forced case feeds the real classifier
T3_CLASSIFIER_COUPLING_TEST: FAIL (1)      rc=1
=== restore ===
  PASS: BROKEN classifier + FORCE -> rc=0 PASS, NOT 4 (got 0)
T3_CLASSIFIER_COUPLING_TEST: PASS
$ bash -n tools/t3_tier.sh   rc=0
```
So the test is not decoration: it fails on precisely the regression it exists for, and the tree is
restored (`git status --short tools/t3_tier.sh` empty).

## Clause 2 — three outcomes unchanged

```
  PASS: clean run -> EXIT 0 PASS
  PASS: genuine tier red -> EXIT 1 FAIL (FAIL line quoted)
  PASS: instrument failure -> EXIT 4 INSTRUMENT-FAILURE (distinct, got rc=4)
T3_THREE_OUTCOME_TEST: PASS
```

## Clause 3 — the real run path's status still fires

```
  PASS: mid-sample monitor close -> INSTRUMENT-FAILURE status fires
  PASS: socket present through the sample -> no INSTRUMENT-FAILURE
  PASS: probe failure cannot abort grading (the gate is an annotation, not a return)
T3_INSTRUMENT_FAILURE_TEST: PASS
```

## Scope and honesty notes

* This card's gate is host-side by its own text ("no QEMU needed"); I did **not** re-run the 5/5 ARM
  monitored runs — that is GB-105-V **#199**'s clause, and #199 is claimed by another verifier
  (`verifier-deep-swarm-farm-1`), so the ARM half is being measured there. I am not implying I measured it.
* **Board hygiene, not a verdict cell:** `GB-105-V` exists **twice** — #198 (`wt-M`, this card) and #199
  (`wt-S`, claimed by the other verifier), both verifying PR #202. Two cards for one ship is how a
  duplicate GREEN or a duplicate claim race happens (GB-074); one should be closed as a duplicate.
* No CI result was consulted, quoted, or gated on at any point.

## Reproduce

```
git worktree add /tmp/gb105v d706c8c && cd /tmp/gb105v
python3 tools/t3/t3_classifier_coupling_test.py
python3 tools/t3/t3_three_outcome_test.py
python3 tools/t3/t3_instrument_failure_test.py
# can-fail proof: replace the force path's `&& instrument_failure_evident 1 "" 1` with a bare
# `[ "$T3_FORCE_INSTRUMENT_FAIL" = 1 ]` -> the coupling test FAILs (rc=1)
```