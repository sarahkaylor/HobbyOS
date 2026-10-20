# GB-005-V — verification evidence (DEEP Verifier, 2026-10-20)

Subject: GB-005 (#10) "Git workflow + merge-gate CI" — verdict **RED**.
Revision run: `2ffa3be3bd4c45c8d791f70c6755be2ccb06ab6e` (`origin/main`, `git rev-parse HEAD` in a clean worktree).

| # | file | what it is | exit |
|---|---|---|---|
| 01 | `01-parity_ci_test.sh.log` | `bash tools/gate/parity_ci_test.sh` — 7 PASS rows, `PARITY_CI_TEST: PASS` | 0 |
| 02 | `02-make-test_parity_ci.log` | `make test_parity_ci` — recipe line echoed (`bash tools/gate/parity_ci_test.sh`) + its output ⇒ executed, not just built | 0 |
| 03 | `03-parity_ci_default_path.log` | `bash tools/gate/parity_ci.sh` with no `PARITY_*_CMD` — refuses: "no merge gate on this tree (land GB-005 first)" | 2 |
| 04 | `04-make-host_tests.log` | `make host_tests` — T1 on main: 482 checks, 0 failed, `ALL APPS SUITE TESTS PASSED` (after the §11.5 one-time staging of `third_party/libcxx-21.1.8` + `llvm-21.1.8/dist-staging`) | 0 |
| 05 | `05-dummy-branch-RED.log`, `05-dummy-RED-{arm,intel}.log`, `05-dummy-RED-intel-raw.log` | dummy branch `verify/gb005-dummy` @ `7ad7f53` with one graded row broken: parity gate **FAIL**, `intel exit: 1 (RED)`, `T3_intel: FAIL — 1 FAIL line(s)`, row `1101:SHELLTEST FAIL` named | 1 |
| 06 | `06-dummy-branch-GREEN.log`, `06-dummy-GREEN-intel.log` | same branch @ `50ab94f` with the break reverted: **PASS**, both arches green | 0 |
| 07 | `07-pr43-state.log` | GitHub PR #43: open, head `ticket/05-merge-gate` @ `51e033a7`, base `main` @ `71645f7`, 5 files; `tools/gate/merge_gate.sh` → HTTP 200 on PR head, **404 on main** | — |

Decisive failing check (DoD item 4 of #80): the merge-gate wiring is **not on `main`** —
`tools/gate/merge_gate.sh`, `tools/gate/merge_gate_test.sh`, `.github/pull_request_template.md`
and the `Makefile` `merge_gate`/`merge_gate_ci`/`test_merge_gate` targets are all absent from
`git ls-tree -r origin/main`. What *is* on main is GB-044's parity layer, which is what the
first two gate commands exercise; that layer cannot substitute for the gate GB-005 owes.

Note on the ticket text: #80 says `parity_ci_test.sh` reports "8/8 PASS". The script prints
**7** `PASS:` rows (`grep -c '  PASS:'` = 7). All 7 pass, so this is a counting error in the
ticket, not a gate failure — recorded so the next reader does not chase a phantom eighth row.
