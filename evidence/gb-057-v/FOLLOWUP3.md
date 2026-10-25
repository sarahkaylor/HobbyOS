# GB-057 — third follow-up: correcting my own previous claim, and an instrument defect in the WAIT scan

Builder: `wverify-deep`, 2026-10-25 ~15:2xZ, still `origin/main` `41ec708`, no new guest run.
This replaces the mechanism sentence of my previous comment. No DoD claim.

## What I withdraw

I wrote that the stalled wave's child "is stuck in a kernel `#PF` retry at `rip=0x7003BEDF`". **That is
not established and I withdraw it.** Two facts kill it (`15-child-fault-records.log`):

1. **The pid is recycled.** `[EXITPC] pid=7 …` appears ~20 times across the run with different
   `last_user_pc` values — pid 7 is not one process, so "pid 7 is the child, and it is faulting" read a
   single watchdog sample as if it belonged to one long-lived process.
2. **Those faults are the known benign family.** The log's fault addresses are exactly two values
   (`cr2=0x17FFFFEFB8` ×44, `cr2=0x17FFFFFCB8` ×22), which is the demand-fault-on-shared-stack-top
   pattern this card established in its 10:37Z and 10:58Z comments — handled, once each, `n=1`.
   There is no repeated-fault record for the wedged child (`[KERNEL] pid=7` records: 0).

## What is established instead, and it is a code fact

The scan that prints the "no parent" line tests the **wrong end** for `WAIT_CHILD`
(`src/kernel/process.c:3554-3583`):

```c
if (st != PROC_STATE_WAIT_SPAWN && st != PROC_STATE_WAIT_CHILD) continue;
int pp = proc_table[k].parent_pid;
int parent_gone = (pp <= 0);
...
uart_puts(" -- no state change attempted; see GB-057\n");
```

but the wake path for `WAIT_CHILD` is `reap_deliver_locked()` (`process.c:678-700`), which matches the
waiter against the **dying child's** group:

```c
if (parent->state != PROC_STATE_WAIT_CHILD ||
    process_group(parent)->pid != grp->parent_pid) continue;
```

So a `WAIT_CHILD` waiter is delivered when its **child** exits; the waiter's own `parent_pid` plays no
part. For `WAIT_SPAWN` the scan's end is right (the parent is the one who must deliver), for
`WAIT_CHILD` it is not. Consequences, both measured in run 2:

* `slot=35 pid=35 st=7 parent=-1 TORTURE.BIN` is **not** unwakeable because `parent=-1`; it is waiting
  for `slot=7 pid=7 st=3 claimed_by=2` — its child — to exit. The kernel's own message is therefore
  mislabelled for `st=7`, and any fix driven by that line would reap a waiter that has a live child.
* The halt blocker is at the **child** end: at the end of the run slot 7 is `st=3 RUNNING`,
  `claimed_by=2`, `rip=0x1000000000` (user VA base) — it has not progressed. The parent is entitled to
  wait; the child is the thing that never finishes. Whether it is faulting, spinning or deadlocked is
  **not** established by the data I have, and I am not going to guess a third time.

## Consequence for the card

* Clause (a) 2/3 halts is explained as: *one non-exiting child leaves one `WAIT_CHILD` waiter, which
  the live-work predicate correctly counts as live, so the wave can never declare completion.* That is
  consistent with the code comment at `process.c:3531-3550` (probe15: three such slots kept
  `!any_alive` true and the wave never halted).
* The fix therefore is **not** in the scheduler's reap scan (the code's own comment explains why forcing
  state there became a use-after-free, and this analysis agrees). It is either (i) at whatever stops the
  child from exiting, or (ii) in making `reap_deliver_locked` cover the case where the child is gone
  without a delivery — neither of which I have measured.
* **Instrument defect worth its own line, verified from the source:** the `st=7` arm of that scan should
  test the *child's* existence, not the waiter's `parent_pid`; as written it cannot distinguish a
  genuinely orphaned `WAIT_SPAWN` from a `WAIT_CHILD` whose child is still alive, which is exactly the
  distinction the fix needs.
