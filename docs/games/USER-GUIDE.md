# HobbyOS Games — User Guide

This guide covers the native game catalog (GB-052): how to boot the OS, launch
a game from the start menu, and the shared controls. All titles are pinned in
the launcher (`src/user/desktop.c` `pinned_games[]`).

## Catalog

| Launcher name | Game | Path | Type |
|---|---|---|---|
| `PONG.BIN` | PONG | `src/user/pong.c` | full-screen native |
| `MILLIPED.BIN` | Millipede-style shooter | `src/user/millipede.c` | full-screen native |
| `XNIBBLER.BIN` | Snake | `src/user/x11/apps/xnibbler/main.c` | X11-window native |
| `XCRYPT.BIN` | Rogue-like | `src/user/rogue/xcrypt.c` | X11-window native |
| `NES.BIN` | NES emulator + homebrew | `src/user/nes/` | full-screen native |

Each has a detailed README in `docs/games/README-<game>.md`.

## Booting + launching
1. Boot `disk.img` (ARM or intel QEMU / hardware).
2. From the desktop, open the start menu.
3. Select the game's `*.BIN` entry and press Enter. It launches (full-screen or
   into its window, per the table).
4. `Esc` returns to the launcher.

## Shared controls
- Movement: **arrow keys** (and **A/D** on PONG).
- Fire / interact: **Space** / **Enter** (per-game README lists exact bindings).
- Back to launcher: **Esc**.

## Notes
- The desktop input path delivers keys to running apps/games (see the S0 native
  input notes in `gaming.md`; desktop-launched games read keys via stdin).
- T5 repro: every pinned game has a replay script under `tools/t5/replays/`
  (GB-043), so any controller-observer can replay a scripted playthrough.
