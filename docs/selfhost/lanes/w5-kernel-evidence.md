# Lane W5-KERNEL — blocker-list fixes: evidence (2026-10-14)

All serial evidence in `evidence-w5-kernel/*.serial`.  Every pre->fixed pair
boots the SAME staged probe image (`copy of the w4-clang probe.img`) with
either the **pre-fix** kernel (base commit 92815fa, built in a scratch
worktree) or the **fixed** kernel (selfhost/w5-kernel head), both
`MODE=selfhost` (MODE=selfhost boots SELFHOST.BIN directly from
`/SRC/SELFHOST.RUN`).  QEMU line = the harness's ARM line (`-smp 8 -m 16G
-accel tcg,thread=multi`).

## (ii) fault-killed child rc=0 -> rc=4

`prefix-t23.serial` (pre-fix):  raw-ELF CLANG.BIN faults at entry (EC=0x0),
the EC=0x00 kill path used `process_exit(tf)` with stale x0:

```
[KERNEL] User process 3 (/TOOLS/CLANG.BIN) fault! EC: 0x0000000000000000 ELR: 0x0000001000000000
[SELFHOST] phase=t23-version-done rc=0          <- false pass
[SELFHOST] ACCEPTANCE-COMPLETE stages=1
```

`fixed-t23-final.serial` (fixed):  same fault, the kill now routes through
`process_fault_set_status` (SIGILL):

```
[KERNEL] User process 3 (/TOOLS/CLANG.BIN) fault! EC: 0x0000000000000000 ELR: 0x0000001000000000
[SELFHOST] phase=t23-version-fail reason=tool-rc rc=4
```

## (i) exec gate 1 MiB -> 128 MiB (v2 path)

Build a tiny probe `EXECX.BIN` (execv()s its argv[1] and prints errno).
`prefix-i2.serial` (pre-fix):  exec of the 94 MB flat CLANG2.BIN is refused
at the 1 MiB gate:

```
EXECX execv(/TOOLS/CLANG2.BIN)...
EXECX exec failed errno=8          <- ENOEXEC (program_too_large, 1 MiB)
```

`fixed-i.serial` (fixed):  the gate now matches v2_map_image's 128 MiB cap,
clang loads and runs in-OS:

```
EXECX execv(/TOOLS/CLANG2.BIN)...
clang version 21.1.8 (... llvm-project-21.1.8.src/clang 0c272a4e...)
```

## (iv)+(iii)+(v) the prefetch race cascade — reproduced pre-fix, clean fixed

The same racing script (`LLD2.BIN -flavor gnu ...` real link) drives the L9
prefetch and lld's execution nose-to-nose.

`prefix-t25b-6.serial` (pre-fix), ONE boot shows the whole cascade:

```
[IMGPRF] pid=3 start: pages=13001 batch=128
[KERNEL] pid=3 (/TOOLS/LLD2.BIN) memory fault VA=0x0000001000673060 PC=0x0000001000673060 rw=rx in=PROT -> killed   <- (iv)
[KERNEL] FATAL: ... EC: 0x25 FAR: 0x0000FFFFFFFFF018 ELR: 0x000000043A6AF1AC, PROC pid=4 name= ...                 <- (iii)
[KERNEL] bytes@ELR: 0x00000000F9400108 0x00000000360000C8 ...                                   <- vm_arch_walk+0x2c (ldr x8,[x8])
[KERNEL] EL1 fault in user address space -> killing pid=4
[IDLESTUCK] ... slot= ... SELFHOST.BIN ...                                                       <- (v) spam
[LOSTWAKE] disposing pid=1 SELFHOST.BIN (stale dead-owner claim, poisoned resume)                <- (v) live driver disposed
[KERNEL] User process 1 (SELFHOST.BIN) fault! EC: 0x20 ... (repeats -> hang)
```

ELR 0x43A6AF1AC maps (runtime base delta 0x3a600000) to `vm_arch_walk+0x2c`
= `ldr x8, [x8]`, the L2-slot deref; LR 0x43A6ABC60 -> `vm_image_prefetch_run`
(the presence probe).  So the pid-5/pid-4 "housekeeping" EL1 fault is the L9
prefetch kernel task walking a page-table chain of an AS that a *peer CPU* is
tearing down.

`fixed-t25b-{1,2,3}.serial` (fixed), the identical script:
no `in=PROT`, no EL1 FATAL, no [IDLESTUCK], no [LOSTWAKE] — lld's own
toolchain BRK (EC 0x3C, known toolchain-build defect) is surfaced cleanly as
`rc=4` and the system halts:

```
[SELFHOST] phase=t25b-link-fail reason=tool-rc rc=4
System halt from CPU X.
```

## (vi) spawn/exec args 255 -> 1024

`prefix-vi-truncated.serial` (pre-fix):  a 330-char compile line through the
driver's spawn2 arrives truncated at 255 (missing the `-DEXTRA` tail and the
rest of the last token):

```
ARGPRT argc=17
 ARG[16] len=11 [/SRC/T2SRC/]          <- cut mid-token at 255
```

`fixed-vi2.serial` (fixed, kernel + crt0 buffers):  the full line round-trips:

```
ARGPRT argc=27
 ARG[16] len=18 [/SRC/T2SRC/HELLO.O]
 ARG[17..26] len=7 [-DEXTRA] ... (10 of them)
```

## (vii) exec of a missing image — clean -ENOENT instead of AS-teardown fault loop

`prefix-vii.serial` (pre-fix):  EXECX execs /TOOLS/NOSUCH.BIN ->
fat16_open creates a zero-size entry -> v2 exec tears the old AS down ->
`v2 loader: bad image size 0` -> the process is left AS-less and re-faults
forever (the MISSING-SHELL-WEDGE signature):

```
EXECX execv(/TOOLS/NOSUCH.BIN)...
v2 loader: bad image size 0
[KERNEL] User process 3 (/EXECX.BIN) fault! EC: 0x20 ELR: 0x00000010000066D4 (repeats)
```

`fixed-vii.serial` (fixed):  file_size==0 is rejected BEFORE the teardown,
the caller keeps its AS and gets a real errno:

```
EXECX execv(/TOOLS/NOSUCH.BIN)...
EXECX exec failed errno=2          <- ENOENT, process intact
[SELFHOST] phase=t-exec-missing-fail reason=tool-rc rc=256   <- EXECX's own rc=1
```

## Unit tier

`unit-arm.log`:  `Tests run: 318 / Tests failed: 0 — UNIT TESTS PASSED`
(suite floor 316 + `test_fault_kill_status_nonzero` (ii) +
`test_vm_walk_dead_as_safe` (iii/iv)); `test_vm_demand_fault` updated to the
new w5(iv) contract (present-permissive fault = bounded retry).

`unit-x64.log`: TBD.  `make-test-arm.log`: TBD.
