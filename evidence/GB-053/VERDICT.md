# GB-053 — fresh both-arch acceptance run at current main (2026-11-02)

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

Committed artefacts: `S0-S1-ACCEPTANCE.md`, `S0-S1-ACCEPTANCE-MANIFEST.json`
(arch arm, sha16 per title).

## Intel leg — FAIL, the guest halts before the menu populates (2/2 attempts)

Both attempts end the same way; the run then reports every title as
`not in [MENU] dump`:

```
| PONG.BIN | - | no | False | FAIL |  - not in [MENU] dump
| MILLIPED.BIN | - | no | False | FAIL |  - not in [MENU] dump
| XNIBBLER.BIN | - | no | False | FAIL |  - not in [MENU] dump
| XCRYPT.BIN | - | no | False | FAIL |  - not in [MENU] dump
| NES.BIN | - | no | False | FAIL |  - not in [MENU] dump
Summary: **FAIL**
```

The serial has no `[MENU] count=` line at all; it has a kernel-mode fault at
user VA 0x10000043B0 with the pushed frame matching the read frame, and the
resume path halting the CPU that holds DESKTOP.BIN (`evidence/acceptance/20261102T040333Z/serial.log`):

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=14 rip=0x00000010000043B0
         cr2=0x00000010000043B0 pid=-1 err=4 cr3=0x0000000020C03000 pd480=0x00000000810000FB
[FRAMESNAP] fatal pushed rip=0x00000010000043B0 cs=0x1B rsp=0x17FFFFFFE8 ss=0x23 |
            read rip=0x00000010000043B0 cs=0x1B rsp=0x17FFFFFFE8 ss=0x23 match=1 snaps=3 faults=3
[DEADCLAIM] slot=1 pid=1 name=DESKTOP.BIN claimed only by halted cpus -- excluded from the
            wave-end predicate (no CPU can run it; the resume path halted that CPU)
System halt from CPU 1
```

Second attempt (`evidence/acceptance/20261102T040333Z`) is identical; the first
(`20261102T040013Z`) crashed the same way. This is the M1 resume/frame family
owned by GB-146/GB-147 (the pushed frame is intact — `match=1` — so this is not
GB-147's torn-frame symptom; the CPU halts on a #PF whose cr2 equals the
instruction address).

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

S0+S1 is **not signed off**: the ARM leg is green 5/5, the intel leg cannot
produce a menu because the guest halts in the resume path. The honest state is
"ARM green, intel blocked by GB-146/GB-147", and the card carries that FAIL
rather than the 10-27 one.
