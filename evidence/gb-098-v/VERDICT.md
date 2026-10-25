# GB-098-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket: GB-098-V (#163) · verifies GB-098 (#159) · PR branch `origin/ticket/GB-098-stale` @ `83d7724`
Base: `origin/main` @ `b874d54` · `git merge-tree --write-tree origin/main HEAD` → clean
Run window: 2026-10-25T06:39Z–06:49Z · worktree `hermes-eebc737f` (swarm-farm-1)

Tier demanded by the DoD: **T1 only** ("the tool is a board client and must not need the network for
its test"). T3/T4/T5 are not named by GB-098 and were therefore not run — this is a stated scope,
not a skipped gate.

## VERDICT: RED

Two checks fail. Each is quoted verbatim below; each can be reproduced from this directory.

### F1 — gate clause 1: the reported count does not match an independent comment/lane-move read

`g2-live.log` (the tool on this branch):

    checked 43 in-progress tickets, threshold 12h (stale clock = last non-coordinator comment; updated_at NOT used)
    stale (no comment or lane-move on the card past threshold): 27

`g3-independent.log` (second implementation, written from the DoD text, state=open read):

    independent read: 40 In Progress cards, threshold 12h
    stale by last non-coordinator comment (comment/created_at clock): 24
    stale by raw issue updated_at (the OLD clock GB-098 removed): 0

The 3-card delta is exactly three **closed** scratch tickets, which `all_issues()` picks up with
`state=all` (`tools/board/check_stale.py:39`) and which therefore keep their `In Progress` label in
the count. A closed card is not a parked claim:

    in state=all read but NOT in open read:
      #113 state=closed last comment 27.2h  GB-990 scratch ticket: claim.sh lock race test
      #114 state=closed last comment 27.2h  GB-991 scratch ticket: claim.sh lock race test
      #115 state=closed last comment 27.2h  GB-992 scratch ticket: claim.sh lock race test

The clock fix itself is real and holds: the old `updated_at` clock reports **0** stale cards on the
same board where the comment clock reports **24**. F1 is the counting predicate, not the clock.

### F2 — gate clause 3: `check_stale_test.py` cannot fail when the clock regresses

`g4-falsifier-old-tool.log` — the PRE-FIX tool from `origin/main` runs the new test and passes:

    --- old tool --self-test:
    self-test result: fails=0
    old-tool-exit=0
    --- new test driving the old tool:
    check_stale.py --self-test: PASS (comment-clock stale detector)
    test-exit=0

`g4b-suite-vs-old-tool.log` — the same swap inside the self-registering T1 suite (restored after):

      PASS  tools/board/check_stale_test.py
    [host-tests] done: 22/22 passed
    suite-exit=0

Cause: `check_stale_test.py` asserts nothing itself — it subprocesses `check_stale.py --self-test`
and looks for `fails=0`. The assertions live *inside the file under test*, so reverting that file
restores the weaker 6-case self-test and the suite still prints PASS. That is the GB-007/GB-080
class ("a skip that prints a pass"): `make host_tests` is green on defective code.

Positive control (`g4c-mutation.log`) — an in-file regression (`classify` → `return False`) *is*
caught, so the failure is specifically the whole-file revert, not a dead instrument:

    self-test result: fails=3
    self-test-exit=1
    test-exit=1

## Checks that pass

- `g1-selftest.log`: 9 checks, `self-test result: fails=0`, incl. the case that failed before
  (`comment_age=40.0 issue_age=0.1 -> stale=True (want True)`).
- `g5-host-tests.log`: `make host_tests` → `PASS tools/board/check_stale_test.py` … `ALL APPS SUITE TESTS PASSED`, `make-exit=0`.
- DoD clause 3's shape requirement is met: the live output names `checked`, `stale` and the
  comment-age distribution (`min=0.0h median=27.2h max=123.4h`).
- DoD clause 4's quoted number (37) has drifted with the board: the same tool now prints 27 and the
  independent open read prints 24, so DECISION-001's next re-read must use a fixed tool.

## What would make this GREEN

1. `all_issues()` (or the counting loop) restricts to `state=open`, so a closed card cannot be a
   stale claim; then tool count == independent count on the same board state.
2. `check_stale_test.py` carries its own assertions instead of trusting the SUT's self-test: drive
   `classify`/`last_activity` with an independent case table (or assert the exact check count, 9)
   so that dropping `origin/main`'s `check_stale.py` in makes the test exit non-zero.
3. Re-quote the number on DECISION-001 after (1), stating `checked`/`stale` from the same run.
