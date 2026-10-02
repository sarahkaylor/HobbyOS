# Class-B IDLESTUCK lost-owner freeze — RESOLVED (l8-idlefix lane)

**Status:** FIXED on branch `browser/l8-idlefix` (commits `5eb9309` + `a10fdf9`, base `b502c65`).
Supercedes the l8-famine lane's TEMP doc (`docs/browser/l8-famine-TEMP-idlestuck-class.md`) for the class-B mechanism; that doc's class-A (claimless RUNNING) analysis remains valid.

## Root cause (confirmed by gated forensics)

A TORTURE exec-child slot (`pid==slot`, state RUNNING) claimed by a CPU that then
wedges permanently IRQ-off in the claim->resume window (between the claim and
`eret`; never prints a fault, per-core timer never fires again) pins the slot
forever:

- `[IDLESTUCK]` forensics: `claims: c3=1`, heartbeat of c3 frozen (1554 ms vs.
  ~4 ms for the other 7 CPUs), `loc=2` (switch-pick). Same signature in the
  frozen `/tmp/l8_soak1.log` (freeze@337) and `/tmp/l8_soak3.log` (freeze@186).
- The LOSTWAKE reaper only reclaimed RUNNING slots with **no** claiming CPU, so
  the stale claim was never requeued/reaped; the world drained (parent parked in
  waitpid, everything else THREAD_DONE), rounds froze, no System halt, the soak
  burned the full 2100 s cap.
- The famine/retry fix (cf860fc..0e1ebd6) merely exposed the latent race.

## Why re-READY was the wrong recovery (empirical)

First attempt (re-READY the stale-claimed slot) RECOVERED nothing: every re-pick
of the wedged child deterministically wedged the picking CPU again — the resume
poison travels with the child's address space (its guest MMU state). One soak
burned 8 CPUs in a cascade (claims moved c3 -> c7 -> c2 -> c1 -> ...) before
silence, with zero progress.

## The fix (landed)

The idle-loop LOSTWAKE reaper (>= 500 idle rounds, under proc_lock) now
recognizes a RUNNING slot whose **every** claimer's heartbeat is
`>= LOSTWAKE_DEAD_OWNER_MS` (2000 ms) stale, gated on the rest of the table
being drained (the exact freeze signature), and **disposes** it instead of
re-READYing:

1. release the zombie claims (`set_current_process_pid(c, -1)`),
2. `group_teardown(process_group(slot), 97)` — frees the poisoned AS/ASID,
   marks the slot EXITED, and wakes the parked WAIT_CHILD parent with a waitpid
   reap whose status reads as the exec-rejection sentinel (exit 97).

The parent's `exec_cycle` treats 97 as a tolerated rejected exec and reforks a
fresh child on a fresh ASID. Rounds continue, violations stay 0, no cascade
(at most one CPU is lost per freeze event).

## Evidence (current commit tree)

- Live recovery soak `/tmp/l8_dispose_soak1.log`: wedge at ~round 199 -> exactly
  one `[LOSTWAKE] disposing pid=1 TORTURE.BIN` -> parent wakes (`sys_fork`, `v2
  loader: image mapped`) -> rounds continue to **800**, `System halt from CPU
  7.`, `violations=0`, `exec_rejected=0`, suite_ms=1680938 (~28 min, i.e. the
  internal cap, NOT the 2100 s timeout). Wave-phase flakes only:
  `shell_test2: FAILED ping execution`, `shell_test3: FAILED cat absolute path`
  (documented wave-pressure class, sibling lane's gate).
- Unit regression `test_lostwake_stale_claim_reclaim` (process_test.c): decision
  `lostwake_stale_claim_reclaimable_at()` — fresh vs stale claimer, busy
  RUNNING/READY tables, no-claimer and non-RUNNING cases. ARM 294/294, x64
  296/296, cstyle clean.
- Note: a clean (no-wedge) run of the new tree is byte-identical in the reaper
  path to the old tree (the disposal never fires without a wedge), so the
  earlier clean 805-round receipt remains representative.
