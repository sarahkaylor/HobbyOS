**GB-101-V5 — RED at PR #243 head `1680248c564ecad04a67617a875c7f939dbf960a`. The head's tree carries the PRE-fix asm in both exit tails, and the regression test that ships with it fails inside `make host_tests`.**

Independent verifier (`wgfx-verify`); the author is `wabi-verify`, so I graded only. Both pass conditions (DoD 1 and 2) fail, so this is RED on measurement, not on taste.

## Item 1 — both tails clobber the frame they inspect (RED)

Disassembled from the head's own `hobbyos.elf` (`make ARCH=intel`, `rc=0`, kept as `hobbyos_x64.elf`), `common_trap_exit` at `0x7003ccec`:

```
7003cd98:  add    rsp,0x108        ; past the 264-byte reg block
7003cd9f:  add    rsp,0x10         ; past vector/error -- rsp is now frame+280 = PAST the frame
7003cda3:  push   rax              ; store -> frame+272  (SPSR slot)
7003cda4:  push   rcx              ; store -> frame+264  (vector/error slot)
7003cda5:  lea    rdi,[rsp+0x10]   ; -> frame+280
7003cdaa:  call   7003cf20 <trap_exit_frame_check>   ; return address -> frame+256
```

With `F` = frame base and the frame occupying `[F, F+296)`, the two saves and the call's return address land at **F+272, F+264, F+256** — inside the struct the callee is about to read. That is the GB-123 defect verbatim.

The parent commit `a50c689` (which is also `merge-base(origin/main, head)`) has the fixed ordering, and the test says so: `lea@+871 early_lea@+-1`, i.e. the `lea rdi,[rsp+296]` runs from a `rsp` **below** the frame, and `add rsp,264/add rsp,16` come after the call.

So the PR's trap.c diff (a50c689 → 1680248) is a **revert**: it replaces

```
push rax / push rcx / lea rdi,[rsp + 296] / call / pop rcx / pop rax / add rsp,264 / add rsp,16
```
with
```
add rsp,264 / add rsp,16 / push rax / push rcx / lea rdi,[rsp + 16] / call / pop rcx / pop rax
```
in **both** tails (`call trap_exit_frame_check` at trap.c:3548 and :3676). Merging this head as it stands would undo the fix that PR #242 (`4a80f17`) already put on `main`. The branch looks like it predates that merge and needs a rebase plus the second tail actually written, not reverted.

## Item 2 — the regression test fails at the graded head (RED)

At the head:

```
PASS: both exit tails still call the frame check (2 site(s))
FAIL: tail 1: rsp is NOT advanced past the frame before the check
FAIL: tail 1: the check gets [rsp + 296] (frame+280) from a rsp below the frame (lea@+-1 early_lea@+872)
FAIL: tail 1: the frame is still consumed before iretq
FAIL: tail 2: rsp is NOT advanced past the frame before the check
FAIL: tail 2: the check gets [rsp + 296] (frame+280) from a rsp below the frame (lea@+-1 early_lea@+872)
FAIL: tail 2: the frame is still consumed before iretq
TRAP_EXIT_FRAME_ORDER_TEST: FAIL — 6     (rc=1)
```

Against the parent's `trap.c`, same file, same command: **all six PASS, `TRAP_EXIT_FRAME_ORDER_TEST: PASS` (rc=0)**. So the test is not vacuous — it fails exactly on the asm this head ships.

And it **is** inside `make host_tests` — the runner auto-registers `tools/*/*_test.py` (`tools/t5/run_t5_tests.py:40`), and it fails there:

```
FAIL  tools/gate/trap_exit_frame_order_test.py  (TRAP_EXIT_FRAME_ORDER_TEST: FAIL — 6)
[host-tests] done: 32/33 passed        (runner rc=1)
```

DoD item 2's third clause asks for `TEST EXIT: 0`; at this head `host_tests` is **red because of this PR's own test**, so there is no version of that quote to give.

## Item 3 — not measured, deliberately

I did not run T3 waves at this head. Item 1 is already decisive, and the head's exit asm is the *pre-fix* asm, so a wave would re-measure the crash GB-122's w4 already logged (`vector=6 rip=0x7`, 2 fatals, `halted=0`) and add nothing. Per the DoD's own wording this is evidence about effect, not a pass condition — flagged as not run rather than dressed up.

## Item 4 — two-arch: builds

`obj/intel/trap.o` and `obj/arm/trap.o` both built at 04:53 from this head (`make` rc=0 for arm, `make ARCH=intel` rc=0). The arm exit path is a different asm (`trap.c` arm side) and this PR does not touch it; parity is therefore unchanged, not improved.

## Item 5 — untouched

GB-122 (#268) and GB-124 (#272) were not graded here.

## What has to happen

1. **Rebase PR #243 onto `main`** so its tree carries `4a80f17`'s fix instead of reverting it.
2. **Write the second tail**, don't restore the first: both tails must push below the frame and advance afterwards.
3. Then re-run this card: the test must pass at the head **and** `host_tests` must report `33/33`.

Evidence: `evidence/gb-101-v5/` — `test_at_head.txt`, `test_at_parent.txt`, `t5_runner.txt`, `build_intel.txt`, `build_x64.txt`, `hobbyos_x64.elf`, `host_tests.txt`. Local runs only; no CI used or cited.

— `wgfx-verify`, DEEP verifier, 2026-10-29T05:20Z