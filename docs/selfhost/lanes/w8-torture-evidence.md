# Lane W8-TORTURE — VM-only `TORTURE exec-wait-cycles` red at tip 288995c (root cause + fix)

## Symptom

At tip `288995c` (merged w6-run), the CI VM's ARM `make test` wave fails the
POSIX torture suite's exec/wait section:

```
merged-w7_20261014-162424_test-arm.log   ->  TORTURE exec-wait-cycles: FAIL
merged-w7r_20261014-162947_test-arm.log  ->  TORTURE exec-wait-cycles: FAIL
pre-w6_20261014-164131_test-arm.log      ->  TORTURE exec-wait-cycles: PASS   (control, 42c6c0b)
```

Both failing runs: `exec_ok=0 exec_rejected=0` (every one of the 8 cycles,
including their bounded retry, returned `-1`); every child still printed
`[TORTURE.CHILD] ok` (~15-16 prints per run). All other suites PASS. Locally
the battery at the same tip showed `exec_ok=8` (301 ms) — because that run
was the NATIVE HOST build (`torture_test_host`, `suite_ms=301`,
`total_kb=65536` mock), which never exercises the ARM user/kernel code.

## Root cause

`proc_marshal_argv()` (kernel, `src/kernel/program_loader.c`) and `sys_exec`'s
flat-args string builder (`src/kernel/arch/{arm,x64}/trap.c`) copy each user
argv element with a helper (`pl_strcpy` / `u_strcpy`) that **range-checks the
WHOLE buffer cap before copying**. The w6-run change raised the per-arg cap
from 64 to `HO_EXEC_ARGV_LEN` (4096), so the pre-check became
`pl_user_ok(src, 4096)`.

`pl_user_ok` -> `vm_range_ok` -> `vm_touch` walks every page in `[src, src+4096)`
and fails when the range crosses the END of a mapped region
(`a + FRAME_SIZE > r->base + r->len` -> -EFAULT).

A fork+exec child builds its argv on the **user stack**, near the stack top.
For `TORTURE.BIN` (v2 loader, stack top `0x1800000000`, disassembled frame
sizes `_start 0x50 + main 0x130 + exec_cycles 0x80 + exec_cycle 0x50`, `code[]`
at `sp+0x20`) the exit-code string `argv[2]` lives at **`0x1800000000-0x238`**
— 568 bytes below the top. A 4096-byte pre-check reaches ~3.5 KB *past* the
stack top into the unmapped hole -> `vm_touch` rejects -> `pl_strcpy` returns 0
-> `proc_marshal_argv` breaks -> the marshalled blob is `["TORTURE.BIN",
"child"]` (argc=2). The re-exec'd child then takes the missing-arg default:
`main()` sets `code = (i+1 < argc) ? atoi(argv[i+1]) : 7` -> `7`.
`want = (seq % 40) + 10` is 10..17, so the parent's
`WIFEXITED && WEXITSTATUS == want` check fails on every cycle -> `-1`
(`exec_ok=0`, `exec_rejected=0`; `-3`/97 never appears). The child itself is
healthy — it prints `[TORTURE.CHILD] ok` and exits normally.

This is deterministic on ARM (same binary, same stack layout) — the VM was the
first place the ARM build of the suite actually ran. The pre-w6 cap (64) kept
the pre-check entirely inside the top stack page, which is why `42c6c0b`
passes.

## Evidence (serial captures in `evidence-w8-torture/`)

Controlled repro on the workstation (local ARM TCG, `MODE=selfhost` disk +
SELFHOST.RUN driving `/TORTURE.BIN`): 100% reproduction.

Unfixed kernel (probe build, `w8-probe-fail.serial.txt`):

```
[W8P] child want=0xa code@=0x17fffffdc8 argv@=0x17fffffda0 argv2=10
[W8P] -1-cycle=0 st=0x700 exited=1 code=7 sig=0 want=10     <- child EXITED with code 7
  TORTURE exec-wait-cycles: FAIL
[TORTURE] counts: ... exec_ok=0 exec_rejected=0 suite_ms=2641
```

- `code@=0x17fffffdc8` == stack top `0x1800000000` - 0x238: the argv[2]
  string is 568 B below the top (frame-depth analysis: 0x230 predicted, +0x8
  for the fork syscall spine).
- `st=0x700`, `exited=1`, `code=7`, `sig=0` on all 16 forks: the child ran the
  argv[2]-missing default. **H1 (argv marshal loss) proven; H2 ruled out**
  (child was NOT fault-killed — it exited normally), **H3/H4 ruled out**
  (64 KiB per-CPU kernel stacks; the flat-args fallback isn't the argv
  source — the eargv blob is, and cxxrt.o is irrelevant to a healthy child).

