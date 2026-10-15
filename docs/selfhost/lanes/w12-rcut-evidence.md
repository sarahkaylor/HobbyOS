# w12-rcut acceptance evidence — FINAL re-cut on the merged-final tip

One-boot full in-OS build S0→S3 (serial kernel, incremental user/kernel, first
real `-j8` data on real TUs), extracted OUT.IMG sha + standalone boot, in-OS-
built program run.  All raw serials are the §8.4 evidence (marker substrings,
zero FAIL tokens).  Companion report: `w12-rcut-report.json`.

## Verdict summary

| stage | attempt-1 (-j8) | attempt-2 (serial) | attempt-3 (receipt, one boot) |
|---|---|---|---|
| S0 check | PASS 11 s | PASS 17 s | PASS 11 s |
| S1 user (-j8) | PASS 33.0 min | PASS 65 min (battery-10 contended) | PASS 101 s (incremental) |
| T2.5 in-OS HELLO | PASS | PASS | PASS |
| S2 kernel | **FAIL** 57/59 + torn dirent (`net_rdma_test.o`), ~20 min | **PASS serial** 59/59 + link, 77 min | PASS 34 s (incremental relink) |
| s2-inc (incremental demo) | — | PASS (`dhcp.o is up to date`) | PASS |
| S3 disk | — | FAIL (RM.BIN staging gap; fixed) | **PASS 354 s → OUT.IMG** |
| ACCEPTANCE-COMPLETE | — | — | **stages=6** |

Final: `[SELFHOST] ACCEPTANCE-COMPLETE stages=6`, zero FAIL tokens,
`System halt`, w5 outcome=0.

## Key serial lines (evidence-w12/)

`rcut-loop3.serial` (receipt):
```
[SELFHOST] phase=s0-check-done rc=0
[SELFHOST] phase=s1-user-done rc=0
[HELLO] entering main
[HELLO] argc=1
[HELLO] argv[0]="./HELLO.BIN"
[HELLO] main returned 0
[SELFHOST] phase=t25-run-done rc=0
[SELFHOST] phase=s2-kernel-done rc=0
MAKE.BIN: '/SRC/obj/arm/dhcp.o' is up to date.
[SELFHOST] phase=s2-inc-done rc=0
[SELFHOST] phase=s3-disk-done rc=0
[SELFHOST] ACCEPTANCE-COMPLETE stages=6
System halt from CPU 5.
```
`rcut-loop2.serial` (full serial S2): 56 `phase=kernel-compile rc=0` +
3 `phase=kernel-ascompile rc=0` + `phase=kernel-link-done rc=0` +
`phase=s2-kernel-done rc=0` (the discriminator: serial S2 is clean on the
merged combo).

## OUT.IMG

- 134,217,728 B (128 MiB FAT16), BPB 512/4/2/512/256, total 262144 sect.
- sha256 `fcfa8a3f1fc9d84cc5a5b1586c19f8787c8e02e05979b7748ecd3892ac6be1ce`
- Layout (acceptance.md §8.1 must-contain):
  `/boot/hobbyos.bin 218356 + limine.conf 99`, `/EFI/BOOT/BOOTAA64.EFI 315392`,
  `/HELLO.BIN 37360 /SH.BIN 195136 /LS.BIN 35200 /CAT.BIN 34960`,
  `/DESKTOP.BIN 555568 /CONSOLE.BIN 44992 /OUTPROG.BIN 37360 /OUTMARK.TXT 44`,
  `/bin/SH.BIN 195136`

## Standalone boot + in-OS-built program (S4/S5)

`tools/w7_accept_boot.py --image rcut-loop3.out.img --launch OUTPROG.BIN`
(`s4-boot.serial`):
```
Booting AArch64 OS  (6.5 s)
Mode: DESKTOP - Launching desktop...
[MENU] 9=OUTPROG.BIN
[LAUNCH] OUTPROG.BIN
[KERNEL] Process 3: OUTPROG.BIN exited unexpectedly or gracefully.
```

## Byte identity (byte-identity check)

```
61b3be6a…   outimg-HELLO.BIN  (in-OS-built, from OUT.IMG)
61b3be6a…   outimg-OUTPROG.BIN (in-OS-built, from OUT.IMG)
61b3be6a…   products-HELLO.BIN (from the boot disk /SRC)
61b3be6a…   obj/arm/hello.bin  (host)
= w6 recorded anchor.  In-OS build output is byte-identical to the host build.
hobbyos.bin  218356 B  00bad4a7…   (in-OS serial S2 kernel, in-OS MCOPY'd on)
hobbyos.elf 1372736 B  bf46a472…
check.o     21144 B    42a430f1…
```

## -j8 finding (new bug, logged for a kernel lane)

Attempt-1 `make -j8 -f Makefile.hobbyos kernel` compiled 57/59 TUs in ~20 min
wall (the parallel speedup is real) but the lld link lost
`net_rdma_test.o`: its short entry + LFN chain absent on the image, a torn
LFN fragment `'.o.tmp'` at slot 47 (between NET_TESTO and NET_RDMAO), while
the other concurrent clangs' temp pairs were deleted cleanly (w9 writer fix
holding) — i.e. a NEW concurrency-sensitive dirent loss under 8-way
concurrent rename-while-open, distinct from w9's serial close bug.
Discriminator: the SAME tree at -j1 builds all 59/59 + links + objcopy's
(attempt 2).  Receipt therefore runs S2 serial; -j8/-j4 remain
unproven-on-real-TUs and are recorded, not required.
(`rcut-attempt1-s2-j8-loss.md` + dirent dumps.)

## Staging fix logged

`tools/selfhost-stage.sh --tools` never staged RM.BIN → attempt-2 S3 failed
127 (`sh: command not found: /TOOLS/RM.BIN`).  Fixed in the --tools list
(rm.bin/RM.BIN) and re-staged in place; receipt S3 (354 s) clean.

## Files in evidence-w12/

```
rcut-loop.serial(.ts)        attempt-1 raw serial + timestamped capture
rcut-loop.phases.json        attempt-1 per-phase wall times
rcut-s2fail.dirents.txt      fatdir_dump of ::/SRC/obj/arm after the -j8 loss
rcut-s2fail.scan.txt         raw slot scan (torn .o.tmp LFN fragment, slot 47)
rcut-attempt1-s2-j8-loss.md  -j8 loss diagnosis
rcut-loop2.serial(.ts)       attempt-2 raw serial + capture (full serial S2)
rcut-loop2.phases.json       attempt-2 per-phase times
rcut-loop3.serial(.ts)       RECEIPT raw serial + capture (ACCEPTANCE-COMPLETE)
rcut-loop3.phases.json       receipt per-phase times (total 433.6 s)
rcut-loop3.out.img           extracted OUT.IMG (128 MiB)
rcut-out.log                 driver's own /SRC/OUT.LOG, pulled from the image
outimg-{HELLO,OUTPROG}.BIN   in-OS-built S1 program from OUT.IMG
outimg-hobbyos.bin           in-OS-built kernel from OUT.IMG
products-{hello,sh}.bin      in-OS-built S1 programs from the boot disk
products-hobbyos.elf         in-OS-built kernel ELF
products-check.o             S0 check.o
s4-boot.serial               standalone OUT.IMG boot + OUTPROG launch
```
