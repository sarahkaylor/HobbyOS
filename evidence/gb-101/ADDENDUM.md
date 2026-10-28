**GB-101 addendum, 22:55Z — I implemented my own 20:05Z recommendation and the wave refuted it. Reverted; the patch is kept as evidence so nobody re-proposes it.**

The recommendation was: for a kernel-mode return, reject `rsp >= 0x75000000` and report `[TORNFRAME]`. I wrote exactly that (`evidence/gb-101/rejected_guard.patch`, 68 lines, one hunk in `trap_exit_frame_check`) and ran a full intel T3 wave on it.

**What the wave said:**

```
T3_intel: 17947 console lines, 111 FAIL lines, 11 user memory fault records across 8 processes, 87 programs exited, halted=1
T3_intel: FAIL — 1 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
```
```
[TORNFRAME] kernel CS with user RSP: rip=0x0000000070039FB6 cs=0x0000000000000008 rsp=0x00000000755FFDC8 eflags=0x0000000000000206 cpu=0x0000000000000000
```
40 hits (2 CPUs × the 20-report cap), **first one at console line 64** — the start of the wave, not a rare crash — all with the *same* frame, and a FATAL still landed later in the run.

**So the constant is wrong, and this file already says why.** `trap.c:2307` records `__stack_top = 0x755ffbf0` — the kernel stack is at **0x755F_xxxx**, inside the same window as the user stacks (the earlier wave's faulting user RSP was `0x755AFC18`). A single global RSP bound cannot separate kernel stacks from user stacks on this machine; my guard classified a normal resume path as torn. That is precisely the false-positive failure GB-061 warns about in the same function's comment, and it is why I am reverting rather than shipping a guard that cries wolf 40 times a wave.

**What the instrument has to be instead:**

1. Compare `frame[3]` against the **current process's kstack bounds** (top and bottom, which the scheduler already knows) — not a global constant. A kernel-mode return whose target RSP is outside *that* kstack is torn; one that is inside it is a normal return, whatever the absolute address happens to be.
2. Name the process in the report (`[TORNFRAME] pid=… cs=… rip=… rsp=…`), because the wave's 40 hits were indistinguishable from each other without it.
3. Keep it out of `trap_exit_frame_check`'s hot path as a per-resume compare-and-branch, not a UART print per hit — the cap exists for that reason.

**Two-wave state of the defect itself:** 2 of 2 intel runs halt on a FATAL kernel-mode fault (run 1: 10859 lines / 83 FAIL; run 2: 17947 lines / 111 FAIL). The FAIL count is not stable between runs, but the crash is — so the DoD's "no impossible-vector fatal in 3 consecutive runs" is not a target that tuning can reach; the mixed frame has to be fixed first, and that producer is GB-100's `enter_user_space` return path.

**Files:** `t3_intel_tornframe.log` (+ `.gz`), `t3_intel_tornframe.tier.txt`, `tornframe_hits.txt`, `rejected_guard.patch`, plus the 20:05Z wave (`t3_intel_1.*`) and `FINDINGS.md`. Local runs only; no CI used or cited.

— `wgfx-verify`, DEEP verifier, 2026-10-28T22:55Z