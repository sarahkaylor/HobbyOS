# GB-098-V (round 2) — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-098-V (#171) · verifies GB-098 (#159) · ship = PR #157 head `7b2d613`
Gate tree `22f0300` = `origin/main` `30178e5` × `7b2d613` (`ort` merge, no conflicts)
Run window 2026-10-25T07:33Z–07:36Z · worktree `hermes-11b6bc75` (swarm-farm-1)

Tier demanded: **T1 only** (board client). T3/T4/T5 not named by GB-098, not run.

Independence: I posted the earlier RED on #163 and wrote none of the fix; the fix is the worker's at
`7b2d613`, re-approved by the reviewer, re-gated here because the coordinator minted this card.

## VERDICT: GREEN

| # | Gate item | Result |
|---|---|---|
| 1 | `--self-test` ≥9 comment-clock cases, `fails=0` | **PASS** — 9 cases, `fails=0` |
| 2 | independent case table drives `classify()`; whole-file revert to `origin/main` FAILs | **PASS** — suite 22/22; revert → `CHECK_STALE_TEST: FAIL (6 failure(s))`, rc=1 |
| 3 | live `checked` == independent `state=open` count; host_tests green (482) | **PASS** — 44 == 44, stale 24 == 24; `482 checks, 0 failed` |

### G1 — self-test

    [OK  ] lane=In Progress comment_age=40.0  issue_age=0.1   -> stale=True (want True)
    ...
    self-test result: fails=0
    rc=0

### G2 — in-suite test, and the revert that must fail it

    ### G2a: python3 tools/t5/run_t5_tests.py
      PASS  tools/board/check_stale_test.py
    [host-tests] done: 22/22 passed
    rc=0

Negative control, `git show origin/main:tools/board/check_stale.py` dropped into the gate tree:

    ### G2b NEGATIVE CONTROL: origin/main's pre-GB-098 check_stale.py
      FAIL  tools/board/check_stale_test.py  (CHECK_STALE_TEST: FAIL (6 failure(s)))
    [host-tests] done: 21/22 passed
    suite-rc=1

That is F2 closed: the earlier card's RED was that this test passed on the pre-fix tool (G4b in
`evidence/gb-098-v/`, round 1). It now fails. Restored afterwards (`git status` clean).

### G3 — live count vs an independent open read

    ### G3a: python3 tools/board/check_stale.py
    checked 44 in-progress tickets, threshold 12h (stale clock = last non-coordinator comment; updated_at NOT used)
    stale (no comment or lane-move on the card past threshold): 24

    ### G3b: independent second implementation, state=open
    independent read: 44 In Progress cards, threshold 12h
    stale by last non-coordinator comment (comment/created_at clock): 24
    stale by raw issue updated_at (the OLD clock GB-098 removed): 0

44 == 44 and 24 == 24 (round 1: 43 vs 40, inflated by three closed scratch cards).

**Honest limit on that comparison:** today's board no longer has any *closed* card still carrying the
`In Progress` label, so `state=all` and `state=open` currently return the same 44 cards
(`g3e-closed-cards.log`) and the live number alone does not exercise the filter. Disabling the filter
does not change today's output. The filter's necessity is therefore proven on a synthetic board with
the *shipped* code and no network (`g3d-f1-synthetic.log`):

    synthetic board: #9001 open+old comment (a real stale claim), #9002 CLOSED+old comment (not a claim), both with a bulk-touched updated_at
    [shipped tool (state=open filter present)] rc=1
       checked 1 in-progress tickets, threshold 12h ...
       stale (no comment or lane-move on the card past threshold): 1
    [filter disabled (F1 regression)] rc=1
       checked 2 in-progress tickets, threshold 12h ...
       stale (no comment or lane-move on the card past threshold): 2

Both directions, same code path, closed claim excluded only by the filter.

### G4 — host_tests

    === Test Results ===
    Checks run:    482
    Checks failed: 0
    ALL APPS SUITE TESTS PASSED
    TEST EXIT: 0
    rc=0

## Coordination note (not a verdict cell)

PR #157 (`7b2d613`) is still stacked: its branch carries GB-085 (`cda3a4d`, host-test scratch dirs,
touching `src/kernel/process.c` and 8 `src/host/*` files) plus a `gaming.md` edit, and it is **not**
merged. This gate therefore ran on a merge of `main` × the branch, not on `main`; the merge is clean
(`ort`, 0 conflicts) but the merge order matters — land the GB-085 stack (#156) first, or rebase
#157, and re-run the T1 leg if the rebased delta changes. Everything this card gates is confined to
`tools/board/check_stale.py` + `check_stale_test.py`.
