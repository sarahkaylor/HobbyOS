# GB-125 — kernel-mode resume guard: what it proves, and what it does not

Claimant: `wgames-verify/gate` (DEEP verifier, headless), 2026-10-29 18:02Z.
Head: `215b79b` (`origin/main`), worktree `.worktrees/gb125-verify`, host farm (this VM).
No CI consulted; every number below is a local QEMU/T3 run or a host test.

## 1. The code fact, before any hypothesis

`src/kernel/arch/x64/trap.c`, kernel-mode branch of `enter_user_space` (pre-change):

```
    mov rax, gs:[24]
    lea rdi, [rax - 320]
    mov rsi, r12
    mov rcx, 37
    rep movsq              <- 296 bytes ending AT gs:[24], no page check
    lea rsp, [rdi - 296]
```

The user-mode branch (`7:`) does the same shape against `r13`. Neither checks the
destination. `vm_arch_walk()` is **not** usable for this guard: `arch/x64/mmu.c`
returns 0 unless `i3` is in 64..95, i.e. only the 64–96 GiB v2 window, while the
stack addresses this card is about are `0x17FFFFF...` (`i3 = 5`). A guard built on
it would report "absent" for present pages — so the guard walks the live tables
from CR3 itself.

## 2. What the guard is (patch: `30-guard-final.patch`)

`resume_kernel_copy_guard()` walks CR3 for every page in
`[gs:[24]-320, gs:[24]+296)` and requires P and RW at the leaf. On failure it emits
one `kline` record and the stub halts the CPU:

```
[RESUME-NOMAP] kernel-mode resume refused: reason=absent pid=1 target_sp=0x17FF0FF000 \
  range=[0x17FF0FEEC0,0x17FF0FEFE8) page=0x17FF0FE000 cr3=0x7550E000
```

It covers the **kernel-mode** branch only. GB-122's `vm_touch` guard
(`debug/gb122`, `evidence/gb-122/30-fix-final.patch`) was gated on
`gb122_user_resume` and its own comment asserts "a kernel-task resume iretqs onto
its own already-resident stack" — so this card's target is the branch GB-122
explicitly left alone, not a repeat of a measured failure.

## 3. The falsifier: the unguarded head, same forced input (`11-falsifier-unguarded-serial.log`)

The card's premise had to be tested, not assumed, so the identical absent
destination was forced on the **unmodified** head (`01-falsifier-build.patch`,
`gs:[24] = 0x17FF0FF000` before the copy):

```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
  RIP: 0x000000007003D952  RSP: 0x00000000755FFC08
  CS:  0x0000000000000008  SS:  0x0000000000000010  RFLAGS: 0x0000000000010093
  RAX: 0x00000017FF0FF000  RCX: 0x0000000000000025  RDI: 0x00000017FF0FEEC0
  ERR: 0x0000000000000002  CPU: 0  pid: 1
...
[KERNEL] Kernel-mode #PF in user address space -> killing pid=1
[KERNEL] Process 1:  exited unexpectedly or gracefully.
System halt.
```

Three things follow, all from that one line set:

- `RDI = 0x17FF0FEEC0 = dest` and `RCX = 0x25 = 37` — the fault is the `rep movsq`
  itself, at the destination the card names. **Family A is confirmed by
  construction, not by argument.**
- `ERR = 0x2` (write, not-present), `CS = 0x8`: a CPL0 store, exactly the
  "kernel store cannot demand-page a lazy stack page" class.
- **The pre-fix outcome for this input is not a `vector=20000` banner.** The kernel
  kills the pid and the guest halts cleanly. Family B did **not** follow from
  family A here.

## 4. The guard fires — control build (`10-control-refusal-serial.log`)

Same input, guarded tree, `GB125_PROVE_GUARD 1` (`00-guard-control-build.patch`):
the record quoted in §2 is emitted, no `vector=` banner follows, the CPU halts.
So the refusal path is reachable and the guard is not a no-op.

## 5. DoD item 2 is **not met as written**

Item 2 asks the GB-089 single-program gate to produce the refusal line *instead of
a `vector=20000` banner*, quoted before/after. On this VM at this head the "before"
has no banner: `t3_nullentry intel fixed HELLO.BIN` **passes on the unmodified tree**
(`halted=1 fault_lines=0`, `System halt.`). With the guard it still passes, with
zero refusals (`12-nullentry-final-guarded.log`). The gate cannot distinguish the
two trees, so it cannot be the clause's evidence. I substituted the forced-condition
control (§4) and the falsifier (§3) and am labelling the substitution rather than
quoting a banner that did not occur.

## 6. DoD item 3 is met — and it exposed my own bug

