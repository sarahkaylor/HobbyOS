# Lane w3-accept — S4/S5 pre-test evidence (2026-10-14)

Branch `selfhost/w3-accept` (tip `f6ded59`). Pre-tests the host-side
acceptance stages S4 (extract OUT.IMG, boot it, assert kernel marker) and S5
(boot OUT.IMG, run a user program) with a **hand-built 128 MiB FAT16
OUT.IMG** (mtools `mformat`+`mcopy` only — mkfs.fat/dd are command-blocked
for this lane), before the in-OS S3 build can produce one.

Raw artifacts (uncommitted, per evidence policy):
`~/.hermes/cache/scratch/w3-accept/` — `runA-*.log(.s4/.s5)`,
`runB-*.log(.s4/.s5)`, `out-selfhost.img`, master images + sha256s.

## 1. Hand-built OUT.IMG (the frozen 128 MiB shape)

```text
truncate -s 134217728 img && mformat -i img ::     # FAT16 (NOT -F: mformat's
                                                   # -F means FAT32!)
```
Verified BPB (host python + fat16img): bps=512, spc=4, nfats=2,
root=512 entries, fatsize=256, total=262144 sectors = 134,217,728 B.
This matches dosfstools' pick at 128 MiB (w2-mkfs "128 MiB → spc4";
DISK_BLOCKS=131072 is 1024-byte units).

- `OUT.IMG-hand-min.img`  — kernel-less: `OUTPROG.BIN` + `OUTMARK.TXT`
  (the minimal "program + marker" mission shape)
- `OUT.IMG-hand-boot.img` — bootable: + `/boot/hobbyos.bin`,
  `/boot/limine.conf`, `/EFI/BOOT/BOOTAA64.EFI`, `/DESKTOP.BIN`,
  `/CONSOLE.BIN`, `/SH.BIN`
- `bootdisk.img` — 1 GiB pre-test boot disk (w2 DESKTOP shape):
  kernel + EFI + desktop set + `SELFHOST.BIN` + `/SRC/SELFHOST.RUN` +
  `OUT.IMG-hand-boot.img` planted at `::/OUT.IMG`

## 2. Run A — bootable OUT.IMG (S4 + S5 full path)

Runner: `tools/run_selfhost_build_test.py --image bootdisk.img
--s5-program OUTPROG.BIN --s5-wait 90 --expected-class stage_failure` →
**exit 0**.

Boot disk main boot (serial `runA-serial.log`):
```text
linux: Loading kernel `boot():/boot/hobbyos.bin`...Booting AArch64 OS...
Mode: DESKTOP - Launching desktop...
[MENU] 9=SELFHOST.BIN
[MENU] count=11
[LAUNCH] SELFHOST.BIN
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
[SELFHOST] phase=s0-check-start
[SELFHOST] phase=s0-check-fail tool=MAKE.BIN reason=tool-missing rc=127
[SELFHOST] ABORT stage=s0-check missing-tool=MAKE.BIN
[SELFHOST] FAILED-AT stage=s0-check
```
Driver verdict: `stage_failure` (clean, first missing tool MAKE.BIN — honest
while no in-OS toolchain; NOT a hang). Markers: driver_up=1, phase_start=1,
phase_fail=1, abort=1, complete=0, fail_token=0.

S4 — extraction + boot of the extracted image (`runA-serial.log.s4`):
```text
[run_selfhost] S4: extracted OUT.IMG (134217728 B, sha256 93a2f80a028f12ad)
linux: Loading kernel `boot():/boot/hobbyos.bin`...Booting AArch64 OS...
FAT-16 filesystem successfully initialized.
Mode: DESKTOP - Launching desktop...
```
s4 = extracted, 134,217,728 B, sha256 93a2f80a…, boot_ok=true, 6.5 s.

S4 byte-identity — runner's `out-selfhost.img` sha256
`93a2f80a028f12ada1a73ca2fb9e5e2c581ece0f3452d339c86bfd59085ea195` ==
hand-built `OUT.IMG-hand-boot.img` sha256 (identical). Host re-extraction:
`OUTPROG.BIN` from the runner file == `obj/arm/hello.bin`
(`7110d326…`); `OUTMARK.TXT` == planted marker text.

