# GB-215-V — GREEN at PR #387 head `e6e1c13e`

Verifies GB-215 (#523) at PR #387 head `e6e1c13e1743db14778c196b87e307589de7e1b8`.
Gate run 2026-11-06 ~13:25-13:35Z on swarm-farm-3, deepseek-v4.1-flash (verifier lane).
Worktree: fresh `git worktree add --detach` at the graded sha. No T3 is demanded by this DoD.
CI is ignored by policy (2026-10-26); no check results are cited, read, or waited on.

## Clause 1 — `python3 tools/board/sync_gaming_md.py` at the head

```
DRIFT GB-212: on the board but NOT in gaming.md §8 (orphan #517) — add it to §8 or close it
DRIFT GB-214: on the board but NOT in gaming.md §8 (orphan #521) — add it to §8 or close it

2 drift item(s); run with --fix to push plan fields to the board
rc=1
```

Exactly the two expected orphan lines (PR #384/#386 payloads), no GB-213 or GB-215 line, 2 items, rc=1.

## Clause 2 — one row each, and a second `--adopt-orphans` adds neither

```
=== clause 2: row counts before adoption ===
1
GB-213 rc=0
1
GB-215 rc=0
=== adopt run 1 ===
ADOPTED 2 row(s):
  | GB-212 | ...
  | GB-214 | ...
IN SYNC — 174 plan tickets, 219 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
rc=0
=== row counts after adopt 1 ===
1
GB-213 rc=0
1
GB-215 rc=0
=== adopt run 2 (must add neither) ===
IN SYNC — 174 plan tickets, 219 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
rc=0
=== row counts after adopt 2 ===
1
GB-213 rc=0
1
GB-215 rc=0
```

The first adopt appends only the two expected PR payload rows (GB-212, GB-214); the second adds nothing.
`gaming.md` restored to the head state before the falsifiers (`git checkout -- gaming.md`).

## Clause 3 — falsifier: each row is load-bearing

Delete GB-213 row (`gaming.md:618`):

```
DRIFT GB-212: on the board but NOT in gaming.md §8 (orphan #517) — add it to §8 or close it
DRIFT GB-213: on the board but NOT in gaming.md §8 (orphan #520) — add it to §8 or close it
DRIFT GB-214: on the board but NOT in gaming.md §8 (orphan #521) — add it to §8 or close it

3 drift item(s); run with --fix to push plan fields to the board
rc=1
```

Delete GB-215 row (`gaming.md:478`):

```
DRIFT GB-212: on the board but NOT in gaming.md §8 (orphan #517) — add it to §8 or close it
DRIFT GB-214: on the board but NOT in gaming.md §8 (orphan #521) — add it to §8 or close it
DRIFT GB-215: on the board but NOT in gaming.md §8 (orphan #523) — add it to §8 or close it

3 drift item(s); run with --fix to push plan fields to the board
rc=1
```

Restored state: 2 items, rc=1 (same as clause 1).

## Clause 4 — `make host_tests`

```
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0
```

## Verdict

**GREEN** — clauses 1–4 pass with raw output quoted at `e6e1c13e1743db14778c196b87e307589de7e1b8`.
This authorises merging PR #387 at this sha. The head moving voids this gate; a fresh gate is required.

Files: `00-head.txt`, `01-sync.txt`, `02-adopt.txt`, `03-falsifier.txt`, `04-host-tests.txt`.
