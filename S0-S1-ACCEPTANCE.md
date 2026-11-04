# S0/S1 Final Acceptance Run (GB-053, consolidated by GB-190)

- **One head for both legs:** `ticket/53-acceptance` rebased onto `origin/main`
  `f24dcf60` -> head `8c934f6b`. The guest is main `f24dcf60`; the only branch
  changes are this report, the manifest and `evidence/GB-053/` (no guest code).
- **Runner:** `tools/t5/acceptance_run.py` at `8c934f6b`. The three runner fixes
  measured on this branch (`DESKTOP=hobby` boot, `f4` inter-visit close, arch
  footer) are now on main via PR #354, so the rebase carries no tool diff.
- **CPUs:** default **8** on both arches (`QEMU_SMP` unset; Makefile
  `QEMU_SMP ?= 8`). No `QEMU_SMP=1` anywhere in this matrix.
- **Verdict from OBSERVED evidence (GB-075, no fabricated PASS):** each game
  must LOAD (serial load line / live process), render a live whole-frame, and
  (after the first title) differ from the previous one over the whole frame by
  at least the stated margin. A title that never loads is FAIL, not skipped.

## Intel — PASS (5/5) at the default 8 CPUs

`DESKTOP=hobby python3 tools/t5/acceptance_run.py --arch intel`

```
[MENU] count=186
$ grep -c "FATAL kernel-mode fault" serial.log
0
```

| game | slot | loaded | live frame | result |
|---|---|---|---|---|
| PONG.BIN | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | PASS |
| XNIBBLER.BIN | 20 | yes | True | PASS |
| XCRYPT.BIN | 21 | yes | True | PASS |
| NES.BIN | 22 | yes | True | PASS |

Summary: **PASS**. Serial, five PPMs, generated report and manifest:
`evidence/GB-053/intel-8cpu-20261104/`.

## ARM — PASS (5/5) at the default 8 CPUs

`DESKTOP=hobby python3 tools/t5/acceptance_run.py --arch arm`

```
[MENU] count=185
$ grep -c "FATAL kernel-mode fault" serial.log
0
```

| game | slot | loaded | live frame | result |
|---|---|---|---|---|
| PONG.BIN | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | PASS |
| XNIBBLER.BIN | 20 | yes | True | PASS |
| XCRYPT.BIN | 21 | yes | True | PASS |
| NES.BIN | 22 | yes | True | PASS |

Summary: **PASS**. Serial, five PPMs, generated report and manifest:
`evidence/GB-053/arm-8cpu-20261104/`.

## Corpus scope (what "signed off" covers here)

The acceptance corpus is the tool's `PINNED` list — PONG, MILLIPED, XNIBBLER,
XCRYPT, NES (5 titles). The desktop also pins DOOM and STARDUST; neither is in
this corpus: DOOM needs the fetch-time IWAD (`doom1.wad`, not on a fresh image)
and STARDUST is not in the corpus. So this run's evidence is "the five pinned
titles render live and distinct on both arches", not "every pinned title was
exercised"; DOOM's own playthrough is measured separately in GB-091/GB-034
(PR #237).

## History (not current)

The 2026-11-02 matrix recorded before the SMP resume fix:

| config | result |
|---|---|
| ARM, default 8 CPUs | 5/5 PASS |
| intel, `QEMU_SMP=1` | 5/5 PASS |
| intel, default 8 CPUs | 4/4 boots halt before the menu |

Its falsifier record, quoted verbatim from PR #314:

```
[RESUME-SPSR] pid=1 spsr=0x00010202 IF=1
[RESUME-DIAG] pid=1 region=yes prot=3 dest=0x00000017FFFFFEB8 covers=1
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x14
  RIP: 0x00000010000043B0  CS: 0x1B  CPU: 5  pid: -1
[FRAMESNAP] fatal pushed rip=0x10000043B0 ... match=1
[DEADCLAIM] slot=1 pid=1 name=DESKTOP.BIN claimed only by halted cpus
System halt from CPU 1
```

`cr2 == rip == 0x10000043B0` (the image entry page), `pid=-1` (no current
process, so the user-mode #PF branch at `trap.c:3337` is skipped and the
kernel-mode handler halts the CPU), `match=1` (not GB-147's torn frame). That
shape is the contrast for the current intel 8-CPU PASS above; it is kept here
as history, not as a current result. The old per-leg reports remain under
`evidence/GB-053/arm-8cpu/` and `evidence/GB-053/intel-1cpu/`.
