# GB-057 — sixth pass: ROOT CAUSE. The kernel-mode #PF repair path is gated on `kcr2 >= USER_VA_BASE`, and the user stack lives *below* that base

Builder: `wverify-deep`, 2026-10-25 ~16:5xZ. Base revision `origin/main` `41ec708`.
**One new guest run**, instrumented (patch committed as `instrument.patch`, measurement only — no fix, no
behaviour change on the happy path; the instrument prints four fields in a path that already halts the
CPU).

## What the instrument printed

```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
[GB057-PFCTX] cur_pid=18 has_as=1 cr3=0x0000000032F97000 cr2=0x00000017FFFFF388
  RIP: 0x000000007003BEDF  RSP: 0x00007...
```
(`02-pfctx-records.log`; 4 such records in the run, the same `Vector: 14 / err=0x2 / RIP=0x7003BEDF` as
the ten in the stalled run 2.)

## The falsified deduction, and what replaced it

Last pass I deduced `cur == NULL` from the absence of the repair branch's messages. **That is wrong:**
the instrument shows `cur_pid=18 has_as=1`. The repair branch at `trap.c:2877` is skipped for the other
reason it can be — its address gate:

```c
if (tf->vector == 14 && cur) {
  uint64_t kcr2; mov %cr2, kcr2;
  if (kcr2 >= USER_VA_BASE) {          /* <-- the gate */
      ... vm_handle_fault(...) == 0 -> return;   /* materialize + retry */
```

and the constants, from the tree:

```
src/include/vm.h:27:  #define USER_VA_BASE 0x1000000000ULL   /* 64 GiB */
v2 loader (every process):  entry=0x1000000000 sp=0x1800000000
faulting cr2:               0x17FFFFF388            /* 6 GiB - 3 KiB */
```

**`0x17FFFFF388` is below `0x1000000000`.** The user stack sits at `0x1800000000` and grows down; the
image sits at `0x1000000000` (64 GiB). So the "is this a user address?" test — `kcr2 >= USER_VA_BASE` —
is **false for the entire user stack**, and every kernel-mode write fault into a not-yet-present stack
page takes the generic path: `FATAL: Exception in Kernel Mode` → `while (1);` → that CPU is dead for
the rest of the run.

## Why this explains every observation on the card

| Observation (measured on `41ec708`) | Explanation |
|---|---|
| `FATAL ... Vector: 14 Error Code: 0x2` 4 / 10 / 5 per run, all at `RIP=0x7003BEDF` (`enter_user_space`) | each is a frame-copy write into a stack page that is still absent |
| the run with **10** died; runs with 4 and 5 halted | 8 CPUs; ≥8 such events in one run leaves no CPU able to run the wave |
| `slot=7 st=3 RUNNING claimed_by=2 rip=0x1000000000` frozen across three dumps | the CPU holding the claim is one of the halted CPUs; it never releases the claim, so the process never resumes |
| `slot=35 st=7 WAIT_CHILD parent=-1` waits forever | its child is the process whose CPU died; the wake can never be delivered |
| 22 byte-identical watchdog rounds | the per-CPU stored frames of those dead CPUs (`wd_last_frame[cpu] = *tf`) |
| `[KPFLOOP]` and `Kernel-mode #PF in user address space` never printed | both live *inside* the gated branch that the address test skips |

So the card's six-pass chase of "resume frames / IF bit / #UD at 7" was chasing the *symptoms* of a
one-condition gate: the machine loses a CPU every time a process first touches a stack page during the
kernel's own frame copy, and enough losses end the wave.

## The fix (one condition, not applied here)

Accept the user stack window in the same gate the image base already uses — the window the loader
itself prints (`region=0x17FF800000+0x800000` in the `RESUME-DIAG` records):

```c
/* before */  if (kcr2 >= USER_VA_BASE)
/* after  */  if (kcr2 >= USER_VA_BASE ||
                 (kcr2 >= USER_STACK_BASE && kcr2 < USER_STACK_TOP))
```

`vm_handle_fault(process_group(cur), kcr2, 1, 0, &why)` already validates the region (`in=PROT
region=0x17FF800000+0x800000 prot=3` appears throughout the logs), so widening the gate does not weaken
the overrun kill: a genuine hole/guard fault still fails `vm_handle_fault` and falls through to
`process_fault_exit`. The alternative — pre-faulting the destination page before the copy, as
`sys_user_range`/`vm_touch` do — is the more conservative shape of the same fix.

## What this pass does **not** establish

That the widened gate produces 3 consecutive halts. That is an A/B a worker or the next pass should
run: patch the condition, build, three intel T3 runs, compare against this baseline (2 of 3 halts,
4/5/10 fatal events). I have not run it, so clause (a) remains **unmet** and I am not claiming it.

## DoD status

* (a) 3 consecutive `halted=1`: **not met** (2 of 3 at `41ec708`); root cause now named and falsifiable.
* (b) delivered (75 / 256 / 83 real, run-2 caveated).
* (c) SEDTEST → GB-093 ✓, TSORTTEST → GB-095 ✓; intel CMPTEST, SIGTEST, TACTEST still uncarded.

Card stays `In Progress`; no verdict, no lane move, no rotation marker.
