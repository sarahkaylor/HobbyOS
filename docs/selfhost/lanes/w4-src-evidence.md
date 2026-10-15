# Lane W4-SRC — the in-OS build driver (evidence, 2026-10-14)

Wave-4 source lane.  Gates: `make disk.img` green with /SRC staged (mdir
evidence + size); S0 dry-run evidence (`[SELFHOST] phase=s0-check-done` or
the precise blocker); S1 attempt evidence; units green.

## 1. Makefile.hobbyos v1 (task A)

Repo-root `Makefile.hobbyos`, transcribed from the L4 sketch
(`docs/selfhost/buildtools-audit.md` §7.1) and the frozen acceptance text
(`docs/selfhost/acceptance.md` §1):

- Targets `check` / `user` / `kernel` / `disk` / `all` (F3); markers use the
  acceptance §1 vocabulary (`phase=s0-check-*`, `s1-user-*`, `s2-kernel-*`,
  `s3-disk-*`, `all-done`) with make's `rc=$$?` in-band.
- `SHELL := /TOOLS/SH.BIN`, `MAKEFLAGS += -r -R`, one command per recipe
  line, no `$(shell)`, no `$(wildcard)`, no `/dev/null`; absolute
  `/TOOLS/*.BIN` command paths; `-isystem /TOOLS/RESDIR/include` (AD-5).
