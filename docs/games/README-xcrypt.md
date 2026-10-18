# XCRYPT.BIN

**Source** `src/user/rogue/xcrypt.c` · **Launcher name** `XCRYPT.BIN` · **Slot** pinned #4

A rogue-like dungeon-crawler on the HobbyOS native ABI (X11-style window app).
Turn-based movement through a generated dungeon.

## Launch
Start menu -> **XCRYPT.BIN** -> Enter. Launches into its window.

## Controls
| Key | Action |
|---|---|
| ← / → / ↑ / ↓ | move / walk |
| Space | interact / pick up |
| Esc | exit to launcher |

## What it demonstrates
- Native turn-based game loop; a strong T5 liveness probe (keys move the player,
  large frame diffs — used by the GB-040 evidence rig / GB-046 perf-cycle).
- Windowed native game path shared with XNIBBLER.

## Evidence
`run_xcrypt_test.py`, `src/host/xcrypt_test.c`, `tools/t5/replays/xcrypt.jsonl`;
exercised in GB-046/GB-045 perf + GB-043 replay runs.
