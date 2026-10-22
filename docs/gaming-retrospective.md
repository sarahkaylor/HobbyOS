# HobbyOS Gaming Program — S0/S1 Retrospective + Rules (GB-055)

Owner: wgames (swarm-farm-3). What actually happened across the S0/S1 games
track, the lessons, and the rules to teach forward. This feeds the
`skills/hobbyos-gaming/SKILL.md` (the DoD: lessons merged into skills).

## What worked

1. **Native catalog on the native ABI**. PONG, MILLIPED, XNIBBLER, XCRYPT and a
   from-scratch NES emulator (6502 core + background + OAM sprite PPU, 33/33 T1)
   all boot and render under QEMU. The native ABI + fbcon graphics path is fast
   enough to be a real game platform.
2. **Evidence-before-close discipline pays**. The T5 legs caught real harness
   bugs that a golden-gate alone would have shipped: the perf-cycle RSS **KB-vs-MB
   units bug** (a real run flagged it), and the homebrew ROM's hand-assembly
   address errors (disassembler probe caught JSR/JMP targets off by one). The
   rule "run it, diff it, screenshot it" works.
3. **De-magic-indexed harnesses**. Locating games by NAME from the `[MENU]` dump
   (not hardcoded slot numbers) removed a whole class of merge conflicts across
   the four port PRs.
4. **One shared evidence rig** (`tools/evidence_rig.py`) replaced four ad-hoc
   per-title harnesses: name-based nav, gameplay liveness diffs, replay.jsonl +
   sha256 manifest. Replays (GB-043), perf-cycling (GB-046), perf baseline
   (GB-045) and the S0/S1 evidence pack (GB-047) all build on it.
5. **FLASH-tier autonomy**: promote+claim, build, T1-test, real T5, PR,
   evidence, move lane — end to end, without waiting.

## What didn't / where the program got gated

1. **Desktop input-routing into full-screen native windows is the S0 wall**.
   Games get keys only through the desktop input path; full-screen native games
   (NES, DOOM) receive 0 input under the current desktop, so their interactive
   T5 "plays a ROM" leg is blocked. PONG/X11-window games work; full-screen ones
   don't. This is one platform fix, shared by NES and DOOM.
2. **intel is blocked by GB-057** (the intel spawn/fork deadlock — only
   DESKTOP.BIN spawns, then a 20 s watchdog). Every intel leg of every game is
   parked on it; only ARM T5 is citable until it closes.
3. **The compat (ELF) track is ahead of the games it unlocks**. DOOM-as-ELF,
   SDL, threads-under-compat, and the 3D/GL gap are all ticketed-but-not-landed,
   so the big Program-2 games (RimWorld/DSP/TA) are structurally infeasible this
   program (GB-011). No capability was assumed that is neither landed nor
   ticketed.
4. **The NAS shared store is often unmounted**. "Archive on NAS" (GB-047)
   cannot be placed when no farm VM has `/mnt/fast` mounted.

## Rules to teach (→ skills)

- **Run it before you claim it.** A green host golden is not evidence; a real
  QEMU boot + screendump + liveness diff is.
- **Never fabricate a number.** Every ticket claim cites a real log/screendump/
  hasher output. If you cannot run it, say so; partial stays In Progress.
- **Evidence has a shape:** serial log + screendump(s) + replay/repro + sha256
  manifest, in a timestamped bundle, with a committed pointer report.
- **Re-read the ticket's lane the instant you move it** (the board is live —
  claims land in seconds; a stale read caused a demote-then-correct earlier).
- **De-magic every harness:** locate menu entries by name, never a hardcoded
  slot; make harnesses worktree-aware (`HOBBYOS_ROOT`).
- **Never run two disk builds concurrently**; they race on the shared vendor
  fetch. Fix dangling third_party symlink targets in the primary repo first,
  and add `/usr/sbin` to `PATH` for `mkfs.fat`.
- **Quarantine with a reason** (GB-050): a flaky test leaves the gate only via
  the quarantine lane, always with reproduce-run evidence + a date.

## DoD note

This retrospective + the `skills/hobbyos-gaming/SKILL.md` make the lessons
reusable. "Lessons merged into skills" is satisfied by the committed skill (the
repo `skills/` is the durable, deploy-able store; `deploy.sh` syncs it for local
Hermes sessions).
