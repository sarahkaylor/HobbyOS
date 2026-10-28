**GB-101 — DEEP verdict: RED, with the mechanism named. Reproduced on `origin/main` @ `4626084`, intel T3, one wave; the impossible-vector fatal is still there and the detector the second clause needs is not merged.**

This is a debug ticket, so I am not posting a gate verdict on it; the DoD has two halves and both are measured below. I moved the card to **In Review** for the Coordinator with the artifacts.

## What the wave shows

`T3_MONITOR=1 T3_LOG=evidence/gb-101/t3_intel_1.log tools/t3_tier.sh intel` → tier says:

```
T3_intel: 10859 console lines, 83 FAIL lines, 6 user memory fault records across 6 processes, 84 programs exited, halted=1
T3_intel: FAIL — 1 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
T3_intel EXIT=1
```

The fatal itself (line 1101, console-glued but readable):

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=1879291950 rip=0x000000002BFFE928 cr2=0x00000017FFFFF2F8 pid=23 err=7 cr3=0x0000000021A20000 pd480=0x00000000810000FB
```

**`1879291950` is `0x7003B82E`** — a user-image code address, not a vector. The wave also carries **six** additional `[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14` panics; one of them, verbatim:

```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
  RIP: 0x000000007003D8DF  RSP: 0x00000000755AFC18
  CS:  0x0000000000000008  SS:  0x0000000000000000  RFLAGS: 0x0000000000010046
```

So: **CS=0x08 (kernel), SS=0x0000 (invalid), RIP and RSP both in the user image.** That is not a frame the CPU can have pushed — the DoD's own words.

## Both halves of the DoD

**Half 1 (no impossible-vector fatal in 3 consecutive runs) — FAILS on the first run.** One occurrence refutes it; I did not need the other two, and I would not spend two more waves on a card that is red on wave 1.

**Half 2 (an injected torn frame FAILS the tier by name) — cannot be evaluated at this head.** The detector is in PR #187 (`ticket/101-trapframe`), which is **open, `merged=false`, `mergeable_state=dirty`** (needs a rebase), and `grep -rn TORNFRAME src/` on main returns nothing. That matches what `wabi` wrote when it released the card at 19:42Z: clause 2 has been green in both prior gates, clause 1 is what fails.

## The mechanism, from the numbers

Two facts pin it to the resume path, and one of them names the guard that lets it through:

1. **The "vector" is a user PC.** `0x7003B82E` is a user-image code address (the same 0x7003xxxx range the wave's user PCs occupy, e.g. the panic's `RIP=0x7003D8DF`), not an interrupt number. The handler is reading a user RIP out of the slot where the vector should be — a shifted or mixed frame.
2. **The panic frame is the shape `enter_user_space`'s kernel-mode branch produces when it is handed a mixed frame.** In `src/kernel/arch/x64/trap.c:3525`:

```c
    mov rax, [r12 + 280]      /* tf->cs */
    and rax, 3
    cmp rax, 3
    je 7f                     /* user-mode resume */
    /* kernel-mode resume: copy 37 quads onto the task's own stack and iretq,
       which pops RIP/CS/RFLAGS only — never RSP/SS */
```
A frame whose RIP/RSP belong to the user but whose `cs` reads `0x08` takes that branch and is iretq'd **at CPL0 onto the user stack** — exactly `CS=0x08, SS=0x0000, RSP=0x755AFC18`.

**Why the existing check cannot fire on it.** `trap_exit_frame_check` (`trap.c:103`) decides "implausible" like this (lines 111-120):

```c
  if ((cs & 3u) != 3u) {
    /* GB-061: ... Judge kernel returns on the LOW bound only;
       the upper range check stays user-only, because 0x70000000..USER_VA_BASE
       is a legitimate kernel text window. */
    if (rip >= 0x1000u)
      return;
```
For a **kernel-mode** return it only rejects `rip < 0x1000`. The guard therefore cannot see a kernel-CS frame carrying a **user-stack RSP** — and RIP is not a usable discriminator here, because the kernel's own `.text` reaches `0x70042cc0` (the file says so at `trap.c:2308`), which covers `0x7003D8DF`. So the one field that does discriminate is RSP, and there is already a bound for it in the same file — `trap.c:2307` records the kernel-stack guard as **`top < 0x75000000`**. The panic's `RSP=0x755AFC18` is above it: a user stack, on a kernel return.

## What I would do next (for the Coordinator, not a fix I can validate inside this claim)

1. **Rebase PR #187** so the detector and the resume path are on one head; without it half 2 is unmeasurable, and the card has cost three rounds for that reason.
2. **Close the kernel-return hole in `trap_exit_frame_check`**: for `(cs & 3) == 0`, also reject `rsp >= 0x75000000` (the same bound at `trap.c:2307`), and print `[TORNFRAME] pid=… cs=… rip=… rsp=… ss=…` so the tier's own FAIL detector catches it by name. That converts this silent halt into the DoD's second half, and it is a two-line change in a function that already exists.
3. **Only then** chase the producer of the mixed frame — the GB-077 discriminator already measured a fork child resumed at `fork+0x10` with RSP `0xBC8` deep, and GB-100 owns `enter_user_space` returning instead of iretq-ing. Both are this family; the frame check is what makes the next run *say which one* instead of halting the CPU.

## Evidence

`evidence/gb-101/` — `t3_intel_1.log` (+ `.gz`), the tier output, and `fatal_excerpt.txt` (the two blocks quoted above). Local runs only; no CI cited, used, or gated on.

— `wgfx-verify`, DEEP verifier, 2026-10-28T20:05Z