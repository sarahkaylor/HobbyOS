# GB-074-V2 — verdict pack (GREEN)

Graded head: **PR #234 @ `d4d11bb`** (`GB-074: claimant_of names the CURRENT (last) claim, not the first`).
Run on: swarm-farm-1 (.230), 2026-10-28 08:46–08:57Z, worktree `/home/sarah/Documents/GitHub/HobbyOS/.worktrees/verify-gb074-v2` (detached at `d4d11bb`).
Local gate only. No CI consulted (2026-10-26 policy).

Parent revision for falsifier: `b5da6be` (= `31d15c4^`, before the claim lock) and `83a0750` (= `origin/main`,
claim lock present, `claimant_of()` still returns the FIRST claim).

| # | criterion | result | where |
|---|---|---|---|
| 1 | exactly one winner; loser exits non-zero naming the winner | PASS 3/3 clean, 3/3 with claim history | `race-graded/summary.txt`, `race-history/summary.txt` |
| 2 | loser posts no claim; exactly one `Ready → In Progress` | PASS (1 comment, 1 transition; 2/2 on the pre-fix tree) | `race-graded/summary.txt` vs `race-prefix/summary.txt` |
| 3 | negative control RUN against the pre-fix script reproduces the double claim | PASS 3/3 | `race-prefix/summary.txt` |
| 4 | `board.py move` prints the observed `X -> Y` | PASS | `race-history/h1.move` |
| 5 | ≥3 repeats | PASS (6 graded trials) | both summaries |
| 6 | `make host_tests` green on the graded head, new test executing | PASS 482/0, `TEST EXIT: 0` | `host_tests.log` |

## Falsifier (the gate can fail)

`claim_race_test.py` from the graded head run against three revisions:

    graded d4d11bb  -> CLAIM_RACE_TEST: PASS (rc 0)            claim_race_test_graded.txt
    83a0750         -> FAIL: claimant_of returns the CURRENT (last) claim  (rc 1)  claim_race_test_83a0750.txt
    b5da6be         -> race checks FAIL, claimant_of absent    (rc 1)  claim_race_test_b5da6be.txt

`83a0750` is the sharp one: the race checks pass there (the lock already exists) and only the
`claimant_of()` clause fails — so criterion 1 is measured against the exact behaviour GB-074-V (#110)
called out, not against a proxy.

## Environment note

`third_party/libcxx-21.1.8` in this worktree is a symlink to the materialised tree in the primary
checkout; without it `make host_tests` fails at `src/host/rtti_test.cpp: 'typeinfo' file not found`
(the same pre-existing condition GB-117 round 3 recorded). No source file changed.

## Scratch tickets

`#248`–`#256` were created as Ready scratch cards for the live race probes and are closed with this
verdict. They are not program work.