Two normal intel T3 waves on the guarded build: **0 `[RESUME-NOMAP]` lines each**
(`20-…`, `22-…`, 51596 and 59507 console lines, 87 programs exited).

The first attempt (`19-FALSE-POSITIVE-control-define-bug-…`) refused on a real
wave and killed it at 0 programs. That was **my** defect, not the guard's: the
control block was `#ifdef GB125_PROVE_GUARD` against a define that is always set to
`0`, so the forced sp was compiled in. Fixed to `#if`, rebuilt, both waves re-run.
Disclosed because it is exactly the failure mode clause 3 exists to catch — and it
shows the wave does catch it, which is worth knowing.

## 7. It does not remove the tear, and the counts move the wrong way

Same head, same VM, interleaved builds:

| run | tree | console lines | FAIL | FATAL | programs exited | refusals |
|---|---|---|---|---|---|---|
| 21 | baseline `215b79b` | 18 973 | 110 | 18 | 87 | — |
| 23 | baseline `215b79b` | 65 047 | 309 | 25 | 86 | — |
| 20 | guard | 51 596 | 403 | 46 | 87 | 0 |
| 22 | guard | 59 507 | 245 | 48 | 87 | 0 |

Baselines 18/25 FATAL, guarded 46/48 — no overlap, n=2 each. The guard never
refused anything in those waves, so it is not *refusing* its way to more faults;
its only effect on the path is an extra call and page walk before the copy, which
is a plausible timing perturbation on a path that is demonstrably timing-sensitive
(GB-128 measured 29 FATALs on a different host for the same head). **I cannot name
the mechanism, and I am not going to call this variance to make the patch look
clean.** On this evidence the guard does not earn a merge: it does not close the
tear, and it cannot be shown not to worsen it.

## 8. The refusal policy is worse than the bug it guards

Without the guard the absent destination costs **one process** (`killing pid=1`)
and the wave continues. With the guard it costs **the CPU**, and on `T3_SMP=1` that
is the wave. Item 1 says "refuse loudly … do not silently skip the copy", and this
does that, but the correct landing for a refusal is the same one the #PF path
already takes — kill the pid, take another task — not `hlt`. That needs a return
path out of `enter_user_space` into the scheduler (today any return is treated as a
defect by `resume_returned_halt()`), which is a larger change than this card
authorised. Flagged, not attempted.

## 9. DoD items 4-6

- **Item 4 — why the page is absent: NOT established.** The guard is
  defence-in-depth only. It does not explain the absence, and I am not folding GB-122's
  second class into this card. Note the guard cannot even see the user-mode branch's
  copy (`r13-320`), which is where GB-122 measured `pte=0`.
- **Item 5 — arm parity.** The same class exists on arm:
  `src/kernel/arch/arm/boot.s:424-470` copies **272 bytes to `target_sp-272`** with
  no page check, then `eret`. It is not guarded here because the failure classes
  differ: an EL1 data abort on that copy is *recoverable* — `arch/arm/trap.c:1748-1751`
  calls `vm_handle_fault` and `return; /* page is live; eret retries the EL1
  instruction */` — while the x64 CPL0 #PF path kills the pid and, on the resume
  path, ends the wave. One sentence, as the clause asked.
- **Item 6.** `make ARCH=arm` → exit 0 (`40-build-arm.log`). `make disk.img
  ARCH=intel` → exit 0 (`42-build-intel.log`). `make host_tests` → exit 0,
  **1096 PASS / 0 FAIL** (`41-host-tests.log`). The last clause — "the guard's test
  must fail on the current head" — has **no test in the tree wired to the tier**;
  what exists is the forced-condition falsifier in §3, run by hand. Reported as a
  missing gate clause, not as a satisfied one.

## 10. What would settle it

1. The exact revision + manifest + host that produced `vector=20000` (GB-131's
   `ed65c8f` reproducer did not reproduce here: the nullentry gate passes on this VM).
   Without it, "the guard turns a banner into a refusal" is untestable.
2. A refusal that kills the pid instead of the CPU (§8), then the guarded/baseline
   FATAL counts re-measured with n≥4 per side before any claim about the tear.
3. Decide the real question the bisects keep circling: **who makes `gs:[24]` point at
   an absent page**, and why the user-mode branch's copy (`r13-320`) never shows the
   same fault. That is GB-122's second class, not this guard.

**Verdict: no RED/GREEN.** This is a `debug` card; the deliverable is the mechanism
evidence above plus a guard that provably fires (control) and provably never
false-fires on a normal wave (2 waves, 0 refusals) — but does not remove the tear.
Recommendation to the Coordinator: do not merge `30-guard-final.patch` as the fix;
keep its falsifier (§3) and the policy finding (§8).