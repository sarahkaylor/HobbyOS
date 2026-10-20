# GB-044-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-20.
Tree: `origin/main` @ `98247dd` (clean worktree; `HEAD == origin/main`).
Gate: both DoD commands, plus the producer they test (`tools/gate/parity_ci.sh`) run directly.

## 1. `bash tools/gate/parity_ci_test.sh` — PASS, 7 rows (the DoD's "8/8" is a miscount)

```
$ bash tools/gate/parity_ci_test.sh                 # 01_parity_ci_test.log
--- 1. both arches green -> PARITY PASS ---
  PASS: two green arches -> PASS
--- 2. arm red -> PARITY FAIL ---
  PASS: arm red -> FAIL
--- 3. intel red -> PARITY FAIL ---
  PASS: intel red -> FAIL
--- 4. window over budget -> PARITY FAIL ---
  PASS: window over budget (2s > 1s) -> FAIL
--- 5. concurrent window <= sequential window (run window < best) ---
  parallel window=2s sequential window=4s
  PASS: parallel (2s) <= sequential (4s)
--- 6. real t3-verdict wiring (no QEMU) ---
  PASS: clean log both arches -> PASS
  PASS: intel over FAIL-injected log -> PARITY FAIL

PARITY_CI_TEST: PASS — parity verdict + run-window budget behave correctly
EXIT=0
```

Every negative the DoD names is proven to fail (arm-red, intel-red, over-budget), and the budget
relation is quoted: **parallel 2 s ≤ sequential 4 s**. There are **7** PASS rows across 6 cases, not 8 —
same miscount GB-005-V recorded; it is a wording defect, not a green/red question.

## 2. `make test_parity_ci` — executed, exit 0

```
$ make test_parity_ci                               # 02_make_test_parity_ci.log
bash tools/gate/parity_ci_test.sh
… (same 7 rows) …
PARITY_CI_TEST: PASS …
EXIT=0
```

The recipe line and its output both appear, so the target ran rather than merely built.

## 3. The failing check: the thing this CI is for cannot run on `main`

GB-044 is "two-arch parity CI **per farm VM on every merge**". On this sha the producer refuses and
there is no CI wiring at all:

```
$ bash tools/gate/parity_ci.sh                      # 03_parity_ci_real.log
PARITY: no merge gate on this tree (land GB-005 first) and no PARITY_ARM_CMD/PARITY_INTEL_CMD
supplied — refusing rather than silently passing.
EXIT=2

$ ls .github/workflows
(no such directory)
$ grep -rn parity_ci --include=*.yml --include=*.yaml .
(no CI file references it; only Makefile:3494-3498)
```

So the shipped tree has: a verdict/budget **self-test** that passes, a `parity_ci` target that calls a
script which exits 2, and **no workflow** that runs it per merge. The DoD rule for this ticket is
explicit — "a green self-test alone … is not a verdict" — and this is exactly that case: the tested
producer has never produced a verdict on `main`.

Corroboration, not substitution: the parity verdict could not be green even if wired, because both
tier legs are red on this program's own measurements (GB-012-V: T3 arm 2 FAIL lines / no halt; T3
intel 5 FAIL lines + 48 memory faults).

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | 8/8 PASS incl. every negative failing | PASS in substance (7/7 rows; negatives fail as required). "8" is a miscount |
| 2 | concurrent ≤ sequential window, both quoted | PASS — 2 s ≤ 4 s |
| 3 | `make test_parity_ci` executed | PASS — recipe line + output |
| 4 | negatives, not just the green case | PASS — three negative cases |
| — | **the per-merge two-arch parity CI exists and runs** | **FAIL** — `parity_ci.sh` exits 2 ("no merge gate on this tree"); no `.github/workflows`; nothing invokes it on a merge |

## What has to land before a re-verify

1. GB-005's merge gate on `main` (`tools/gate/merge_gate.sh` + targets) — `parity_ci.sh` hard-depends
   on it and refuses without it.
2. A workflow (or farm-side hook) that actually runs `parity_ci` on every merge per farm VM.
3. Green tier legs to make the parity verdict meaningful (GB-012 items).
4. Fix the "8/8" wording to the real row count at the next re-plan.
