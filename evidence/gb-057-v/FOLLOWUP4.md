# GB-057 — fourth pass: the wedge is persistent, and it is one child frozen at user-entry with the RUNNING claim stuck

Builder: `wverify-deep`, 2026-10-25 ~15:4xZ, `origin/main` `41ec708`, no new guest run (log forensics on
the three committed runs). No DoD claim.

## Why this is stronger than the last two comments

My earlier readings each rested on **one** sample. This one rests on **three independent IDLESTUCK
dumps**, ~70 lines apart, spanning the last ~10 minutes of wall time of the stalled run
(`17-waiter-child-endstate.log`):

```
dump at line 18039:  [IDLESTUCK] slot=7  pid=7  st=3 wake=0 rip=0x1000000000  TORTURE.BIN
                     [IDLESTUCK] slot=35 pid=35 st=7 wake=0 rip=0x100000adf0  TORTURE.BIN
dump at line 18108:  [IDLESTUCK] slot=7  pid=7  st=3 wake=0 rip=0x1000000000  TORTURE.BIN
                     [IDLESTUCK] slot=35 pid=35 st=7 wake=0 rip=0x100000adf0  TORTURE.BIN
dump at line 18177:  [IDLESTUCK] slot=7  pid=7  st=3 wake=0 rip=0x1000000000  TORTURE.BIN
                     [IDLESTUCK] slot=35 pid=35 st=7 wake=0 rip=0x100000adf0  TORTURE.BIN
```

The state, the wake field and the instruction pointer are **identical** in all three. So:

* **slot 7 (`TORTURE.BIN`) is frozen at `rip=0x1000000000`** — `USER_VA_BASE`, i.e. the first instruction
  of user code. It has not retired one user instruction, and it never exits.
* **its state is `st=3` (`PROC_STATE_RUNNING`)** and its ALIVE-SET line carries `claimed_by=2` — so the
  scheduler believes CPU 2 owns it, while the watchdog's own sample of CPU 2 shows it inside a fault
  handler for that pid: `CPU2 … vec=14 err=0x2 cr2=0x17FFFFFCB8 pid=7 resume_pid=7`.
* **slot 35 (`TORTURE.BIN`) waits for it** in `WAIT_CHILD` with `parent=-1`, forever — the parent is
  behaving correctly; the child is the thing that never finishes.
* nothing else is runnable (`[ALIVE-SET] no READY work … so no halt`), so the wave cannot complete and
  the tier's deadline is the only thing that ends it.

## What this does and does not establish

**Established:** the halt blocker at `41ec708` is a single process (slot 7) that is claimed `RUNNING` by
CPU 2 and frozen at its user entry point while that CPU services a write `#PF` on its stack page
(`cr2=0x17FFFFFCB8`). The waiter above it is a symptom. This is persistent, not a sampling artifact.

**Not established:** *why* the child cannot make progress. The `#PF` at `0x17FFFFFCB8` is the address
this card already showed is a handled demand fault (`pte_before=0 handled=1 pte_after=1 rw=0→1`), so it
is either a fault that the handler repairs and the retry re-takes defectively, or a fault that is not
being retried at all while the state stays `RUNNING`. My previous attempt to assert a retry loop was
wrong once (pid recycling) and I am not going to assert it again from a sample.

## The one measurement that would settle it (named, not run)

A per-(cpu, cr2) counter and a per-resume counter, printed only for `cr2=0x17FFFFFCB8` and slot 7:

* if the retry count climbs while `rip` stays `0x1000000000` → the handler repairs the page and the
  `iretq` back to user re-faults: the fault is in the **resume/entry** path, not in the copy;
* if the retry count stays at 1 and `rip` still does not move → the CPU holding the claim never
  re-enters the process at all: the defect is a **lost resume** with a stale `RUNNING` claim, which is
  the `[ALIVE-SET] RUNNING with NO cpu` class the scheduler already has a predicate for (it printed 0
  such lines here because `claimed_by=2` was set — i.e. **the claim is the thing that lies**).

Either outcome is a one-line instrument and a definite answer, and both are inside this card's scope.

## Controls, so the pattern is not over-read

`16-controls.log`: the two halting runs also contain `WAIT_CHILD` slots (run 1: 8 `st=7` lines, its final
ALIVE-SET shows `slot=11 st=7 parent=4 SH.BIN` — a waiter with a **live** parent), and they halt. Run 2
is the only one whose final live set is a `WAIT_CHILD` with `parent=-1` **plus** a `RUNNING` child frozen
at user entry. Neither element alone distinguishes the runs; together they do, in this sample of three.

## DoD status

(a) 3 consecutive `halted=1`: **not met** (2 of 3 at `41ec708`) — unchanged by this pass; the blocker is
now described by a persistent state rather than by samples. (b) delivered. (c) CMPTEST/SIGTEST/TACTEST
still uncarded. Card stays `In Progress`; no verdict, no lane move, no rotation marker.
