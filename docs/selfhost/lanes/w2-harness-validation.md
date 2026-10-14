# Lane W2-harness — validation evidence (2026-10-14)

Host: workstation, 64 cores, QEMU 10.2.1, TCG. Branch `selfhost/w2-harness`
(tip `3699651`). Companion to `w2-harness-report.json` (machine summary).

## 1. What this wave shipped

- `src/user/selfhost/selfhost.c` → **SELFHOST.BIN**: the in-OS acceptance
  build driver (acceptance.md §5.3). Reads a command script
  (`/SRC/SELFHOST.RUN`, overridable via argv[1]), executes the S0..S3 stages
  (`<phase>|<tool>|<args>` lines), prints every `[SELFHOST]` phase marker to
  the serial console and mirrors it to `/SRC/OUT.LOG` (§5.4 evidence).
  Tool resolution probes `/TOOLS`, `/BOOT`, `/` via `access()` (a stat-based
  probe — never creates, unlike `open()`'s find-or-create). When the tool is
  missing the driver reports a **clean staged failure** (`phase=<sX>-fail
  reason=tool-missing rc=127` + `ABORT` + exit 1) — the honest wave-2 outcome
  with no in-OS make/clang yet. The tool-present path (pipe + `spawn2` +
  relay + `waitpid`) is implemented and documented for wave 3.
- `tools/run_selfhost_build_test.py` — the M4 verify loop (acceptance §5.1
  exact QEMU line): boots the staged disk, launches the driver through the
  Apps menu via QMP keystrokes (index parsed from the `[MENU]` dump,
  `run_console_test.py` precedent), and verdicts from markers with a
  FAIL-token scan (§8.4). Classifier distinguishes `boot_failure` /
  `driver_absent` / `script_missing` / `stage_failure` / `hang` / `success` +
  `harness_error`; writes a result JSON; `--expected-class` CI gate;
  `--classifier-self-test` (7/7 green).
- `tools/selfhost-stage.sh` v2 — adds `/tmp` ensure (idempotent), stages
  `::/SELFHOST.BIN` (root) + `::/SRC/SELFHOST.RUN`, keeps `STAGE_LIBCXX`
  default = 1 (per acceptance §2; flips at the M3 AD-9 freeze), stays
  idempotent (no-`-f` abort verified; `-f` replace + root-bin overwrite
  verified with `mcopy -o`).
- `fat16img.py` — extended for deep paths + VFAT long names (the §5.4
  evidence-recovery contract): root behavior unchanged, `get SRC/OUT.LOG`
  and deep LFN trees now work (hashes verified).

## 2. Boot disk used for the run

The workstation's live `disk.img` boots an **APPS_TEST** kernel whose
`[MENU]` dump is the test harness's 12-app list — it cannot surface an
arbitrary driver in a real desktop menu. The harness is specified against
repo-built disks, so this wave built one from the lane tree:

```text
truncate -s 1073741824 disk-w2-desktop.img
mformat -i disk-w2-desktop.img ::            # FAT16, spc=32 — same geometry
                                             # as the real disk (L5's technique)
mmd  ::/EFI ::/EFI/BOOT ::/boot
mcopy bootloader/BOOTAA64.EFI   ::/EFI/BOOT/
mcopy <arm limine.conf template> ::/boot/limine.conf
mcopy hobbyos.bin (lane build)  ::/boot/hobbyos.bin
mcopy DESKTOP.BIN SH.BIN HELLO.BIN SELFHOST.BIN  ::/
tools/selfhost-stage.sh -f disk-w2-desktop.img    # full tree + driver + RUN
```

Lane build: `make -j16 ARCH=arm all obj/arm/desktop.bin obj/arm/sh.bin
obj/arm/hello.bin obj/arm/selfhost.bin` — clean, SELFHOST.BIN = 48,576 B,
zero warnings, cstyle-clean. `hobbyos.bin` = 214,592 B via `llvm-objcopy`.
`mkfs.fat`/`dd` are agent-blocked; mformat (same mtools family as the
staging tool) is the L5-proven formatting path.

## 3. Staging verification (mdir + content hashes, L5's technique)

`selfhost-stage.sh -f disk-w2-desktop.img`: **2338 files, 30,243,199 bytes,
exit 0**; 1365/2338 basenames exceed 8.3 (informational, LFN proven).

| staged file | on-image sha256 | source sha256 | result |
|---|---|---|---|
| `::/SELFHOST.BIN` | `11fbcce5…` | obj/arm/selfhost.bin `11fbcce5…` | MATCH |
| `::/SRC/SELFHOST.RUN` | `1ddcdf38…` | `1ddcdf38…` | MATCH |
| `::/SRC/src/kernel/program_loader.c` (LFN) | `e00bd74c…` | `e00bd74c…` | MATCH |
| `::/tmp` | exists (pre-existing; ensure idempotent) | — | OK |

Idempotence: re-run without `-f` → aborts rc 1 ("already contains /SRC");
`-f` re-stage → rc 0 with the root `::/SELFHOST.BIN` overwritten in place
(`mcopy -o` — plain `mcopy` refuses on a pre-existing destination).

## 4. Boot + driver run (the honest wave-2 bar)

Exact QEMU line = acceptance §5.1 (QMP socket added for the keystroke
driver). Committed curated serial: `w2-harness-serial-excerpt.log`.

```text
linux: Loading kernel `boot():/boot/hobbyos.bin`...Booting AArch64 OS...
Mode: DESKTOP - Launching desktop...
[MENU] 5=SELFHOST.BIN          <- real desktop Apps menu: driver enumerated
[MENU] count=8
[LAUNCH] SELFHOST.BIN
Loading program for scheduler (v2 AS): SELFHOST.BIN
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
[SELFHOST] phase=s0-check-start
[SELFHOST] phase=s0-check-fail tool=MAKE.BIN reason=tool-missing rc=127
[SELFHOST] ABORT stage=s0-check missing-tool=MAKE.BIN
[SELFHOST] FAILED-AT stage=s0-check
[KERNEL] Process 3: SELFHOST.BIN exited unexpectedly or gracefully.
```

Runner verdict (`--expected-class stage_failure` → exit 0): boot marker
6.5 s; driver menu index 5; **failure_class = stage_failure** (clean,
first missing tool — NOT a hang); markers driver_up=1, phase_start=1,
phase_fail=1, abort=1, complete=0, fail_token=0; whole run 11.1 s; S4/S5 =
not_run (no OUT.IMG — the in-OS build cannot succeed until MAKE.BIN exists).

Evidence recovery (§5.4): `python3 fat16img.py get disk-w2-desktop.img
SRC/OUT.LOG` returns the driver's log (s0-start / s0-fail / ABORT /
FAILED-AT) — the O_CREAT fix made this real (see §5).

