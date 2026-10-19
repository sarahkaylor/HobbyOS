# NES.BIN

**Source** `src/user/nes/` (emulator) + `src/user/nes/homebrew_rom.h` (homebrew ROM)
**Launcher name** `NES.BIN` · **Slot** pinned #5

A compact single-source NES emulator (6502 CPU + minimal PPU) on the native
HobbyOS ABI, running a small swarm-authored homebrew ROM. Full-screen.

## Components
- CPU: 6502 interpreter via a 256-entry decode table; host-tested for shifts,
  branches, indirect JMP, stack ops, and the full bus.
- PPU: 2 KiB VRAM, NROM cart, nametable background + OAM sprite rendering.
- Homebrew: a 43+-byte hand-authored NROM ROM (license-clean) that boots to a
  bright field with a dark band and a rendered sprite, and toggles the band on
  the controller A button.

## Launch
Start menu -> **NES.BIN** -> Enter. Boots the homebrew immediately (full-screen).

## Controls
| Input | Action |
|---|---|
| A (gamepad/desktop A) | toggle the dark band (renders the homebrew responsive) |
| Esc | exit to launcher |

## What it demonstrates
- A from-scratch emulator on the native ABI — CPU + PPU correctness proven by
  `make nes_test_host` (T1 33/33) and an ARM T5 boot+render (`bright_frac=0.97`).
- Swarm-authored, license-clean ROM (no copyrighted NES BIOS/ROM).

## Evidence
`src/host/nes_test.c` (T1), `tools/t5/replays/nes.jsonl`; T5 run + sprite/OAM
work on the GB-035 branch/PR.