Fixed kernel, same harness (`w8-fix-probe.serial.txt`): same stack addresses,
all 8 exits match want:

```
[W8P] child want=0xa code@=0x17fffffdc8 argv@=0x17fffffda0 argv2=10
  ... 10..17 ...
  TORTURE exec-wait-cycles: PASS
[TORTURE] counts: ... exec_ok=8 exec_rejected=0 suite_ms=1740
ALL TESTS PASSED SUCCESSFULLY!
```

Clean (probe removed) fixed build: `w8-fix-clean.serial.txt` /
`w8-fix-clean2.serial.txt` — `exec-wait-cycles: PASS`, `exec_ok=8`;
`poll-many-fds` flakes in the SELFHOST relay harness only (see below).

## Why `exit(7)` and not a fault (H2) — status decode

`waitpid` status `0x700` = exit code 7 (`WEXITSTATUS = (st>>8) & 0xff`),
`WIFEXITED` true, `WTERMSIG` 0. The child reached `[TORTURE.CHILD] ok` and
`exit(code&0xff)` normally; only the *received* argv[2] was missing. `-3`
(exec rejected, exit 97) never occurred -> the exec itself succeeded.

## The fix (`9159e86`)

Validate **per page, exactly the bytes copied**, instead of the whole cap up
front, at all three marshal sites, matching the established `exec_read_str`
envp pattern:

- `src/kernel/program_loader.c` `pl_strcpy`
- `src/kernel/arch/arm/trap.c` `u_strcpy`
- `src/kernel/arch/x64/trap.c` `u_strcpy`

A short string near the top of a mapped region is now copied (its owning page
is validated/materialised); a string that truly runs into unmapped memory is
still dropped (same `0` return). Long argv elements (the w6-run `sh -c
'<recipe line>'` use case) are unaffected — every page they span is
validated as read.

## Verification (post-fix tree `9159e86`)

| gate | result |
|---|---|
| local ARM TCG `MODE=selfhost` harness | exec-wait-cycles PASS ×3 runs incl. probe build; exec_ok=8 |
| local ARM full wave (`make MODE=test run`) | TORTURE PASS, poll PASS, exec_ok=8, `System halt`, rc=0 (suite_ms=32248) |
| VM test-arm (192.168.0.20) | 3/3 runs `TORTURE exec-wait-cycles: PASS`, exec_ok=8 (labels w8-fix-a/b/c; only ambient SHELLTEST WATCHDOG tokens otherwise) |
| ARM unit (`./run_unit_tests.sh`) | 318 tests, 0 failures, UNIT TESTS PASSED |
| x64 unit (`EXTRA_QEMU_ARGS='-enable-kvm' ./run_unit_tests_intel.sh`) | 0 failures, UNIT TESTS PASSED |

Pre-fix control numbers: every ARM run at 288995c failed (VM ×2, local battery
rerun, selfhost harness 6/6); the pre-w6 control (cap 64) passes every run.


The failure looked VM-only and then intermittent because the only "local PASS at
tip" (merged-battery6-2026-10-14.log, `suite_ms=301`, `total_kb=65536` mock) was
the **native HOST build** (`torture_test_host`, glibc), which never runs the ARM
user/kernel code. Every ARM execution of the suite at tip `288995c` fails:

| environment | runs at 288995c (pre-fix) | result |
|---|---|---|
| CI VM battery | merged-w7, merged-w7r | FAIL (exec_ok=0) ×2 |
| local ARM battery | merged-battery6-wave-rerun (suite_ms=33898) | FAIL (exec_ok=0) ×1 |
| local ARM selfhost harness (probe build) | 6 runs | FAIL ×6, `code@=0x17fffffdc8` stable, `st=0x700 code=7` every fork |
| local HOST build | merged-battery6 | PASS (suite_ms=301) — not ARM |
| pre-w6 ARM control (42c6c0b) | VM + batteries | PASS ×many |

At the failing tip the ARM code path trips 100% of the time once actually run;
the mechanism is geometric (argv string 0x238 below the stack top, cap-4096
pre-check crosses it) and binary-deterministic. The fix removes the distance
dependence entirely (only the copied bytes are validated), so it holds at any
stack depth/load, and post-fix every ARM run passes.

- `poll-many-fds` failed in exactly 2 selfhost-relay harness runs with the
  clean binary but passed with two other build variants of the same suite and
  passes in every full battery/VM run (all `merged-battery*.log` +
  `pre-w6`/`merged-w7` VM logs show PASS). Nothing in the fix touches the
  poll path (kernel was byte-identical across those runs); the harness's
  serial-relay pipe perturbs the wall-clock poll(30/2000 ms) sub-checks. The
  real-environment gates below are green.
- The probe commit `ac59b9c` (diagnostic prints) is kept in history for
  evidence; the fixed tree carries no diagnostics.