- Kernel source set = the 59 ARM TUs measured on this worktree (47 top-level
  .c, 9 arch/arm/.c, 3 arch/arm/.s), object rules = 3 explicit pattern rules
  (top .c / arch .c / arch .s) producing flat `obj/arm/*.o`; the kernel link
  uses the explicit object list (`-T linker.ld`, the repo-root script the
  host `LDFLAGS` references — corrected from the sketch's `src/linker.ld`).
- `user` set = AD-9 HELLO/SH/LS/CAT; v1 links each against the carried
  `obj/arm/libc.a` (`/SRC/obj/arm/libc.a`) per the sketch; the in-OS libc
  rebuild is the make-port lane's follow-up.
- `disk` target = the §8.1 proven-booting OUT.IMG layout (128 MiB FAT16,
  DISK_BLOCKS=131072 = 1024-byte MKFSFAT -C units per w2-mkfs), /boot +
  /EFI/BOOT + /DESKTOP.BIN + /CONSOLE.BIN + /SH.BIN + /OUTPROG.BIN +
  /OUTMARK.TXT sourced from the in-OS build + the carried /SRC/bin products.

Verification: host GNU make parses it and resolves every pattern (main.o via
the top .c rule, cpu.o via the arch .c rule, boot.o via the .s rule); dry-run
of `check` prints exactly the frozen compile line + the s0 markers.

## 2. /SRC source staging (task B)

`tools/selfhost-stage.sh --src` stages the BUILD-REQUIRED subset:

| item | files | bytes |
|---|---|---|
| src/kernel + src/include + src/user_include + src/libc (`/SRC/src/**`) | 242 | 2,560,... |
| user set + driver (hello/sh/sh_aux/ls/cat + linker.ld + selfhost dir) | 13 | ~120 KB |
| linker.ld + Makefile.hobbyos + bootloader/BOOTAA64.EFI + limine.conf | 4 | ~325 KB |
| carried build products (libc.a, DESKTOP/CONSOLE/SH, OUTMARK) | 5 | ~1.6 MB |
| **total** | **~226** | **2,771,368 B (2.6 MiB)** |

(measured on the scratch image pre-build; carried products warn-skip until
the tree is built).  Root Makefile hookup = one line:
`tools/selfhost-stage.sh --src disk.img` after the `--tools` line (guarded
by the same `STAGE_TOOLS` ifneq).

Disk-recipe integration detail (found by failing): the recipe pre-creates
`::/src/...` (L3 LFN fixture dirs, Makefile:1865-1874) which IS the /SRC
directory on FAT (8.3 names are stored upper-case).  `--src` therefore
merges into an existing /SRC (mcopy -o) and never deletes it, so the L3
fixtures survive; the payload lands at /SRC/src/... (same nesting as the
full payload mode) so the in-OS relative `src/...` paths resolve with
cwd=/SRC.

## 3. Driver cwd fix

SELFHOST.BIN spawns stage tools from cwd `/` (every HobbyOS process starts
with cwd=/, src/kernel/process.c) but `make -f Makefile.hobbyos` needs
cwd=/SRC.  SELFHOST.RUN stages now run via `SH.BIN /SRC/SELFHOST.SH
<target>` — sh script mode (`cd /SRC` builtin + `/TOOLS/MAKE.BIN -f
Makefile.hobbyos "$1"`); sh script mode + cd + externals are proven in-OS.

The disk recipe only staged SH.BIN at the disk root; the L4 sketch's
`SHELL := /TOOLS/SH.BIN` needs it in /TOOLS.  First dry-run (run1) without
it: make's first recipe-line exec of the missing SHELL hit
`v2 loader: bad image size 0` and then an INFINITE fault/exit respawn loop
(`[KERNEL] User process 5 (/TOOLS/MAKE.BIN) fault! EC: 0x20 ELR:
0x100002A5FC` repeated ~370k serial lines → harness verdict `hang`).
`selfhost-stage.sh --tools` now stages `::/TOOLS/SH.BIN` (13 /TOOLS
entries); with it present the loop is gone and stages fail cleanly.

## 4. S0 dry-run (task C) — the first true in-OS build attempt

Runner: `tools/run_selfhost_build_test.py --image disk.img` (no --stage:
the make-built image already carries the toolchain + source payload).
Boot 6.5 s, SELFHOST.BIN launched from the Apps menu (index 74) via QMP.

Run2 (full SELFHOST.RUN) — clean `stage_failure` at s0-check, evidence:

```text
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
[SELFHOST] phase=s0-check-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN       <- driver spawns sh
v2 loader: PID=4 ...                                       <- sh up, cwd wrapper
... [make runs: echoes its own s0-check-start via /TOOLS/SH.BIN] ...
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf
  -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only
  -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/obj/arm/check.o
sh: exec failed: /TOOLS/CLANG.BIN
[SELFHOST] phase=s0-check-fail reason=tool-rc rc=512
[SELFHOST] FAILED-AT stage=s0-check
```

`check` (a single tiny TU compile in-OS) got as far as the CLANG exec and
fails there.  Root cause (kernel-side, w4-kernel lane — reported, not
edited): the exec path's `program_too_large()` rejects anything over
`MAX_PROGRAM_SIZE = USER_INITIAL_CLEAR_SIZE = 0x100000` (1 MiB)
(src/include/process.h:42; src/kernel/program_loader.c:63, gate :855-860 →
-ENOEXEC).  CLANG.BIN is 126,060,320 B; LLD (72 MB), LLVMAR (13 MB),
OBJCOPY (10 MB) fail identically.  The v2 loader's own cap is
`USER_IMG_SIZE = 128 MiB` (vm.h:44), so the standalone 1 MiB gate is the
blatant contradiction to resolve there.

## 5. S1 attempt (task C)

S1-first SELFHOST.RUN variant (`s1-user` only) — run3, clean
`stage_failure` at s1-user:

```text
[SELFHOST] phase=s1-user-start
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include
  -Isrc/user_include/graphics -Isrc/include
  -Isrc/libc/include --target=aarch64-none-elf ... -isystem /TOOLS/RESDIR/include
  -c src/user/hello.c -o /SRC/obj/arm/hello.o
sh: exec failed: /TOOLS/CLANG.BIN
MAKE.BIN: *** [Makefile.hobbyos:125: /SRC/obj/arm/hello.o] Error 127
[SELFHOST] phase=s1-user-fail reason=tool-rc rc=512
[SELFHOST] FAILED-AT stage=s1-user
```

In-OS make parsed `Makefile.hobbyos`, resolved the `user` target's
prerequisites (hello.o) and began compiling the AD-9 set — blocked at the
same 1 MiB exec gate.  This independently PROVES Makefile.hobbyos is
parseable + target-resolvable under the in-OS GNU make port (it named the
failing rule from my makefile, line 125).

Both verdicts: `exit 13` (stage_failure), zero FAIL tokens
(`fail_token=0`), boot 6.5 s, driver menu index 74.  Result JSONs at
~/.hermes/cache/scratch/w4-src/run{2,3}/result.json; raw serials at
~/.hermes/cache/scratch/w4-src/run{1,2,3}/serial-selfhost.log (uncommitted).

## 6. AD-9 (task D)

Frozen: `STAGE_LIBCXX=0` default for both staging modes.  Evidence: every
TU on the S0..S3 path is C (the two dry-run compile lines above carry no
libcxx include path; DESKTOP for the S5 path is C).  Frozen in
Makefile.hobbyos, selfhost-stage.sh, acceptance.md §2/§7.

## 7. Gate summary

| gate | status |
|---|---|
| `make disk.img` green | PASS (exit 0; mformat shim for mkfs.fat) |
| /SRC staged (mdir + size) | PASS — 307 files, 4,770,966 B, 688 MiB free |
| S0 dry-run | PASS-as-expected — clean `stage_failure` at s0-check with the precise blocker (1 MiB exec gate); compile line reached |
| S1 attempt | PASS-as-expected — make parsed Makefile.hobbyos, resolved user set, attempted hello.o, blocked on the same gate |
| units | PASS — classifier self-test 7/7, bash -n, py_compile, host make parse |


