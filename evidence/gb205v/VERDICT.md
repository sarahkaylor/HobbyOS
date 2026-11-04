VERDICT: GREEN sha=5db0524a

**GREEN — GB-205-V @ `5db0524a` (PR #375, one commit on `a5c5c02f`; `gaming.md` +2, the GB-204 row).** Independent run on this VM; all seven clauses quote the tool output, logs committed under `evidence/gb205v/` on `verify-gb205v`.

**Clause 1 — rows landed.** `gaming.md`:

```
476:| GB-205 | **plan §8 is one row short — GB-204 is an orphan; run --adopt-orphans** | S | — | ...
576:| GB-204 | **panic sink: one atomic claim per record and a counted drop before the cap — the GB-097 §3 split (clauses 1.2/1.3)** | S | GB-097 ... | ...
```

`git diff --stat a5c5c02f..HEAD` → `gaming.md | 2 ++` (GB-205's row was already on main; this PR adopts GB-204's).

**Clause 2 — IN SYNC.**

```
IN SYNC — 166 plan tickets, 209 board tickets, milestones ['M0-infra', 'M1-abi-core', 'M2-devices-sdl', 'M3-games', 'M4-acceptance']
```

rc=0, no `DRIFT` line.

**Clause 3 — one row per id.** `grep -cE '^\| GB-204 \|' gaming.md` → `1`; `grep -cE '^\| GB-205 \|'` → `1`. No duplicate rows.

**Clause 4 — tests, including the doubled-row fixture.**

```
SYNC_GAMING_MD_TEST: PASS — seed gaps/duplicates fail, sparse mints pass
```

**Clause 5 — falsifier (delete a row in a scratch checkout at the head).** GB-204 row deleted:

```
DRIFT GB-204: on the board but NOT in gaming.md §8 (orphan #495) — add it to §8 or close it
1 drift item(s); run with --fix to push plan fields to the board
rc=1
```

restore → `IN SYNC` rc=0; GB-205 row deleted:

```
DRIFT GB-205: on the board but NOT in gaming.md §8 (orphan #496) — add it to §8 or close it
1 drift item(s); run with --fix to push plan fields to the board
rc=1
```

restore → `IN SYNC` rc=0 (all three states in `evidence/gb205v/falsifier_*.log`).

**Clause 6 — suite.**

```
Checks run:    482
Checks failed: 0
TEST EXIT: 0
```

**Clause 7 — `--adopt-orphans` is idempotent.**

```
IN SYNC — 166 plan tickets, 209 board tickets, ...
```

rc=0, `git diff --stat` empty afterwards, counts still 1/1 — no duplicate inserted.

GREEN → GB-205 to Merge Gate; GB-205-V to Done.
