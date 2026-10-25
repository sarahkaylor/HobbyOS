# GB-057 — second follow-up: the `halted=0` mechanism is named (parent WAIT_CHILD on a child wedged in a kernel #PF retry)

Builder: `wverify-deep`, DEEP verifier session, 2026-10-25 ~15:0xZ. Same revision `origin/main`
`41ec708`; every figure below is re-read from the three committed logs (`01/02/03-t3-intel.log`).
No new guest run, no code change. This does not claim the DoD is met.

## The mechanism, in one paragraph

In the stalled run (run 2) the only thing keeping the wave "alive" at the end is **slot 35
(`TORTURE.BIN`) parked in `PROC_STATE_WAIT_CHILD` with `parent=-1`**, waiting for its child **slot 7
(`TORTURE.BIN`)**, which is marked `RUNNING` and claimed by **CPU 2** but has not progressed past
`rip=0x1000000000` (user VA base) — because CPU 2 is taking a **`#PF` (vec=14, err=2, write) at kernel
`rip=0x7003BEDF` with `cr2=0x17FFFFFCB8`** on that same pid. Parent waits for child, child is stuck in
a kernel fault-retry, nothing else is runnable, and the scheduler's live-work predicate counts the
parked parent as live, so the tier never reaches "all done" and the wave never halts.

## The measurements

**(1) The parked-wait and its child, from the ALIVE-SET dump at line 16622-16629** (`14-unwakeable-waiter.log`):

```
[ALIVE-SET] slot=7  pid=7  st=3 parent=35  claimed_by=2  TORTURE.BIN
[ALIVE-SET] slot=35 pid=35 st=7 parent=-1  claimed_by=-1 TORTURE.BIN  <-- parked WAIT with no parent to deliver the wake
...
[KERNEL] parked WAIT slot with no parent to wake it: slot=35 pid=35 st=7 parent=-1 TORTURE.BIN -- no state change attempted; see GB-057
```

**(2) The child's CPU is faulting, not running it normally** — the 20 s-silent watchdog sample taken
immediately before the IDLESTUCK dump:

```
CPU2 rip=0x7003BEDF rflags=0x10046 vec=14 err=0x2 cr2=0x17FFFFFCB8 pid=7 resume_pid=7
```

`vec=14` is `#PF`, `err=0x2` is a **write** to a not-present page, `cr2` is a user-stack address, and
`pid=7` is exactly the child the parked parent is waiting for.

**(3) Nothing else in the machine is runnable, and no poll-park loop is involved** (`13-liveness.log`):

```
16091: [ALIVE-SET] no READY work, yet the scheduler counts live work (the predicate: anything not FREE/EXITED/ALLOCATED/THREAD_DONE), so no halt. Live slots:
16617: [ALIVE-SET] no READY work, ...
16734: [ALIVE-SET] no READY work, ...
...: [IDLESTUCK] claims: ... lock_owner=8 park_returns=0
state census of the last 40 IDLESTUCK slot lines: 34x st=9 (THREAD_DONE), 5x st=4 (EXITED), 1x st=7 (WAIT_CHILD)
"System halt" lines in run 2: 0
```

`park_returns=0` retires the `sys_poll` park-re-execution loop the code comments warn about, and it also
refines my previous comment: the five `syscall=76` (`SYS_POLL`) records are the **last syscall** of CPUs
that have since parked — not five cores spinning inside `poll`. The poll lead is therefore downgraded;
the deadlock above does not need it.

**Why run 1 and run 3 halt:** the same "no READY work" idle state occurs in them too (7 and 8 occurrences
respectively) and they still halt — because in those runs nothing is left in `WAIT_CHILD` at the end.
Run 2 differs by exactly one slot and one stuck child. That also explains a 1-in-3 stall frequency
without any timing race: it depends on whether the last `TORTURE.BIN` parent/child pair survives to the
end of the wave.

## Where the fix belongs (named, not applied)

1. **The live-work predicate** — `anything not FREE/EXITED/ALLOCATED/THREAD_DONE` counts `WAIT_CHILD`
   (`st=7`) as live. A `WAIT_CHILD` whose `parent`/`ppid` is `-1` can never be woken, so the wave can
   never be declared complete. Either exclude it, or reparent/reap it.
2. **The kernel already detects this exact state and deliberately does nothing**:
   `[KERNEL] parked WAIT slot with no parent to wake it: slot=35 pid=35 st=7 parent=-1 … -- no state
   change attempted; see GB-057`. That line is the fix site the card asked for.
3. **The `#PF` retry at `rip=0x7003BEDF` for pid 7** is the other half: the child never exits, so the
   parent is right to wait. Whatever wedges that fault (cr2 `0x17FFFFFCB8`, write, user stack) has to be
   resolved or the child has to be killed — a fault-retry that cannot make progress should end the
   process, not spin.

## DoD status

* (a) 3 consecutive `halted=1`: **still NOT met** at `41ec708` (2 of 3), but the failure is now
  mechanism-named rather than "the stall", and the fix sites are concrete.
* (b) delivered (75 / 256 / 83 real, run-2 number caveated as infra-inflated).
* (c) SEDTEST → GB-093 ✓, TSORTTEST → GB-095 ✓; intel CMPTEST, SIGTEST, TACTEST still uncarded.

Card stays `In Progress`; no verdict, no lane move, no rotation marker (still open).
