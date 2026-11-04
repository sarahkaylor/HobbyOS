# GB-198-V — VERDICT: GREEN

- Graded: PR #364 head **5006a6b70485a66ab1d39a5d83ce6b8d45f54b33** (`GB-198: adopt the GB-196/GB-197 rows; join wrapped DoD lines`)
- Base: `origin/main` 138a6874 (ancestor of HEAD, checked)
- Host: swarm-farm-3 (QEMU 10.2.1); CI not consulted (policy 2026-10-26)
- Worktree: fresh detached worktree at the graded head

## Clause 1 — rows landed (PASS)

`gaming.md` §8 carries all three rows; `git diff origin/main...HEAD -- gaming.md` adds exactly one line per table:

```
@@ -470,6 +470,7 @@ final list.
+| GB-198 | **plan §8 is two rows short — GB-196 and GB-197 are orphans; run --adopt-orphans** | S | — | ...
@@ -567,6 +568,7 @@ final list.
+| GB-196 | **compat-track PT_INTERP capture + ld-linux image map + DT_NEEDED report — a glibc binary names its interpreter and first missing DSO** | M | ...
@@ -583,6 +585,7 @@ final list.
+| GB-197 | **3D-device availability on the graded QEMU + GL2 surface gap — probe virtio-gpu-gl, record the capset or the absent device** | S | ...
```

Placement: GB-198 at `gaming.md:473` (M0 table, before `### Milestone M1`), GB-196 at `gaming.md:571` (M1 table, before `### Milestone M2`), GB-197 at `gaming.md:588` (M2 table, before `### Milestone M3`). Full diff: `pr.diff`.

## Clause 2 — IN SYNC (PASS)

```
$ python3 tools/board/sync_gaming_md.py
IN SYNC — 161 plan tickets, 202 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
rc=0
```
Raw: `sync.log`. No `DRIFT` line.

## Clause 3 — tests, including the wrapped-DoD case (PASS)

```
$ python3 tools/board/sync_gaming_md_test.py
...
  PASS: row includes both DoD items
  PASS: row stops at the next section
  PASS: a wrapped DoD line joins its item
  PASS: a wrapped DoD line does not start a new item
...
SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass
rc=0
```
Raw: `test.log` (21 PASS lines, 0 failures).

## Clause 4 — row-deletion falsifier (PASS)

Scratch checkout at 5006a6b7 (`.worktrees`-style second worktree), gaming.md restored with `git checkout --` between states:

```
baseline:  IN SYNC — 161 plan tickets, 202 board tickets, ...            rc=0
delete GB-196 row:
DRIFT GB-196: on the board but NOT in gaming.md §8 (orphan #477) — add it to §8 or close it

1 drift item(s); run with --fix to push plan fields to the board
rc=1
delete GB-197 row (after restoring the GB-196 row):
DRIFT GB-197: on the board but NOT in gaming.md §8 (orphan #478) — add it to §8 or close it

1 drift item(s); run with --fix to push plan fields to the board
rc=1
restored:  IN SYNC — 161 plan tickets, 202 board tickets, ...            rc=0
```
Raw: `fals_base.log`, `fals_del196.log`, `fals_del197.log`, `fals_restored.log`.

## Clause 5 — suite (PASS)

```
$ make host_tests
...
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0
```
Raw: `host_tests.log` (1m35s).

## Clause 6 — --adopt-orphans idempotent (PASS)

```
$ python3 tools/board/sync_gaming_md.py --adopt-orphans
IN SYNC — 161 plan tickets, 202 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
rc=0
```
No new rows: `git status --short` after the run was empty. Raw: `adopt.log`.

## Verdict

All six clauses pass at 5006a6b7. The document check is not a no-op: deleting either adopted row reproduces its own `DRIFT` line and rc=1. GREEN.
