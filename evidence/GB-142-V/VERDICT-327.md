# GB-142-V (#327) — VERDICT: GREEN

**Graded:** `1f22f41b` (PR #281) — same head as the earlier GB-142-V card #326, which this run supersedes.
**Graded by:** verifier (wgames, DEEP), 2026-10-31, QEMU on this VM, worktree `.worktrees/gb142v` (detached at `1f22f41b`); parent tree `.worktrees/gb142base` (detached at `42ff51ff`).
**Diff:** `src/kernel/process.c` +9/−2, one hunk.

| # | Clause | Result |
|---|---|---|
| 1 | native exit code quoted from a real boot | PASS |
| 2 | compat (`abi=0x1`) exit code, same wave | PASS |
| 3 | `return 0` → `0`, explicit `exit(N)` → `N`, value confirmed | PASS |
| 4 | signal distinguishable from a normal exit | PASS (with the convention stated below) |
| 5 | `grep -c '[EXITPC]'` unchanged before/after, both counts quoted | PASS |
| 6 | suites: `merge_gate_test.sh`, `groom_test`, `check_stale_test`, `drift_check_test` | PASS (rc quoted; caveat below) |

---

## Clause 1 — native exit code

Complete intel T3 boot (`t3_intel_1f22f41b.log`, 13698 console lines, `System halt`):

```
[KERNEL] Process 8: CAT.BIN exited unexpectedly or gracefully.
[EXITPC] pid=8 last_user_pc=0x10000008EF code=0x0
```

## Clause 2 — compat, in the same wave

The default T3 wave is native flat `.bin` only (measured: `abi=0x0` x651, `abi=0x1` x0), so the compat leg is
booted through `tools/gate/t4_w0_dispatch.sh`, which stages the musl static-PIE W0PIE member and dispatches it
through the real ELF loader. **One wave, both ABIs** (`t3_intel_w0pie_1f22f41b.log`):

```
v2 loader: PID=2 entry=0x1000000000 sp=0x1800000000 resident=17 tables=6 abi=0x0
v2 loader: PID=3 entry=0x1000000000 sp=0x1800000000 resident=17 tables=6 abi=0x0
v2 loader: PID=4 entry=0x1000000000 sp=0x1800000000 resident=17 tables=6 abi=0x0
v2 loader: PID=5 entry=0x1000000000 sp=0x1800000000 resident=17 tables=6 abi=0x0
v2 loader: PID=6 entry=0x10000011B8 sp=0x1800000000 resident=26 tables=6 abi=0x1
...
[KERNEL] Process 6: W0PIE.BIN exited unexpectedly or gracefully.
[EXITPC] pid=6 last_user_pc=0x1000005687 code=0x0
```

`abi=0x1` is `PROC_FLAG_LINUX_ABI` (process.h:389), set at program_loader.c:868 for ELF members. A native-only
sample would not have satisfied this clause; the compat member's own exit carries `code=0x0`.

## Clause 3 — `return 0` vs `exit(N)`, value confirmed

Both from the graded boot, two distinct processes:

```
[KERNEL] Process 8: CAT.BIN exited unexpectedly or gracefully.
[EXITPC] pid=8 last_user_pc=0x10000008EF code=0x0            <- return 0

[KERNEL] Process 20: PROCCHLD.BIN exited unexpectedly or gracefully.
[EXITPC] pid=20 last_user_pc=0x1000000ADF code=0x2A           <- exit(42)
```

**Base is hexadecimal** (`kl_hex`): `0x2A` = 42, which is `src/user/proc_child.c`'s documented default exit code
(argv[1], default 42) — the value matches the program's own argument, not a re-encoding. Second sample:
`pid=19 … code=0x21` = 33, the `env`-mode value (`PROCTEST_ENV`). Other distinct codes in the same boot:
`GREP.BIN 0x1/0x2`, `SIG_T.BIN 0x7`, `SPAWNEX.BIN 0x9`, `SH.BIN 0x7E` (126). 149 of 720 rows are non-zero.

## Clause 4 — signal vs normal exit

```
[KERNEL] pid=10 (MEMTEST.BIN) memory fault VA=0x0000000040000000 PC=0x0000001000000051 rw=w  ... -> killed
[KERNEL] Process 10: MEMTEST.BIN exited unexpectedly or gracefully.
[EXITPC] pid=10 last_user_pc=0x1000000CB6 code=0x4000000B
```

`0x4000000B` = `PROCESS_STATUS_SIGNAL (0x40000000, process.h:469) | 11` (SIGSEGV), stamped by the kill path at
process.c:4327. A normal `exit(11)` logs `code=0xB` — different line, different meaning.

**Stated precisely, because the card asks for precision:** the log prints `regs[0]` raw and so distinguishes
signals by the *convention* that bit 30 marks them — the same convention `process_exit` already uses for the
waitpid status (`process.c:770`: `if ((int)code & PROCESS_STATUS_SIGNAL) grp->exit_status = code & 0x7f`).
A program that itself called `exit(0x4000000B)` would print the same hex **and** already be reported to its
parent as "signal 11". So the log does not introduce an ambiguity: it inherits the kernel's single existing
encoding, and for every ordinary code (`< 0x40000000`) a signal death is unambiguous. Clause 4 is met on that
reading; the collision case is the pre-existing kernel convention, not a new confusion in this diff.

## Clause 5 — `grep -c '[EXITPC]'` unchanged, both counts

Apples-to-apples: the **same** manifest boot (`t4_w0_dispatch.sh`), completed on both heads.

| head | exits | `grep -c '[EXITPC]'` | rows with `code=` |
|---|---|---|---|
| parent `42ff51ff` (`t3_intel_w0pie_42ff51ff.log`) | 5 | **5** | 0 |
| graded `1f22f41b` (`t3_intel_w0pie_1f22f41b.log`) | 5 | **5** | 5 |

Identical pid set and identical `last_user_pc` values in both boots (`0x10000008FF, 0x10000008EF, 0x100001095F,
0x1000005687, 0x100000087F`) — the only difference is the appended ` code=` field:

```
[EXITPC] pid=2 last_user_pc=0x10000008FF                 <- parent
[EXITPC] pid=2 last_user_pc=0x10000008FF code=0x0        <- graded
```

Large wave, graded head: 720 exits → 720 `[EXITPC]` lines (1:1, nothing dropped). Diff-level: `kl_emit` count is
3 in both trees; `grep -c EXITPC src/kernel/process.c` is 1 in the parent and 2 in the graded tree, the second
hit being the added comment, not an emit.

**Limitation disclosed:** full-wave parent boots do not complete on this VM today — three attempts stalled in
the console watchdog (`[WATCHDOG] console silent for more than 20s`) after 528 / 536 / 550 exits
(`t3_intel_base*_42ff51ff_stalled.log`), so a complete 720-scale "before" number does not exist. Each stalled
parent boot was still 1:1 (exits == `[EXITPC]` lines), and clause 5 is settled by the completing 5-exit pair above.
(This stall is on the parent tree and is not attributed to the diff; the graded head completed at 720 with
`System halt`. It is nonetheless a fact about `42ff51ff` worth a card of its own.)

## Clause 6 — suites

```
$ bash tools/gate/merge_gate_test.sh            rc=0   MERGE_GATE_TEST: PASS — a dummy branch fails, a clean branch passes
$ python3 tools/board/groom_test.py             rc=0   GROOM_TEST: PASS — 15 checks, all can fail and all held
$ python3 tools/board/check_stale_test.py       rc=0   CHECK_STALE_TEST: PASS -- comment-only stale clock, independent case table green
$ python3 tools/board/drift_check_test.py       rc=0   drift_check_test: 9 checks, 0 failed -- ALL PASSED
```

**Caveat, quoted rather than hidden:** that `merge_gate_test.sh` run reports `Part B: skipped (TEST_RUN_T1=1 to run
the real T1 gate)`. Forcing it (`TEST_RUN_T1=1 bash tools/gate/merge_gate_test.sh`) exits **1** on
`FAIL tools/board/check_verdicts_test.py (ValueError: not enough values to unpack (expected 4, got 3))` — a
crash in `check_verdicts.governing` (`check_verdicts.py:201`). That failure is **pre-existing**: the identical
crash reproduces on the parent at `42ff51ff` (`suite_check_verdicts_test_parent.log`, rc=1), so it is not caused
by this diff — but it means the T1 host suite is not clean at either head today.

## Not counted

The graded full-wave intel tier exits 1 on lines this card does not grade, present on the parent too:
`FAIL — 5 FATAL kernel-mode fault line(s)` and `FAIL — 7 FAIL line(s)` (`[SIGTEST] FAIL pipe() …`,
`TORTURE poll-many-fds: FAIL`). None touch `process_exit`'s emit path. This GREEN does not claim the tier is clean.

## Raw artefacts

`t3_intel_1f22f41b.log` · `t3_intel_summary.txt` · `t3_intel_w0pie_1f22f41b.log` + `t4_w0_dispatch_summary.log` ·
`t3_intel_w0pie_42ff51ff.log` + `t4_w0_dispatch_parent.log` · `t3_intel_base{,2,3}_42ff51ff_stalled.log` ·
`suite_*.log` · `arm_build.log`