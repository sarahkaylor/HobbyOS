---
name: hobbyos-gaming
description: "Use when porting a game to HobbyOS, running a native-game T5 (QEMU) evidence run, or producing per-game evidence (screendumps, replays, perf). Covers the evidence rig, de-magic name-based launching, the shared evidence-before-close rules, and the platform blockers (full-screen input routing, intel GB-057, compat gating)."
version: 1.0.0
author: wgames (HobbyOS Gaming swarm)
license: GPL-2.0
platforms: [linux]
metadata:
  hermes:
    tags: [hobbyos, games, native-port, qemu, t5, evidence, replay, perf]
    related_skills: [hobbyos-build-and-run, hobbyos-gui-test, hobbyos-run-tests, hobbyos-screenshot, hobbyos-add-userland-program]
---

# HobbyOS Gaming: evidence rig + native-port rules

## Overview

HobbyOS has a native-game track (games compiled to the HobbyOS ABI with the
fbcon/graphics lib, launched from the desktop start menu) and a compatibility
track (ELF dual-ABI, SDL, ... — mostly ticketed, not landed). This skill is for
the **native track**: porting a game, running it under QEMU, and producing the
real evidence a ticket needs. Everything below saved from actual mistakes that
cost the swarm time.

## When to Use

- Porting/adding a native game (see also `hobbyos-add-userland-program`).
- Running any QEMU game T5 run, replay, or perf/leak pass.
- Writing a ticket `EVIDENCE:` comment for a games ticket.

## Core rules (do not skip)

- **Evidence-before-close is law.** A ticket claim must cite a real run: serial
  log + QEMU screendump(s) + a repro/replay + sha256 manifest, in a timestamped
  bundle with a committed pointer report. If you cannot run it, say so — a
  partially-done ticket stays **In Progress**, never "Done".
- **Never fabricate a number.** A green host golden is not evidence; a real T5
  boot + screendump + liveness diff is. Fabricated output is worse than an
  honest blocker.
- **Run the T5 leg before trusting the harness.** Real QEMU runs caught: an
  RSS **KB-vs-MB** unit bug in the perf-cycle bound, and hand-assembled homebrew
  ROM branch addresses off by one. Always have the T5 diff check what the T1
  already passed.
- **Re-read the ticket's lane the instant you move it.** The board is live and
  other agents claim in seconds; a stale view causes a wrong demote.

## De-magic native launching (avoid merge conflicts)

- Locate a game in the launcher by **NAME** in the `[MENU]` dump, never a
  hardcoded slot number. Keep slot constants informational only.
- Make every harness worktree-aware: `root = os.environ.get("HOBBYOS_ROOT", <repo>)`
  and print the resolved root.
- Menu nav pattern: `click(38, H-26+13)` (open start menu) -> `home` -> `down`
  × slot -> `ret`. `H = 1080`.

## The shared evidence rig

`python3 tools/evidence_rig.py --game <g> --bin <GAME.BIN> --arch arm --root $PWD
--input "up,left,up,left"` boots once, launches by name, captures
`evidence/<game>/<ts>/{serial.log, *.ppm, replay.jsonl, manifest.json}`. Use it
for every per-game run. Per-game replay scripts live in `tools/t5/replays/`
(replay via `tools/t5/replay.py`); perf/leak pass via `tools/perf_game_cycle.py`;

## Platform blockers (cite honestly, don't "fix" around them)

- **Full-screen native input routing**: full-screen native games (NES, DOOM)
  get 0 keys under the current desktop; the input path only reaches X11-style
  windows (PONG, XNIBBLER, XCRYPT). Their interactive "plays a ROM" T5 leg is
  blocked on one shared platform fix. Never claim an interactive input sub-assert
  for a full-screen game as passing.
- **intel is gated on GB-057** (intel spawn/fork deadlock — only DESKTOP.BIN
  spawns, then a 20 s watchdog). Only **ARM** T5 is citable until GB-057 closes;
  always note the intel gating in evidence.
- **compat/ELF track is not landed**, so Program-2 games (RimWorld/DSP/TA) are
  infeasible this program (GB-011). Do not assume a capability that is neither
  landed nor ticketed.

## The disk build gotchas (native track)

In a fresh worktree `make disk.img` fails until you:
1. Fix **dangling third_party symlink targets** — worktree `src`/`build-host`
   symlinks point at the primary repo whose target dirs don't exist.
   `find .worktrees/<wt>/third_party -type l` and `mkdir -p` each dangling
   target **in the primary** repo, then re-run that vendor's `fetch.sh`.
   (`mkdir -p` on a dangling symlink short-circuits and does NOT create it.)
2. `export PATH=/usr/sbin:$PATH` before `make disk.img` (for `mkfs.fat`).
3. Never run two disk builds concurrently (they race on the shared vendor fetch
   -> "mkdir: Already exists").
4. It needs a **second** `make disk.img` run (first pass builds the bins);
   the second pass assembles a valid FAT16 image.

## Definition of "done" for a native game

boots + renders under QEMU (ARM) with a screendump showing the game frame,
host T1 green, a replay script in `tools/t5/replays/`, and an `EVIDENCE:` comment
with the bundle pointer. Interactive input sub-asserts are only required for
games on the working input path until the full-screen routing fix lands.
