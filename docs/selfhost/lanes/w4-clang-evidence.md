# Lane W4-CLANG — in-OS T2.3/T2.4/T2.5 smokes: evidence (2026-10-14)

Independent probe lane for the in-OS toolchain smokes. Everything below is
serial-backed (raw captures in `evidence-w4-clang/*.serial.txt`; the
`obj/w4clang/*.serial.txt` files in the worktree hold the untrimmed boots).
Driven with staged SELFHOST.RUN script files (no QMP keystrokes for command
text), via the existing `SELFHOST.BIN` desktop-launched driver
(`tools/w4_clang_probe.py` wraps `tools/run_selfhost_build_test.py` without
editing it — the harness file is untouched).

Probe image: copy of the w3-tools staged `disk.img` (`/TOOLS` with
CLANG/LLD/LLVMAR/OBJCOPY/MAKE/MKFSFAT + RESDIR, SELFHOST.BIN on the root).

## Verdict table

| smoke | in-OS command (driver stage) | result |
|---|---|---|
| T2.3 | `CLANG.BIN --version` (ELF as staged) | **WALL — executes its ELF header; entry fault (EC=0, ELR=entry); falsely reported rc=0** |
| T2.4 | `CLANG.BIN … -c HELLO.C` (ELF as staged) | same entry-fault WALL |
| T2.5 link | `LLD.BIN …` (ELF as staged) | same mechanism (all four toolchain ELFs share it) |
| T2.3b | `CLANG2.BIN --version` (host `objcopy -O binary`, staged as /TOOLS/CLANG2.BIN) | **PASS — full clang 21.1.8 banner on serial, exit 0** |
| T2.4b | `CLANG2.BIN … -c HELLO.C` (flat) | got past parse into AArch64 codegen, then **trapped at libc++ `operator new(nothrow)` override-assert (`brk #1`, EC=0x3C)** |
| T2.5b | `LLD2.BIN` link, `OBJ2.BIN` objcopy, run (flat) | lld **killed by an EXECUTE fault classified `in=PROT`** (VA=PC=0x100064E000) mid-image |
| T2.5 run | `/HELLO.BIN` in-OS | **PASS — `[HELLO] entering main … argv[0]="/HELLO.BIN" … main returned 0`, driver rc=0** |
| control | `NPNEW.BIN` (tiny C++ `new(std::nothrow)`, same prebuilt libcxx.a) | **PASS — `[NPNEW] alloc ok`, rc=0** |

## 1. T2.3 wall (as-staged toolchain) — the M2 conversion is missing

The staged `/TOOLS/*.BIN` toolchain binaries are **raw aarch64 ELF objects**
(first bytes `7f 45 4c 46 …`), while the in-OS loader contract — and every
working in-OS binary (HELLO.BIN, MAKE.BIN, SELFHOST.BIN: first bytes
`fd 7b bb a9`, a real `stp x29,x30` prologue) — is a **flat .bin loaded at
offset 0 with entry = USER_IMG_BASE**. w3-tools recorded this gap verbatim:
"binaries staged as raw aarch64-none-elf ELF (not flat .bin — **M2 converts
via objcopy -O binary + strips**; names frozen, content swaps in place)".
M2's conversion step simply has not been applied to the staged image.

Serial proof (t23 / t24, `evidence-w4-clang/t23.serial.txt`):

```
Loading program for scheduler (v2 AS): /TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=126060320 pages=30777 first=in elap=0ms
v2 loader: PID=4 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[KERNEL] User process 4 (/TOOLS/CLANG.BIN) fault! EC: 0x0000000000000000 ELR: 0x0000001000000000
[KERNEL] Process 4: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=t23-version-done rc=0          <- false pass: crash-kill surfaced as rc=0
```

The 126 MB load is **not** the wall (lazy image map + prefetch handle it fine —
see T2.3b). The wall is that the ELF's first byte (ELF magic `7f 45 4c 46`)
is executed as code: the CPU immediately faults (EC=0, ELR = the entry =
file offset 0) and the process is killed **before any page of the image is
prefetched** (`IMGPRF … start` visible, nothing mapped).

**Fix (validation): host-side `llvm-objcopy -O binary` of each toolchain ELF.**
Measured on `obj/selfhost/dist-staging/usr/local/bin/*` → the toolchain ELFs
are already linked with entry == USER_IMG_BASE (0x1000000000) == first LOAD
vaddr, so the flat conversion lands `_start` exactly at offset 0. Verified:
flat offset 0 is byte-identical to ELF file offset 0x10000 (cmp on 64 B), and
`llvm-objcopy -O binary clang-21 CLANG2.BIN` yields 94,192,000 B (126 MB
ELF → 94 MB flat; lld 72.6 MB → 53.3 MB; objcopy 10.1 MB → 5.0 MB; llvm-ar
13.6 MB → 8.3 MB). This is a staging/M2 change (owned outside this lane), no
kernel change required for the load path.

