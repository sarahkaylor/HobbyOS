# Lane W6-RUN — merged-tip end-to-end drive: S0/S1/T2.5/S2 evidence (2026-10-14)

Branch `selfhost/w6-run`, base = merged tip 42c6c0b.  Worktree w6-run only
(main repo /home/sarah/Documents/GitHub/HobbyOS untouched, no pushes).
Serial captures: `evidence-w6-run/*.serial.txt`; artifacts pulled off the
image with `fat16img.py`.

## Verdict table

| stage | in-OS command | result | evidence |
|---|---|---|---|
| A (cxxrt) | — | **PASS** — `cxxrt_test_host` 25 checks 0 failures; `_Znwm/_Znam` in `__lcxx_override` (host + aarch64 obj) | commit e49820a |
| S0 | `make -f Makefile.hobbyos check` | **PASS** — `phase=s0-check-done rc=0`, `ACCEPTANCE-COMPLETE` | s0.serial.txt + s0-check.o (21144 B, AArch64 ELF REL) |
| S1 | `make -f Makefile.hobbyos user` | **PASS** — 5 user .o compiled in-OS (sh.o 245528 B = the 53-min sh.c -O2 TCG compile), 4 links + 4 objcopies in-OS, `phase=s1-user-done rc=0` | s1.serial.txt + s1-products/ (HELLO.BIN 37360 B BYTE-IDENTICAL to host hello.bin) |
| T2.5 | in-OS run of S1-built HELLO.BIN | **PASS** — `[HELLO] entering main / argc=1 / argv[0]="./HELLO.BIN" / main returned 0`; `phase=t25-run-done rc=0`; `ACCEPTANCE-COMPLETE stages=2` | s1.serial.txt |
| S2 | `make -f Makefile.hobbyos kernel` | **RUNNING** (bounded, 7800 s budget); per-TU markers counted (see live section) | s2.serial.txt |

## Newly found + fixed this lane (fix-forward)

### F1: cxxrt.cpp os-wide followup — section + delete->free
operator new/new[] get `section("__lcxx_override")`; delete -> `free(ptr)`
(raw-payload semantics, mirror of the toolchain shim).  Host test now binds
the HobbyOS allocator (obj/host_malloc_hb.o + hb_* wrappers in the TU) —
`free(ptr)` is a contract of the HobbyOS allocator's BLOCK_MAGIC check, not
glibc.  25/25 checks.

### F2: kernel execv argv element cap 64B -> 4096B (S0 blocker)
make runs recipe lines as `sh -c '<line>'`: the whole line is ONE argv
element.  proc_marshal_argv capped elements at 63 bytes; sys_exec's flat
argbuf at 64/255.  clang got a partial command -> "no input files".
Raised per-arg + argbuf to HO_EXEC_ARGV_LEN (4096), both arches.  Proven:
argvprobe (direct 16 args all pass), probemake (through make, argc 16->7
pre-fix), post-fix S0 passes.

### F3: lld unified-driver flavor (S1 blocker)
In-OS first link: `lld is a generic driver`.  lld v21 invoked as bare
`lld` (frozen /TOOLS/LLD.BIN name) needs `-flavor gnu`.  Host confirmed;
Makefile.hobbyos `LD := /TOOLS/LLD.BIN -flavor gnu`.  All 4 S1 links pass.

### F4: in-OS make pattern rules never match (S2 blocker)
`make kernel` -> `No rule to make target /SRC/obj/arm/X.o` for every X,
even one-rule minimal makefiles (minpat/minpatd evidence).  make -d shows
our rule IS tried with the right stem but the prereq existence check fails.
In-OS LS.BIN of /SRC/src/kernel shows 8.3-clean names UPPERCASE (DHCP.C,
MAIN.C): the in-OS FAT driver's readdir returns the SFN for 8.3-clean
files, and GNU make's pattern prereq check is a case-sensitive dir-cache
compare -> no match.  Explicit rules use stat() (case-insensitive) and
work.  Fix: Makefile.hobbyos kernels section rewritten as 59 explicit
rules (the file's own 'no implicit-rule reliance' design intent).  Host
validation: 59/59 resolve, zero errors; in-OS dry-run now lists all 59.

### F5 (evidence of TCG reality): in-OS clang under ARM TCG ~900-1500x
HOST glang -O2 aarch64: sh.c 1.38 s, whole kernel 5.8 s.  In-OS: sh.c >=
53 min, kernel TUs 1-15 min each.  The acceptance's 8x TCG placeholder is
off by ~2 orders of magnitude for clang.  S2 is a ~2 h proposition in one
boot; S3 (disk) has its own TCG cost (128 MiB MKFSFAT measured 284 s in
w4-kernel).

## Live S2 progress (also in report gating)

TUs done / 59 with wall-clock, last TU; final count + verdict on s2.serial.txt.
