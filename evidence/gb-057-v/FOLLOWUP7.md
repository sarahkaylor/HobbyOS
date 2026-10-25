# GB-057 — seventh pass: RETRACTION. The "FATAL" record is printed *before* the repair, and `0x1000000000` is 4 GiB, not 64 GiB

Builder: `wverify-deep`, 2026-10-25 ~17:1xZ, base `41ec708`. No new guest run — this is a code re-read of
the same tree that produced the instrumented run, and it invalidates the "root cause" I posted one turn
ago.

## Retraction 1 — a `FATAL: Exception in Kernel Mode` record does not mean a CPU was lost

I claimed "each FATAL ends in `while (1)`, so the CPU dies holding its claim, and 10 events > 8 CPUs
explains the freeze". **Wrong.** The control flow in `src/kernel/arch/x64/trap.c` is:

```
~2846  "[KERNEL] FATAL: Exception in Kernel Mode! Vector: ..."      <- the DUMP (unconditional)
~2880  "[GB057-PFCTX] ..."                                          <- my instrument, same place
~2885  if (tf->vector == 14 && cur) { if (kcr2 >= USER_VA_BASE) {
~3012      return;                    /* PTE live; iretq retries the faulting instruction */
~3015    "[KERNEL] Kernel-mode #PF in user address space -> killing pid="
~3033  "[KERNEL] FATAL kernel-mode fault, halting this CPU: vector="  <- the HALT
```

The dump is emitted **before** the repair attempt, and a successful `vm_handle_fault` `return`s at 3012
and never reaches the halt at 3033. In all runs, `Kernel-mode #PF in user address space -> killing pid=`
printed **0** times and `FATAL kernel-mode fault, halting this CPU` printed **0** times — so **all ten
of those faults were repaired and resumed**. The alarming name is on the dump, not on the outcome, and I
read the name instead of the code.

## Retraction 2 — the `USER_VA_BASE` gate does exclude the stack: false, and the arithmetic was mine

`src/include/vm.h:27` says `#define USER_VA_BASE 0x1000000000ULL /* 64 GiB */`. **`0x1000000000` is
4 GiB**, not 64 GiB — the header comment is itself wrong, and I trusted it. With the real value:

```
USER_VA_BASE           = 0x1_0000_0000  = 4 GiB
USER_MAIN_STK_TOP_V2   = 0x1_8000_0000  = 6 GiB      (= VA_BASE + 0x8000_0000, per vm.h:70)
faulting cr2           = 0x17FFFFF388   = 6 GiB - 3 KiB
```

`0x17FFFFF388 >= 0x1000000000` is **true**, so the repair gate passes and `vm_handle_fault` runs — which
is exactly consistent with Retraction 1. The "one-condition defect" I proposed does not exist.

## What actually survives from the instrumented run

* `enter_user_space` takes **repeated kernel-mode write #PFs into the user stack** — `cur_pid=18
  has_as=1 cr3=0x32F97000 cr2=0x17FFFFF388 RIP=0x7003BEDF` — 4 of them in that run, 10 in the stalled
  run, 5 in the other halting run. They are **handled** (no kill, no halt record), so they are a symptom
  of demand-paging the stack during the frame copy, not a CPU-killer.
* The end-state facts from the earlier passes are untouched: `[ALIVE-SET] no READY work … so no halt`, one
  `WAIT_CHILD` waiter with `parent=-1`, and one slot frozen at `st=3 rip=0x1000000000` across three
  dumps.
* **The `USER_VA_BASE` header comment is arithmetically wrong** (`0x1000000000` labelled 64 GiB when it
  is 4 GiB). That is a real, one-line documentation defect, it cost this pass a wrong root cause, and it
  is worth fixing on its own — a reader doing exactly this arithmetic gets the wrong answer.

## The instrument that is now clearly needed (placement matters)

The `[KPFLOOP]` counter the card already has sits **inside** the repair block and only fires for
`kcr2 >= USER_VA_BASE` with `cur` set — i.e. it should have fired here, and did not, because it is
gated on `kpf_n > 8` *per CPU with the same cr2 consecutively*, and these four faults arrive on
different CPUs/different cr2s. The measurement that would settle the card is a **per-(cpu, cr2) repair
counter printed at the `return` at 3012**, plus the process's `rip` on the next resume of that pid:

* same (cpu, cr2) repaired repeatedly **and** `rip` unmoved → the repair is not making progress: retry
  loop;
* each repaired once, `rip` unmoved → the resume after repair is lost or the process is never scheduled
  again.

Neither has been measured, so clause (a) remains unmet and this card has no named mechanism again.

## DoD status

* (a) 3 consecutive `halted=1`: **not met** (2 of 3 at `41ec708`).
* (b) delivered (75 / 256 / 83 real, run-2 caveated as infra-inflated).
* (c) SEDTEST → GB-093 ✓, TSORTTEST → GB-095 ✓; intel CMPTEST, SIGTEST, TACTEST still uncarded.

Card stays `In Progress`; no verdict, no lane move, no rotation marker.
