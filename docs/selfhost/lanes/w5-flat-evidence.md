# Lane W5-FLAT — M2 flat .bin conversion in the staging pipeline (evidence)

Wave-5 staging lane.  Mission: make the /TOOLS toolchain ELF→FLAT .bin
conversion (w4-clang's proven fix for the in-OS entry-fault wall, blocker
b1) a FUNCTION of the `--tools` staging pipeline, rebuild the image, and
prove from the pipeline that /TOOLS/CLANG.BIN runs in-OS printing its full
version banner.

## What shipped

- `tools/selfhost-stage.sh --tools` now converts every LLVM distribution
  ELF (clang-21, lld, llvm-ar, llvm-objcopy) to a FLAT .bin at stage time
  via the HOST `llvm-objcopy -O binary` (new `elf_flat()`, [v5]).
  - Sanity checks before staging: source must be a 64-bit AArch64 ELF
    (magic + EI_CLASS + e_machine=EM_AARCH64); `e_entry == USER_IMG_BASE`
    (0x1000000000); FIRST PT_LOAD `p_vaddr == e_entry` (the loader's
    entry-at-0 contract); after conversion flat[0:64] byte-identical to the
    ELF's first-LOAD file content.  Any check failure ABORTS the staging —
    a broken toolchain must not land silently.
  - Already-flat sources (ported `obj/arm/*.bin`) skip cleanly (elf_flat
    returns 1 → flat-copy), keeping idempotence + `-f` replace behavior.
  - Frozen F1 /TOOLS names unchanged — only the staged CONTENT is now
    runnable in-OS.  Made `make disk.img` produces a RUNNABLE /TOOLS.
  - Host dependency: `llvm-objcopy` (checked only when an ELF is found).
- `tools/w5_flat_boot.py` — lane helper: boots a MODE=selfhost disk.img
  (kernel auto-launches SELFHOST.BIN with /SRC/SELFHOST.RUN, no desktop),
  waits for terminal [SELFHOST] markers, captures serial.

## Gates

### G1 — make disk.img green, /TOOLS staged FLAT (PASS)

`make MODE=selfhost disk.img -j64` → rc=0.  Staging lines from the real
pipeline run (build-w5.log):

```
selfhost-stage:   CLANG.BIN <- obj/selfhost/dist-staging/usr/local/bin/clang-21 (converted, 94192000 B)
selfhost-stage:   LLD.BIN <- obj/selfhost/dist-staging/usr/local/bin/lld (converted, 53250208 B)
selfhost-stage:   LLVMAR.BIN <- obj/selfhost/dist-staging/usr/local/bin/llvm-ar (converted, 8348856 B)
selfhost-stage:   OBJCOPY.BIN <- obj/selfhost/dist-staging/usr/local/bin/llvm-objcopy (converted, 4982328 B)
```

