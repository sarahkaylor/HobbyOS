# GB-202-V2 — VERDICT: GREEN

- Graded: PR #372 head **5554562fbd3edf563b7f1ca846e9c706b9ef9f7f** (`GB-202: one row per id — collapse the GB-196/197 duplicates and make the checker see them`)
- Base: `origin/main` (ancestor of HEAD, checked)
- Host: swarm-farm-3; CI not consulted (policy 2026-10-26). Small infra gate, no T3 boot.
- Diff: `gaming.md` (-4/+1), `tools/board/sync_gaming_md.py` (+19), `sync_gaming_md_test.py` (+13); `pr.diff`.

## Clause 1 — the duplicates are gone (PASS)

```
$ grep -c '^| GB-196 ' gaming.md
1
$ grep -c '^| GB-197 ' gaming.md
1
```

Rows: GB-196 at `gaming.md:573` (M1 table) and GB-197 at `gaming.md:591` (M2 table). The tables are contiguous: GB-196 is followed immediately by GB-199 (`:574`), GB-197 is the last row before `### Milestone M3` (`:592` is the blank line after the table, `:593` the heading). No blank line splits either table.

## Clause 2 — the checker can see a doubled row (PASS)

```
$ python3 tools/board/sync_gaming_md_test.py
...
  PASS: a doubled §8 row is named
  PASS: the live plan has one row per id
...
SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass
rc=0
```
Raw: `test.log` (21 PASS lines).

## Clause 3 — the live plan is IN SYNC (PASS)

```
$ python3 tools/board/sync_gaming_md.py
IN SYNC — 164 plan tickets, 207 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
rc=0
```
Raw: `sync.log`.

## Clause 4 — falsifier (PASS)

Scratch edit of the same worktree: a second `| GB-196 |` row inserted with `awk`, then restored with `git checkout -- gaming.md`:

```
$ grep -c '^| GB-196 ' gaming.md
2
$ python3 tools/board/sync_gaming_md.py
DRIFT plan: duplicate §8 rows for GB-196 (one row per id)

1 drift item(s); run with --fix to push plan fields to the board
rc=1
$ git checkout -- gaming.md; python3 tools/board/sync_gaming_md.py
IN SYNC — 164 plan tickets, 207 board tickets, ...
rc=0
```
Raw: `fals_dup.log`, `fals_restored.log`. The checker is not a no-op: the duplicate is named and exits 1.

## Clause 5 — suites and links (PASS)

```
$ make host_tests
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0

$ make ARCH=intel hobbyos.elf   rc=0
$ make ARCH=arm   hobbyos.elf   rc=0
```

## Verdict

All five clauses pass at 5554562f. GREEN.
