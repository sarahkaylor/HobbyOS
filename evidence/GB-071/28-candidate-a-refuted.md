**GB-071 — candidate (a) is now fully refuted, and the static side of the bootstrap is closed off too.**

Two measurements/reads, no new wave:

**1. All 62 kernel-mode saves are the boot thread.** Sorting the `SAVECTX-KBUF` lines by privilege:

```
55  pid=1 cs=0 vec=129 rdi=0x7557EBD8
 5  pid=1 cs=0 vec=129 rdi=0x74540E78
 2  pid=1 cs=0 vec=129 rdi=0x74801008
```

Every `cs=0` (kernel-mode) save belongs to **pid 1** on vector 129 (the yield IPI) — the boot/kernel
context, which never returns to user mode. So *no user process* acquires a kernel-range `rdi` from a
kernel-mode save. The "C handler's `mov rdi,rsp` frame pointer gets saved as the user rdi" hypothesis is
dead, not merely rare.

**2. A fresh user process cannot inherit it either.** `process_create_internal` zeroes the whole register
block (`for (i=0;i<36;i++) p->context[i]=0;`, `process.c:1077`) and `process_set_entry` then writes only
ELR/SP/SPSR/CS/SS — so a new user process starts with `rdi = 0`. The *only* place `context[5]` is written
with a pointer argument is `process_create_kernel` (`process.c:1149`: `p->context[5] = (uint64_t)arg`),
and that is the kernel-thread constructor (pid 1's seed: `0x7557EBD8`, a kernel address).

So the remaining gap is narrow and specific: **some path hands a user process a `context[5]` taken from a
kernel-thread-shaped context** — most plausibly `fork`/`spawn` copying a context captured while the
*caller* was kernel-side, or a PCB slot whose context was written by the kernel-thread constructor and
not re-zeroed on the next create. Both are answerable by one print at `process_create_internal` (does a
slot arrive with a non-zero `context[5]` before the zeroing) plus one at `fork`.

Status unchanged: DoD 1 answered; DoD 2/3/4 open; ticket **In Progress**. This loop's net: two live
candidates eliminated, one measurement site named.