## 2. T2.3b PASS — 94 MB in-OS clang runs

`evidence-w4-clang/t23b.serial.txt`:

```
v2 loader: image mapped (lazy), bytes=94192000 pages=22997 first=in elap=1ms
v2 loader: PID=4 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[IMGPRF] pid=4 start: pages=22997 batch=128
clang version 21.1.8 (/home/sarah/hobbyos-selfhost-lanes/l1/third_party/llvm-21.1.8/src/llvm-project-21.1.8.src/clang 0c272a4e0cfc70e854e1958baaa2094524b4f9af)
Target: aarch64-unknown-none-elf
Thread model: posix
InstalledDir:
[IMGPRF] pid=4 done: pages=22997 mapped=22127 skipped=331 batches=176 stop=1 ms=4745
[SELFHOST] phase=t23b-version-done rc=0
[SELFHOST] ACCEPTANCE-COMPLETE stages=1
```

The lazy loader + background prefetch materialized 22,127 / 22,997 pages
(~86 MB) in 4.7 s during the run; the version banner goes to the driver's
stdout relay. **This validates lane w4-kernel's L9 prefetch work at the
126 MB-class end — the load path is NOT a blocker.**

## 3. T2.4b WALL — clang compiles until codegen, then traps in libc++

`evidence-w4-clang/t24b.serial.txt` (flat CLANG2.BIN, acceptance flags,
compiling /SRC/T2SRC/HELLO.C):

```
[IMGPRF] pid=4 done: pages=22997 mapped=22678 skipped=319 batches=180 stop=0 ms=3211
[KERNEL] FATAL: Unhandled EL0 Synchronous Exception!
EC: 0x000000000000003C
[KERNEL] Process 4: /TOOLS/CLANG2.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=t24b-compile-fail reason=tool-rc rc=24576
[SELFHOST] FAILED-AT stage=t24b-compile
```

EC=0x3C is the AArch64 **BRK instruction** exception. Disassembly of the
faulting address (ELR=0x1004842408, flat offset 0x4842408, symbol
`_ZnwmRKSt9nothrow_t` = `operator new(std::size_t, std::nothrow_t)`):

```
10048423a4: adr x8, _Znwm
10048423ac: adr x9, __start___lcxx_override
10048423b0: cmp x8, x9
10048423b4: b.lo 0x1004842408          -> brk #0x1  (executed here)
```

Root cause (libc++ source, `libcxx/src/new.cpp` + `overridable_function.h`,
confirmed against the vendored 21.1.8 tree): with `-fno-exceptions` and
override detection enabled, the nothrow `operator new` runs
`_LIBCPP_ASSERT(!__is_function_overridden<&operator new>())`; the assertion
fails (→ `brk #1`) when the throwing `operator new`'s address is **outside
the `__lcxx_override` section** (ELF `__start/__stop` auto-symbols). In this
cross-built clang-21, `_Znwm` = 0x100484e410 but the section spans
[0x1004866cc0, …] → "overridden" → assert → BRK → the OS (no BRK handler)
kills the process. Not an OOM and not an OS bug: on the failing path the
wrapper never even reaches `malloc`.

Counter-proof (control, `npnew.serial.txt`): a 85 KB C++ program linking the
**same prebuilt `libcxx.a`** (this build's libc++), compiled with the same
`-fno-exceptions` flags, has `_Znwm == __start___lcxx_override` (both
0x1000012610) → check passes → `[NPNEW] alloc ok`, rc=0 in-OS. So the OS +
libc++ runtime pair is healthy; **clang-21's own link lays `_Znwm` outside
the override section** (LTO/function-section merge is the suspected cause —
the LLVM final link commonly LTOs libc++ in). This is a *toolchain-build*
defect, owned by the toolchain builder lane, not the kernel.

Candidate toolchain-side fixes (to propose to the owning lane):
(a) build the in-OS toolchain with exceptions enabled (kills the assert
path), or (b) disable `_LIBCPP_CAN_DETECT_OVERRIDDEN_FUNCTION` for the
static single-CRT build, or (c) keep `operator new`'s section attribute
through the final link (linker script / no-LTO for libc++ members).

## 4. T2.5b WALL (kernel-class) — lld executed a page mid-prefetch → PROT kill

`evidence-w4-clang/t25b.serial.txt` (flat LLD2.BIN, real link cmd):

```
v2 loader: image mapped (lazy), bytes=53250208 pages=13001 first=in elap=1ms
v2 loader: PID=4 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[IMGPRF] pid=4 start: pages=13001 batch=128
[KERNEL] pid=4 (/TOOLS/LLD2.BIN) memory fault VA=0x000000100064E000 PC=0x000000100064E000 rw=rx in=PROT -> killed
[KERNEL] Process 4: /TOOLS/LLD2.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=t25b-link-fail reason=tool-rc
```

An **instruction-fetch fault on a page that `vm_handle_fault` classified as
PROT** (`in=PROT`: the page walk said "present" so the demand-fill path was
not taken — the handler kills rather than re-maps). VA == PC == 0x100064E000
is ~6.6 MB into the image (mid `.text`). The whole IMAGE region is mapped
RWE, so a genuine execute-denied is impossible on a healthy leaf; the most
likely mechanism is the lazy-fill protocol's "present-but-pending (negative
entry)" page being hit by an instruction fetch while the background prefetch
was still filling it → classified PROT → kill. This is a **kernel-side
finding for lane w4-kernel (fresh L9 prefetch code)**: exec faults on a
not-yet-filled image page must be demand-filled, not killed. Suggested
start: classify `in=PROT`/present-but-faulting leaf for VMK_IMG the same as
an absent page, or fill the whole page before flipping the leaf present.

