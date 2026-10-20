# GB-005-V — gate run manifest

- Verifier: DEEP verifier session, DeepSeek v4.1 Flash (dgx1), worktree `hermes-a693d109`
- Date: 2026-10-20 (UTC)
- Revision run: **2ffa3be3bd4c45c8d791f70c6755be2ccb06ab6e** — `HEAD` == `origin/main`, worktree clean
- Host: this VM (Linux), QEMU TCG. `PATH` includes `/usr/sbin:/sbin`.

## Ticket gate, run exactly as written

| # | command | result | log |
|---|---|---|---|
| 1 | `bash tools/gate/parity_ci_test.sh` | exit 0 — 7/7 PASS (script asserts 7 cases, ticket says "8/8") | `02-parity-ci-selftest.log` |
| 2 | `make test_parity_ci` | exit 0 — recipe line `bash tools/gate/parity_ci_test.sh` appears in output, terminal line `PARITY_CI_TEST: PASS` | `03-make-test-parity-ci.log` |
| 3 | dummy branch, break one graded row then revert | BEFORE exit 1 naming `401:SHELLTEST FAIL: expected 42 got 7`; AFTER exit 0 | `04-dummy-branch-fail-pass.log`, `logs/` |
| 4 | wiring state on `origin/main` | **ABSENT** — `bash tools/gate/parity_ci.sh` with no runner exits **2**: "no merge gate on this tree (land GB-005 first)" | `01-parity-ci-no-runner.log` |

## Provable-failure checks (the honesty law)

- Mutation test: blinded `tools/t3_tier.sh`'s FAIL detector (`FAIL_LINES=0`, temporary, reverted) →
  `parity_ci_test.sh` goes **FAIL** at case 6 ("FAIL-injected intel should make parity FAIL"), exit 1.
  So the self-test grades a real instrument and is not vacuously green — `05-mutation-selftest-must-fail.log`.
  Reverted with `git checkout -- tools/t3_tier.sh`; `git status` clean afterwards.

## Wiring audit (DoD item 4)

At `2ffa3be` (`origin/main`):

- `git ls-files | grep -E 'merge_gate|pull_request_template|\.github/'` → **(none)**
- Makefile has `parity_ci:` (3494) and `test_parity_ci:` (3497) — both GB-044 — and no
  `merge_gate:` / `merge_gate_ci:` / `test_merge_gate:` targets.
- `tools/gate/parity_ci.sh` therefore has no default runner and refuses (exit 2) rather than passing.
- PR sarahkaylor/HobbyOS#43: `state=open merged=false mergeable=false mergeable_state=dirty head=51e033a`.

GB-005's deliverable (`merge_gate.sh`, its self-test, the PR template, the merge_gate Makefile
targets) exists only in that dirty PR. So the half that *is* on main (GB-044's parity-CI verdict
plus window budget) is green and provably gradeable; the half GB-005 owes is not on main.

## Context that is not a gate result

- T1 `make host_tests` at the same sha: **ALL APPS SUITE TESTS PASSED / TEST EXIT: 0**
  (`06-t1-host-tests.log`). The first attempt failed with
  `src/host/rtti_test.cpp:29:10: fatal error: 'typeinfo' file not found` because a fresh worktree
  lacks the gitignored vendored trees (gaming.md §11.2b #4); fixed with
  `tools/swarm/seed-worktree-vendored.sh`, then the run above. Recorded so nobody reads that red
  as a code failure.
- QEMU T3 both arches was **not** run: the only gate on main that boots a guest is inside
  `merge_gate.sh`, which is exactly the artefact that is absent. Its absence is the verdict, and
  running a QEMU wave would not change it.

## Verdict

**RED** — DoD item 4: "wiring not on `main` ⇒ RED, not GREEN". GB-005 returns to In Progress with
the missing artefact named; the parts that are on main pass and are proven able to fail.
