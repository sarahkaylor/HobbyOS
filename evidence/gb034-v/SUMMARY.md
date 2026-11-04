# GB-034-V — independent DOOM gate, swarm-farm-1, 2026-11-04

Graded revision: `main 138a6874` (no PR; the card verifies the native DOOM port
on the current tree). Worktree branch `verify-gb034v`.

## Commands

```
bash tools/fetch_doom_wad.sh          # Freedoom 0.13.0 IWAD, 27 MiB, sha256 in fetch_wad.log
make disk.img DESKTOP=hobby ARCH=intel   # rc=0; image carries DOOM.BIN, DOOML.BIN, DOOM1.WAD
python3 tools/evidence_rig.py --game DOOM --bin DOOM.BIN --arch intel --run-seconds 8
python3 <keyrun script>               # boot, launch by name, screendump A, QMP key esc, screendump B
```

## Results

- Serial: `[LAUNCH] DOOM.BIN`, `[ARRIVED] DOOM.BIN rc>0`, `/doom1.wad` opened twice, the
  process stays alive (`[ALIVE-SET] ... DOOM.BIN`).
- Slot resolved by name: `[rig] DOOM.BIN slot: found by name = 18` (no magic index).
- Centre region 960x540: gameplay 178 colours, 518400/518400 nonzero; the launch
  frame was 3 colours / 90336 nonzero and the boot desktop 61 colours.
- Injected key: ESC changed 101008/518400 centre pixels (19.5%); the after frame is
  the Freedoom main menu (NEW GAME / OPTIONS / LOAD GAME / SAVE GAME / READ THIS! /
  QUIT GAME) — the game consumed the key, not the desktop.
- `replay.jsonl` present (key sequence, screendump events).

## Verdict

`VERDICT: GREEN sha=138a6874`.
