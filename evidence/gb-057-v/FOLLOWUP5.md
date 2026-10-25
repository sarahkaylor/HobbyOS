# GB-057 — fifth pass: the frozen CPUs are `enter_user_space` dying in a kernel-mode #PF, one CPU per event

Builder: `wverify-deep`, 2026-10-25 ~16:1xZ, `origin/main` `41ec708`. Log forensics on the three
committed runs plus a symbol lookup in an ELF built from that same revision. No new guest run. No DoD
claim.

## The measurement that ties the whole card together

**(1) Kernel-mode fatal exceptions, by run — all the same fault** (`23-kernel-fatal.log`):

```
run1 (halted=1): 4   [KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x2
run2 (halted=0): 10  [KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x2
run3 (halted=1): 5   [KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x2
```

Every one of run 2's ten is `Vector: 14` (#PF), `Error Code: 0x2` (write, not-present), and every
recorded `RIP` is the **same address**: `0x7003BEDF`. Ten such events on an 8-CPU machine.

**(2) That address is inside `enter_user_space`** (`24-fault-rip-symbol.log`, symbols from a kernel
built at `41ec708`):

```
0x7003BEDF -> enter_user_space
0x7003BFCD -> enter_user_space     # CPU5's stored frame
0x7003BDA5 -> common_trap_wrapper  # CPU1's frame is inside the same call chain
0x70038A16 -> safe_wfi  (cpu.c:44) # the five parked CPUs
```

The kernel's own comment at `trap.c:2880-2884` already records this exact shape from an earlier probe:
*"a CPU sat on the `rep movsq` inside `enter_user_space` … for 14 MINUTES re-executing it, with
vm_handle_fault returning success each time"*. The difference now is that it is not looping — it is
**halting a CPU each time it happens**, and there are ten of them.

**(3) Each FATAL halts that CPU, permanently** — `trap.c:2955-2960`'s kernel-mode fallback ends in
`while (1);`. A halted CPU never releases the process it claimed. That is exactly the stuck claim I
reported last pass: `slot=7 st=3 RUNNING claimed_by=2 rip=0x1000000000` — CPU 2's stored frame is
`0x7003BEDF`, i.e. **the CPU that holds the claim is one of the CPUs that died in `enter_user_space`**,
so the claim can never be released and the child never runs again. Its parent waits in `WAIT_CHILD`
forever, nothing is runnable, the wave cannot halt, and the tier's deadline is the only thing that ends
it. **10 fatal events > 8 CPUs** is why run 2 froze completely while runs 1 (4) and 3 (5) survived.

**(4) The 22 identical watchdog rounds are those halted CPUs' last frames.** `wd_last_frame[cpu] = *tf`
is written on every trap (`trap.c:2340`) and printed by the watchdog, so the byte-identical CPU lines
across 22 samples are *stored* frames, not live fault claims — which retires my earlier reading that
"three CPUs are faulting" at the end.

## Why the repairable path did not take it (code-path deduction — the one inference here)

`trap.c:2877-2932` handles exactly this case *if it can attribute the fault*:

```c
if (tf->vector == 14 && cur) {
  ... if (kcr2 >= USER_VA_BASE) {  ... vm_handle_fault(...) == 0 -> return; /* retry */
      [KERNEL] Kernel-mode #PF in user address space -> killing pid=...
```

Both of that branch's messages are **absent** from every run (`Kernel-mode #PF in user address space`:
0 occurrences; `[KPFLOOP]`: 0 occurrences), while the generic kernel-mode `FATAL` printed 10 times. The
only entry conditions that produce that split are `tf->vector == 14` (true — the record says so) and
`cur == NULL`: with no current process the handler cannot attribute the fault, falls through to the
generic kernel-mode dump, and halts the CPU.

So the mechanism is: **`enter_user_space`'s frame copy to the user stack takes a write #PF while no
current process is set, and the handler's attribution requires one — so a perfectly repairable
not-present user page costs a CPU instead of a page.**

This is a deduction from the code path plus a message census, not a direct measurement, and I label it
as such.

## The two-line instrument that would confirm it

In the generic kernel-mode `FATAL` path (`trap.c:2786-2800`), print `cur`, `cr3` and `kcr2` alongside
the vector. Prediction, falsifiable: those ten records will show `cur=NULL`, a user CR3, and
`kcr2=0x17FFFFF…` — and the fix is then either to attribute the fault from CR3 (a user AS is
identifiable without `cur`), or to guarantee `cur` is set before `enter_user_space` copies, or to make
the copy pre-fault the destination page the way `sys_user_range`/`vm_touch` already do elsewhere.

## DoD status

* (a) `halted=1` on 3 consecutive runs: **not met** (2 of 3 at `41ec708`) — but the mechanism is now
  named with a symbol, a count and a code path, and the count (10 vs 4 vs 5) explains which runs die.
* (b) delivered (75 / 256 / 83 real; run 2's number caveated as infra-inflated).
* (c) SEDTEST → GB-093 ✓, TSORTTEST → GB-095 ✓; intel CMPTEST, SIGTEST, TACTEST still uncarded.

Card stays `In Progress`; no verdict, no lane move, no rotation marker.
