VERDICT: RED sha=2962e574

**RED — GB-212-V @ `2962e574` (PR #384). Clauses 1–3 fail at the graded head: the live board gained two cards after this commit, so `--check` prints DRIFT and `--adopt-orphans` adopts 2 rows. Clauses 4–6 pass, and the `--check` flag itself works. GB-212 stays in In Review (open PR, WORK-POLICY §2).**

**Clause 1 — `--check` FAIL.** On `2962e574`:

```
DRIFT GB-213: on the board but NOT in gaming.md §8 (orphan #520) — add it to §8 or close it
DRIFT GB-214: on the board but NOT in gaming.md §8 (orphan #521) — add it to §8 or close it

2 drift item(s); run with --fix to push plan fields to the board
rc=1
```

**Clause 2 — `--adopt-orphans` FAIL (expected no ADOPTED line).**

```
ADOPTED 2 row(s):
  | GB-213 | **STARDUST has no T5 replay: ...** | M | GB-038 ...
  | GB-214 | **a fresh worktree cannot build disk.img: the ICU host stage ...** | S | — ...
IN SYNC — 173 plan tickets, 218 board tickets, ...
```

A second run adopts 0 and prints IN SYNC (idempotent after the adoption), but the first run is not the "no ADOPTED line" clause.

**Clause 3 — deleted-row falsifier FAIL (count).** Deleting the `| GB-212 |` row and running `--adopt-orphans`:

```
ADOPTED 3 row(s):
IN SYNC — 173 plan tickets, 218 board tickets, ...
```

The DoD expects `ADOPTED 1 row(s)` naming GB-212; with GB-213/GB-214 live orphans the tool adopts three. The adoption itself is correct — the count is the live board's.

**Timing — this is board movement, not a PR omission.** PR #384 head commit `2962e574` is dated 2026-11-06 12:12:57Z. GB-213's first board comment is 12:36:29Z and GB-214's is 12:51:55Z — both after the head. A head cannot contain cards minted after it; the `--check` flag is doing its job by reporting them.

**Clause 4 — tests PASS.**

```
SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass
rc=0
```

**Clause 5 — suite/link PASS.**

```
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
make ARCH=intel hobbyos.elf rc=0
```

**Clause 6 — tool-change check PASS.**

```
$ sync_gaming_md.py --check --fix
usage: sync_gaming_md.py [-h] [--check | --fix | --adopt-orphans] [-v]
sync_gaming_md.py: error: argument --fix: not allowed with argument --check
rc=2
```

`--help` shows `[--check | --fix | --adopt-orphans]` with `--check  report drift only (the default; explicit for scripts)`.

**Fix:** run `--adopt-orphans` (it adopts GB-213/GB-214 plus GB-212 if the row is missing), commit the two rows, and re-mint this gate at the new head. The tool itself is correct; the tree is simply not in sync at `2962e574` because the board moved during the gate. No clause is relaxed.

RED → GB-212 stays in In Review; GB-212-V to Done.
