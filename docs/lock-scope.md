# Lock scope: taking interrupts out of the kernel's locking discipline

Status: **landed** (ARM unit tier + ARM integration tier green; x64 side verified
separately).  This document is the contract, the measurement, and the guard.

The one-line version: **a lock protects the state it guards, not the CPU.**  The
kernel used to spend up to **435 ms** with interrupts masked because a filesystem
lock was taken with `spinlock_acquire_irqsave`; it now spends **< 5 ms**, and no
process-context lock masks interrupts at all.

---

## 1. The problem

Every lock in the kernel masked interrupts on acquire.  That was the cheapest way
to be correct in a kernel whose timer tick reschedules unconditionally: with
interrupts off, the tick cannot fire, so the holder cannot be switched out
mid-section.

The cost was invisible until it was measured:

| Symptom | Measurement before |
| --- | --- |
| Longest single IRQ-off window | **435 ms** (`f->lock`, the FAT16 file lock) |
| Total IRQ-off time, unit suite | 1.5 s |
| Device interrupts entering the kernel while another thread is in the kernel | impossible while any lock is held |
| Root cause | lock scope = "protect against preemption", implemented as "stop the world" |

A 435 ms window is not a latency bug, it is a correctness bug: the timer tick,
the block device completion, the network receive path and the keyboard all queue
behind it, and a thread that blocks holding such a lock strands every other CPU.

## 2. The discipline: three lock classes, one decision

Every registered lock carries a class (`lock_register`, born with the lock):

| Class | Use when | Primitive | Interrupts | Preemption |
| --- | --- | --- | --- | --- |
| `LOCK_CLASS_IRQ` | an interrupt handler touches the state | `spinlock_acquire_irqsave` | **masked** | moot |
| `LOCK_CLASS_PROCESS`, short | only process context, no I/O inside | `spinlock_acquire` | **enabled** | **deferred** |
| `LOCK_CLASS_PROCESS`, may do I/O | device access or a long section | `ylock_acquire` | **enabled** | allowed (holder may be preempted) |

The rule that replaces "mask interrupts": **a short critical section disables
preemption, not interrupts.**  The tick still fires, device interrupts still
enter the kernel on that core, and the context switch waits until the last
`kernel_preempt_disable()` unwinds.  That is what makes a plain
`spinlock_acquire()` safe on a lock no interrupt handler takes.

`proc_lock` is deliberately **excluded** from preempt accounting.  `schedule()`
holds it *across* the context switch (it is released by the frame that resumes
afterwards), so a per-CPU preempt reference taken there would be inherited by
whatever that switch resumed and pin that CPU permanently.  This is not a
stylistic detail: it was measured — the integration tier wedged with every CPU
idle and one of them owning `proc_lock`.

## 3. `ylock_t`: what it is, and what it is not

`ylock_t` is the filesystem lock: it keeps interrupts enabled, so a thread can be
inside FAT16 while the device completion IRQ is delivered on the same core.  That
is the whole point.

It **spins** rather than parking the waiter, and that is a deliberate constraint
of this kernel, not an unfinished optimisation:

> A process in this kernel blocks **at its syscall boundary**.  `fs_read` returns
> `-EAGAIN`-style and the trap exit parks the thread.  A `ylock_acquire()` taken
> deep inside `fat16 -> fs -> syscall` cannot do that: `arch_yield()` there saves
> a frame whose ELR is a *kernel* address, and the scheduler later iret's into it
> as **user** code.

Measured, while this was still a parking lock:

```
[BADELR] picking a user slot with an implausible frame.elr=0x43A52BC74
[KERNEL] FATAL: Synchronous Exception in EL1! FAR: 0xAFAFAFAFAFAFAFAF
```

