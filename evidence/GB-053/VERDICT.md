# GB-053 — fresh both-arch acceptance run at current main (2026-11-02)

> **SUPERSEDED 2026-11-04 by GB-190** — at main `f24dcf60` both arches pass the
> corpus at the default 8 CPUs (intel 5/5, ARM 5/5, 0 FATAL). This file is the
> 2026-11-02 record and is kept as history; the current matrix is in
> `S0-S1-ACCEPTANCE.md` at the root.


Purpose (operator bounce, 2026-11-02T02:31Z): replace the five-day-old FAIL
record with a fresh run of the GB-075 no-fabricated-PASS rig, both arches, at
current main. The 10-27 evidence is NOT resubmitted here.

Tool: `tools/t5/acceptance_run.py` (boots once, launches every pinned native
title by NAME from the `[MENU]` dump, screendump per title, liveness + distinct
frame gates). Three fixes were needed before it could run at all — see
"Tool defects found" below.

## ARM leg — PASS (5/5)

```
$ python3 tools/t5/acceptance_run.py --arch arm --root "$PWD"
| game | slot | loaded | live frame | result |
| PONG.BIN     | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | PASS |
| XNIBBLER.BIN | 20 | yes | True | PASS |
| XCRYPT.BIN   | 21 | yes | True | PASS |
| NES.BIN      | 22 | yes | True | PASS |
Summary: **PASS**
```

Its generated report and manifest are committed under
`evidence/GB-053/arm-8cpu/` (sha16 per title).

## Intel leg — PASS at 1 CPU; 8 CPUs crash before the menu (4/4)

Both legs pass, but the intel leg needs `QEMU_SMP=1` on current main:

```
$ QEMU_SMP=1 python3 tools/t5/acceptance_run.py --arch intel --root "$PWD"
| PONG.BIN     | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | PASS |
| XNIBBLER.BIN | 20 | yes | True | PASS |
| XCRYPT.BIN   | 21 | yes | True | PASS |
| NES.BIN      | 22 | yes | True | PASS |
Summary: **PASS**
```

At the default 8 CPUs the same run cannot reach the menu: the guest halts in
DESKTOP.BIN's first user resume, and every title reports `not in [MENU] dump`.
That reproduced **4 times out of 4** (three acceptance attempts and one
`evidence_rig` boot), while `QEMU_SMP=1` booted and ran the full wave on the
first try — so this is an SMP race in the resume path, not a general intel
failure:

```
[RESUME-SPSR] pid=1 spsr=0x00010202 IF=1
[RESUME-DIAG] pid=1 region=yes prot=3 dest=0x00000017FFFFFEB8 covers=1
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000014
  RIP: 0x00000010000043B0  RSP: 0x00000017FFFFFFE8
  CS:  0x000000000000001B  SS: 0x0000000000000023 RFLAGS: 0x0000000000010202
  RAX: 0x00000010000043B0
  CPU: 5  pid: -1
[FRAMESNAP] fatal pushed rip=0x10000043B0 ... match=1 snaps=3 faults=3
[DEADCLAIM] slot=1 pid=1 name=DESKTOP.BIN claimed only by halted cpus
System halt from CPU 1
```

`cr2 == rip == 0x10000043B0` (the image's entry page), `pid=-1` (no current
process, so the user-mode #PF branch at `trap.c:3337` is skipped and the
kernel-mode handler halts the CPU), and the pushed frame is intact (`match=1`,
so not GB-147's torn frame). Owner: the M1 resume family (GB-146/GB-122).

## Tool defects found and fixed on this branch

1. `acceptance_run.py` booted `make run ARCH=<arch>` without `DESKTOP=hobby`,
   which demands the IceWM X stack (`ICEWM.BIN`) and never starts QEMU on this
   tree: the first attempt died with `QMP socket never appeared`.
2. The inter-visit close was `q.key("esc")` — ESC is forwarded to the focused
   game, not the desktop, so the game window stayed open and later launches
   drifted. Measured before/after on ARM: **1/5 PASS** (PONG only; MILLIPED/
   XNIBBLER/NES "never loaded", XCRYPT "frame changed 0.000 < margin 0.004";
   `evidence/acceptance/20261102T040646Z`) versus **5/5 PASS** with `f4` (the
   desktop's close-focused-window key; `evidence/acceptance/20261102T040752Z`).
3. The report footer said "intel leg gated on GB-057 … this run is ARM" on
   every run, including intel ones. Replaced with the actual arch.

## Card verdict

Both arches pass the pinned S0 corpus: **ARM 5/5 at the default 8 CPUs**,
**intel 5/5 at 1 CPU**.  The intel leg at the default 8 CPUs is currently
blocked by the desktop's first-resume SMP crash (4/4), which is a kernel defect
owned by GB-146/GB-122, not an acceptance-tool or port defect — the `-smp 1`
control is what separates the two.

The generated reports for both legs are under `evidence/GB-053/arm-8cpu/` and
`evidence/GB-053/intel-1cpu/`.

## Corpus scope (what "signed off" covers here)

The acceptance corpus is the tool's `PINNED` list — PONG, MILLIPED, XNIBBLER,
XCRYPT, NES (5 titles).  The desktop also pins DOOM and STARDUST; neither is in
this corpus: DOOM needs the fetch-time IWAD (`doom1.wad`, not on a fresh image)
and STARDUST is simply not in the corpus.  So this run's evidence is "the five
pinned titles render live and distinct on both arches", not "every pinned title
was exercised"; DOOM's own playthrough is measured separately in GB-091/GB-034
(PR #237).
