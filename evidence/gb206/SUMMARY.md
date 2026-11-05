# GB-206 — return-slot snapshot of spinlock_release_irqrestore

Head: `f337e7a31d52d1069dc3bc7ad47ec62625cdd859` (origin/main, 2026-11-05). Host: swarm-farm-3, QEMU 10.2.1, KVM intel.

## Instrument

`src/kernel/arch/x64/locks.c`: under `-DGB206_RELSNAP` (Makefile knob `GB206_RELSNAP=1`), the function snapshots the return slot at `rbp+8` (the compiler prologue is `push rbp; mov rsp,rbp; push r14; push rbx`, so the slot is at rbp+8) and `%rsp` at entry and again immediately before the flags restore. Raw sink only (`uart_putc_raw`, no locks), one line per sample, rate-limited to `vm_lock 0x75303EE0` or the first 8 pairs, hard cap 16 pairs per boot. The instrumented function keeps the original prologue and epilogue:

```
1390: pushq %rbp ; movq %rsp,%rbp ; pushq %r14 ; pushq %rbx
...
14e9: pushq %rbx ; popfq ; popq %rbx ; popq %r14 ; popq %rbp ; retq
```

## DoD 1 — one intel T3 run, ≥5 entry/ret pairs: PASS

`GB206_RELSNAP=1 T3_SMP=8 T3_TIMEOUT=600 bash tools/t3_tier.sh intel` (full default wave), `serial_instr4.log`: **76,142 console lines, 320 pairs, 70 programs exited, 14 FATAL, halted=0**, tier `EXIT=1`.

A `vm_touch` pair (the call site GB-146 symbolicates, `retslot=0x70047493` = vm_touch's post-call address):

```
[RELSNAP] entry lock=0x0000000075303ee0 retslot=0x0000000070047493 rbp=0x0000000023ffe6c8 rsp=0x0000000023ffe6b8
[RELSNAP] ret lock=0x0000000075303ee0 retslot=0x0000000070047493 rbp=0x0000000023ffe6c8 rsp=0x0000000023ffe6b8
```

Distribution of the 160 vm_lock pairs in this run: 30 from `vm_touch` (`0x70047493`), 30 from `vm_region_insert` (`0x70046821`), 10 from `v2_image_materialize` (`0x700477bb`), 10 from `vm_as_create` (`0x70045a84`).

## DoD 2 — verdict: the return slot does NOT change; reading 1 is refuted

**320/320 pairs identical** in run4 (`retslot` and `rsp` at entry == at ret). A second completed instrumented run (`serial_instr2.log`) had **96/96 identical**; the cut run1 had 32/32. Total 448/448 sampled pairs across three instrumented runs, including 160 vm_lock pairs (30 from the exact `vm_touch` post-call site) in the run that reproduced the FATAL.

In every pair `rsp == rbp-0x10` both times, i.e. the epilogue's `push %rbx; popfq` is balanced and the three pops (`rbx`,`r14`,`rbp`) take `rsp` from `rbp-0x10` to `rbp+8`, where `ret` reads the unchanged return slot. The epilogue does not write its own return address. **Reading 1 is refuted; reading 2 (the RIP/vector is a frame-write artifact, GB-123's exit-path claim) governs.**

The same run produced the GB-146 family, so the refutation is not a quiet wave:

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0000000000000093 cr2=0x0000001040002020 pid=17 ...
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0000000000000093 cr2=0x00000010000752A0 pid=46 ...
```
plus `vector=1879357835` (a corrupt vector) and `vector=6 rip=0x7`.

## DoD 3 — FATAL reproduced; baseline caveat

The wave reproduced 14 FATAL kernel-mode faults including two `vector=13 rip=0x93`; 70 programs exited, no halt (600 s deadline). The card's 86-exited baseline is **not reached by any arm on current main** — control runs reached 69 and 72 exited — so the "null only at 86" condition cannot be met on this head; the FATALs are present, which is the stronger case.

## DoD 4 — non-perturbing

```
$ make host_tests
Checks run: 482 / Checks failed: 0 / TEST EXIT: 0

$ make ARCH=intel hobbyos.elf   rc=0
$ make ARCH=arm   hobbyos.elf   rc=0
```

Program counts at the same head: instrumented 70 (run4) and 66 (run2, an outlier) vs un-instrumented controls 69 and 72. Run4 is inside the observed control spread (69–72); run2 is 3 below the lower control and is disclosed as a possible timing perturbation. Neither arm reaches the 86 baseline on current main.

## Falsifier ruling

Every sampled entry/ret pair is identical → reading 1 is refuted; GB-123's frame-write reading governs. Raw pairs in `serial_instr4.log` / `serial_instr2.log`; instrument diff in `instrument.diff`.
