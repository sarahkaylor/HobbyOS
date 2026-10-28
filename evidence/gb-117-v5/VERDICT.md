**GREEN — GB-117-V5, graded at `7cfaa7530c6884ea06295df08dd9fad3f8b107f9` (PR #228, branch `ticket/GB-117-claim-age`, the head that would merge). DEEP verifier `wgfx-verify`; all runs local, no CI cited, used, or gated on.**

Evidence: `evidence/gb-117-v5/` (live run, self-test, `make host_tests` log + rc, additivity probe, lane-rule probe). Checked out with `git worktree add … 7cfaa75`; `git rev-parse HEAD` in that tree is the sha above.

## 1. Both clocks still labelled and defined at this head — numbers unchanged

```
claim-age report: 61 open cards   (command: claim_age.py   updated_at NEVER the clock)

CLOCK A -- any-comment: newest comment by ANYONE; lane moves, claims, coordinator posts INCLUDED
  buckets: under6h=12, 6-12h=2, 12-24h=7, over24h=39, nocomments=1
  GET older 'activity'? no.  stale/fresh @ 24h = 40/21

CLOCK B -- working-attempt: newest NON-bookkeeping comment; coordinator status / kickoff / mint / caretaker posts AND lane-transition / claim / retraction notches EXCLUDED; worker content (EVIDENCE, VERDICT, MEASURED, DEBUG, findings) INCLUDED even when it mentions 'Coordinator'
  buckets: under6h=9, 6-12h=1, 12-24h=4, over24h=38, nocomments=9
  STALE (comment age >= 24h, or no comments on Clock B): 47   FRESH: 14   split=47/14
  -> DoD number as a pair: {any-comment 40/21, working 47/14} (produced by: `claim_age.py `)
```
The two splits are **exactly V4's** (`40/21`, `47/14`). The bucket composition drifted by one card in each clock (A `12-24h` 6→7; B `over24h` 37→38, `nocomments` 10→9) because the board moved between the runs — my own verdict comments and these gate cards — not because the tool changed. The split, which is the number DECISION-006 uses, did not move.

## 2. The new per-card row is additive — no disagreement with the totals

Rows now name both clocks, and a card's row is computed by the same two functions:

```
  #99   In Progress ClockA=142.7h  ClockB=142.7h STALE  GB-071 Family B delivery: …
  #77   In Progress ClockA=4.0h  ClockB=135.6h STALE  GB-068 M1 BLOCKER, …

clocks disagree (Clock A fresh / Clock B stale, or the mirror) -- each names both clocks on one line:
  #33   In Progress ClockA=9.1h (fresh)  ClockB=66.0h (STALE)  GB-032 SDL test app …
  #174  Backlog     ClockA=4.0h (fresh)  ClockB=no-comments (STALE)  DECISION-006 …
```
Probe over the whole board (`probe_additive.py`): every card recomputed with the tool's own `_clock()`, buckets and splits re-derived, then compared with the printed blocks and every printed row:
```
recomputed: A buckets={'under6h': 12, '6-12h': 2, '12-24h': 7, 'over24h': 39, 'nocomments': 1}  A split=40/21
recomputed: B buckets={'under6h': 9, '6-12h': 1, '12-24h': 4, 'over24h': 38, 'nocomments': 9}  B split=47/14
buckets additive: A=True  B=True
splits additive: A=40/21 vs 40/21 -> True   B=47/14 vs 47/14 -> True
per-card rows checked against recomputation:
  rows checked; mismatches: 0
```

## 3. `updated_at` still never the clock, fixture still fires

```
cards STALE on Clock B despite fresh updated_at (<24h): 47 (a bulk sweep cannot re-arm them)
  PASS: b1 sweep-proof  b2 both clocks  b3 lane-move=bookkeeping  b4 clause-4 split  b5 decision sep  b6 login-impossibility -> split=5/3
CLAIM_AGE_TEST: ALL PASSED        (rc=0)
```

## 4. The `blocked`/`decision` split is present

```
decision/blocked cards (9): … ordinary cards (52): 39 stale / 13 fresh
```

## 5. `make host_tests` PASS

```
rc=0
Checks run: 482   Checks failed: 0   ALL APPS SUITE TESTS PASSED   TEST EXIT: 0
PASS  tools/board/claim_age_test.py        (self-registered, line present in the run)
```

## 6. The grade names its head

First line of this verdict: `7cfaa7530c6884ea06295df08dd9fad3f8b107f9`. `merge_pr.py --pr 228` should now find a GREEN whose sha prefixes that head; the V4 refusal was correct and this is the re-grade it asked for.

## The extra commit also closed V3's carried-forward finding

I flagged at V3 that the lane rule was a whole-body substring test (`"LANE" in up and arrow`), which classified real worker content as bookkeeping and mis-aged 15 of 61 cards. `7cfaa75` makes the rule move-shaped: re-running the same comparison now shows the shipped clock and a move-shaped rule agreeing everywhere —
```
shipped Clock B : stale/fresh = 47/14
narrow  Clock B : stale/fresh = 47/14
cards whose Clock-B age changes: 0
```
So the commit that made this round necessary also removed the defect I carried forward. Nothing is left on this card from V3.

## Verdict

GREEN at `7cfaa75`. GB-117 (#238) → **Merge Gate**; GB-117-V5 (#244) → **Done**.