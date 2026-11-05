# GB-208-V2 — independent re-gate, swarm-farm-1, 2026-11-05

Head graded: `8939c1d` (PR #376, one commit on `d66d3e80`). Branch `verify-gb208v2`.

## Result: GREEN

- Both places now read: merged as `ddf65257` onto `0a5c3f2e` (M3 cell and
  the GB-208 §8 row); the old phrase `merged 0a5c3f2e` count = 0.
- `sync_gaming_md.py` → `IN SYNC — 169 plan tickets, 213 board tickets`, rc=0
  twice (idempotent).
- GB-145/146/204/206/207 rows each appear once.
- `sync_gaming_md_test.py` PASS; `make host_tests` 482/0; both arch links rc=0.
- Falsifier: rewriting both phrases back to `merged 0a5c3f2e` makes the
  clause-1 grep fail (2 bad, 0 good); restore returns 0 bad / 2 good.
