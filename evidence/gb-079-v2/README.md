# GB-079-V (re-open) — verification evidence (DEEP Verifier, 2026-10-23)

Subject: GB-079 re-opened (#123) "t4_device --check catches a probe change in the default mode" — verdict **GREEN**.
Revision graded: **`9e107c3`** on `ticket/079-t4-both-modes` (PR #79, not on `origin/main`).
Worktree `/tmp/gb079c`, fresh; tree clean at the end of the matrix (0 modified/untracked).

Run with `gb079c_matrix.sh` (in this directory), which is the card's six cases verbatim, each starting from a
clean probe and no `obj/t4_legA.out`, with the injection placed before `prow("write3")` as specified.

## The matrix — raw exit codes

| case | what | want | got | mode named by the verdict line |
|---|---|---|---|---|
| 0 | `--self-test` at head | 0, `fail_count=0` | **0**, `fail_count=0` | — |
| 1 | healthy, leg A absent | 0 | **0** | `PASS (mode=source)` |
| 2 | healthy, leg A present (39-row capture) | 0 | **0** | `PASS (mode=source+emitted)` |
| **3** | **injected row, leg A present — THE GATE** | **non-zero** | **1** | `FAIL (mode=source+emitted)` |
| 4 | same injection, leg A absent | non-zero | **1** | `FAIL (mode=source)` |
| 5 | probe restored, capture missing one row | non-zero, `STALE` | **1** | `FAIL (mode=source+emitted)` |
| 6 | all restored, `--self-test` | 0, `fail_count=0` | **0**, `fail_count=0` | — |

### Case 3 (the gate) — what it actually said
```
  injected: 1 line(s) in src/user/t4_probe.c
rc=1
  mode=emitted: PASS
  FAIL: recorded leg A is STALE — it disagrees with the device probe's source:
        -bogus_unconditional
t4_device --check: FAIL (mode=source+emitted)
```
This is the exact scenario that was a silent PASS on `82ecc29`: probe edited, capture present, check green.
Now the capture is compared to the probe source, the disagreement is named, and the verdict is a FAIL.

### Cases 4 and 5 — two other directions still fail
```
CASE 4: FAIL: device probe emits extra row 'bogus_unconditional' not in golden [mode=source]   rc=1
CASE 5: FAIL row: dent_have_charlie (device=0 host=1) [mode=emitted] — missing from a probe     rc=1
        FAIL: recorded leg A is STALE — it disagrees with the device probe's source             rc=1
```
So the check fails on a probe-added row, a missing golden row, and a stale capture — three distinct
directions, not one hard-coded trip.

### The self-test asserts the gate's own failure path
```
$ bash tools/gate/t4_device.sh --self-test
  OK — injected probe row FAILS (leg A present)
self-test result: fail_count=0
```
That line is the check proving it can fail — and `fail_count=0` at both the head and after the matrix.

## Wiring (T1), checked rather than assumed
```
$ make t4_device_check
  mode=emitted: PASS
  PASS: recorded leg A agrees with the probe source (39 golden rows, no drift)
  t4_device --check: PASS (mode=source+emitted)
rc=0
$ grep -n host_tests Makefile | ...
host_tests: … $(T4_DEVICE_SELFTEST) $(T4_DEVICE_CHECK) $(T4_W0_SELFTEST) …
tools/gate/merge_gate.sh:72:  T1_host : make host_tests
```
`t4_device_check` is a `host_tests` dependency, so T1 and the merge gate both run it; `make t4_device_check`
itself grades both modes (it materialises leg A), which is why the wiring path is the stronger of the two.

## Why this is a GREEN and not a "looks green"
The gate was graded in the mode that used to be blind (case 3), it failed there for a nameable reason, and
the same script re-ran the healthy tree to PASS afterwards (cases 1/2/6). A check that cannot fail is not
evidence; this one failed three ways on demand and passed when the tree was clean.

## Not on main yet
`origin/main` is `b3082f8`; this fix is branch-only at `9e107c3`. The GREEN grades the branch head, so the
merge is what puts it in the shipped tree — say that in the merge commit or the next `-V` will grade `main`
and find the old blind mode.