Note: clang's run (T2.4b) survived its whole prefetch (`stop=0`, all pages
handled) and died LATER on the libc++ assert — so this PROT race is not
deterministic per-run; it is a timing-sensitive loader/prefetch classification
issue. One more exact serial + disassembly probe would isolate the leaf state
(candidate for w4-kernel or the next probe pass).

## 5. Secondary kernel crash (deterministic) — pid=5 housekeeping, after every failed launch

After every failed tool launch (t24, t24b, t25b), the capture shows a
deterministic kernel-mode fault in an unnamed housekeeping process:

```
[KERNEL] FATAL: CPU 0x0000000000000001 Synchronous Exception in EL1! ESR: 0x0000000096000004 EC: 0x0000000000000025 ISS: 0x0000000000000004, FAR: 0x0000FFFFFFFFF000, ELR: 0x000000043A6AF37C, PROC pid=5 name= ...
[KERNEL] EL1 fault in user address space -> killing pid=5
```

Same ELR/LR (0x43A6AF37C / 0x43A6ABE30), FAR ≈ 0xFFFF_FFFF_F000 (top-of-AS
window), every run — a reproducible kernel bug in the pid=5 housekeeping
process (likely the `[IDLESTUCK]` probe — it flags the driver as stuck while
it legitimately relays a long-running child; a recovery/probe spawn then
faults in kernel mode). Report to w4-kernel; `[IDLESTUCK] slot=3 pid=3
st=3 parent=1 SELFHOST.BIN` spam itself is a driver-legitimacy false positive
(the driver is supposed to sit in a pipe read while its child runs).

## 6. Acceptance-verdict gap (driver, userspace) — crash-killed tools can look like rc=0

t23 showed `[SELFHOST] phase=t23-version-done rc=0` + `ACCEPTANCE-COMPLETE`
for a process that **fault-killed at entry** — a false pass that a
marker-only verdict cannot catch (zero FAIL tokens). Mechanism: the EC=0
"Unknown reason" and EC=0x3C ("Unhandled") kill paths call bare
`process_exit(tf)` (stale x0 → arbitrary exit_status), while the SIGSEGV
path uses `process_fault_exit(tf,11)` which sets `PROCESS_STATUS_SIGNAL`
(0x40000000) so waitpid sees signal 11 (nonzero). Fix candidates:
(a) kernel — route the EC=0x00 and EC=0x3C kill paths through
`process_fault_exit(tf, SIGTRAP/SIGSEGV)` (2 call sites, trap.c) so a
fault-killed child never reports a clean 0 status (w4-kernel lane);
(b) driver — fail stages when the relayed stream contains no stdout yet the
child died (fragile). (a) is the clean fix; recommended.

## 7. Probe-side notes (this lane)

- **spawn args are capped at 255 chars** (`process.h args[256]`): the
  driver's direct tool invocations truncate long compiler command lines
  (first t24b attempt: ~330 chars → clang saw "no input files"). The real
  acceptance path (make → shell → execvp) has a larger budget; for direct
  driver probes keep commands ≤255 or route through a staged script file
  (`SH.BIN /SRC/…/X.SH`, sh script-mode buffer is 64 KiB).
- Flat-conversion drop: clang 126.1 MB → 94.2 MB, lld 72.7 → 53.3, objcopy
  10.1 → 5.0, ar 13.6 → 8.3 (all byte-exact `_start`-at-0 verified).
- The worktree's `tools/w4_clang_probe.py` reproduces every probe (boot the
  staged disk copy, mcopy a SELFHOST.RUN, launch the driver, capture serial).
