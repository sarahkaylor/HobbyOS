VERDICT: RED sha=d66d3e80

**RED — GB-208-V @ `d66d3e80` (PR #376, base `f337e7a3`). One failing cell: §7's M3 row calls `0a5c3f2e` the merge, but the record says the merge is `ddf65257`; `0a5c3f2e` is its first parent (the matrix main).** Everything else passes; the fix is one clause in one cell. GB-208 stays in In Review (open PR, WORK-POLICY §2).

**Clause 1 — rows landed.** Counts: GB-206 `1`, GB-207 `1`, GB-208 `1`; the three rows are at `gaming.md:578`, `:579`, `:477` (GB-208's row is the plan-sync card itself).

**Clause 2 — IN SYNC.**

```
IN SYNC — 169 plan tickets, 213 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
```

rc=0, no `DRIFT` line.

**Clause 3 — one row per id.** `grep -cE '^\| GB-206 \|'` → 1; GB-207 → 1; GB-208 → 1. No duplicates.

**Clause 4 — tests.**

```
SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass
```

**Clause 5 — falsifier.** GB-208 row deleted:

```
DRIFT GB-208: on the board but NOT in gaming.md §8 (orphan #500) — add it to §8 or close it
1 drift item(s); run with --fix to push plan fields to the board
rc=1
```

restored:

```
IN SYNC — 169 plan tickets, 213 board tickets, ...
rc=0
```

**Clause 6 — §7 cells: FAIL on M3.** The cell says:

```
GB-053-V2 GREEN at PR #360 head `42cbbb5c`, merged `0a5c3f2e` — both arches 5/5 ...
```

The record:

- GB-190's merge comment: "PR #360 head 42cbbb5c merged to main as **ddf65257**, on GB-053-V2's GREEN ... matrix at 0a5c3f2e".
- GB-053-V2's verdict: "GREEN sha=42cbbb5c (PR #360 head, GB-190's final both-arch matrix at **main 0a5c3f2e**)".

`git show -s --format='%h %p' ddf65257` → `ddf65257 0a5c3f2e 42cbbb5c` — `0a5c3f2e` is the first parent, `ddf65257` is the merge. The cell's "merged `0a5c3f2e`" contradicts the record; the DoD's clause 6 repeats the same phrasing, but the citations are checked against the -V record, not the DoD's paraphrase.

The other citations verify:

- M1 directed GREEN `2d7605d` = GB-024-V3's `VERDICT: GREEN sha=2d7605d`; the full-wave RED citation is GB-146 (open, `rip=0x93`/`vector=1879307563` records on the card).
- M2 `7c8f16a6c22f...` = GB-033-V2's `VERDICT: GREEN — GB-033 at current main 7c8f16a6...`.
- M4 `4cc60a4a` = GB-188-V's `VERDICT: GREEN sha=4cc60a4a`; GB-190's matrix at `0a5c3f2e`, graded by GB-053-V2 at `42cbbb5c`, matches the record.
- M0's "90/90" is the editorial slip the DoD excuses; the 169/212-vs-213 board count is live and explicitly not a gate clause.

**Clause 7 — suite/links.**

```
Checks run:    482
Checks failed: 0
TEST EXIT: 0

make ARCH=intel hobbyos.elf   rc=0
make ARCH=arm hobbyos.elf     rc=0
```

**Fix:** in §7 M3, replace "merged `0a5c3f2e`" with "merged as `ddf65257` onto `0a5c3f2e`" (or "matrix at `0a5c3f2e`"). No other cell changes.

RED → GB-208 stays in In Review; GB-208-V to Done.
