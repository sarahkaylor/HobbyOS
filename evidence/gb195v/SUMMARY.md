# GB-195-V — independent gate, swarm-farm-1, 2026-11-04

Head graded: `28456cea` (PR #361, one commit on `ddf65257`). Worktree
`hermes-ae8feebb`, branch `verify-gb195v`.

## Results

- `git diff --stat origin/main...HEAD`: only `Makefile` (+9/-1) and
  `tools/gate/t3_resume_page.sh` (+102); no `src/kernel/process.c`.
- `grep -c 'GB-122 (fix)' src/kernel/process.c` = 0; same on `origin/main` = 0.
- `make t3_resume_page_selftest` rc=0:
  - clean: `resume=0 unattributable=0 total_vector14=0`
  - violation: `resume=1`
  - shredded: `unattributable=1`
  - absent: `rc=2`
  - `SELF-TEST PASS`
- `bash tools/gate/t3_resume_page.sh /nonexistent/absent.log` → rc=2,
  `absent: an absent serial is not a passing gate` (`absent.out`).
- Clean fixture (`clean.log`, the self-test's own) → `resume=0` rc=0.
- Resume fixture (`resume.log`, the self-test's own) → `resume=1` rc=1.
- `grep -c t3_resume_page tools/t3_tier.sh` = 0.
- `make host_tests` 482/0, `TEST EXIT: 0`; both arch links rc=0.

## Verdict

`VERDICT: GREEN sha=28456cea`.
