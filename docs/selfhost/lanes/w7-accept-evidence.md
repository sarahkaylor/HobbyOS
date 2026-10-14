# Lane w7-accept — full-loop acceptance evidence (T4.3, merged tip 288995c)

Branch `selfhost/w7-accept`, worktree `/home/sarah/hobbyos-selfhost-lanes/w7-accept`.
Base = merged tip `288995c`. Main repo untouched; local commits on
`selfhost/w7-accept` only. Raw serials + pulled artifacts under
`docs/selfhost/lanes/evidence-w7/`.

## A. w6 OUT.IMG boots and runs an in-OS-built program (S4/S5, out-of-order)

Image under test: `obj/selfhost/w6-out/OUT.IMG` — the OUT.IMG ASSEMBLED
IN-OS by the w6 S3 stage. sha256 `7e24b4ca1db8deff4dee75c3b1c71e63460f8eb90e4f
a3027934b1126e20286b` (verified at boot time).

Layout, re-verified with `fat16img.py` (missions' mdir-record reproduced):

```
 0 dir  BOOT                   0      /boot/hobbyos.bin 219564 B (ARM64 boot Image)
 3 file HELLO.BIN          37360      /boot/limine.conf 99 B
 4 file SH.BIN            195136      /EFI/BOOT/BOOTAA64.EFI 315392 B
 5 file LS.BIN             35200      /bin/SH.BIN 195136 B
 6 file CAT.BIN            34960
 7 file DESKTOP.BIN       555568      root: HELLO/SH/LS/CAT/DESKTOP/CONSOLE/
 8 file CONSOLE.BIN        44992      OUTPROG.BIN/OUTMARK.TXT
 9 file OUTPROG.BIN        37360
10 file OUTMARK.TXT           44
```

Boot (standalone, acceptance.md §5.1 QEMU line, `tools/w7_accept_boot.py`,
serial `s4-w6outimg.serial`):

```
linux: Loading kernel `boot():/boot/hobbyos.bin`...Booting AArch64 OS...   (6.5 s)
FAT-16 filesystem successfully initialized.
Mode: DESKTOP - Launching desktop...
...
[MENU] 0=CONSOLE.BIN ... [MENU] 9=OUTPROG.BIN ... [MENU] count=11
[LAUNCH] OUTPROG.BIN
Loading program for scheduler (v2 AS): OUTPROG.BIN
v2 loader: image mapped (lazy), bytes=37360 pages=10 first=in elap=1ms
[KERNEL] Process 3: OUTPROG.BIN exited unexpectedly or gracefully.
```

- Kernel boots from the in-OS-assembled OUT.IMG: `Booting AArch64 OS` 6.5 s,
  FAT-16 initialised, DESKTOP mode, Apps menu enumerated from the image root.
- OUTPROG.BIN (the in-OS-built HELLO copy) launched from that menu
  (`[LAUNCH] OUTPROG.BIN`), 37360 B mapped, process ran and exited cleanly.
- Byte identity: `OUTPROG.BIN` extracted from OUT.IMG sha256
  `61b3be6ad97e0286…` == extracted `HELLO.BIN` sha256 == w6 in-OS
  HELLO.BIN (BYTE-IDENTICAL to host `obj/arm/hello.bin`, w6-recorded).
  Both are flat ARM64 binaries (first bytes `fd 7b bb a9` = `stp x29,x30,…`
  — ELF header stripped by `llvm-objcopy -O binary`).

Program-side serial note (frozen ceiling, acceptance.md §8.3): a
desktop-launched program's stdout goes to its window (framebuffer), so the
serial proof on this boot path is the loader lines + process-exit line +
host file hash. The program's own `[HELLO] entering main` marker on serial
comes from the MODE=selfhost driver path (T2.5, below) — that requires the
selfhost-mode kernel, which OUT.IMG's default kernel does not pick.

## B. T4.3 — full in-OS loop on the merged tip (fresh disk.img)

Flow (runbook: acceptance.md §9):
`make MODE=selfhost disk.img` (host rebuild + /TOOLS + /SRC staging) →
stage w7 `SELFHOST.RUN` (+ T25.RUN) → boot with `tools/w5_flat_boot.py`
(one long boot, S0→S3) → extract + sha-compare OUT.IMG → boot it → launch
OUTPROG.BIN.

### B0. Host pre-build + staging (make-diskimg.log, uncommitted)

- `make MODE=selfhost disk.img`: full host cross-build then
  `selfhost-stage.sh --tools` — CLANG.BIN 94,192,328 B, LLD.BIN
  53,250,536 B, LLVMAR.BIN 8,349,200 B, OBJCOPY.BIN 4,982,672 B
  (flat-converted), 282 files / 176,717,454 B into ::/TOOLS (+ RESDIR
  295 files, 14,727,254 B); `--src` 232 files / 4,805,464 B into ::/SRC
  (incl. SELFHOST.RUN/SELFHOST.SH/Makefile.hobbyos).
- drive script `SELFHOST-RUN-w7-full.txt` staged as ::/SRC/SELFHOST.RUN:
  `s0-check, s1-user, t25-run, s2-kernel, s3-disk`.
- MODE file `obj/arm/.mode` = selfhost (boots ONLY SELFHOST.BIN,
  serial-console relay, w4-kernel).

### B1. The loop serial (t43-loop.serial)

Stage-by-stage markers (final rows filled on completion; see
`w7-accept-report.json`):

| stage | result | marker / timing |
|---|---|---|
| S0 check | **PASS** | `phase=s0-check-done rc=0` |
| S1 user | (below) | `phase=s1-user-start` … |
| T2.5 | (below) | `[HELLO] entering main` expected |
| S2 kernel | (below) | 59 TU compile + lld link + objcopy |
| S3 disk | (below) | MKFSFAT + MCOPY → /SRC/OUT.IMG |

### B2. OUT.IMG comparison vs w6 (from the in-OS build)

- `fat16img.py get disk.img OUT.IMG > w7-loop.out.img`
- sha256 vs w6 (7e24b4ca…): EXPECTED to differ — in-OS make is
  non-incremental, every rebuild is a fresh mkfs + fresh copy order
  (fresh BPB/FAT/root). Compare *content classes* instead: BPB geometry,
  layout, per-file sizes + HELLO.BIN/OUTPROG.BIN sha (must equal
  `61b3be6a…`).

### B2.5 S2 tail FAILURE on the un-fixed tip (bd58474-class bug reproduced)

The full loop FAILED at S2's link on the merged tip 288995c
(`phase=s2-kernel-fail reason=tool-rc rc=512`, `FAILED-AT stage=s2-kernel`,
then `System halt from CPU 1.`). w5_flat_boot outcome=1, elapsed 8637 s.

Precise failure state (serial evidence, t43-loop.serial):

```text
LLD.BIN: error: cannot open /SRC/obj/arm/nfs_test.o: No such file or directory
LLD.BIN: error: cannot open /SRC/obj/arm/cpu.o: No such file or directory
MAKE.BIN: *** [Makefile.hobbyos:424: kernel] Error 1
[SELFHOST] phase=s2-kernel-fail reason=tool-rc rc=512
```

- 56 `phase=kernel-compile rc=0` + 3 `phase=kernel-ascompile rc=0` recipe
  runs = all 59 recipe lines reported rc=0, and make proceeded to the link.
- **No clang error block anywhere** in the capture; **no OOM/alloc
  diagnostics**; 57 of 59 `.o` files landed on the in-OS FAT — exactly
  `nfs_test.o` and `cpu.o` are missing.
- The two compile command lines ARE present on the wire and look complete
  (`… -c src/kernel/nfs_test.c -o /SRC/obj/arm/nfs_test.o`, and
  `… -c src/kernel/arch/arm/cpu.c -o /SRC/obj/arm/cpu.o` at serial lines
  935 / 1661–1665 — the cpu.c line is split across the serial buffer with a
  loader message interleaved), yet no output file was written and the
  recipe still returned rc=0.

Interpretation: make echoes its recipe (what it THINKS it ran), but the
actual `exec` of CLANG.BIN for those two TUs received a cropped argv — an
argument was **silently dropped** near the top of a mapped region. This is
exactly the class fixed on main at **`bd58474` (w8-torture)**: w6's cap
bump 64→4096 moved `proc_marshal_argv`'s whole-cap range check past the
mapped-region end, so a short string near the region top got mis-bounded;
the fix replaces the whole-cap check with **per-page validation**
(`src/kernel/program_loader.c`, `arch/arm/trap.c`, `arch/x64/trap.c`);
288995c is an ancestor, the fix cherry-picked cleanly. (Both dropped-TU
recipes hitting rc=0 via sh is consistent with a silently failed exec
feeding `sh -c` an empty/garbled line.)

### B2.6 Fix applied + S2 re-run (one stage only, per integrator guidance)

- Cherry-picked `bd58474` (w8-torture exec-argv fix) onto `selfhost/w7-accept`
  → `c7c5823`; the same 3 kernel files compile host-side.
- `make MODE=selfhost disk.img` rebuilt on the fixed kernel (log
  `make-diskimg-fix.log`), re-staging /TOOLS + /SRC.
- Re-boot with an S2-only drive (`s2-kernel|SH.BIN|/SRC/SELFHOST.SH kernel`)
  on the FIXED boot kernel; serial `t43-s2-fix.serial`.

| run | kernel | disk state | result |
|---|---|---|---|
| S0–S3 full loop | 288995c (unfixed) | fresh disk, sequential stages in one boot | S0 ✓ S1 ✓ T2.5 ✓ S2 ✗ (2 TUs silently dropped) |
| S2 only re-run | + bd58474 fix | **fresh disk.img / empty /SRC/obj/arm, single stage** | S2 ✗ **IDENTICAL failure** (nfs_test.o + cpu.o) |

### B2.7 DISCRIMINATOR VERDICT — it is a FAT-directory-write bug, NOT argv, NOT prior state

The S2-only re-run was the integrator's cheap discriminating experiment
(fresh `make disk.img` → `dd`+`mkfs`+staging, `/SRC/obj/arm` verified EMPTY
pre-boot, exactly one `make -f Makefile.hobbyos kernel` in one boot) and it
**failed identically**: `LLD.BIN: error: cannot open /SRC/obj/arm/nfs_test.o`
+ `cpu.o`, 56×`kernel-compile rc=0` + 3×`kernel-ascompile rc=0`, no clang
errors, no OOM, at `Makefile.hobbyos:424`. Two runs, two kernels (pre- and
post-argv-fix), two independent fresh boots → same two objects missing.

Root cause is a **FAT16 directory-entry (dirent/LFN) write bug** in the
in-OS path, evidenced by reading the FAILED image's `::/SRC/obj/arm`
directory (`python3 fat16img.py list`-style deep scan, committed raw probes):

- The two "missing" objects ARE on the image — under mangled dirents:
  - `NFS_TEST.O.T` (832 B, cluster 19822) — valid ELF relocatable, sha
    `47804dee…`; the 8.3 SFN extension field literally contains `O.T`
    (embedded dot, invalid 8.3);
  - `CPU-8167.O.T` (6968 B, cluster 20007) — valid ELF relocatable, sha
    `21d8dc63…`.
- Every other object has BOTH a clean dirent (`FAT16.O`) AND a mangled
  duplicate twin (`FAT16-AB.O.T`) pointing at the SAME cluster with
  BYTE-IDENTICAL content (both sha `efde5cec…`) — the writer emits a
  duplicate dir-entry per file (VFAT LFN chain `seq 0x41/0x42/0x01…` +
  SFN), and for exactly 2 of 59 files only the mangled twin survives; lld
  then cannot open the clean names. Deterministic file-for-file across
  runs (same names both times → filename/content-keyed, not address-layout
  → rules out the exec-argv class, which is address-layout-keyed).

So nothing is lost at the cluster level — the S2 objects exist; the in-OS
directory writer misnames/duplicates entries. `bd58474` is a real fix for
the w8-torture argv case but does NOT address this; the final acceptance
RC must be re-cut after a kernel-side dirent-writer fix (parent's call —
likely a w9/w8-adjacent kernel lane). Pulled evidence in `evidence-w7/sB/`
(`CPU-8167.O.T`, `NFS_TEST.O.T`, `FAT16.O`, `FAT16-AB.O.T`) + raw dirent
dump in `t43-s2-fix.dirents.txt`.

### B3. Boot the extracted OUT.IMG + run the in-OS-built program

`tools/w7_accept_boot.py --image w7-loop.out.img --launch OUTPROG.BIN` →
assert `Booting AArch64 OS`, `[MENU] …=OUTPROG.BIN`, `[LAUNCH] OUTPROG.BIN`,
`[KERNEL] Process … exited`.

## Artifacts in this dir

- `s4-w6outimg.serial` — Mission A full serial (w6 OUT.IMG boot + launch)
- `sA/OUTPROG.BIN`, `sA/HELLO.BIN` — pulled from w6 OUT.IMG (sha 61b3be6a…)
- `t43-loop.serial` — Mission B full loop serial (S0→S3)
- `t43-loop.outcome` — w5_flat_boot result line
- `make-diskimg.log` (uncommitted build log)
- `w7-loop.out.img` + `sB/…` — extracted OUT.IMG + pulled artifacts
