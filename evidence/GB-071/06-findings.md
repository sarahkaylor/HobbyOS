# GB-071 — measured findings (DEEP debug pass, 2026-10-21)

Revision: `83ff46b` (`origin/main` at claim time), host swarm-farm-2, intel T3 waves.
Instrument added (temporary, in this worktree): the user-register set on the fault line.

## What the shipped instrument already told us

`src/kernel/arch/x64/trap.c` already prints `sys=` (`last_syscall`, set on every native
syscall at trap.c:1854) and `ret=`. No patch was needed for that.

## What the new instrument measured

Two waves on `83ff46b` (+ the register print):

| run | fault lines | family B (`rw=r`, VA in kernel range) | family A-ish (`rw=w`, region=NONE) |
|---|---|---|---|
| plain (`obj/t3_intel.log`, before patch) | 60 | 18 | 42 |
| + registers | 40 | 13 | 27 |

(The plain run's raw serial was overwritten by the second wave; its numbers are the parse output in
`02-census.txt`, kept for that reason. The instrumented run's raw serial is `logs-t3-intel-usp.log`
and its fault lines are `07-fault-lines.txt`.)

So the family split varies run to run; the totals are wave-dependent (as GB-068 recorded).

**Every family-B line now names the carrier.** 13/13 in the instrumented run have `rdi == VA`:

```
[KERNEL] pid=3 (SHTEST.BIN) memory fault VA=0x755DDBC8 rax=0x1000000020 rdi=0x755DDBC8
  rsi=0xFFFFFFFFFFFFFFFF rdx=0x0 usp=0x17FFFFFFF8 PC=0x10000029F0 rw=r in=PROT region=NONE
  cr3frame=0x20C03000 asroot=0x20C03000 sys=-1 ret=0x0 -> killed
```

- `rdi` **is** the kernel-range pointer; `rsi = -1`; `rdx = 0`.
- `usp = 0x17FFFFFFF8` — the saved user stack pointer is legitimate, so the process is **not**
  running on a kernel stack (this rules out the "user SP was replaced" shape).
- `sys=` histogram over the 13 family-B faults: `-1` ×10, `20` ×2, `75` ×1.

## What the faulting instruction is

Disassembling the faulting PC in `obj/intel/shtest.elf`:

```
00000010000029e0 <print_console>:
  ...29e0: movq  $-0x1, %rsi
  ...29f0: cmpb  $0x0, 0x1(%rdi,%rsi)     <-- faults here
  ...29f5: leaq  0x1(%rsi), %rsi
  ...29f9: jne   ...29f0
```

That is libc's inlined `strlen` prologue: `rdi` is `print_console(const char *s)`'s **string
argument**, and the first backward byte probe (`p[-1]`) reads it. PC low-16 differs per program
(`29F0`, `21F0`, `22E0`, `5550`, `1230`, …) because each binary has its own `print_console` copy —
i.e. every wave program dies in the same helper, not in one shared location.

## What this REFUTES (measured, not argued)

- **Candidate 1 in the ticket ("a kernel address handed back by a syscall") is refuted for 10 of 13
  faults**: `sys=-1` means no native syscall had been made at all before the fault (trap.c:1854 sets
  `last_syscall` on every syscall). A syscall cannot have returned a pointer it was never called to
  return.
- **"The user ran out of / was switched onto a kernel stack"** is refuted by `usp=0x17FFFFFFF8`.

## What this does NOT yet name

The *producer* of the value in `rdi`. The remaining candidates, in order:

1. **The user-entry / register-restore path**: if a process's initial `rdi` (or the `rdi` restored on
   an interrupt return) carries the kernel stack pointer of the CPU it resumes on, the first string
   call in user code dies exactly this way — and it explains why the address is always
   `target_sp(cpu) - 0x28` (the frame size), which GB-068 derived arithmetically.
2. **A struct field written by the kernel**: `process_get_info_list` writes `char name[32]` inline
   (no pointer), so the obvious ABI-shape candidate in `shell_test.c:278` is *not* the carrier.

The decisive next experiment (one wave, prepared but not run): a probe that stores `rdi` to memory
and prints it **before any call**, so `rdi` at process entry is measured directly rather than
inferred. If it is kernel-range at entry, candidate 1 is the delivery and the fix belongs in the
x64 context-restore path; if it is a user address at entry and only becomes kernel-range later, the
producer is user-side and the search moves to the libc call sites.

## Honest status

DoD 1 (delivery named) is **not met** — the carrier (`rdi` of `print_console`) and the instruction are
named with evidence, and the syscall hypothesis is refuted, but the value's producer is still
candidate-1-vs-candidate-3, not a measurement. DoD 2/3/4 are therefore untouched. The ticket stays
**In Progress**; this comment is the handoff, not a completion.
