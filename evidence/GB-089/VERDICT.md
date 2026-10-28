# GB-089 — null-entry control: fixed-state verification on current main (2026-10-28)

Claimed under the coordinator's critical-path reserve (global `Ready`, M1 path).
The control + make target are already on `main` (#110/#114); the fix `a53f26e`
(kernel_exit in test_wave_loader, `src/kernel/main.c:190-192`) is also on main.
So the assertable DoD clause today is the **fixed** state; the **unfixed** controls
were evidenced 2026-10-24 (all four measurements) and the gate DETECTS the tree
state, refusing a mislabeled STATE.

## Fixed-state gate run (make target, never by hand)

Current `main` (branch ticket/89-nullentry-control off origin/main).

```
$ make t3_nullentry ARCH=intel STATE=fixed
  t3_nullentry: tree has the a53f26e fix (STATE=fixed)
  t3_nullentry (intel, SMP=1, HELLO.BIN, fixed)
  halted=1 fault_lines=0
  t3_nullentry: PASS — fixed run halted cleanly (a53f26e verified).

$ make t3_nullentry ARCH=arm STATE=fixed
  t3_nullentry: tree has the a53f26e fix (STATE=fixed)
  t3_nullentry (arm, SMP=1, HELLO.BIN, fixed)
  halted=1 fault_lines=0
  t3_nullentry: PASS — fixed run halted cleanly (a53f26e verified).
```

Tier logs (obj/t3_*_nullentry.tier.out):
```
T3_intel: 120 console lines, 0 FAIL lines, halted=1
T3_arm:   66 console lines, 0 FAIL lines, halted=1
```
(The "only 1 programs exited (floor 55)" line is expected: this gate boots a
single-program manifest at T3_SMP=1 by design — the full-wave floor does not
apply to the null-entry control, which the gate interprets as halted=1 + 0 faults.)

## DoD mapping

- "with a53f26e it reaches halted=1 on both" — **PASS on both arches** (this run).
- "exits non-zero naming vector=13 (intel) / EL1/ELR=0 (ARM) on main-no-fix" —
  evidenced 2026-10-24 (wabi, PR #110); the gate DETECTS tree state so running
  STATE=unfixed against today's fixed main is correctly refused.
- "executed by a make target inside the gate, never by hand" — `make t3_nullentry`
  (on main, Makefile:3606).

GB-089 → **In Review** with this evidence.
