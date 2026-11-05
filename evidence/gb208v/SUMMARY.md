# GB-208-V — independent gate, swarm-farm-1, 2026-11-05

Head graded: `d66d3e80` (PR #376, base `f337e7a3`). Branch `verify-gb208v`.

## Result: RED — one §7 cell

- Rows: GB-206/207/208 each appear once; `sync_gaming_md.py` → `IN SYNC — 169 plan
  tickets, 213 board tickets`, rc=0; `sync_gaming_md_test.py` → PASS.
- Falsifier: deleting the GB-208 row → `DRIFT GB-208 ... orphan #500`, rc=1;
  restore → IN SYNC.
- §7 M1/M2/M4 cells verify against their -V records (2d7605d / 7c8f16a6 /
  4cc60a4a; GB-190 matrix at 0a5c3f2e graded by GB-053-V2 at 42cbbb5c).
- §7 M3 says `merged 0a5c3f2e`; the record says the merge is `ddf65257`
  (`git show -s ddf65257` → parents `0a5c3f2e 42cbbb5c`). `0a5c3f2e` is the
  first parent/main, not the merge. The document's own convention uses the merge
  commit (`GREEN at X, merged Y`). One-word fix.
- `make host_tests` 482/0; both arch links rc=0.

GB-208 stays in In Review (open PR); GB-208-V to Done.
