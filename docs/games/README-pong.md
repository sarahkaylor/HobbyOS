# PONG.BIN

**Source** `src/user/pong.c` · **Launcher name** `PONG.BIN` · **Slot** pinned #1

Classic two-paddle PONG on the native HobbyOS ABI. Full-screen native game
using the OS graphics library (not an X11 window).

## Launch
Open the start menu, select **PONG.BIN**, press Enter. Full-screen.

## Controls
| Key | Action |
|---|---|
| ← / → | move the paddle |
| A / D | move the paddle (alternate bindings) |
| Esc | exit to launcher |

## What it demonstrates
- Native game loop on the HobbyOS ABI (graphics init, draw, flip, input).
- Reads the keyboard through the desktop input path (stdin key bytes); see the
  S0 native-games input notes in `gaming.md`.

## Evidence
`src/user/actual_pong.png` / `actual_pong.ppm`; T1 host: `PONG_TEST_BIN`
(`src/host/pong_test.c`) in `host_tests`; T5 replay script
`tools/t5/replays/pong.jsonl`.
