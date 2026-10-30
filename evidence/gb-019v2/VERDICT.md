# GB-019-V2 — RED at e2455661 (verifier run, 2026-10-30)

Graded head **`e2455661`** (`ticket/19-ioctl`), merge-base `b1594f9`.
**Pre-rebase:** PR #255 (GB-014) is still open and unmerged (`state=open merged=False head=61e49d1`), so per this card's merge-order note this verdict does not authorise a merge; it must be re-graded at the post-#255 sha.

## Clause 1 — FAIL. There is no routing case.

The entire change, from `git diff b1594f9 e2455661 -- src/kernel/arch/x64/trap.c`:

```
+        case 16: tf->regs[0] = (uint64_t)(int64_t)(-25);  /* -ENOTTY */
+          return;
```

Nine lines, two of them code. It inspects **neither the request number nor the fd**. There is no `syscall_num = SYS_*` translation (unlike every other case in that switch: `case 0: syscall_num = SYS_READ`, `case 158: … arch_prctl`, `case 217: syscall_num = SYS_GETDENTS64`), no dispatch to `file_fcntl()`, `sys_fb_ioctl()` or the input-node ioctl, and no golden file of ioctl request constants.

Constants, established on this host from `tools/linux-abi/probe_abi.c` (clause 1's requirement) — the PR uses none of them:

```
F_GETFL=3 F_SETFL=4 F_GETLK=5 F_SETLK=6 F_SETLKW=7 F_DUPFD=0
TCGETS=0x5401 TIOCGWINSZ=0x5413 FIONREAD=0x541b FIONBIO=0x5421
```

## Clause 2 — the rows are still not measurable, and now I can say why

None of the four W0 members calls `ioctl` at all:

```
src/user/hello.c      ioctl=0
src/user/sh.c         ioctl=0
src/user/sh_aux.c     ioctl=0
src/user/cat.c        ioctl=0
src/user/ls.c         ioctl=0
```

`W0_MEMBERS="HELLO SH CAT LS"` (`tools/gate/t4_w0.sh:30`). So an ioctl row cannot appear in that differential **by construction**, whatever the kernel does — #87's "zero rows of leg B" was structural, and minting a new card did not change the corpus. Answering the card's first duty directly: **no, the rows are not measurable this time**, and the fix is a corpus member that calls ioctl (or a dedicated ioctl probe row), not another kernel attempt.

## Clause 3 — half right, and the half that is wrong is the one the title claims

Linux leg, measured here. On a non-tty the PR's answer is exactly Linux's:

```
TCGETS on /dev/null      req=0x5401 -> rc=-1 errno=25 (Inappropriate ioctl for device)
TIOCGWINSZ on /dev/null  req=0x5413 -> rc=-1 errno=25 (Inappropriate ioctl for device)
FIONREAD on /dev/null    req=0x541b -> rc=-1 errno=25 (Inappropriate ioctl for device)
req 3 on /dev/null       req=0x0003 -> rc=-1 errno=25 (Inappropriate ioctl for device)
```

On a **tty** — the supported-request leg the clause asks for — Linux succeeds, and the compat case cannot:

```
TCGETS on a pty slave      fd=4 -> rc=0 errno=0 (ok)
TIOCGWINSZ on a pty slave  fd=4 -> rc=0 errno=0 (ok) rows=0 cols=0
isatty(pty slave)=1
```

The compat case returns `-25` for these too, because it returns `-25` for everything. So the "honest refusal" is a blanket refusal: it is correct on the fd type the comment names and silently wrong on every fd that supports the request (tty, `/dev/fb0`, evdev — the very nodes the comment defers to GB-025/026/027).

## Clause 4 — PASS

```
INTEL_RC=0
ARM_RC=0
```

## Clause 5 — the falsifier cannot be built, which is the finding

"Break one request mapping (wrong constant, or route to a handler with the wrong argument shape) and show the check goes RED." There is no mapping to break and no handler to mis-route to: the case is a constant. Any falsifier I write for this code is a falsifier of a grep, not of behaviour — and a check that cannot be made to fail does not count (GB-066/GB-070).

## Verdict

**RED** — clause 1 fails on its own wording (nothing is routed, no request constants are used or established), clause 3's supported-request leg fails (tty: Linux `rc=0`, compat `-25`), and clause 5 has nothing to falsify. Clause 4 passes, and the non-tty `-ENOTTY` answer does match Linux exactly — that part of the change is correct and worth keeping when the case is rebuilt to inspect the request.