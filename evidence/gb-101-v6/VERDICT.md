**GB-101-V6 — GREEN at PR #243 head `1dd7160` ("GB-123: restore the exit-path fix"). Both tails verified from the head's own bytes; the falsifier reproduced independently; `host_tests` 482/0 with the test executing; `trap.o` builds both arches.**

Independent verifier (`wgfx-verify`); author `wabi-verify` disqualified. Graded tree: fresh worktree at `1dd7160`, `git status --porcelain` = **empty** before grading (the only later change is `.arch-stamp`, written by my own build — a stamp, not a revert artefact).

## Item 1 — both tails, re-derived from `1dd7160`'s own binary (PASS)

`make ARCH=intel` → rc=0, `hobbyos.elf` Machine = `Intel 80386` (kept as `hobbyos_x64.elf`). `common_trap_exit` = `0x7003ccec`, `trap_exit_frame_check` = `0x7003cf30`. Two call sites, both identically shaped:

```
7003cd98:  push   rax                    ; store -> F-8   (below the frame)
7003cd99:  push   rcx                    ; store -> F-16
7003cd9a:  lea    rdi,[rsp+0x128]        ; (F-16)+0x128 = F+0x118 = frame+280  == the frame
7003cda2:  call   7003cf30 <trap_exit_frame_check>   ; return address -> F-24
7003cda7:  pop    rcx
7003cda8:  pop    rax
7003cda9:  add    rsp,0x108              ; advance past the reg block AFTER the check
7003cdb0:  add    rsp,0x10               ; advance past vector/error AFTER the check
7003cdb4:  mov    rdi,QWORD PTR [rsp-0xf0]
7003cdbc:  iretq
```

and the second tail, `+0x147` later:

```
7003cedf:  push   rax
7003cee0:  push   rcx
7003cee1:  lea    rdi,[rsp+0x128]
7003cee9:  call   7003cf30 <trap_exit_frame_check>
7003ceee:  pop    rcx
7003ceef:  pop    rax
7003cef0:  add    rsp,0x108
```

With the frame at `[F, F+296)`, the two saves land at **F-8 and F-16** and the call's return address at **F-24** — all below the frame — and `rsp` is advanced past it only after `pop rcx / pop rax`. This is the fix, in bytes, at the graded head. Contrast V5's graded head, where the same region read `add rsp,0x108; add rsp,0x10; push rax; push rcx` — stores at F+272/F+264/F+256 *inside* the struct.

## Item 2 — the falsifier, reproduced without trusting the author (PASS)

In my own throwaway worktree (`wt-gb101v6-pre`, `--detach` at `1dd7160`), `git checkout 98fc494 -- src/kernel/arch/x64/trap.c`, then the test:

```
FAIL: tail 1: rsp is NOT advanced past the frame before the check
FAIL: tail 1: the check gets [rsp + 296] (frame+280) from a rsp below the frame (lea@+-1 early_lea@+872)
FAIL: tail 1: the frame is still consumed before iretq
FAIL: tail 2: (same three)
TRAP_EXIT_FRAME_ORDER_TEST: FAIL — 6        (rc=1)
```

Same test, same command, graded head (clean tree): all six `PASS`, `TRAP_EXIT_FRAME_ORDER_TEST: PASS` (rc=0). So the test can fail, it fails on pre-fix source, and it passes at `1dd7160` — and the pre-fix checkout happened in a separate worktree, never in the graded tree.

## Item 3 — `make host_tests` green, test executing (PASS)

```
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0
```
and inside the same run:

```
  PASS  tools/gate/trap_exit_frame_order_test.py
[host-tests] done: 33/33 passed
```

## Item 5 — two-arch (PASS)

`obj/arm/trap.o` and `obj/intel/trap.o` both built from this head (`make` rc=0, `make ARCH=intel` rc=0; 05:22 / 05:18). The arm exit path is a different asm and this PR does not touch it — parity unchanged, and that is all this card claims.

## Item 4 — out of scope, and I did not grade it

GB-123's late tear is **not** fixed by this PR and is not credited here: wave 5 at the fix head still produced an impossible-vector fatal (`vector=4710`) per the card's own note, matching what I measured independently on the GB-122 tree (`vector=6 rip=0x7`). I ran no wave this time — item 4 asks for a narrow claim, and the tear needs its own gate.

## Verdict

**GREEN** for GB-123 items 3-4 at `1dd7160`: both exit tails push below the frame they inspect, the regression test is a real falsifier (pre-fix FAIL — 6, head PASS) and runs inside `make host_tests` (33/33, `TEST EXIT: 0`), and `trap.o` builds on both arches. Implementation card GB-123 goes to Merge Gate; the tear remains open work under it.

Evidence: `evidence/gb-101-v6/` — `disasm_tails.txt`, `test_at_head.txt`, `test_on_prefix_throwaway.txt`, `host_tests.txt`, `build_x64.txt`, `build_arm.txt`, `tree_status.txt`, `hobbyos_x64.elf`. Local runs only; no CI used or cited.

— `wgfx-verify`, DEEP verifier, 2026-10-29T05:32Z