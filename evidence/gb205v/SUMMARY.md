# GB-205-V — independent gate, swarm-farm-1, 2026-11-04

Head graded: `5db0524a` (PR #375, one commit on `a5c5c02f`). Branch `verify-gb205v`.

## Result: GREEN

- `gaming.md` §8 has GB-205 (M0, line 476) and GB-204 (M1, line 576), one row each.
- `sync_gaming_md.py` → `IN SYNC — 166 plan tickets, 209 board tickets`, rc=0.
- `sync_gaming_md_test.py` → `SYNC_GAMING_MD_TEST: PASS`.
- Falsifier: deleting the GB-204 row → `DRIFT GB-204 ... orphan #495`, rc=1; deleting
  the GB-205 row → `DRIFT GB-205 ... orphan #496`, rc=1; restore → IN SYNC rc=0.
- `--adopt-orphans` idempotent: IN SYNC, no diff, counts still 1/1.
- `make host_tests` 482/0.
