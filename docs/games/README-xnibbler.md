# XNIBBLER.BIN

**Source** `src/user/x11/apps/xnibbler/main.c` · **Launcher name** `XNIBBLER.BIN` · **Slot** pinned #3

A snake ("nibbler") arcade game running as an X11-style window app on HobbyOS
(native graphics + windowed input). One of the first native X11-app games.

## Launch
Start menu -> **XNIBBLER.BIN** -> Enter. Launches into its window.

## Controls
| Key | Action |
|---|---|
| ← / → / ↑ / ↓ | turn the snake |
| Esc | exit to launcher |

## What it demonstrates
- An animated, interactive native game (frames change every tick → good T5
  "liveness" probe; the shared GB-040 evidence rig uses it for gameplay diffs).
- X11-app window path (window + input) distinct from the full-screen native
  path used by PONG/MILLIPED.

## Evidence
`run_xnibbler_test.py`/`run_evidence_xnibbler.sh`, `xnibbler-EVIDENCE-REPORT.md`,
`tools/t5/replays/xnibbler.jsonl`; used by GB-043 replay + GB-046 perf-cycle +
GB-045 perf baseline runs.
