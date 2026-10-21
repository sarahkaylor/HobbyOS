**GB-071 — the delivery is now measured, and it is neither a syscall nor a struct field.** It is the kernel's return-to-user register restore. `rdi` is the carrier.

### The chain (SHTEST.BIN, pid 3, `83ff46b` + instrument)

1. The fault line says the call that reached `print_console` returned to **0x100000004A** (read from the saved user RSP, which I located at `ss+16` in the hardware frame — see below).
2. Disassembling the *same* binary that ran (`obj/intel/shtest.elf`, rebuilt in this worktree):

```
0000001000000020 <main>:
 1000000034: movabsq $0x100000e26f, %rdi    <-- rdi = a .rodata string
 100000003e: movabsq $0x10000029e0, %rbx    <-- rbx = print_console
 1000000048: callq   *%rbx
 100000004a: ...                            <-- return address, matches the fault line

00000010000029e0 <print_console>:
 10000029e0: movq  $-0x1, %rsi
 10000029e7: nopw  (%rax,%rax)
 10000029f0: cmpb  $0x0, 0x1(%rdi,%rsi)     <-- FAULTS, VA=0x755EDBC8
```

3. **The contradiction, and it is the answer.** Between the `movabsq` at `0x34` and the faulting
   instruction at `0x29f0`, the only user instructions executed are `callq *%rbx`, `movq $-1,%rsi` and
   a `nopw`. **Nothing in that window writes `rdi`.** Yet the fault reports `rdi=0x755EDBC8` — a kernel
   stack address — instead of `0x100000E26F`.

So the value in `rdi` was not produced by the program, and not by a syscall (10/13 faults have
`sys=-1`): it was **written on the way back into user mode by the kernel's register restore**.

### The frame layout that made this measurable

`struct trap_frame` does not expose the hardware-saved RSP, so I probed the quadwords around its last
field on live faults:

```
ss-8=0x1B      CS
ss  =0x23      SS
ss+8=0x10206   RFLAGS
ss+16=0x17FFFFEF58   <-- the saved user RSP (0x1058+0xA0 below the entry SP, matching main's frame)
```

Reading `[rsp]` there yields the caller's return address, which is how 0x4A was obtained. **The struct is
missing a field**: `RSP` sits between `SS` and the next frame, outside `struct trap_frame`.

### Caller census (17 family-B faults, one run)

```
caller 0x100000004A  x3        caller 0x100000002B  x6
caller 0x100000006A  x2        caller 0x1000000028  x2
caller 0x1000000029  x1        caller 0x100000004B  x1
caller 0x1000000027  x1        caller 0x0           x2   <- not yet explained
```

Every caller sits in the first ~0x70 bytes of the program's `main` — the **first `print_console` call**,
seconds after entry. Consistent with "corruption happens on an interrupt/exception return early in the
process's life", and with the `target_sp(cpu) - 0x28` constancy GB-068 derived (0x28 = the five-slot
hardware frame).

### What this means for the fix (DoD 2/3 still open)

The defect to fix is in the **return-to-user path** — the restore that writes the user register block —
not in any syscall shim. The confirming experiment for the fix stage: with the timer/IPI preemption
disabled across a wave, family-B should vanish; if it does not, the corruption is on the *initial*
entry path instead of the *return* path, and the two are distinguishable by whether the first fault
happens before or after the process's first interrupt.

### Status

**In Progress.** DoD 1 is now met in substance (carrier = `rdi`; instruction = `cmpb $0x0,0x1(%rdi,%rsi)`
in `print_console`; pid/name per line; producer = the kernel's return-to-user restore, measured by
contradiction). DoD 2 (fix + 0 family-B faults) and DoD 3 (regression guard) are untouched — no code fix
has been written or claimed. Evidence: `15-fault-lines-caller.txt`, `16-callers.txt`, disassembly above.