## 5. Real bugs found and fixed by the validation

1. **`file_open()` honors O_CREAT** (POSIX, fs.c): `open("/SRC/OUT.LOG", 0)`
   on a missing file returns ENOENT — create-on-open is gone. The driver
   now passes `O_CREAT`; OUT.LOG is actually written. (First run showed
   NO OUT.LOG on the disk — silent log loss would have rippled into wave-3
   evidence.)
2. **mtools `mcopy` refuses overwrite**: `-f` re-staging failed silently on
   the root `::/SELFHOST.BIN` until `mcopy -o` (the file lives outside the
   /SRC mdeltree).
3. **`fat16img.py` only resolved root paths** despite acceptance §5.4's
   deep-path claim — subdir + VFAT-LFN support added (hashes verified).
4. **Runner FAIL scan matched the driver's own `-fail` / `FAILED-AT`**
   markers; excluded (`(?<!-)\bFAIL(?:ED)?\b(?!-AT)`).

## 6. Classifier matrix (real boots, not just unit cases)

| image | outcome | class (exit) |
|---|---|---|
| staged DESKTOP disk | driver clean-fails at s0 | stage_failure (0 with --expected-class) |
| workstation APPS_TEST disk, SELFHOST.BIN not in its menu | driver unreachable | driver_absent (11) |
| 64 MiB zeroed raw image | kernel never comes up | boot_failure (10) |
| synthetic captures (7) | classifier unit matrix | 7/7 PASS |

`hang` is covered by the unit matrix (serial shows DRIVER-UP + sX-start but
no terminal marker); a real hang is not producible today by the driver
(missing tools fail fast) — noted as wave-3 work.

## 7. Raw artifacts (uncommitted, per program evidence policy)

`~/.hermes/cache/scratch/w2/` — `disk-w2-desktop.img` (staged), `run/`
(runner transcripts, result-final.json, full serials), `build-lane.log`
(lane make log), `stage-v2*.transcript.txt`.
