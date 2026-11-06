# GB-212-V — independent gate, swarm-farm-1, 2026-11-06

Head graded: `2962e574` (PR #384, one commit on `58dbf59a`). Branch `verify-gb212v`.

## Result: RED — clauses 1–3 (live-board drift); clauses 4–6 pass

- `--check` on the head → `DRIFT GB-213 ... orphan #520`, `DRIFT GB-214 ... orphan #521`,
  rc=1. GB-213 was minted 12:36:29Z and GB-214 12:51:55Z; the head commit is 12:12:57Z —
  both cards post-date the graded tree.
- `--adopt-orphans` → `ADOPTED 2 row(s)` (GB-213, GB-214), then IN SYNC; second run
  adopts 0. The DoD expected no ADOPTED line.
- Deleted-GB-212 falsifier → `ADOPTED 3 row(s)` (GB-212 + the two live orphans), not 1.
- `sync_gaming_md_test.py` PASS; host_tests 0 failed / ALL APPS / TEST EXIT 0; intel
  link rc=0; `--check --fix` rc=2 with `argument --fix: not allowed with argument
  --check`; `--help` shows `[--check | --fix | --adopt-orphans]`.

Fix: adopt GB-213/GB-214 rows and re-gate at the new head; the tool itself is correct.
