# Lane W6-RUN — merged-tip end-to-end drive: S0/S1/T2.5/S2 evidence (2026-10-14)

Branch `selfhost/w6-run`, base = merged tip 42c6c0b (all waves 1-5).
Worktrees: w6-run only (main repo untouched).  One QEMU per tree; serial
captures in `evidence-w6-run/*.serial.txt`; artifacts pulled off the image
with `fat16img.py`.

## Verdict table

| stage | in-OS command | result | evidence |
|---|---|---|---|
| A | cxxrt.cpp fix (section + delete->free) | **PASS** — `cxxrt_test_host` 25 checks 0 failures; `_Znwm/_Znam` inside `__lcxx_override` (host + aarch64 obj) | commit e49820a |
| S0 | `make -f Makefile.hobbyos check` | **PASS** — `phase=s0-check-done rc=0`, `ACCEPTANCE-COMPLETE` | s0.serial.txt + s0-check.o (21144 B, AArch64 ELF REL) |
| S1 | `make -f Makefile.hobbyos user` | (pending) | s1.serial.txt |
| T2.5 | in-OS run of the S1-built HELLO.BIN | (pending) | s1.serial.txt |
| S2 | `make -f Makefile.hobbyos kernel` (59 TUs) | (pending) | s2.serial.txt |

## Newly found + fixed (fix-forward, this lane)

### F1: kernel execv argv element cap 64B -> 1024B (S0 blocker)
Symptom: S0 chain ran (driver -> sh -> make -> sh -c -> clang) but clang
died `CLANG: error: no input files` even though the same line compiled when
run directly.  Root cause (argvprobe.serial.txt, probemake.serial.txt):
make runs recipe lines as `sh -c '<line>'` — the whole ~200+ char line is
ONE argv element, and the kernel's execv marshal (`proc_marshal_argv`,
`char one[64]`) truncated it to 63 chars; `sys_exec`'s flat-args loop
(trap.c, `char one[64]` + `argbuf[256]`) had the same caps.  Fix: per-arg
1024 (HO_FLAT_ARGS_LEN parity) + argbuf 1024, both arches
(program_loader.c, arch/arm/trap.c, arch/x64/trap.c).  After the fix S0
passes.  x64 trap.o host compile-checked.

### F2 (landed in w5 clang-21, re-proven here): none new.