`mdir ::/TOOLS` + sha256 of the staged entries (flat sizes, matching
w4-clang's hand-converted measurements exactly):

| entry | size (B) | sha256 (first 16) |
|---|---|---|
| CLANG.BIN  | 94,192,000 | 099e25f997b51674… |
| LLD.BIN    | 53,250,208 | f6c2edd6afee22e3… |
| LLVMAR.BIN |  8,348,856 | e85a3f294afbfa4c… |
| OBJCOPY.BIN|  4,982,328 | 6b4e78b402f49982… |
| MAKE.BIN | 308,072 | (ported, untouched) |
| (+ MCOPY/MDEL/MMD/MKFSFAT/SH, RESDIR/include 295 files) | | |

All four sha256 match the scratch-verified flat conversions byte-for-byte;
the scratch check also confirmed flat[0:64] == ELF[0x10000:64] for every
tool and that the flat head is a real `stp x29,x30` prologue (`fd 7b bb a9`),
not `\x7fELF`.

Idempotence: `--tools` re-run green (mcopy -o overwrite), `--tools -f`
green (mdeltree + fresh).  Unit tests for elf_flat: real ELF→0 (converted),
non-ELF→1 (skip), bad e_entry→2, bad PT_LOAD vaddr→2.

### G2 — in-OS /TOOLS/CLANG.BIN version banner FROM THE PIPELINE (PASS)

Booted the pipeline-built disk.img (MODE=selfhost), with a lane SELFHOST.RUN
stage `w5-clang-version|CLANG.BIN|--version` (full capture
`evidence-w5-flat/w5-clang-version.serial.txt`, raw `serial-w5-clang.log`):

```
Mode: SELFHOST - Running in-OS acceptance build driver...
Loading program for scheduler (v2 AS): SELFHOST.BIN
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
[SELFHOST] phase=w5-clang-version-start
Loading program for scheduler (v2 AS): /TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192000 pages=22997 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[IMGPRF] pid=3 start: pages=22997 batch=128
[IMGPRF] pid=3 done: pages=22997 mapped=22574 skipped=423 batches=180 stop=0 ms=4198
clang version 21.1.8 (.../clang 0c272a4e0cfc70e854e1958baaa2094524b4f9af)
Target: aarch64-unknown-none-elf
Thread model: posix
InstalledDir:
[SELFHOST] phase=w5-clang-version-done rc=0
[SELFHOST] ACCEPTANCE-COMPLETE stages=1
```

The FULL clang 21.1.8 banner (version, Target, Thread model, InstalledDir)
lands on serial; the driver reports `phase=w5-clang-version-done rc=0` and
`ACCEPTANCE-COMPLETE stages=1` (guest exit code 0).  The 94 MB flat image
was prefetched in 4.2 s (22,574 / 22,997 pages) while clang ran.  This is
w4-clang's T2.3b PASS reproduced through the pipeline — no hand conversion.

### G3 — S0 attempt (`make -f Makefile.hobbyos check`) — blocked at exec gate (expected, handed off)

Restored the pipeline's real /SRC/SELFHOST.RUN (s0..s3 make stages) and
booted again (full capture `evidence-w5-flat/w5-s0-check.serial.txt`):

```
[SELFHOST] phase=s0-check-start
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include ... -c src/kernel/main.c -o /SRC/obj/arm/check.o
sh: exec failed: /TOOLS/CLANG.BIN
MAKE.BIN: *** [Makefile.hobbyos:149: check] Error 127
[SELFHOST] phase=s0-check-fail reason=tool-rc rc=512
[SELFHOST] FAILED-AT stage=s0-check
```

In-OS make PARSED Makefile.hobbyos, resolved the check target (compile
`src/kernel/main.c` → `obj/arm/check.o`) and put the compile line on the
wire — then sh's `execv` path rejected /TOOLS/CLANG.BIN at the kernel's
**1 MiB MAX_PROGRAM_SIZE exec gate**
(`program_loader.c program_too_large`; `src/include/process.h:42`), the same
CLANG-EXEC-1MIB-GATE w4-src recorded.  This is NOT the nothrow wall: it is
kernel-side, hit before clang's first instruction.  Note the contrast with
G2 — the driver's scheduler spawn of the same flat CLANG.BIN runs fine; the
blocker is the execv path's size gate, which the w5-kernel lane owns
(raise/remove to USER_IMG_SIZE).  The `_Znwm`/nothrow wall (lane w5-nothrow)
is DOWNSTREAM: clang must actually execute to reach codegen.

## Files

- `tools/selfhost-stage.sh` — [v5] elf_flat() + stage_flat() in stage_tools
  (the M2 conversion).  Sole source-side change.
- `tools/w5_flat_boot.py` — lane boot/capture helper (new).
- `docs/selfhost/lanes/evidence-w5-flat/w5-clang-version.serial.txt`,
  `docs/selfhost/lanes/evidence-w5-flat/w5-s0-check.serial.txt` — trimmed
  serial evidence (raw: `serial-w5-clang.log`, `serial-w5-s0.log`, both
  gitignored).
- `docs/selfhost/lanes/SELFHOST-RUN-w5-clang-version.txt` — the lane probe
  script used for the G2 run.
- `docs/selfhost/lanes/w5-flat-report.json` — lane report.
