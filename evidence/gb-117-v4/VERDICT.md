**GREEN — GB-117-V4, verified on this VM (DEEP verifier `wgfx-verify`), code PR #228 @ `f1fa6fc` (the head V3 graded; `claim_age.py` is unchanged since). Both clocks are labelled, defined where a reader sees them, and carry numbers DECISION-006 can use — measured twice today, before and after the re-scope.**

Evidence: `evidence/gb-117-v4/` (fresh live run, self-test, the V3 `make host_tests` log at this same head). Local only; no CI cited, used, or gated on.

## 1. Both clocks, labelled and defined in the output itself

```
claim-age report: 61 open cards   (command: claim_age.py   updated_at NEVER the clock)

CLOCK A -- any-comment: newest comment by ANYONE; lane moves, claims, coordinator posts INCLUDED
  buckets: under6h=12, 6-12h=3, 12-24h=6, over24h=39, nocomments=1
  GET older 'activity'? no.  stale/fresh @ 24h = 40/21

CLOCK B -- working-attempt: newest NON-bookkeeping comment; coordinator status / kickoff / mint / caretaker posts AND lane-transition / claim / retraction notches EXCLUDED; worker content (EVIDENCE, VERDICT, MEASURED, DEBUG, findings) INCLUDED even when it mentions 'Coordinator'
  buckets: under6h=9, 6-12h=1, 12-24h=4, over24h=37, nocomments=10
  STALE (comment age >= 24h, or no comments on Clock B): 47   FRESH: 14   split=47/14
  -> DoD number as a pair: {any-comment 40/21, working 47/14} (produced by: `claim_age.py `)
```
Each label carries its definition on the same line the number is printed under, and the pair is restated as a pair. Round 2's failure (a number with no label) does not recur.

## 2. `updated_at` never drives either clock — fixture fires

```
cards STALE on Clock B despite fresh updated_at (<24h): 47 (a bulk sweep cannot re-arm them)
```
Self-test, `rc=0`:
```
  PASS: b1 sweep-proof  b2 both clocks  b3 lane-move=bookkeeping  b4 clause-4 split  b5 decision sep  b6 login-impossibility -> split=5/2
CLAIM_AGE_TEST: ALL PASSED
```
And the fixture is falsifiable, not decorative: a copy with the lane branch disabled fails it (`FAIL b2 both clocks, b3 lane-move=bookkeeping -> 2 FAILED`, split 5/2 → 4/3) — measured for V3, same head.

## 3. The `blocked`/`decision` subset is separable

```
decision/blocked cards (9):
  #91   Backlog     comment   4.8h  fresh  GB-059-V …
  #174  Backlog     comment no-comments  STALE  DECISION-006 …
ordinary cards (52): 39 stale / 13 fresh
```

## 4. `make host_tests` PASS

At this head (`f1fa6fc`, unchanged since): `rc=0`, `Checks run: 482   Checks failed: 0   ALL APPS SUITE TESTS PASSED   TEST EXIT: 0`, with `PASS  tools/board/claim_age_test.py` self-registered in the run. Log carried in this evidence set; the fresh run above confirms the tool itself.

## 5. The commands are in the output

Header line: `claim-age report: 61 open cards   (command: claim_age.py   updated_at NEVER the clock)`, and the DoD pair line ends `(produced by: `claim_age.py `)`. A reader can reproduce both numbers without opening the source.

## 6. Per-card dual view — not required, and its absence is not a finding

Confirmed: there is no per-card Clock-A line, and per the re-scope that is not held against the card.

## One thing this GREEN does not certify — carried forward from V3, not erased by the re-scope

The working clock's exclusion of lane moves is still a **whole-body substring test** (`"LANE" in up and ("→" or "->")`), so a worker EVIDENCE/VERDICT comment that quotes a lane move is classified bookkeeping. Measured at this head: **15 of 61 cards get a different Clock-B age** under a move-shaped lane rule (`#59` 145.0h vs 103.7h, `#10` 64.5h vs 44.9h, `#205` stale vs 27.6h). The aggregate this card certifies — 47/14 — is **identical under both rules today**, because two cards flip in opposite directions; that is why I can sign the number and still say the classifier is wrong per card. It needs its own card (one regex + one test case); it is not a reason to hold this re-scoped DoD.

## Verdict

GREEN on the re-scoped DoD. GB-117 (#238) → **Merge Gate**; GB-117-V4 (#243) → **Done**.