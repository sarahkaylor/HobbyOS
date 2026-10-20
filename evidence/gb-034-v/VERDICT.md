# GB-034-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-20.
Tree: `origin/main` @ `e2b8e93` (clean worktree; `HEAD == origin/main`).
Gate: the two DoD commands, plus the closest executable form of the rig (`--bin`/`--slot` supplied,
because the DoD's own command line is not runnable — see 2).

**Bottom line: GB-034's deliverable is not on `main` at all, and on intel the desktop it would launch
from is killed by the GB-068 fault before any menu exists.** Every DoD item is unmet.

## 1. `make disk.img` — builds, but the image contains no DOOM

```
$ make disk.img
make disk.img EXIT=0                       # 01_disk_img.log

$ mdir -i disk.img ::/ | grep -i doom
grep EXIT=1 (1 = no match)                 # 03_image_doom_check.log
$ mdir -i disk.img ::/ | wc -l
177
$ mdir -i disk.img ::/ | grep -iE "PONG|XNIBBLER|STARDUST|XCRYPT|NES"
PONG_T   BIN    562320      PONG     BIN     69960
STARDUST BIN     72536      XNIBBLER BIN     88440
XCRYPT   BIN    117184      NES      BIN    173472
```

The image is complete and carries the other five games — the absence is DOOM-specific, not a broken build.

## 2. `python3 tools/evidence_rig.py --game DOOM --arch intel` — cannot start as written

```
$ python3 tools/evidence_rig.py --game DOOM --arch intel       # 02_rig_exact_command.log
evidence_rig.py: error: the following arguments are required: --bin, --slot
EXIT=2
```

The rig still takes a menu name and a **magic slot index**; the by-name lookup that the DoD assumes is
GB-059's deliverable and GB-059 is still In Review. So the DoD's gate command is not executable on the
shipped tree — a gate-definition defect in this mint, reported as such rather than as an implementation failure.

## 3. Rig run with `--bin DOOM.BIN --slot 0` (closest executable form) — no menu, desktop killed

Two boots, both ended with the desktop process dead and `System halt`; `[MENU]` appears **0 times** in
either serial log, so the rig's `find_menu_slot()` can never return a slot:

```
$ python3 tools/evidence_rig.py --game DOOM --bin DOOM.BIN --slot 0 --arch intel    # sample 1 (031834Z)
[KERNEL] pid=1 (DESKTOP.BIN) memory fault VA=0x00000000755ADBE0 PC=0x0000001000008610 rw=r
         in=PROT region=NONE cr3frame=0x0000000020C03000 asroot=0x0000000020C03000 -> killed
[KERNEL] Process 1: DESKTOP.BIN exited unexpectedly or gracefully.
System halt from CPU 5.
RuntimeError: QMP socket never appeared: /tmp/evidence-qmp-DOOM        (EXIT=1)

# sample 2 (032202Z), same fault, different stack base:
[KERNEL] pid=1 (DESKTOP.BIN) memory fault VA=0x000000007558DBE0 PC=0x0000001000008610 … -> killed
[KERNEL] Process 1: DESKTOP.BIN exited unexpectedly or gracefully.
[rig] DOOM.BIN slot: expected=0 found=None
[rig] FATAL: DOOM.BIN not in start-menu dump                            (EXIT=1)
```

`VA = <stack base> + 0xDBE0` at a constant `PC=0x1000008610`, 2 of 2 boots: this is **GB-068**'s
family-B signature (kernel stack pointer in user space), not a DOOM defect — attributed, not
re-diagnosed here. Consequence: **no intel T5 evidence of any kind is obtainable on this tree**, for
DOOM or for any other title, until GB-068 is fixed. ARM was not attempted: with no DOOM.BIN in the
image there is nothing to launch on either arch.

## 4. DOOM on the shipped tree — zero files

```
$ git ls-files | grep -i doom
(no output — 0 files)
$ grep -n "pinned_games" -A2 src/user/desktop.c
813: static const char *const pinned_games[] = {
       "PONG.BIN", "MILLIPED.BIN", "STARDUST.BIN", "XNIBBLER.BIN", "XCRYPT.BIN", "NES.BIN",
```

No DOOM source, no WAD, no Makefile rule, and the launcher's pinned list would drop a `DOOM.BIN` even
if one were staged (the menu caps at `pinned_games[]`). The port lives on the unmerged GB-034 branch
(PR #1 was DIRTY against `main` as the ticket context says), so there is nothing here to verify.

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | serial log proves `DOOM.BIN` loaded and ran | **FAIL** — no DOOM.BIN exists on `main`; image has none; 0 `[MENU]` dumps |
| 2 | centre-region screendump non-blank and non-uniform | **FAIL** — never reached; desktop killed by GB-068 before any frame |
| 3 | injected keypress changes the frame | **FAIL** — never reached |
| 4 | `replay.jsonl` exists; no magic slot index | **FAIL** — rig aborts before writing it, and the gate command itself requires `--slot` |

## What has to happen before a re-verify

1. Land the DOOM port (`doomgeneric` + shareware WAD) on `main` **and** add `DOOM.BIN` to
   `src/user/desktop.c:pinned_games[]`, or the launcher drops it.
2. Fix GB-068 (intel desktop dies at `<stack base> + 0xDBE0`, PC `0x1000008610`) — without it no title
   launches on intel at all.
3. Re-mint this verify ticket with a runnable gate (either after GB-059's by-name lookup lands, or with
   `--bin DOOM.BIN --slot N` spelled out and the slot taken from the `[MENU]` dump at run time).
