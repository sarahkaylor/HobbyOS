# TEMP characterization — l8-famine lane (flagged; revert after follow-up lane fixes it)

**Status: TEMP / continuation artifact. This file documents a SECOND,
distinct kernel stall class discovered while verifying the famine fix.
It is NOT fixed here. Revert this commit (or delete this file) once the
follow-up lane that fixes the class lands. Do not merge as-is.**

## Class A — the famine wedge (FIXED on this branch)

Root cause and fix shipped in cf860fc + e0650ee + 0e1ebd6. Summary:

- The loader's create-retry loop spun 100 ms x 18000 (~30 min) per spawn
  under a full 63-slot table, and every failed attempt printed an
  UNTHROTTLED "no free process slots" line. Measured: 138k flood lines +
  145k "acquiring lock" lines in one 35-min run; zero process exits for
  the middle 25 min — the print storm saturated the UART and stalled the
  very exits that free slots. Wedge: make soak stuck at 1 round.
- Fix: bounded retry (child 20 s / boot 30 s budgets, ~10 ms park);
  rate-limited slot forensics (throttled to first events, with zombie
  counts); per-call trace prints gated; progress-based early-exit for
  CHILD spawns only (a boot load must never bail — it has no caller to
  retry; observed consequence of bailing: TORTURE's final-wave boot load
  skipped and MODE=soak ran as a plain wave, clean halt, zero [SOAK]
  rounds — fixed in 0e1ebd6).
- Verified: unit-arm 293 PASS; wave green at 18/18 suites with 0 FAIL
  tokens; soak attempt reached 337 rounds with violations=0 and ZERO
  flood lines (the old kernel wedged at 1 round).

## Class B — IDLESTUCK lost-owner freeze on a claimed RUNNING slot (NOT fixed; TEMP)

Observed on 2 of 3 post-fix soak runs: soak froze at round 337 and round
186; rounds stop permanently; the run burns the full 2100 s cap.

Forensics (from the kernel's own [IDLESTUCK] table dump, which fired
exactly as designed):

    [IDLESTUCK] slot=1 pid=1 st=3 parent=13 TORTURE.BIN
    ... every other slot st=9 (THREAD_DONE) ...
    [IDLESTUCK] claims: c0=-1 c1=-1 c2=-1 c3=1 c4=-1 c5=-1 c6=-1 c7=-1

Interpretation:
- Slot 1 (reused for TORTURE's exec children each round) is in
  PROC_STATE_RUNNING (st=3) and is CLAIMED by c3 (cpu_current_pids[3]==1).
- But c3 is itself IDLING (it is the core printing the IDLESTUCK dump).
  The claim was never cleared when c3 stopped "running" pid 1 — i.e.,
  the wake was consumed by the switch machinery but the body never ran,
  and the schedule-to-idle path did not clear the claim.
- The LOSTWAKE reaper (process.c ~2523) only reclaims RUNNING processes
  with NO claiming CPU. Because c3 still claims pid 1, the reaper skips
  it (claimers=1) even though the claiming CPU is idle. A THREAD_DONE /
  stale-claimed slot is never reclaimed, so the freeze is permanent.
- The wave + all TORTURE threads drain to THREAD_DONE behind it; nobody
  runs; all cores idle-cycle printing the dump; rounds frozen; the run
  only ends when `timeout 2100` fires (rc 124; no System halt).

Event immediately preceding both freezes: a TORTURE exec-cycle fork
("sys_fork: tf->regs[0] is now 1" -> exec child in slot 1), with the
same op sequence elsewhere in the soak working fine thousands of times —
so this is a rare lost-wake/lost-claim race in the wake/switch machinery,
not a deterministic op.

Fix candidates for the follow-up lane (NOT implemented here; needs a full
soak verify and both-arch consistency):
1. Idle-loop claim hygiene: when a CPU enters the idle loop it should
   clear its own cpu_current_pids claim (it is by definition not running
   anything), or the LOSTWAKE reaper should treat claims from CPUs that
   are currently executing the idle loop as absent (per-CPU idle flag).
2. Reclaim RUNNING slots whose claiming CPU is idle: extend the reaper's
   "no claimer" gate to also reclaim when every claimer has idle==1,
   mirroring the existing LOSTWAKE grace reasoning (the comment at
   process.c ~2508 documents the exact signature: "the wake was consumed
   by the switch machinery but the body never runs").

Why it is not the famine class: violations stayed 0, the flood was
throttled to 3 lines, matches the documented x64 IDLESTUCK family in
hobbyos-kernel-constraints ("idle frozen", the [IDLESTUCK] storm), and it
was latent at base (base 806-round clean run showed IDLESTUCK 0) — the
famine fix EXPOSED it by letting soaks run far enough to hit the race.

Evidence logs (this lane): /tmp/l8_soak1.log (freeze @337),
/tmp/l8_soak3.log (freeze @186), /tmp/l8_soak2.log (invalid — boot-load
skip, fixed in 0e1ebd6). Clean baseline: /tmp/w4_soak2.log (806 rounds,
IDLESTUCK 0).
