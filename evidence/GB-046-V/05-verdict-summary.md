# GB-046-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-2bee80ef`, host **swarm-farm-2**
- Date: 2026-10-21 (UTC), run 03:45:14 → 04:25 (≈ 40 min of cycling)
- Revision run: **79d14e7dc2ce166b1886aa93e7e2d6d21b49cc52** — `HEAD` == `origin/main` (fetched), tree clean
- Env: `dist-staging` copied from this VM's main checkout; `make ARCH=arm disk.img` pre-built

## Gate, as written (with cycle flags sized to ≥30 min)

```
ARCH=arm python3 tools/perf_game_cycle.py \
    --games 'XNIBBLER.BIN:25,XCRYPT.BIN:25,NES.BIN:25' --cycles 18 --hold 25 --root $PWD
```

All three titles are on the image (`mdir`: `XNIBBLER BIN 88440`, `XCRYPT BIN 117184`, `NES BIN 173472`).

## Result — `[perf] summary: FAIL`, EXIT=1

```
[perf] whole-run RSS: floor=335.57 MB peak=363.34 MB growth=27.77 MB (bound 64 MB) -> PASS
[perf] summary: FAIL
EXIT=1
```

Tally over the 54 attempted visits (18 cycles × 3 games):

```
visits judged (cycle= lines): 12
frozen=True: 12
frozen=False: 0
launch FAILs: 42
```

## DoD scorecard

| clause | result |
|---|---|
| 1 ≥30 min of cycling | **PASS** — 03:45:14 → 04:25, 18 cycles, 54 visits |
| 2 no game flagged FROZEN | **FAIL** — 12/12 judged visits `frozen=True`, 0 `frozen=False` |
| 3 QEMU RSS within `max_growth_mb` | **PASS** — floor 335.57, peak 363.34, growth **27.77 MB** < 64 |
| 4 a sample hanging >30 s is a defect | **FIRES** — guest serial goes silent (0 new bytes for the last three cycles ≈ 7 min); final line `make: *** [Makefile:2337: run] Killed` |

## Why the FROZEN verdict is not an artifact of the crop

The rig crops `0,0,1820,1010` to exclude the ticking desktop clock. On the cycle-0 XNIBBLER window the
crop is **byte-identical across all 25 samples**, and a pixel diff of s0 vs s24 shows the *only* changed
pixels in the whole 1920×1080 frame are the clock digits:

```
full-frame diff bbox: (1865, 1063, 1895, 1070)
differing px in crop 0,0,1820,1010: 0
differing px in clock corner 1820,0,1920,1080: 29
```

The frame does show the game (a snake with `SCORE 0` and the food pellet — `logs/xnibbler-c0-s5.png`), and
`src/user/x11/apps/xnibbler/main.c:304-314` steps the snake every 90 ms and repaints. So a 25-second stretch
with **zero** changed playfield pixels while the clock advances is the game not advancing — either the game
stalled after launch or the compositor is not re-blitting its window; both are the same failure the DoD names,
and neither is the clock false-positive the crop was built to avoid.

## The second failure: the rig stops launching its own games

42 of 54 visits never launched the requested title, and the serial shows the menu navigation drifting onto
apps that were never requested:

```
[LAUNCH] XCRYPT.BIN     6
[LAUNCH] XNIBBLER.BIN   4
[LAUNCH] NES.BIN        4
[LAUNCH] STARDUST.BIN   3      <- never requested
[LAUNCH] TASKS.BIN      2      <- never requested
[LAUNCH] XEYES.BIN      1
[LAUNCH] PONG.BIN       1
```

Two of the "launch" lines are in fact console diagnostics spliced into the stream (`[LAUNCH] XN[ALIVE-SET] …`),
the interleaving hazard the program has recorded before.

## Verdict

**RED.** GB-046 (#46) → In Progress: FROZEN on every judged visit (DoD 2), the guest hangs past the 30 s rule
(DoD 4), and the rig launches the wrong apps for 42/54 visits. The memory assertion (DoD 3) does pass, and the
duration (DoD 1) is genuine.
