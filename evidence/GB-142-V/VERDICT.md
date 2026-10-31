# GB-142-V — VERDICT: GREEN

**Graded:** `1f22f41b` (PR #281, branch `ticket/142-exitcode`) — "GB-142: [EXITPC] logs the exit code for native and compat processes alike"
**Graded by:** verifier (wgames, DEEP), 2026-10-31, worktree `.worktrees/gb142v` (detached at `1f22f41b`), QEMU on this VM.
**Diff under test:** 1 file, +9/−2, `src/kernel/process.c` (parent `42ff51ff`).

Verdict summary — every clause of the card was run on this VM and its output is quoted below.

| # | Clause | Result |
|---|---|---|
| 1 | normal `return 0` logs `code=0`, native AND compat | PASS |
| 2 | explicit `exit(N)` logs `code=N` | PASS |
| 3 | abnormal/signal death distinguishable from a normal code | PASS |
| 4 | no `[EXITPC]` line removed/reordered | PASS (in-boot 1:1 + diff) |
| 5 | suites `groom_test`, `check_stale_test`, `drift_check_test` | PASS (rc=0 each) |

---

## Build (both arches)

```
$ make ARCH=intel MODE=unit_tests hobbyos.elf     -> RC=0
$ make ARCH=arm   MODE=unit_tests hobbyos.elf     -> ARM_RC=0
```

Raw: `arm_build.log`. No ARM boot was run — the card's clauses name the intel guest; ARM is compile-only here.

## Clause 1 — `return 0` → `code=0`, native

Intel T3 wave, complete boot (`t3_intel_1f22f41b.log`, 13698 console lines, `System halt` at line 13698):

```
[KERNEL] Process 8: CAT.BIN exited unexpectedly or gracefully.
[EXITPC] pid=8 last_user_pc=0x10000008EF code=0x0
```

571 of 720 `[EXITPC]` lines carry `code=0x0`.

## Clause 1 (compat leg) — a Linux-ABI member also logs `code=0`

The default T3 wave is native flat `.bin` only (`abi=0x0` x651, `abi=0x1` x0 in the plain T3 boot), so the
compat leg was run with `tools/gate/t4_w0_dispatch.sh`, which stages the musl **static-PIE ELF** W0WPIE member
and boots it through the real ELF loader (`t3_intel_w0pie_1f22f41b.log`):

```
v2 loader: PID=6 entry=0x10000011B8 sp=0x1800000000 resident=26 tables=6 abi=0x1
[ARRIVED] W0PIE.BIN rc>0
[KERNEL] Process 6: W0PIE.BIN exited unexpectedly or gracefully.
[EXITPC] pid=6 last_user_pc=0x1000005687 code=0x0
```

`abi=0x1` = `PROC_FLAG_LINUX_ABI` (process.h:389), set by program_loader.c:868 for ELF. The compat member
exits and its code is logged — the DoD's "native and compat alike".

## Clause 2 — explicit `exit(N)` → `code=N`

`src/user/proc_child.c` exits with the code given in argv[1] (default 42) and with the env value (33) in `env` mode:

```
[KERNEL] Process 20: PROCCHLD.BIN exited unexpectedly or gracefully.
[EXITPC] pid=20 last_user_pc=0x1000000ADF code=0x2A        <- 42
...
[EXITPC] pid=19 last_user_pc=0x1000000ADF code=0x21        <- 33 (PROCTEST_ENV)
```

Other distinct non-zero codes in the same boot: `GREP.BIN 0x1/0x2`, `SIG_T.BIN 0x7`, `SPAWNEX.BIN 0x9`,
`SH.BIN 0x7E` (126). 149 of 720 lines are non-zero.

## Clause 3 — a signal death is distinguishable

`MEMTEST.BIN` deliberately writes to kernel space and is killed by the fault path:

```
[KERNEL] pid=10 (MEMTEST.BIN) memory fault VA=0x0000000040000000 PC=0x0000001000000051 rw=w  ... -> killed
[KERNEL] Process 10: MEMTEST.BIN exited unexpectedly or gracefully.
[EXITPC] pid=10 last_user_pc=0x1000000CB6 code=0x4000000B
```

`0x4000000B` = `PROCESS_STATUS_SIGNAL (0x40000000, process.h:469) | 11` — signal 11, set at process.c:4327.
A normal `exit(11)` would log `code=0xB`; the high bit makes the two cases distinct, which is exactly the
DoD's "a code that must not mean two things". Same shape on `MMTEST.BIN` (x5) and `TORTURE.BIN`.

## Clause 4 — no `[EXITPC]` line removed or reordered

Same-boot invariant (graded run):

```
$ grep -ac 'exited unexpectedly or gracefully' t3_intel_1f22f41b.log   -> 720
$ grep -ac '\[EXITPC\]' t3_intel_1f22f41b.log                          -> 720
```

Every exit produced exactly one line — nothing dropped. Diff-level: `grep -c EXITPC src/kernel/process.c`
is 1 in the parent tree and 2 in the graded tree, where the second hit is the added *comment*; the
`kl_emit` count is 3 in both trees, i.e. the existing emit block was extended, not duplicated.

Before/after line shape, from a `42ff51ff` boot (pre-change kernel, `t3_intel_base_42ff51ff_stalled.log`):

```
[EXITPC] pid=10 last_user_pc=0x1000000CB6
[EXITPC] pid=11 last_user_pc=0x10000008EF
[EXITPC] pid=12 last_user_pc=0x10000007AF
```

536 such lines, **0** with `code=` — the only difference from the graded kernel is the appended field.

**Disclosure:** two baseline boots of `42ff51ff` today stalled in the console watchdog
(`[WATCHDOG] console silent for more than 20s`, 13 occurrences, no `System halt`) after 528 and 536 exits,
so no complete pre-change wave was obtainable for a full cross-boot count. That is why clause 4 is measured
in-boot and at the diff; the partial baseline is archived, and its 536 exits produced 536 `[EXITPC]` lines
(also 1:1).

## Clause 5 — suites

```
$ python3 tools/board/groom_test.py        rc=0  GROOM_TEST: PASS — 15 checks, all can fail and all held
$ python3 tools/board/check_stale_test.py  rc=0  CHECK_STALE_TEST: PASS -- comment-only stale clock, independent case table green
$ python3 tools/board/drift_check_test.py  rc=0  drift_check_test: 9 checks, 0 failed -- ALL PASSED
```

Logs: `suite_*.log`.

## Not counted (pre-existing, unrelated to this diff)

The intel T3 tier of the graded boot exits 1 on lines this card does not grade, and they are present on the
parent too:

```
T3_intel: FAIL — 5 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, ...
T3_intel: FAIL — 7 FAIL line(s): [SIGTEST] FAIL pipe() for the spawn_ex fdmap got=-1 want=0 (x4 rows),
          TORTURE poll-many-fds: FAIL, TORTURE FAILED: 1
```

None of these touch `process_exit`'s emit path; they are the known SIGTEST/TORTURE blockers. The tier's
red is **not** used as evidence for or against this card, and this GREEN does not claim the tier is clean.

## Raw artefacts

`t3_intel_1f22f41b.log` (graded T3 console) · `t3_intel_summary.txt` (tier verdict) ·
`t3_intel_w0pie_1f22f41b.log` + `t4_w0_dispatch_summary.log` (compat boot) ·
`t3_intel_base_42ff51ff_stalled.log` (pre-change kernel, partial) · `suite_*.log` · `arm_build.log`.