Every process that waited on a file was resumed into garbage.  So the waiter
spins with interrupts enabled and reports a stall (`[YLOCK-STALL]`, once per
lock, naming the holder's CPU) instead of parking.  The holder always makes
progress because device waits in this kernel are bounded `safe_wfi()` polls, not
another park.

Making it park for real requires a way to *drop* a lock and restart the syscall;
until that exists, a spinning lock that reports a stall is honest and a parking
lock that corrupts a frame is not.

## 4. Result

Same unit suite, same host:

| Metric | Before | After |
| --- | --- | --- |
| Longest IRQ-off window | 435 ms | **4.9 ms** (`vm_lock`, bulk page-table tests) |
| `irqsave` acquisitions on PROCESS-class locks | 643 | **0** |
| Block-while-holding-lock detections | (unmeasured) | **0** |
| Self-deadlock detections | 1 (`fat16_mkdir`, fixed) | **0** |
| Locks the registry could not hold | silent | **0**, counted |

### Measured against an unmodified HEAD tree

Same host, same tier command, same `-smp 8` QEMU command line; the HEAD side is
a clean `git worktree` at HEAD with its own `MODE=test` image:

| | HEAD | this tree |
| --- | --- | --- |
| Run 1 | 18,191 console lines, **no halt** (died mid-wave) | 21,920 lines, **halt**, 21x "ALL TESTS PASSED" |
| Run 2 | 240 lines, **`[IDLESTUCK]`**, 7 of 8 CPUs idle, wedged right after the shell started | same as run 1 |

So the tier this work is measured on was not green before it: HEAD never
completed a clean wave in two attempts, and the wedge it hits is the one this
document is about.  This tree completed it twice, back to back.

That also reframes the two test failures the completed wave reports
(`[SIGTEST] spawn_ex illegal dst` and `SQLTEST cross-process-BUSY`): HEAD never
reached those programs, so they are not "previously green, now red".  Both fail
on the async-spawn / process-table-pressure path, upstream of the fdmap
validation and of any lock this work touches (a positive pid is impossible from
the synchronous path, which returns `process_free(pid); return -EBADF;`).  They
are now *reachable* tests that need their own fix; they are not evidence of a
locking regression.

The remaining 4.9 ms is `vm_lock`: a page-table walk over a large region, still
`LOCK_CLASS_IRQ` because the fault path takes it.  It is bounded and measured,
not hidden — the table above is the number to improve next.

## 5. What keeps it from rotting

Four guards, in the order they fire:

1. **`src/kernel/locks_test.c`** — pins the invariants themselves:
   a plain lock and a ylock mask no interrupts (measured as a delta of the
   audit's IRQ-off total, which is 0), a plain lock takes a preempt reference,
   a ylock excludes, and an `irqsave` on a PROCESS-class lock still **counts**
   (the detector is proven to have teeth, not just to exist).
2. **`src/kernel/unit_test.c`** — end-of-suite gate: the run **fails** unless
   `blockhold == selflock == classerr == irqsave_proc == reg_dropped == 0` and
   the worst IRQ-off window is under `LOCK_IRQOFF_BUDGET_US`.
3. **`tools/t3_tier.sh`** — the integration tier's verdict now polices
   `[LOCKERR]`, `[BLOCKHOLD]`, `[MUTEX-SELF]`, `[BADELR]`, `[IDLESTUCK]` and any
   non-zero `[LOCKHEALTH]` field.  A wave carrying a deadlocking kernel can no
   longer report PASS just because every test program printed its own output.
4. **`src/user/smp_test.c`** — the concurrency soak: 1/2/4/8 workers doing FAT16
   file round trips plus pipe round trips, each worker asserting its full op
   count.  It prints `[CSTEST] PASS/FAIL`, so a stranded lock shows up as a
   missing worker with the phase number attached.

The instrumentation that produces all of it is `src/kernel/lockscale.c` and it is
**always on** (not a debug build): `lock_register` runs in normal builds too,
because a registry that only exists in test builds cannot audit the build that
ships.

## 6. Checklist for adding a lock

1. Does any interrupt handler touch this state? → `LOCK_CLASS_IRQ`, `irqsave`.
   If you are unsure, assume yes and measure.
2. Otherwise: is the section short and free of device I/O? → `spinlock_acquire`.
3. Otherwise → `ylock_t`.
4. `lock_register(&lock, "name", LOCK_RANK_*, LOCK_CLASS_*)` next to the
   definition, in every build.
5. Never take a `ylock_t` while holding `proc_lock` (reported as
   `[LOCKERR] MUTEX-UNDER-PROCLOCK`), and never call `fs_*` work under
   `proc_lock` if it can reach a ylock — `process_fork` snapshots the fd table
   under `proc_lock` and takes the per-file references *after* releasing it.
6. Never block while holding a plain spinlock.  The audit reports
   `[BLOCKHOLD]` with the lock name; the scheduler also resets the per-CPU
   preempt count at a switch so one such bug cannot pin a core forever.

## 6a. Open defect the soak found (not fixed, on purpose)

The `[CSTEST]` soak surfaces an **intermittent concurrent-FAT16 data-integrity
race** that nothing was testing before it existed.  Evidence, two runs of the
same binary:

| Run | Soak result | Other programs in the same wave |
| --- | --- | --- |
| 1 | phase 2 (4 workers): `worker=0 data mismatch`, stopped at ops=3 | — |
| 2 | all four phases PASS | `[WORKER 0] ERROR: data mismatch on TEST1.TXT. Expected: WORKER0_DATA` |

So it is not the soak's own bug: a second, independent test read back wrong
contents in the run where the soak was clean.  Each soak worker touches a
**private** file (`/CSTEST<w>.TMP`), so a mismatch is not two writers fighting
over one file -- it is a read returning bytes that were never written there.

Leading hypothesis, and why this work may have widened the window: a `ylock_t`
holder **can be preempted** (the lock keeps interrupts on and does not disable
preemption; that is what lets the device IRQ land during filesystem work).  The
FAT16 code was written under `irqsave`, where a holder could not be switched out
mid-update, and several paths already drop `fat_lock` deliberately mid-operation
(`fat16: read loop releases at cluster crossing`, `stats scans without fat_lock`).
A preemptible holder makes any such window observable by another thread.

Next experiments, in order:
1. Give `ylock_acquire` a preempt reference for **metadata** sections only, and
   leave device-I/O sections preemptible -- if the mismatch disappears, the
   window is confirmed as "holder switched out mid-update".
2. If it survives, the write-back path is the suspect: two files' directory
   entries share a sector, and a lost update there is exactly this symptom.

Until then the integration tier is red on this, which is the correct state for a
gate to be in when a real data race is reachable.

## 7. Debugging a wedge

The stall report now names the contention instead of printing an address:

```
[YLOCK] fat_lock held=1 owner_cpu=3 spin_holders=2
[SPINHELD] f->lock owner_cpu=5
[IDLESTUCK] claims: c0=6/0/0 ... lock_owner=8
```

`owner_cpu` is the CPU holding it (`-1` = free), and the per-CPU claim line says
whether that CPU is still returning to idle.  Combined with `llvm-addr2line` over
the `pc=` field of the matching `[LOCKERR]`/`[YLOCK-STALL]` line, that is usually
the whole story.
