# GB-090-V — gate run at 6b6f8a2

Worktree `hermes-9642df51`, branch `verify-gb090v` at **6b6f8a2** (= `origin/main`), swarm-farm-2.

## Blocking finding first: this sha does not build for ARM

Clauses 2 and 3 of this card require ARM runs. ARM cannot be linked at all:

```
$ make ARCH=arm hobbyos.elf
ld.lld: error: undefined symbol: resume_mark
>>> referenced by process.c:1687 (src/kernel/process.c:1687)
>>>               obj/arm/process.o:(schedule)
>>> referenced by process.c:3331 (src/kernel/process.c:3331)
>>>               obj/arm/process.o:(start_scheduler)
make: *** [Makefile:736: hobbyos.elf] Error 1

$ nm obj/arm/process.o | grep resume_mark
                 U resume_mark
```

`resume_mark` is defined **only** in `src/kernel/arch/x64/trap.c:2274`, while `src/kernel/process.c`
(shared by both arches) declares and calls it unconditionally at lines 1686-1687 and 3330-3331. The
instrumentation arrived in **25f8512** ("GB-057: resume_mark instrumentation"), which is an ancestor of
this sha. ARM has no definition, so the link fails; x64 links fine.

That is a two-arch parity break on `main` — a `#ifdef __x86_64__` guard (or an empty ARM stub) is all it
needs — and it blocks every ARM gate in the lane, not just this one.

## Clause 1 — the mechanism, partially proven at instruction level

The card's mechanism: short-read accounting across `STAGE_CAP` in `console_write_user` against an 8 KB
`char stage[8192]` frame on the syscall path. What the binary says:

```
$ objdump -d hobbyos.elf | grep -A20 '<console_write_user>:'
70010bbe:  81 ec 28 20 00 00    sub    $0x2028,%esp        # 8232-byte frame
```

and the syscall-entry stack guard it runs under:

```
src/kernel/arch/x64/trap.c:1916      if (kernel_stack_low(8u * 1024u)) { panic }
src/kernel/arch/arm/trap.c:1884      if (kernel_stack_low(8u * 1024u)) { panic }
```

**8232 > 8192.** The guard refuses to enter a handler only when *less than* 8192 bytes remain, so a
syscall entering with exactly 8192 free passes the check and then takes a 8232-byte frame — 40 bytes
below the slice base. The comment above the guard says the stacks are "contiguous static 64 KiB per-CPU
slices — an overflow faults nowhere, it corrupts the adjacent core's stack or kernel data". So the
underflow lands in the neighbouring slice, with no fault to announce it.

That is store/stack-bound evidence for a 40-byte hole in the SEC-10 margin. It is **not** yet the card's
full claim: clause 1 asks for the falsifier (a short-read `vm_user_read`, or a `stage[]` overflow)
reproducing `MD5TEST FAIL abc digest` and `CMPTEST FAIL identical files`. With ARM unbuildable I cannot
run that on the card's platform.

## Clauses 2, 3, 4

- **2 (ARM run: 0 `MD5TEST`/`CMPTEST` content failures)** — not runnable: ARM does not link at this sha.
- **3 (write > `STAGE_CAP` byte-exact on both arches)** — same blocker; the x64 half is runnable in
  principle, but "both arches" cannot be satisfied.
- **4 (grade on raw vs genuine, not `main`'s glued count)** — not reached; no ARM serial exists to count.

## Verdict

**RED** — not because the parent card is wrong, but because the gate cannot be executed: `main` at
6b6f8a2 does not build for ARM, so the card's own platform is unavailable. What I can add is a concrete
lead for clause 1: the syscall-stack guard's 8192-byte margin is 40 bytes smaller than the
`console_write_user` frame (0x2028), and the guard's own comment says an overflow there corrupts the
adjacent core's stack silently.

Two things for the Coordinator: (a) fix the ARM link (one `#ifdef`), after which this card can be graded
properly; (b) decide whether the 8192 → ≥0x2030 margin fix belongs to this card or its own.
