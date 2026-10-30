# GB-095-V — verdict evidence (DEEP verifier, 2026-10-30)

Graded head: **`0d78a4d`** (PR #248, base `56a7ab0`), diff `merge-base(56a7ab0, 0d78a4d)=8c30226..0d78a4d`
= `src/kernel/arch/x64/trap.c` (+3/−1), `src/kernel/program_loader.c` (+20).

Baseline, same host, same hour: **`56a7ab0`**.

## Clause 1 — is the ABI claim true in this source?

`src/kernel/arch/x64/trap.c:515-518`:

```
 * rdi, rsi, rdx, r10, r8.  The ISR saves [rsp+40]=rdi -> regs[5],
 * [rsp+32]=rsi -> regs[4], [rsp+24]=rdx -> regs[3], [rsp+72]=r10 ->
 * regs[9], [rsp+56]=r8 -> regs[7] (regs[2] holds rcx, the clobbered
 * return address — never an argument).
```

Checked against the asm that builds the frame, not the comment:

```
3401:"    mov [rsp + 0], rax"      3410:"    mov [rsp + 72], r10"
3403:"    mov [rsp + 16], rcx"     3411:"    mov [rsp + 80], r11"
```

`regs[k]` is the qword at `rsp+8k`, so offset 72 is `regs[9]` and offset 16 is
`regs[2]`. **regs[9] is r10 (4th argument); regs[2] is rcx, clobbered by
`syscall` with the return address.** The PR's premise is correct in this source.

## Clause 2 — the class

4+ argument handlers, and the register each reads its 4th argument from
(`30-regs-audit.txt`, 65 handlers scanned):

| handler | 4th arg read | verdict |
|---|---|---|
| `sys_select` | `regs[9]` (r10) | correct |
| `sys_socketpair` | `regs[9]` | correct |
| `sys_thread_create` | `regs[9]` | correct |
| `sys_getsockopt` | `regs[9]` | correct |
| `sys_setsockopt` | `regs[9]` | correct |
| `sys_spawn` | `regs[9]` (was `regs[2]`) | **this fix** |
| `sys_spawn_ex` | `regs[9]` | correct |
| `sys_wait4` | `regs[9]` | correct |
| `sys_getenv` | `regs[9]` + `regs[7]`/`regs[8]` (6 args) | correct |

Direct grep for the failure shape, both heads:

```
(pre-fix 56a7ab0)   935:  int stderr_fd = (int)tf->regs[2]; // rcx
                    1676:  tf->regs[2] = opt;      <- write, return slot
                    1690:  tf->regs[2] = opt;      <- write
                    2928:  uart_print_hex_raw(tf->regs[2]);  <- diagnostic

(graded 0d78a4d)    1678:  tf->regs[2] = opt;
                    1692:  tf->regs[2] = opt;
                    2930:  uart_print_hex_raw(tf->regs[2]);
```

**No sibling instance.** `regs[2]` is otherwise only written or printed, and
`tf->regs[10]` (r11) is never read as an argument at either head.

## Clause 3 — failing first, then passing

Pre-fix (`56a7ab0`), `22-base-wave.log`:

```
EXPAND.BIN: [TSORTTEST] FAIL cycle from FILE (stderr + rc 1) (stderr)
[TSORTTEST] FAIL cycle from stdin names '-' (stderr)
[TSORTTEST] FAIL two operands rejected (rc 1 + stderr) (stderr)
[TSORTTEST] FAILURES: 4
```

Graded (`0d78a4d`), `12-graded-wave.log`: **16 × `[TSORTTEST] PASS`, 0 ×
`[TSORTTEST] FAIL`**.

**Deviation, stated plainly:** the string the PR quotes, `ALL PASSED (16
checks)`, **does not appear in my serial.** The suite starts three times and
emits 16 PASS lines with no summary line (other suites in the same wave do
print `ALL PASSED (n checks)`). The substance — 16 checks, 0 failures — holds;
the quoted string is not reproducible from this wave.

## Clause 4 — the probe

Graded head, `TSORT.BIN`:

```
[GB095-FD] TSORT.BIN stderr_fd=5 pipeinstall=1 fd2=0x72
```

Pre-fix counterfactual, **measured** by transplanting the identical probe block
onto `56a7ab0` with the `regs[2]` read site untouched (`24-base-probe-wave.log`):

```
[GB095-FD] TSORT.BIN stderr_fd=29108 pipeinstall=0 fd2=0xFFFFFFFFFFFFFFFF
[GB095-FD] SH.BIN   stderr_fd=31012 pipeinstall=0 fd2=0xFFFFFFFFFFFFFFFF
[GB095-FD] CAT.BIN  stderr_fd=28468 pipeinstall=0 fd2=0xFFFFFFFFFFFFFFFF
```

`29108` is a return address, not a descriptor: the clobbered rcx arrived as
`stderr_fd`, the loader's range guard rejected it, and `open_fds[2]` stayed
unset. That is the root cause, observed rather than quoted.

**Cap does not hold.** The source is `if (gb095_n < 40)` — but the graded wave
printed **71** `[GB095-FD]` lines and the instrumented baseline **84**. The
counter is a non-atomic static read-modify-write shared by all CPUs, so under an
8-CPU wave several spawns pass the test together. "Rate-limited" is bounded but
not the stated 40; the per-wave count is 71, not 40.

## Clause 5 — one intel wave, and a fresh baseline

```
graded 0d78a4d : 15405 console lines, 74 FAIL, 6 FATAL kernel-fault lines,
                 80 programs exited, halted=1
base   56a7ab0 : 12026 console lines, 81 FAIL, 7 FATAL kernel-fault lines,
                 82 programs exited, halted=1
```

Both waves halt, both on the same signature —

```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002
```

— so the wedge is **not** introduced and **not** fixed by this change, exactly
as the PR says. The graded wave's FAIL count is lower (74 vs 81) but neither
wave completes, so no completion claim is made here either.

## Clause 6 — builds and host tests

```
make disk.img ARCH=intel   EXIT=0
make ARCH=arm              EXIT=0
make host_tests            32/33 passed
```

`host_tests` is **not** 33/33: `tools/board/merge_pr_test.py` fails with
`merge_pr: error: unrecognized arguments: --head deadbeef`. The same test fails
identically at `56a7ab0` (`25-base-host-tests.log`), so it is pre-existing and
not attributable to PR #248 — but the count is 32/33, not 33/33.

## Clause 7 — arm

`src/kernel/arch/arm/trap.c:798-802` reads `filename/stdin_fd/stdout_fd/
stderr_fd` from `regs[0..3]` (AAPCS r0..r3, 4th argument in r3). Arm has no
`syscall`-clobbers-a-register-with-the-return-address behaviour, and the patch
does not touch the arm tree. **No arm test exercises the x64 asm changed here**;
arm is a build-and-regress statement only.