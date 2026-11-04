# GB-200-V — VERDICT: GREEN

- Graded: PR #366 head **bddb1eb6b73b45147106bd7f441c0d4adb44c689** (`GB-200: adopt the GB-199 plan row — sync_gaming_md.py IN SYNC`)
- Base: `origin/main` 90a672e6 (ancestor of HEAD, checked)
- Host: swarm-farm-3; CI not consulted (policy 2026-10-26)
- Worktree: fresh detached worktree at the graded head

## Clause 1 — live plan IN SYNC at the head (PASS)

```
$ python3 tools/board/sync_gaming_md.py
IN SYNC — 163 plan tickets, 204 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
rc=0
```
Both open mints have rows: GB-200 at `gaming.md:474` (M0 table) and GB-199 at `gaming.md:575` (M1 table). Raw: `sync.log`.

## Clause 2 — row matches the card (PASS)

`gaming.md:575`, inside `### Milestone M1 — ABI core / ELF loader`:

```
| GB-199 | **enter ld.so: map the interpreter's DT_NEEDED closure, build the Linux startup stack, and name the first unhandled syscall/fault** | M | GB-196 (landed: PT_INTERP path + LD64.SO map + DT_NEEDED report) | ...
```

Weight `M` (wt-M), milestone M1, title matches GB-199. Board card #485 is open, lane **In Review**: `GB-199 enter ld.so: map the interpreter's DT_NEEDED closure, build the Linux startup stack, and name labels=In Review,abi,elf-compat,wt-M`.

## Clause 3 — row-deletion falsifier (PASS)

Scratch checkout at bddb1eb6; row deleted, then restored with `git checkout -- gaming.md`:

```
baseline:  IN SYNC — 163 plan tickets, 204 board tickets, ...   rc=0
delete GB-199 row:
DRIFT GB-199: on the board but NOT in gaming.md §8 (orphan #485) — add it to §8 or close it

1 drift item(s); run with --fix to push plan fields to the board
rc=1
restored:  IN SYNC — 163 plan tickets, 204 board tickets, ...   rc=0
```
Raw: `fals_base.log`, `fals_del.log`, `fals_restored.log`.

## Clause 4 — suite and links (PASS)

```
$ python3 tools/board/sync_gaming_md_test.py
...
SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass
rc=0

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
Raw: `test.log`, `host_tests.log`, `link_intel.log`, `link_arm.log` (the last commands in each link log are the `ld.lld` line, exit 0).

## Verdict

All four clauses pass at bddb1eb6. The sync check is not a no-op: deleting the GB-199 row reproduces `DRIFT GB-199 … orphan #485` rc=1. GREEN.