S5 — second boot with QMP; program launched from the OUT.IMG Apps menu
(`runA-serial.log.s5`):
```text
[MENU] 0=CONSOLE.BIN
[MENU] 1=BOOT
[MENU] 2=EFI
[MENU] 3=DESKTOP.BIN
[MENU] 4=SH.BIN
[MENU] 5=OUTPROG.BIN
[MENU] 6=OUTMARK.TXT
[MENU] count=7
[LAUNCH] OUTPROG.BIN
Loading program for scheduler (v2 AS): OUTPROG.BIN
[KERNEL] Process 3: OUTPROG.BIN exited unexpectedly or gracefully.
```
s5 = boot_ok=true, menu_index=5, launched=true, launch_line=true,
exit_line=true.

## 3. Run B — kernel-less OUT.IMG (bare FAT16 expected class)

`OUT.IMG` swapped to `OUT.IMG-hand-min.img` (program+marker only, no kernel,
no EFI). Runner → **exit 0** with `--expected-class stage_failure`:

```text
VERDICT: stage_failure — S4 failed: OUT.IMG did not reach the kernel marker
'Booting AArch64 OS'
```
s4 = extracted, 134,217,728 B, sha256 a1c57e61… (== master), boot_ok=false.
S5 = blocked ("S4 boot failed — S5 skipped"). The AAVMF firmware's own
message on the bare image:
```text
BdsDxe: No bootable option or device was found.
BdsDxe: Press any key to enter the Boot Manager Menu.
```

## 4. W3 finding — the exact S5 OUT.IMG layout requirement

A standalone FAT16 OUT.IMG boots ONLY with the limine/AAVMF layout, and S5's
Apps-menu path needs the desktop set — the w3 pre-test empirically isolated
this (first S5 attempt had no DESKTOP.BIN on OUT.IMG: kernel printed
"Mode: DESKTOP ... Loading program for scheduler (v2 AS): DESKTOP.BIN" but
no desktop / `[MENU]` ever came up, because DESKTOP.BIN was not on the
image; adding it fixed S5). Required content (8.3 root names):

| path | why |
|---|---|
| `/EFI/BOOT/BOOTAA64.EFI` | firmware PE loader (bootloader/BOOTAA64.EFI) |
| `/boot/limine.conf` | verbatim ARM template, `path: boot():/boot/hobbyos.bin` |
| `/boot/hobbyos.bin` | kernel flat binary (llvm-objcopy of the in-OS-built ELF) |
| `/DESKTOP.BIN` | **required** — kernel DESKTOP mode loads it at boot |
| `/CONSOLE.BIN`, `/SH.BIN` | desktop shell stack (w2 desktop-disk set) |
| `<S1 program>.BIN` | the in-OS-built user program to launch on S5 |
| marker file | host file-based check (§5.4 fallback) |

Program stdout is framebuffer-only when launched from the desktop, so S5's
serial evidence today is the loader lines (`[LAUNCH]`, `[KERNEL] Process …
exited`) + host file checks; a full program-marker-on-serial needs the
`MODE=selfhost` serial-console-fd driver (kernel follow-up).

## 5. Tool changes this wave (commit f6ded59)

- `tools/run_selfhost_build_test.py` — real S5 stage (boot OUT.IMG with
  QMP, launch `--s5-program` from the Apps menu, assert `[LAUNCH]` +
  process-exit lines); S4 records the extracted sha256 and boots the image;
  S4/S5 override the verdict only on their own failure; new
  `--s5-program` / `--s5-wait` options; docstring updated. Classifier
  self-test 7/7.
- `fat16img.py` — `read_file` linearized (chunk list + join): the 128 MiB
  OUT.IMG extraction dropped from >120 s (O(n²) bytes concat, effectively a
  hang) to ~1 s. This is what makes S4's automatic extraction viable in the
  real run.
- `tools/w3_accept_pretest.sh` — reproducible assembly of the hand OUT.IMG
  (min + bootable) and the 1 GiB pre-test boot disk; documents the mformat
  FAT16 recipe and the `-F`=FAT32 pitfall.

## 6. Classifier matrix (this wave)

| image | outcome | class (exit) |
|---|---|---|
| pre-test boot disk + bootable OUT.IMG | driver clean-fails s0; S4/S5 green | stage_failure (0 with --expected-class) |
| same + kernel-less OUT.IMG | S4 boot of OUT.IMG fails | stage_failure, "S4 failed: OUT.IMG did not reach the kernel marker" (0 with --expected-class) |
| synthetic captures | classifier unit matrix | 7/7 PASS |
