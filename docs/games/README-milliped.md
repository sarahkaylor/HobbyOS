# MILLIPED.BIN

**Source** `src/user/millipede.c` · **Launcher name** `MILLIPED.BIN` · **Slot** pinned #2

A centipede-style arcade shooter on the native HobbyOS ABI. Full-screen native
game (not an X11 window).

## Launch
Start menu -> **MILLIPED.BIN** -> Enter.

## Controls
| Key | Action |
|---|---|
| ← / → / ↑ / ↓ | move the player |
| Space / Enter | fire |
| Esc | exit to launcher |

## What it demonstrates
- Native full-screen game loop + input path shared with PONG.
- Enemy wave simulation each tick on the native ABI.

## Evidence
T5 replay script `tools/t5/replays/milliped.jsonl`; native-port T1/T5 gate in
`host_tests` where wired; see `gaming.md` S0 native track.
