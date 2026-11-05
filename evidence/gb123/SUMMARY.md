**INTERIM FINDING (GB-123) — the family reproduced as `vector=6` with the RIP/RBP/R15 holding a DIFFERENT stack's addresses; the faulting CPU is on `0x45ffe...`. This is a cross-stack frame copy, not a return-address splice. Full store-level watchpoint not completed; releasing the card to Ready rather than parking it.**

Run: origin/main `f337e7a3`, `T3_SMP=8 T3_TIMEOUT=600` with `T3_QEMU_ARGS="-d int -D qemu_int.log"`, full default intel wave. Result: 36,758 console lines, 65 programs exited, 25 FATAL kernel-mode faults, 1 shredded, halted=0 (`t3_int.log`); 159 MB QEMU exception trace (`qemu_int.log`, kept in `evidence/gb123/`).

**The run's shape.** The 0x93 shape did not appear in this run; the torn-frame class did, as `vector=6` with RIP = a stack address:

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000035FFE709 cr2=0x0000001080019000 pid=29 ...
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000043FFE709 cr2=...
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000029FFE709 ...
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x000000002BFFE6FF ...
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x000000003BFFE708 ...
```

**The QEMU trace for pid=29's fault** (same `cr2=0x1080019000`, line 33526 of `qemu_int.log`):

```
 33526: v=06 e=0000 i=0 cpl=0 IP=0008:0000000035ffe709 ... SP=0010:0000000045ffe340
RAX=00000000ef11b523 RBX=000000007004c8d2 ... RBP=0000000035ffe408 RSP=0000000045ffe340
R15=0000000035ffe8e8
```

The CPU faults at IP `0x35ffe709`, with RBP `0x35ffe408` and R15 `0x35ffe8e8` — all in the `0x35ffe...` stack — while its **live stack is `0x45ffe340`**. The preceding interrupt on that CPU was serviced at `IP=0x7004dc00 SP=0x45ffe8d8`. The `0x45ffe...` stack has been overwritten with another stack's frame contents (RIP/RBP/R15 from `0x35ffe...`), so the return chain resumes on a stack address and takes `#UD`.

This is the same mechanism the `enter_user_space` comment already names: the exit path copies a trap frame through `[target_sp-296, target_sp)` on the shared per-CPU stack, where parked kernel tasks keep live frames; the copy overwrites them. The observed values being another stack's addresses (not a single return address) point at the **frame copy destination**, not the `spinlock_release_irqrestore` epilogue (GB-206 already refuted that).

**Not yet done (why this card is released, not closed):** the DoD's clause 1 requires the *store instruction and slot* named. The next instrument is a watchpoint on the `0x45ffe...` return slot (or a canary around the `enter_user_space` copy destination) on a run that reproduces this shape; the `-d int` trace alone localises the corruption to the cross-stack copy but does not name the writing instruction. Clause 2 (the exit path stops writing into the frame) is a code change after that.

Releasing to **Ready** with this measured fact so the assigned debug lane can take the watchpoint step; no claim is parked.
