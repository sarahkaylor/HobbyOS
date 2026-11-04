# GB-188 — intel acceptance at the default 8 CPUs, current main

- **Tree under test:** `origin/main` = `7c8f16a6` ("Merge pull request #347 from
  sarahkaylor/ticket/184-games-rebase"), `make disk.img DESKTOP=hobby ARCH=intel` rc=0.
- **Command:** `DESKTOP=hobby python3 tools/t5/acceptance_run.py --arch intel`
  (`QEMU_SMP` unset -> Makefile default `QEMU_SMP ?= 8`; no `QEMU_SMP=1`).
- **Runner:** `tools/t5/acceptance_run.py` from PR #314 head `0ecb037d` — main's
  copy still boots without `DESKTOP=hobby` and closes with `esc`; PR #314 is the
  card's named vehicle and carries both fixes. The kernel/boot under test is main
  `7c8f16a6`; only the Python runner differs.
- **Run:** 2026-11-04T08:15:43Z, outdir `evidence/acceptance/20261104T081451Z/`,
  5 pinned titles.

## Clause 1 — the 8-CPU boot reaches the menu

```
[MENU] count=186
```

## Clause 2 — zero kernel-mode faults

```
$ grep -c "FATAL kernel-mode fault" serial.log
0
$ grep -c "FATAL" serial.log
0
```

(The `[RESUME-SPSR]`/`[RESUME-DIAG]` lines at serial 445-455 are the normal resume
trace, `covers=1` each; no `Vector: 14`, no `System halt`.)

## Clause 3 — every title graded on its observed load

```
[LAUNCH] PONG.BIN       [ARRIVED] PONG.BIN rc>0
[LAUNCH] MILLIPED.BIN   [ARRIVED] MILLIPED.BIN rc>0
[LAUNCH] XNIBBLER.BIN   [ARRIVED] XNIBBLER.BIN rc>0
[LAUNCH] XCRYPT.BIN     [ARRIVED] XCRYPT.BIN rc>0
[LAUNCH] NES.BIN        [ARRIVED] NES.BIN rc>0
```

| game | slot | loaded | live frame | result |
|---|---|---|---|---|
| PONG.BIN | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | PASS |
| XNIBBLER.BIN | 20 | yes | True | PASS |
| XCRYPT.BIN | 21 | yes | True | PASS |
| NES.BIN | 22 | yes | True | PASS |

Summary: **PASS** (5/5)

## Clause 4 — falsifier: PR #314's recorded pre-fix shape

PR #314 measured intel at the default 8 CPUs as **4/4 boots halt before the menu**:

```
[RESUME-SPSR] pid=1 spsr=0x00010202 IF=1
[RESUME-DIAG] pid=1 region=yes prot=3 dest=0x00000017FFFFFEB8 covers=1
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x14
  RIP: 0x00000010000043B0  CS: 0x1B  CPU: 5  pid: -1
[FRAMESNAP] fatal pushed rip=0x10000043B0 ... match=1
[DEADCLAIM] slot=1 pid=1 name=DESKTOP.BIN claimed only by halted cpus
System halt from CPU 1
```

The current run has `[MENU] count=186`, 0 FATAL lines, and 5/5 live titles at the
same 8-CPU setting. GB-053's intel-8-CPU row is therefore stale on current main.

## Clause 5 — evidence

`serial.log`, the five `.ppm` + `.png` screendumps, `REPORT.md` (the generated
report) and `MANIFEST.json` (sha16 of each frame) are in this directory.
