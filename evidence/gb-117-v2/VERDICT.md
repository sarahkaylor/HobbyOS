**RED — GB-117-V2, verified on this VM (DEEP verifier `wgfx-verify`), code PR #228 @ `3a55959`. The three gate commands pass; the number they print is not produced by the rule the tool itself states, and that is the round-1 defect again in a new place.**

Evidence: `evidence/gb-117-v2/` — self-test, `make host_tests` (rc captured), the live report, and the lane-classification probe. Local only; no CI cited, used, or gated on.

## The gate commands, run as written

```
$ python3 tools/board/claim_age.py --self-test
  PASS: b1 sweep-proof stale  b2 bucket map  b3 decision/blocked sep -> split=4/1
CLAIM_AGE_TEST: ALL PASSED                                   (exit 0)

$ make host_tests
  PASS  tools/board/claim_age_test.py                        (line 1276)
Checks run: 482   Checks failed: 0   ALL APPS SUITE TESTS PASSED   TEST EXIT: 0
rc=0

$ python3 tools/board/claim_age.py
claim-age report (clock = NEWEST activity comment; coordinator bookkeeping + lane notches do NOT re-arm it; updated_at NOT used): 61 open cards
buckets: under6h=8, 6-12h=2, 12-24h=5, over24h=42, nocomments=4
STALE (comment age >= 24h, or no comments): 46   FRESH: 15   split=46/15
```

(The first `make host_tests` attempt died on `src/host/rtti_test.cpp:29: fatal error: 'typeinfo' file not found` — this worktree's `third_party/libcxx-21.1.8` was incomplete. I rsync'd it from the primary checkout and re-ran; that is an environment gap in my tree, not a defect in this PR, and the rc above is from the fixed tree.)

## 1. MET — buckets, oldest N with ids, `updated_at` never in the clock

```
buckets: under6h=8, 6-12h=2, 12-24h=5, over24h=42, nocomments=4
oldest 10 by comment age (ids, stale only):
  #67   Icebox      comment 213.7h  EPIC Program-2: Dyson Sphere Program (Unity 3D)
  #66   Icebox      comment 213.7h  EPIC Program-2: RimWorld (Mono CLR on HobbyOS)
  #68   Icebox      comment 213.7h  EPIC Program-2: Total Annihilation / Spring RTS (C++
  #99   In Progress comment 141.0h  GB-071 Family B delivery: ...
  ...
```
`updated_at` is read only to *report* the sweep-proof count (`claim_age.py:166`, `:191`), never as the clock; the newest-activity selection is `activity[-1]` over the ascending comment list.

## 2. MET — a fresh `updated_at` does not re-arm

```
cards STALE despite fresh updated_at (<24h): 46 (a bulk sweep cannot re-arm them)
```
and fixture `b1 sweep-proof stale` passes in the self-test.

## 3. MET — decision/blocked separated

```
decision/blocked cards (9): ... ordinary cards (52): 38 stale / 14 fresh
```

## 4. FAILS — the number is not by the rule the tool prints

The tool's own header states the clock as *"newest activity comment; coordinator bookkeeping + **lane notches do NOT re-arm it**"*, and `_is_bookkeeping`'s docstring says a post "is **bookkeeping** … when it is a **lane transition**/claim/status/…".

A real lane-move comment is classified **ACTIVITY**:
```
$ python3 evidence/gb-117-v2/probe_lane_classification.py
  ACTIVITY     <- lane move: 'GB-117-V2: lane - → **Ready**'
  ACTIVITY     <- lane move: 'GB-117: lane Backlog → **Ready**'
```
`_is_bookkeeping` (`claim_age.py:82-90`) matches only prefixes `COORDINATOR / SWARM HEALTH / CARETAKER / VERIFICATION MINTED / RE-GATE MINTED / SWARM-HEALTH / VERDICT-`; a line starting `GB-nnn: lane …` matches none.

**17 open cards' newest "activity" comment is a lane move**, including #240 (0.2 h), #152 (12.5 h), #47 (14.1 h), #46 (33.0 h), #178/#51 (43 h), #78/#63 (60 h), #9/#25 (65-67 h), #143 (69 h), #130/#121/#59 (102 h) and the three Icebox epics (213.7 h). Treating a lane move as bookkeeping — the rule the tool prints — gives a different report:

| rule | buckets | stale/fresh |
|---|---|---|
| as shipped (lane move = activity) | 8 / 2 / 5 / **42** / 4 | **46 / 15** |
| stated rule (lane move = bookkeeping) | 7 / 2 / 5 / **38** / **9** | **47 / 14** |

So the printed 46/15 is not the stated clock's number, and neither the DoD cell's **42/28** nor the re-delivery's **45/14** reproduces today: measured **46/15** as shipped, **47/14** under the stated rule. Per clause 4 the DoD cell must be amended to the measured values — but it cannot be amended to a number the stated rule does not produce, which is why this is RED rather than an amendment request.

## 5. FAILS — the lane-transition classification, which is the same defect

Reported as clause 5 asks: lane moves fall **ACTIVITY**, they change the count (stale 46 → 47, fresh 15 → 14, and `nocomments` 4 → 9 because five cards have no comment other than their lane moves), and they contradict both the docstring and the printed header. Reconciled against the stated rule, the report is wrong by one card in each direction.

**Fix (small, either direction — but the card has to pick one and mean it):** add the lane-move/claim shapes (`^<id>: lane … → **<lane>**`, `claimed by …`) to `_is_bookkeeping`, which yields **47/14** and matches the printed rule; *or* keep the current classification and change the header, the docstring and the DoD to say lane moves **do** re-arm. Then amend the DoD cell to the measured numbers, and add a self-test case for a lane-move comment so a fifth round cannot regress quietly.

## Housekeeping

`GB-117-V2` is on the board **twice** — **#240** (`infra,tests,verify`, Coordinator's) and **#241** (`deep,verify`, the reviewer's, this gate). I claimed and gated #241 and left #240 untouched; one of them should be closed as a duplicate so a third round does not run twice.

## Consequence

GB-117 (#238) → **In Progress** with clauses 4 and 5 named; GB-117-V2 (#241) → **Done**. Clauses 1-3 should not be re-run: they pass and are quoted above.