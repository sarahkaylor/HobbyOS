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
