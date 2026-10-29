# GB-101-V4 — verdict: **GREEN** at `d50be85` (fix `4a80f17`), PR **#242**

Independent gate: I did not write this patch (`wabi-verify`, farm-1, is the author and is disqualified for this
card). I worked GB-123's instrument clause earlier, which never produced a line; the `[EXITFRAME]` capture and
the fix are the author's. Graded head: branch `ticket/GB-101-exit-frame-push` @ **`d50be85`** (fix commit
`4a80f17`, one file, `src/kernel/arch/x64/trap.c` +13/−16). farm-3, intel 8 CPUs, QEMU TCG. **No CI result is
cited, waited on, or used** (policy 2026-10-26).

| # | Clause | Result |
|---|---|---|
| 1 | re-derive the pre-fix stores from the graded head's binaries | **PASS** — offsets derived from both ELFs, not taken from the card |
| 2 | clobber gone: `[EXITFRAME]` before/after, same instrument, same run kind | **PASS** — the −24 slot goes from a kernel return address to a pushed register |
| 3 | invariant stated; what else could write those slots | **PASS with one window named** — all synchronous stores accounted for; an asynchronous push in the last 8 bytes before `iretq` is not tested |
| 4 | five intel waves, `halted=1` and FATAL counts, vector split | **PASS on the patch's own claim** — 0 impossible-vector in 5 waves, 58 vector-14 counted (GB-122's); **`halted=1` in 3 of 5**, see below |
| 5 | `make host_tests` green, `trap.o` both arches, arm stated | **PASS** — 482/0, `TEST EXIT: 0`; intel `trap.o` 222640, arm 154416 |
| 6 | a PR exists before merge | **PASS** — I opened **#242** (the branch had none) |

## 1 — the offsets, derived here

Pre-fix binary (`98fc494`), disassembled with `evidence/gb-101-v4/disasm_exit.py`:

```
== PRE-FIX-98fc494: common_trap_exit 0x7003d7bc..0x7003d88b (207 bytes)
   add rsp,+264          rsp now frame+264
   add rsp,+16           rsp now frame+280
   push rax/rcx          store lands at frame+272
   push rax/rcx          store lands at frame+264
   lea rdi,[rsp+0x10]    -> &frame argument = frame+280
   call 0x234            return address lands at frame+256
   iretq
```

Fixed binary (`d50be85`), same script:

```
== FIXED-d50be85: common_trap_exit 0x7003ccec..0x7003cdbe (210 bytes)
   push rax/rcx          store lands at frame-8
   push rax/rcx          store lands at frame-16
   lea rdi,[rsp+0x128]   -> &frame argument = frame+280
   call 0x244            return address lands at frame-24
   add rsp,+264
   add rsp,+16
   iretq
```

Two facts matter and both are mine, not the card's: the three stores move from **+272/+264/+256** to
**−8/−16/−24** (below the frame), and `&frame` handed to the helper is **+280 in both builds** — so the helper
inspects the same address with the same semantics, which is what makes the change behaviour-preserving rather
than just "different".

## 2 — the clobber, observed before and after

Author's instrument (`evidence/gb-123/instrument.patch`, applied to the fixed tree here, run taken by me):

```
before (pre-fix, author's capture in the branch):
[EXITFRAME] vector_slot(-8)=0x0  error_slot(-16)=0x100  spsr_slot(-24,retaddr)=0x7003D87F  rip=0x70039FB6  cs=0x8
after  (fixed head, my run, evidence/gb-101-v4/04-instrumented-wave.serial.log):
[EXITFRAME] vector_slot(-8)=0x0000000000000000 error_slot(-16)=0x0000000000000020 spsr_slot(-24,retaddr)=0x0000000000000206 rip=0x0000000070039FB6 cs=0x0000000000000008
```

Same instrument, same `rip`, same `cs`. The −24 slot held `0x7003D87F` — a kernel text address, i.e. the call's
own return address sitting in the frame — and now holds `0x206`, a value pushed from a register. The store
moved; the frame stopped being the stack.

## 3 — the invariant, and the one window left

**Invariant: after `common_trap_exit` begins, no `push`, `call` or register save may write a word the fatal
printer reads as a frame field.** The patch satisfies it for every synchronous store in both exit tails
(`common_trap_exit` and the inline tail in `enter_user_space`).

Residual, stated rather than glossed: between the final `add rsp,0x10` and the `iretq`, `rsp` sits at
frame+280, so a *hardware* push there (an interrupt or NMI taken in that 2-instruction window with IF=1) would
land at frame+272 — the error slot. I did not force that window, so it is **untested**, not cleared. It is a
different mechanism from the one this patch fixes (the CPU's own entry frame, not the kernel's own pushes).

## 4 — five intel T3 waves at `d50be85`

Counting only **whole, unshedded** lines (`tally_clean.py`); the console is shredded by concurrent writers, and
a lenient regex reads `142` or `1879291950` out of two lines run together — I checked one such hit and its
`cr2` tail was garbage, so it is a printout artefact, not a frame.

| wave | clean fatal lines | impossible-vector (this patch's business) | vector 14 (GB-122's) | `halted=` |
|---|---|---|---|---|
| 1 | 4 | **0** | 4 | 1 |
| 2 | 15 | **0** | 15 | 1 |
| 3 | 17 | **0** | 17 | 0 |
| 4 | 20 | **0** | 15 (+5 vector 6) | 0 |
| 5 | 7 | **0** | 7 | 1 |

`grep -c "FATAL kernel-mode fault"` per serial: 9, 22, 22, 31, 7 — the difference from the clean counts is
shredded printouts, which is GB-121/#265's territory, not this patch's.

**The deviation, named:** the card asks for `halted=1` on each of the five; three halted, two did not. Those
two are the ones with the most CPU-killing vector-14 page faults (22 and 31), and wave 3's serial ends in
`[IDLESTUCK] … lock_owner=4 park_returns=0` — CPUs lost to GB-122's defect, so the wave could not print
`System halt`. That is a different defect, which the card itself says not to blame on this patch, but the
literal clause is 3/5 and I am not going to call it 5/5.

Per the card's RED conditions — unmeasured item, author's session posting, *any* impossible-vector fatal at the
graded head — none apply. Hence GREEN for what this patch claims to fix, with the halted count reported as
measured.

## 5, 6 — build and delivery

```
Checks run:    482
Checks failed: 0
TEST EXIT: 0           HOST_RC=0
obj/intel/trap.o 222640   obj/arm/trap.o 154416   (ARM_RC=0)
```

Arm: the exit tail is x86-specific inline asm; the arm tree has its own trap exit and no `common_trap_exit`
with this push sequence, so the patch is x64-only **by construction**, and arm still builds and links. Stated,
not left silent, per the clause.

PR **#242** opened by me (the branch had none): https://github.com/sarahkaylor/HobbyOS/pull/242

## Reproduce

```
cd .worktrees/verify-gb101v4        # d50be85
python3 evidence/gb-101-v4/disasm_exit.py hobbyos.elf FIXED
python3 evidence/gb-101-v4/disasm_exit.py ../gb123/hobbyos.elf PRE-FIX   # 98fc494 build, same asm tail
bash evidence/gb-101-v4/five_waves.sh && python3 evidence/gb-101-v4/tally_clean.py
git apply evidence/gb-123/instrument.patch && make disk.img ARCH=intel -j32   # then one wave
make host_tests && make disk.img ARCH=arm -j32
```